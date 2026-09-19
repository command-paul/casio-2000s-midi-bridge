/*
 * bridge.c — core of casio-2000s-midi-bridge: user-space CoreMIDI bridge for Casio USB keyboards
 * that are not USB-MIDI class compliant (USB ID 07CF:6802 and friends) and therefore get no
 * port on macOS. Public API in bridge.h; used by src/cli.c and the app in app/.
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
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge.h"
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
    char port_name[64];
    int verbose;

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
    pthread_mutex_t lock;   /* guards intf/out_buf/stats between the run loop and MIDI threads */

    IONotificationPortRef notify;
    io_iterator_t iters[8]; int n_iters;
    CFRunLoopTimerRef retry;        /* re-open attempts while another process holds the device */
    io_service_t retry_service;
    int running;
    bridge_stats stats;
} bridge;

static bridge g = { .lock = PTHREAD_MUTEX_INITIALIZER };

static double now_abs(void) { return CFAbsoluteTimeGetCurrent(); }

static void logmsg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[casio-2000s-midi-bridge] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
static void logerr(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    pthread_mutex_lock(&g.lock);
    vsnprintf(g.stats.last_error, sizeof g.stats.last_error, fmt, ap);
    g.stats.errors++;
    pthread_mutex_unlock(&g.lock);
    va_end(ap);
    fprintf(stderr, "[casio-2000s-midi-bridge] error: %s\n", g.stats.last_error);
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
    pthread_mutex_lock(&g.lock);
    g.stats.connected = 1; g.stats.busy = 0; g.stats.vid = vid; g.stats.pid = pid;
    g.stats.connected_since = now_abs();
    g.stats.msgs_in = g.stats.msgs_out = g.stats.notes_in = g.stats.bytes_in = g.stats.bytes_out = 0;
    g.stats.last_in_len = g.stats.last_out_len = 0;
    snprintf(g.stats.device_desc, sizeof g.stats.device_desc, "%s", desc);
    snprintf(g.stats.port_name, sizeof g.stats.port_name, "%s", g.port_name);
    pthread_mutex_unlock(&g.lock);
}

/* ------------------------------------------------------------------ USB -> MIDI */

static void usb_close(void);

static void on_usb_in(void *refcon, IOReturn result, void *arg0) {
    (void)refcon;
    size_t n = (size_t)arg0;
    if (!g.intf) return;                            /* closed while a read was in flight */
    if (result != kIOReturnSuccess) {
        if (result == kIOReturnAborted || result == kIOReturnNoDevice || result == kIOReturnNotAttached)
            logmsg("read stopped (0x%x), closing device", result);
        else
            logerr("USB read failed (0x%x), closing device", result);
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ usb_close(); });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return;
    }
    if (n >= 4) {
        Byte buf[1024];
        MIDIPacketList *pl = (MIDIPacketList *)buf;
        MIDIPacket *pkt = MIDIPacketListInit(pl);
        MIDITimeStamp now = mach_absolute_time();
        pthread_mutex_lock(&g.lock);
        for (size_t i = 0; i + 4 <= n; i += 4) {
            const uint8_t *p = g.in_buf + i;
            size_t len = usbmidi_decode(p);
            if (!len) continue;
            logpkt("IN ", p);
            pkt = MIDIPacketListAdd(pl, sizeof buf, pkt, now, len, p + 1);
            if (!pkt) break;
            g.stats.msgs_in++; g.stats.bytes_in += len;
            if ((p[1] & 0xF0) == 0x90 && p[3]) g.stats.notes_in++;
            if (p[1] != 0xFE) { memcpy(g.stats.last_in, p + 1, 3); g.stats.last_in_len = (uint8_t)len; g.stats.last_in_time = now_abs(); }
        }
        pthread_mutex_unlock(&g.lock);
        if (pl->numPackets) MIDIReceived(g.src, pl);
    }
    IOReturn r = (*g.intf)->ReadPipeAsync(g.intf, g.in_pipe, g.in_buf, sizeof g.in_buf, on_usb_in, NULL);
    if (r) {
        logerr("ReadPipeAsync failed (0x%x), closing device", r);
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ usb_close(); });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    }
}

/* ------------------------------------------------------------------ MIDI -> USB */

static void usb_flush_out(void) {
    if (!g.out_len || !g.intf) { g.out_len = 0; return; }
    IOReturn r = (*g.intf)->WritePipe(g.intf, g.out_pipe, g.out_buf, (UInt32)g.out_len);
    if (r) logerr("USB write failed (0x%x)", r);
    g.out_len = 0;
}
static void emit_out(void *ctx, const uint8_t pkt[4]) {
    (void)ctx;
    logpkt("OUT", pkt);
    memcpy(g.out_buf + g.out_len, pkt, 4);
    g.out_len += 4;
    size_t len = usbmidi_decode(pkt);
    g.stats.msgs_out++; g.stats.bytes_out += len;
    memcpy(g.stats.last_out, pkt + 1, 3); g.stats.last_out_len = (uint8_t)len; g.stats.last_out_time = now_abs();
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

int bridge_send(const uint8_t *midi, size_t len) {
    pthread_mutex_lock(&g.lock);
    int ok = g.intf != NULL;
    if (ok) { usbmidi_encode(&g.enc, midi, len, emit_out, NULL); usb_flush_out(); }
    pthread_mutex_unlock(&g.lock);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ USB open/close */

static void usb_close(void) {
    pthread_mutex_lock(&g.lock);
    int was_open = g.dev != NULL;
    if (g.evtsrc) { CFRunLoopRemoveSource(CFRunLoopGetMain(), g.evtsrc, kCFRunLoopCommonModes); g.evtsrc = NULL; }
    if (g.intf) { (*g.intf)->USBInterfaceClose(g.intf); (*g.intf)->Release(g.intf); g.intf = NULL; }
    if (g.dev)  { (*g.dev)->USBDeviceClose(g.dev); (*g.dev)->Release(g.dev); g.dev = NULL; }
    if (g.interest) { IOObjectRelease(g.interest); g.interest = 0; }
    if (g.service)  { IOObjectRelease(g.service); g.service = 0; }
    g.stats.connected = 0; g.stats.busy = 0; g.stats.connected_since = 0;
    pthread_mutex_unlock(&g.lock);
    if (was_open) { midi_dispose_ports(); logmsg("keyboard disconnected"); }
}

static void retry_cancel(void) {
    if (g.retry) { CFRunLoopTimerInvalidate(g.retry); CFRelease(g.retry); g.retry = NULL; }
    if (g.retry_service) { IOObjectRelease(g.retry_service); g.retry_service = 0; }
    pthread_mutex_lock(&g.lock); g.stats.busy = 0; pthread_mutex_unlock(&g.lock);
}

static void on_device_interest(void *refcon, io_service_t service, natural_t msg, void *arg) {
    (void)refcon; (void)arg;
    if (msg == kIOMessageServiceIsTerminated && service == g.service) usb_close();
}
static void on_retry_interest(void *refcon, io_service_t service, natural_t msg, void *arg) {
    (void)refcon; (void)arg;
    if (msg == kIOMessageServiceIsTerminated && service == g.retry_service) { logmsg("busy keyboard unplugged"); retry_cancel(); }
}

static const device_id *lookup_id(uint16_t vid, uint16_t pid, device_id *scratch) {
    for (size_t i = 0; i < N_KNOWN; i++)
        if (known_ids[i].vid == vid && known_ids[i].pid == pid) return &known_ids[i];
    scratch->vid = vid; scratch->pid = pid; scratch->desc = "USB keyboard"; return scratch;
}

static int usb_open(io_service_t service) {
    IOCFPlugInInterface **plug; SInt32 score; IOReturn r;
    if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score)) {
        logerr("could not create device plug-in"); return -1; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID182), (LPVOID *)&g.dev);
    (*plug)->Release(plug);

    UInt16 vid = 0, pid = 0; (*g.dev)->GetDeviceVendor(g.dev, &vid); (*g.dev)->GetDeviceProduct(g.dev, &pid);
    device_id scratch; const device_id *id = lookup_id(vid, pid, &scratch);
    logmsg("found %04X:%04X %s", vid, pid, id->desc);

    r = (*g.dev)->USBDeviceOpen(g.dev);
    if (r == kIOReturnExclusiveAccess) {
        /* Another copy of the bridge (e.g. the LaunchAgent) owns it. Don't fight; report and retry. */
        (*g.dev)->Release(g.dev); g.dev = NULL;
        pthread_mutex_lock(&g.lock);
        g.stats.busy = 1; g.stats.vid = vid; g.stats.pid = pid;
        snprintf(g.stats.device_desc, sizeof g.stats.device_desc, "%s", id->desc);
        snprintf(g.stats.last_error, sizeof g.stats.last_error, "keyboard is in use by another program (is the background service running?)");
        pthread_mutex_unlock(&g.lock);
        if (!g.retry) {
            logmsg("keyboard is in use by another program, will retry every 3 s");
            g.retry_service = service; IOObjectRetain(service);
            IOServiceAddInterestNotification(g.notify, service, kIOGeneralInterest, on_retry_interest, NULL, &g.interest);
            g.retry = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 3, 3, 0, 0, ^(CFRunLoopTimerRef t) {
                (void)t; if (g.dev || !g.retry_service) return;
                io_service_t svc = g.retry_service;
                IOObjectRetain(svc);
                if (g.interest) { IOObjectRelease(g.interest); g.interest = 0; }   /* drop the retry-phase interest */
                if (usb_open(svc) == 0) { IOObjectRelease(svc); if (g.retry) { CFRunLoopTimerInvalidate(g.retry); CFRelease(g.retry); g.retry = NULL; } if (g.retry_service) { IOObjectRelease(g.retry_service); g.retry_service = 0; } }
                else IOObjectRelease(svc);
            });
            CFRunLoopAddTimer(CFRunLoopGetMain(), g.retry, kCFRunLoopCommonModes);
        }
        return -1;
    }
    if (r) { logerr("USBDeviceOpen failed (0x%x)", r); goto fail; }

    UInt8 cfg = 0; (*g.dev)->GetConfiguration(g.dev, &cfg);
    if (cfg == 0) {
        IOUSBConfigurationDescriptorPtr cd;
        if ((*g.dev)->GetConfigurationDescriptorPtr(g.dev, 0, &cd)) { logerr("no configuration descriptor"); goto fail; }
        if ((r = (*g.dev)->SetConfiguration(g.dev, cd->bConfigurationValue))) { logerr("SetConfiguration failed (0x%x)", r); goto fail; }
    }

    IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                      kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t it;
    if ((*g.dev)->CreateInterfaceIterator(g.dev, &req, &it)) { logerr("interface iterator failed"); goto fail; }
    io_service_t isvc = IOIteratorNext(it); IOObjectRelease(it);
    if (!isvc) { logerr("device has no USB interface"); goto fail; }
    kern_return_t kr = IOCreatePlugInInterfaceForService(isvc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(isvc);
    if (kr) { logerr("could not create interface plug-in"); goto fail; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID *)&g.intf);
    (*plug)->Release(plug);
    if ((r = (*g.intf)->USBInterfaceOpen(g.intf))) { logerr("USBInterfaceOpen failed (0x%x)", r); goto fail; }

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
    if (!g.in_pipe || !g.out_pipe) { logerr("could not find IN and OUT endpoints"); goto fail; }

    if ((*g.intf)->CreateInterfaceAsyncEventSource(g.intf, &g.evtsrc)) { logerr("async event source failed"); goto fail; }
    CFRunLoopAddSource(CFRunLoopGetMain(), g.evtsrc, kCFRunLoopCommonModes);

    g.service = service; IOObjectRetain(service);
    IOServiceAddInterestNotification(g.notify, service, kIOGeneralInterest, on_device_interest, NULL, &g.interest);

    midi_create_ports(vid, pid, id->desc);
    if ((r = (*g.intf)->ReadPipeAsync(g.intf, g.in_pipe, g.in_buf, sizeof g.in_buf, on_usb_in, NULL))) {
        logerr("ReadPipeAsync failed (0x%x)", r); goto fail; }
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
    if (g.n_iters < (int)(sizeof g.iters / sizeof g.iters[0])) g.iters[g.n_iters++] = it;
    on_device_added(NULL, it);   /* arm the notification and pick up already-attached devices */
    return 0;
}

/* ------------------------------------------------------------------ public API */

int bridge_start(const char *port_name, int vid, int pid, int verbose) {
    if (g.running) return 0;
    snprintf(g.port_name, sizeof g.port_name, "%s", port_name && *port_name ? port_name : "Casio USB MIDI");
    g.verbose = verbose;
    memset(&g.stats, 0, sizeof g.stats);
    snprintf(g.stats.port_name, sizeof g.stats.port_name, "%s", g.port_name);

    if (MIDIClientCreate(CFSTR("casio-2000s-midi-bridge"), NULL, NULL, &g.client)) { logerr("MIDIClientCreate failed"); return -1; }
    g.notify = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(g.notify), kCFRunLoopCommonModes);
    g.running = 1;
    if (vid >= 0 && pid >= 0) watch((uint16_t)vid, (uint16_t)pid);
    else for (size_t i = 0; i < N_KNOWN; i++) watch(known_ids[i].vid, known_ids[i].pid);
    if (!g.dev && !g.retry) logmsg("waiting for a keyboard to be plugged in...");
    return 0;
}

void bridge_stop(void) {
    if (!g.running) return;
    retry_cancel();
    usb_close();
    for (int i = 0; i < g.n_iters; i++) IOObjectRelease(g.iters[i]);
    g.n_iters = 0;
    if (g.notify) { CFRunLoopRemoveSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(g.notify), kCFRunLoopCommonModes);
                    IONotificationPortDestroy(g.notify); g.notify = NULL; }
    if (g.client) { MIDIClientDispose(g.client); g.client = 0; }
    g.running = 0;
}

void bridge_get_stats(bridge_stats *out) {
    pthread_mutex_lock(&g.lock);
    *out = g.stats;
    pthread_mutex_unlock(&g.lock);
}

void bridge_list_known(bridge_id_fn fn, void *ctx) {
    for (size_t i = 0; i < N_KNOWN; i++) fn(known_ids[i].vid, known_ids[i].pid, known_ids[i].desc, ctx);
}

const char *bridge_version(void) { return VERSION; }
