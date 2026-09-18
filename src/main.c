/*
 * casio-midi-bridge — user-space CoreMIDI bridge for Casio USB keyboards that are not
 * USB-MIDI class compliant (USB ID 07CF:6802 and friends) and therefore get no port on macOS.
 *
 * How it works:
 *   - IOKit matching notifications tell us when a known keyboard is plugged in or removed.
 *   - We open the device, select configuration 1, claim interface 0 and locate its IN and
 *     OUT endpoints. The wire format is plain USB-MIDI 1.0 event packets (see usbmidi.h),
 *     the device just doesn't advertise the audio/MIDI-streaming interface class.
 *   - A virtual CoreMIDI source and destination are created while the keyboard is connected.
 *     Every app (GarageBand, Logic, Ableton, ...) sees them like a normal MIDI port.
 *
 * No kernel extension, no root, no entitlements. MIT license.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/usb/IOUSBLib.h>
#include <getopt.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbmidi.h"

#ifndef VERSION
#define VERSION "dev"
#endif

typedef struct { uint16_t vid, pid; const char *desc; } device_id;

/* IDs handled by the Linux snd-usb-audio "Yamaha-style" quirk; same wire format. */
static const device_id known_ids[] = {
    { 0x07CF, 0x6802, "Casio USB keyboard (LK / CTK / WK / PX / AP series, ~2001-2010)" },
    { 0x07CF, 0x6801, "Casio PL-40R" },
};
#define N_KNOWN (sizeof known_ids / sizeof known_ids[0])

typedef struct {
    /* options */
    const char *port_name;
    int verbose;
    int vid_override, pid_override;

    /* USB state (valid while a keyboard is open) */
    io_service_t service;
    io_object_t interest;
    IOUSBDeviceInterface182 **dev;
    IOUSBInterfaceInterface **intf;
    CFRunLoopSourceRef evtsrc;
    uint8_t in_pipe, out_pipe;
    uint16_t out_max;
    uint8_t in_buf[64];

    /* MIDI state */
    MIDIClientRef client;
    MIDIEndpointRef src, dst;
    usbmidi_encoder enc;
    uint8_t out_buf[64];
    int out_len;
    pthread_mutex_t lock;   /* guards intf/out_buf between the run loop and MIDI threads */

    IONotificationPortRef notify;
} bridge;

static bridge g = { .port_name = "Casio USB MIDI", .vid_override = -1, .pid_override = -1,
                    .lock = PTHREAD_MUTEX_INITIALIZER };

static void logmsg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[casio-midi-bridge] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
static void logpkt(const char *dir, const uint8_t p[4]) {
    if (g.verbose) fprintf(stderr, "  %s %02X %02X %02X %02X\n", dir, p[0], p[1], p[2], p[3]);
}

/* ------------------------------------------------------------------ CoreMIDI */

static void midi_dispose_ports(void) {
    if (g.dst) { MIDIEndpointDispose(g.dst); g.dst = 0; }
    if (g.src) { MIDIEndpointDispose(g.src); g.src = 0; }
}

static void on_midi_out(const MIDIPacketList *pl, void *rc, void *src);

static void midi_create_ports(uint16_t vid, uint16_t pid, const char *desc) {
    CFStringRef name = CFStringCreateWithCString(NULL, g.port_name, kCFStringEncodingUTF8);
    MIDISourceCreate(g.client, name, &g.src);
    MIDIDestinationCreate(g.client, name, on_midi_out, NULL, &g.dst);
    CFRelease(name);
    /* Stable unique IDs so apps remember the port across reconnects. */
    SInt32 uid = ((SInt32)vid << 16) | pid;
    MIDIObjectSetIntegerProperty(g.src, kMIDIPropertyUniqueID, uid);
    MIDIObjectSetIntegerProperty(g.dst, kMIDIPropertyUniqueID, uid + 1);
    CFStringRef model = CFStringCreateWithCString(NULL, desc, kCFStringEncodingUTF8);
    MIDIEndpointRef eps[2] = { g.src, g.dst };
    for (int i = 0; i < 2; i++) {
        MIDIObjectSetStringProperty(eps[i], kMIDIPropertyManufacturer, CFSTR("Casio"));
        MIDIObjectSetStringProperty(eps[i], kMIDIPropertyModel, model);
    }
    CFRelease(model);
    usbmidi_encoder_init(&g.enc, 0);
    g.out_len = 0;
}

/* ------------------------------------------------------------------ USB -> MIDI */

static void usb_close(void);

static void on_usb_in(void *refcon, IOReturn result, void *arg0) {
    (void)refcon;
    size_t n = (size_t)arg0;
    if (!g.intf) return;                            /* closed while a read was in flight */
    if (result == kIOReturnAborted || result == kIOReturnNotResponding ||
        result == kIOReturnNoDevice || result == kIOReturnNotAttached) {
        logmsg("read stopped (0x%x), closing device", result);
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{ usb_close(); });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return;
    }
    if (result == kIOReturnSuccess && n >= 4) {
        Byte buf[1024];
        MIDIPacketList *pl = (MIDIPacketList *)buf;
        MIDIPacket *pkt = MIDIPacketListInit(pl);
        MIDITimeStamp now = mach_absolute_time();
        for (size_t i = 0; i + 4 <= n; i += 4) {
            const uint8_t *p = g.in_buf + i;
            size_t len = usbmidi_decode(p);
            if (!len) continue;
            logpkt("IN ", p);
            pkt = MIDIPacketListAdd(pl, sizeof buf, pkt, now, len, p + 1);
            if (!pkt) break;
        }
        if (pl->numPackets) MIDIReceived(g.src, pl);
    } else if (result != kIOReturnSuccess) {
        logmsg("read error 0x%x", result);
    }
    IOReturn r = (*g.intf)->ReadPipeAsync(g.intf, g.in_pipe, g.in_buf, sizeof g.in_buf, on_usb_in, NULL);
    if (r) {
        logmsg("ReadPipeAsync failed 0x%x, closing device", r);
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{ usb_close(); });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    }
}

/* ------------------------------------------------------------------ MIDI -> USB */

static void usb_flush_out(void) {
    if (!g.out_len || !g.intf) { g.out_len = 0; return; }
    IOReturn r = (*g.intf)->WritePipe(g.intf, g.out_pipe, g.out_buf, (UInt32)g.out_len);
    if (r) logmsg("write error 0x%x", r);
    g.out_len = 0;
}
static void emit_out(void *ctx, const uint8_t pkt[4]) {
    (void)ctx;
    logpkt("OUT", pkt);
    memcpy(g.out_buf + g.out_len, pkt, 4);
    g.out_len += 4;
    if (g.out_len >= g.out_max) usb_flush_out();
}
static void on_midi_out(const MIDIPacketList *pl, void *rc, void *src) {
    (void)rc; (void)src;
    pthread_mutex_lock(&g.lock);
    if (g.intf) {
        const MIDIPacket *p = &pl->packet[0];
        for (UInt32 k = 0; k < pl->numPackets; k++, p = MIDIPacketNext(p))
            usbmidi_encode(&g.enc, p->data, p->length, emit_out, NULL);
        usb_flush_out();
    }
    pthread_mutex_unlock(&g.lock);
}

/* ------------------------------------------------------------------ USB open/close */

static void usb_close(void) {
    pthread_mutex_lock(&g.lock);
    int was_open = g.dev != NULL;
    if (g.evtsrc) { CFRunLoopRemoveSource(CFRunLoopGetMain(), g.evtsrc, kCFRunLoopDefaultMode); g.evtsrc = NULL; }
    if (g.intf) { (*g.intf)->USBInterfaceClose(g.intf); (*g.intf)->Release(g.intf); g.intf = NULL; }
    if (g.dev)  { (*g.dev)->USBDeviceClose(g.dev); (*g.dev)->Release(g.dev); g.dev = NULL; }
    if (g.interest) { IOObjectRelease(g.interest); g.interest = 0; }
    if (g.service)  { IOObjectRelease(g.service); g.service = 0; }
    pthread_mutex_unlock(&g.lock);
    if (was_open) { midi_dispose_ports(); logmsg("keyboard disconnected"); }
}

static void on_device_interest(void *refcon, io_service_t service, natural_t msg, void *arg) {
    (void)refcon; (void)arg;
    if (msg == kIOMessageServiceIsTerminated && service == g.service) usb_close();
}

static const device_id *lookup_id(uint16_t vid, uint16_t pid, device_id *scratch) {
    for (size_t i = 0; i < N_KNOWN; i++)
        if (known_ids[i].vid == vid && known_ids[i].pid == pid) return &known_ids[i];
    scratch->vid = vid; scratch->pid = pid; scratch->desc = "USB keyboard"; return scratch;
}

static int usb_open(io_service_t service) {
    IOCFPlugInInterface **plug; SInt32 score; IOReturn r;
    if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score)) {
        logmsg("could not create device plug-in"); return -1; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID182), (LPVOID *)&g.dev);
    (*plug)->Release(plug);

    UInt16 vid = 0, pid = 0; (*g.dev)->GetDeviceVendor(g.dev, &vid); (*g.dev)->GetDeviceProduct(g.dev, &pid);
    device_id scratch; const device_id *id = lookup_id(vid, pid, &scratch);
    logmsg("found %04X:%04X %s", vid, pid, id->desc);

    r = (*g.dev)->USBDeviceOpen(g.dev);
    if (r == kIOReturnExclusiveAccess) { logmsg("device held by another process, seizing it"); r = (*g.dev)->USBDeviceOpenSeize(g.dev); }
    if (r) { logmsg("USBDeviceOpen failed 0x%x", r); goto fail; }

    UInt8 cfg = 0; (*g.dev)->GetConfiguration(g.dev, &cfg);
    if (cfg == 0) {
        IOUSBConfigurationDescriptorPtr cd;
        if ((*g.dev)->GetConfigurationDescriptorPtr(g.dev, 0, &cd)) { logmsg("no configuration descriptor"); goto fail; }
        if ((r = (*g.dev)->SetConfiguration(g.dev, cd->bConfigurationValue))) { logmsg("SetConfiguration failed 0x%x", r); goto fail; }
    }

    IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                      kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t it;
    if ((*g.dev)->CreateInterfaceIterator(g.dev, &req, &it)) { logmsg("interface iterator failed"); goto fail; }
    io_service_t isvc = IOIteratorNext(it); IOObjectRelease(it);
    if (!isvc) { logmsg("device has no USB interface"); goto fail; }
    kern_return_t kr = IOCreatePlugInInterfaceForService(isvc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(isvc);
    if (kr) { logmsg("could not create interface plug-in"); goto fail; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID *)&g.intf);
    (*plug)->Release(plug);
    if ((r = (*g.intf)->USBInterfaceOpen(g.intf))) { logmsg("USBInterfaceOpen failed 0x%x", r); goto fail; }

    UInt8 nep = 0; (*g.intf)->GetNumEndpoints(g.intf, &nep);
    g.in_pipe = g.out_pipe = 0; g.out_max = 64;
    for (UInt8 i = 1; i <= nep; i++) {
        UInt8 dir, num, type, ivl; UInt16 mps;
        (*g.intf)->GetPipeProperties(g.intf, i, &dir, &num, &type, &mps, &ivl);
        if (g.verbose) logmsg("  pipe %d: %s ep%d %s maxPacket=%d", i, dir == kUSBIn ? "IN " : "OUT", num,
                              type == kUSBBulk ? "bulk" : type == kUSBInterrupt ? "interrupt" : "other", mps);
        if (type != kUSBBulk && type != kUSBInterrupt) continue;
        if (dir == kUSBIn  && !g.in_pipe)  g.in_pipe = i;
        if (dir == kUSBOut && !g.out_pipe) { g.out_pipe = i; g.out_max = mps < 64 ? mps : 64; }
    }
    if (!g.in_pipe || !g.out_pipe) { logmsg("could not find IN and OUT endpoints"); goto fail; }

    if ((*g.intf)->CreateInterfaceAsyncEventSource(g.intf, &g.evtsrc)) { logmsg("async event source failed"); goto fail; }
    CFRunLoopAddSource(CFRunLoopGetMain(), g.evtsrc, kCFRunLoopDefaultMode);

    g.service = service; IOObjectRetain(service);
    IOServiceAddInterestNotification(g.notify, service, kIOGeneralInterest, on_device_interest, NULL, &g.interest);

    midi_create_ports(vid, pid, id->desc);
    if ((r = (*g.intf)->ReadPipeAsync(g.intf, g.in_pipe, g.in_buf, sizeof g.in_buf, on_usb_in, NULL))) {
        logmsg("ReadPipeAsync failed 0x%x", r); goto fail; }
    logmsg("keyboard connected, MIDI port \"%s\" is up", g.port_name);
    return 0;

fail:
    usb_close();
    return -1;
}

static void on_device_added(void *refcon, io_iterator_t it) {
    (void)refcon;
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        if (g.dev) logmsg("another keyboard appeared but one is already open; ignoring");
        else usb_open(svc);
        IOObjectRelease(svc);
    }
}

static int watch(uint16_t vid, uint16_t pid) {
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    SInt32 v = vid, p = pid;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v), np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), nv); CFDictionarySetValue(m, CFSTR(kUSBProductID), np);
    CFRelease(nv); CFRelease(np);
    io_iterator_t it;
    if (IOServiceAddMatchingNotification(g.notify, kIOFirstMatchNotification, m, on_device_added, NULL, &it)) return -1;
    on_device_added(NULL, it);   /* arm the notification and pick up already-attached devices */
    return 0;
}

/* ------------------------------------------------------------------ main */

static void on_signal(int s) { (void)s; CFRunLoopStop(CFRunLoopGetMain()); }

static void usage(FILE *f) {
    fprintf(f,
        "casio-midi-bridge %s — CoreMIDI port for non-class-compliant Casio USB keyboards\n\n"
        "usage: casio-midi-bridge [options]\n"
        "  -n, --name NAME   name of the virtual MIDI port (default \"Casio USB MIDI\")\n"
        "      --vid 0xVVVV  only watch this USB vendor ID (with --pid)\n"
        "      --pid 0xPPPP  only watch this USB product ID\n"
        "  -v, --verbose     log every USB-MIDI packet\n"
        "  -l, --list        list built-in USB IDs and exit\n"
        "  -V, --version     print version and exit\n"
        "  -h, --help        this help\n", VERSION);
}

int main(int argc, char **argv) {
    static const struct option opts[] = {
        { "name", required_argument, NULL, 'n' }, { "vid", required_argument, NULL, 1 },
        { "pid", required_argument, NULL, 2 },    { "verbose", no_argument, NULL, 'v' },
        { "list", no_argument, NULL, 'l' },       { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },       { 0, 0, 0, 0 } };
    int c;
    while ((c = getopt_long(argc, argv, "n:vlVh", opts, NULL)) != -1) {
        switch (c) {
        case 'n': g.port_name = optarg; break;
        case 1: g.vid_override = (int)strtol(optarg, NULL, 0); break;
        case 2: g.pid_override = (int)strtol(optarg, NULL, 0); break;
        case 'v': g.verbose = 1; break;
        case 'l': for (size_t i = 0; i < N_KNOWN; i++) printf("%04X:%04X  %s\n", known_ids[i].vid, known_ids[i].pid, known_ids[i].desc); return 0;
        case 'V': printf("casio-midi-bridge %s\n", VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if ((g.vid_override >= 0) != (g.pid_override >= 0)) { fprintf(stderr, "--vid and --pid must be given together\n"); return 2; }

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    if (MIDIClientCreate(CFSTR("casio-midi-bridge"), NULL, NULL, &g.client)) { logmsg("MIDIClientCreate failed"); return 1; }

    g.notify = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(g.notify), kCFRunLoopDefaultMode);
    if (g.vid_override >= 0) watch((uint16_t)g.vid_override, (uint16_t)g.pid_override);
    else for (size_t i = 0; i < N_KNOWN; i++) watch(known_ids[i].vid, known_ids[i].pid);

    if (!g.dev) logmsg("waiting for a keyboard to be plugged in...");
    CFRunLoopRun();

    usb_close();
    MIDIClientDispose(g.client);
    logmsg("exiting");
    return 0;
}
