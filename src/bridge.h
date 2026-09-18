/*
 * bridge.h — public API of the Casio USB-MIDI bridge core, shared by the command-line
 * tool (src/cli.c) and the macOS app (app/). Plain C so Swift can import it directly.
 *
 * All functions must be called from the main thread; the bridge schedules its IOKit and
 * USB event sources on CFRunLoopGetMain() in the common run-loop modes, so it works inside
 * both a bare CFRunLoopRun() and an AppKit application.
 *
 * Part of casio-midi-bridge. MIT license.
 */
#ifndef BRIDGE_H
#define BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int connected;               /* 1 while a keyboard is open and the MIDI port exists */
    int busy;                    /* 1 if the keyboard is held by another process (retrying) */
    uint16_t vid, pid;           /* USB IDs of the connected (or busy) keyboard */
    char device_desc[96];        /* human readable model description */
    char port_name[64];          /* name of the virtual CoreMIDI port */
    double connected_since;      /* CFAbsoluteTime of connection, 0 if not connected */
    uint64_t msgs_in, msgs_out;  /* MIDI messages keyboard -> Mac and Mac -> keyboard */
    uint64_t notes_in;           /* note-on messages received from the keyboard */
    uint64_t bytes_in, bytes_out;/* USB payload bytes */
    uint64_t errors;             /* USB / setup errors since start */
    uint8_t last_in[3];  uint8_t last_in_len;  double last_in_time;
    uint8_t last_out[3]; uint8_t last_out_len; double last_out_time;
    char last_error[128];
} bridge_stats;

/* Start watching for keyboards. port_name is copied. vid/pid < 0 means every built-in ID;
 * otherwise only that one device. verbose logs every USB-MIDI packet to stderr. */
int  bridge_start(const char *port_name, int vid, int pid, int verbose);

/* Close the keyboard, remove the MIDI port and stop watching. Safe to call twice. */
void bridge_stop(void);

/* Snapshot of the current state. Thread-safe. */
void bridge_get_stats(bridge_stats *out);

/* Send raw MIDI bytes straight to the keyboard (bypassing CoreMIDI). Returns 0, or -1 if no
 * keyboard is connected. Thread-safe. */
int  bridge_send(const uint8_t *midi, size_t len);

/* Built-in USB IDs. */
typedef void (*bridge_id_fn)(uint16_t vid, uint16_t pid, const char *desc, void *ctx);
void bridge_list_known(bridge_id_fn fn, void *ctx);

const char *bridge_version(void);

#ifdef __cplusplus
}
#endif
#endif /* BRIDGE_H */
