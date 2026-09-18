/* cli.c — command-line front end for casio-midi-bridge. See README.md. MIT license. */
#include <CoreFoundation/CoreFoundation.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include "bridge.h"

static void on_signal(int s) { (void)s; CFRunLoopStop(CFRunLoopGetMain()); }
static void print_id(uint16_t vid, uint16_t pid, const char *desc, void *ctx) { (void)ctx; printf("%04X:%04X  %s\n", vid, pid, desc); }

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
        "  -h, --help        this help\n", bridge_version());
}

int main(int argc, char **argv) {
    static const struct option opts[] = {
        { "name", required_argument, NULL, 'n' }, { "vid", required_argument, NULL, 1 },
        { "pid", required_argument, NULL, 2 },    { "verbose", no_argument, NULL, 'v' },
        { "list", no_argument, NULL, 'l' },       { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },       { 0, 0, 0, 0 } };
    const char *name = "Casio USB MIDI"; int vid = -1, pid = -1, verbose = 0, c;
    while ((c = getopt_long(argc, argv, "n:vlVh", opts, NULL)) != -1) {
        switch (c) {
        case 'n': name = optarg; break;
        case 1: vid = (int)strtol(optarg, NULL, 0); break;
        case 2: pid = (int)strtol(optarg, NULL, 0); break;
        case 'v': verbose = 1; break;
        case 'l': bridge_list_known(print_id, NULL); return 0;
        case 'V': printf("casio-midi-bridge %s\n", bridge_version()); return 0;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if ((vid >= 0) != (pid >= 0)) { fprintf(stderr, "--vid and --pid must be given together\n"); return 2; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    if (bridge_start(name, vid, pid, verbose)) return 1;
    CFRunLoopRun();
    bridge_stop();
    fprintf(stderr, "[casio-midi-bridge] exiting\n");
    return 0;
}
