/* midimon — print every MIDI message arriving from a CoreMIDI source.
 * Usage: midimon [source name]   (default "Casio USB MIDI"; lists sources if not found)
 * Part of casio-midi-bridge. MIT license. */
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <time.h>

static const char *names[] = {"note off","note on","poly pressure","control change","program change","channel pressure","pitch bend"};

static void on_midi(const MIDIPacketList *pl, void *rc, void *src) {
    (void)rc; (void)src;
    const MIDIPacket *p = &pl->packet[0];
    for (UInt32 k = 0; k < pl->numPackets; k++, p = MIDIPacketNext(p)) {
        time_t t = time(NULL); char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
        printf("%s |", ts);
        for (int i = 0; i < p->length; i++) printf(" %02X", p->data[i]);
        Byte s = p->data[0];
        if (s >= 0x80 && s < 0xF0 && p->length >= 2) {
            printf("  %s ch%d", names[(s >> 4) - 8], (s & 0x0F) + 1);
            if ((s & 0xF0) == 0x90 || (s & 0xF0) == 0x80) printf(" key=%d vel=%d", p->data[1], p->length > 2 ? p->data[2] : 0);
        } else if (s == 0xF0) printf("  sysex (%d bytes)", p->length);
        printf("\n"); fflush(stdout);
    }
}

int main(int argc, char **argv) {
    const char *want = argc > 1 ? argv[1] : "Casio USB MIDI";
    CFStringRef wantRef = CFStringCreateWithCString(NULL, want, kCFStringEncodingUTF8);
    MIDIClientRef client; MIDIPortRef port;
    MIDIClientCreate(CFSTR("midimon"), NULL, NULL, &client);
    MIDIInputPortCreate(client, CFSTR("in"), on_midi, NULL, &port);
    MIDIEndpointRef found = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) {
        CFStringRef n = NULL; MIDIObjectGetStringProperty(MIDIGetSource(i), kMIDIPropertyName, &n);
        if (n && CFStringCompare(n, wantRef, 0) == kCFCompareEqualTo) found = MIDIGetSource(i);
    }
    if (!found) {
        fprintf(stderr, "source \"%s\" not found. Available sources:\n", want);
        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) {
            CFStringRef n = NULL; char buf[256] = "?"; MIDIObjectGetStringProperty(MIDIGetSource(i), kMIDIPropertyName, &n);
            if (n) CFStringGetCString(n, buf, sizeof buf, kCFStringEncodingUTF8); fprintf(stderr, "  %s\n", buf);
        }
        return 1;
    }
    MIDIPortConnectSource(port, found, NULL);
    printf("listening on \"%s\" (ctrl-c to stop)\n", want); fflush(stdout);
    CFRunLoopRun();
    return 0;
}
