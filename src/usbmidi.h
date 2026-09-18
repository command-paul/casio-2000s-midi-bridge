/*
 * usbmidi.h — USB-MIDI 1.0 event packet codec (header-only, no OS dependencies).
 *
 * A USB-MIDI event packet is 4 bytes:
 *   byte 0 : cable number (high nibble) | Code Index Number (low nibble)
 *   byte 1-3 : up to three MIDI bytes, zero padded
 * The CIN says how many of the three MIDI bytes are meaningful (see usbmidi_cin_len).
 *
 * Part of casio-midi-bridge. MIT license.
 */
#ifndef USBMIDI_H
#define USBMIDI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Number of meaningful MIDI bytes for each Code Index Number. */
static const uint8_t usbmidi_cin_len[16] = {
    0, /* 0x0 miscellaneous / reserved      */
    0, /* 0x1 cable events / reserved       */
    2, /* 0x2 two-byte system common        */
    3, /* 0x3 three-byte system common      */
    3, /* 0x4 SysEx start or continue       */
    1, /* 0x5 one-byte system common / SysEx end with 1 byte */
    2, /* 0x6 SysEx end with 2 bytes        */
    3, /* 0x7 SysEx end with 3 bytes        */
    3, /* 0x8 note off                      */
    3, /* 0x9 note on                       */
    3, /* 0xA poly key pressure             */
    3, /* 0xB control change                */
    2, /* 0xC program change                */
    2, /* 0xD channel pressure              */
    3, /* 0xE pitch bend                    */
    1, /* 0xF single byte (realtime)        */
};

/* Decode one packet: returns how many MIDI bytes pkt[1..3] carry (0 = nothing). */
static inline size_t usbmidi_decode(const uint8_t pkt[4]) {
    return usbmidi_cin_len[pkt[0] & 0x0F];
}

/* Encoder: turns a MIDI byte stream into USB-MIDI packets. Handles messages split across
 * calls, running status, SysEx of any length, and realtime bytes interleaved anywhere. */
typedef void (*usbmidi_emit_fn)(void *ctx, const uint8_t pkt[4]);

typedef struct {
    uint8_t cable;   /* cable number 0..15, placed in the high nibble of byte 0 */
    uint8_t status;  /* running status for channel messages, 0 = none */
    uint8_t need;    /* data bytes required by the message being collected */
    uint8_t n;       /* bytes collected in buf (including the status byte) */
    uint8_t sysex;   /* nonzero while inside a SysEx message */
    uint8_t buf[3];
} usbmidi_encoder;

static inline void usbmidi_encoder_init(usbmidi_encoder *e, uint8_t cable) {
    memset(e, 0, sizeof *e);
    e->cable = cable & 0x0F;
}

static inline void usbmidi__emit(const usbmidi_encoder *e, uint8_t cin, const uint8_t *m,
                                 size_t len, usbmidi_emit_fn emit, void *ctx) {
    uint8_t p[4] = { (uint8_t)((e->cable << 4) | cin), 0, 0, 0 };
    for (size_t i = 0; i < len && i < 3; i++) p[1 + i] = m[i];
    emit(ctx, p);
}

static inline uint8_t usbmidi__chan_need(uint8_t status) {
    uint8_t hi = status & 0xF0;
    return (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
}

static inline void usbmidi_encode(usbmidi_encoder *e, const uint8_t *data, size_t len,
                                  usbmidi_emit_fn emit, void *ctx) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        /* Realtime bytes may appear anywhere, even inside another message. */
        if (b >= 0xF8) { usbmidi__emit(e, 0xF, &b, 1, emit, ctx); continue; }

        if (e->sysex) {
            if (b == 0xF7) {
                e->buf[e->n++] = b;
                usbmidi__emit(e, e->n == 1 ? 0x5 : e->n == 2 ? 0x6 : 0x7, e->buf, e->n, emit, ctx);
                e->n = 0; e->sysex = 0;
                continue;
            }
            if (!(b & 0x80)) {
                e->buf[e->n++] = b;
                if (e->n == 3) { usbmidi__emit(e, 0x4, e->buf, 3, emit, ctx); e->n = 0; }
                continue;
            }
            /* A status byte inside SysEx: the SysEx was unterminated. Drop what we have
             * and process the status byte normally. */
            e->sysex = 0; e->n = 0;
        }

        if (b == 0xF0) {
            e->sysex = 1; e->status = 0; e->n = 0;
            e->buf[e->n++] = b;
            continue;
        }

        if (b & 0x80) {
            if (b < 0xF0) {                       /* channel message */
                e->status = b; e->need = usbmidi__chan_need(b);
                e->buf[0] = b; e->n = 1;
            } else {                              /* system common: cancels running status */
                e->status = 0;
                uint8_t need = (b == 0xF1 || b == 0xF3) ? 1 : (b == 0xF2) ? 2 : 0;
                if (need == 0) { usbmidi__emit(e, 0x5, &b, 1, emit, ctx); e->n = 0; }
                else { e->buf[0] = b; e->n = 1; e->need = need; }
            }
            continue;
        }

        /* Data byte. */
        if (e->n == 0) {
            if (!e->status) continue;             /* stray data byte with no status: ignore */
            e->buf[0] = e->status; e->n = 1;      /* running status */
            e->need = usbmidi__chan_need(e->status);
        }
        e->buf[e->n++] = b;
        if (e->n == 1 + e->need) {
            uint8_t st = e->buf[0];
            uint8_t cin = st < 0xF0 ? (uint8_t)(st >> 4) : (e->need == 1 ? 0x2 : 0x3);
            usbmidi__emit(e, cin, e->buf, e->n, emit, ctx);
            e->n = 0;
        }
    }
}

#endif /* USBMIDI_H */
