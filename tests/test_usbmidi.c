/* Unit tests for src/usbmidi.h. Build & run: make test */
#include <stdio.h>
#include <stdlib.h>
#include "usbmidi.h"

static uint8_t got[64][4]; static int ngot;
static void collect(void *ctx, const uint8_t p[4]) { (void)ctx; memcpy(got[ngot++], p, 4); }
static int fails;

static void check(const char *name, const uint8_t *in, size_t inlen, const uint8_t *exp, int npk, uint8_t cable) {
    usbmidi_encoder e; usbmidi_encoder_init(&e, cable); ngot = 0;
    usbmidi_encode(&e, in, inlen, collect, NULL);
    int ok = ngot == npk && memcmp(got, exp, (size_t)npk * 4) == 0;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) { fails++; printf("  expected %d packets:", npk); for (int i = 0; i < npk * 4; i++) printf(" %02X", exp[i]);
               printf("\n  got      %d packets:", ngot); for (int i = 0; i < ngot; i++) for (int j = 0; j < 4; j++) printf(" %02X", got[i][j]); printf("\n"); }
}
#define T(name, cable, in, exp) check(name, in, sizeof in, exp, (int)(sizeof exp / 4), cable)

int main(void) {
    { uint8_t in[] = {0x90,0x3C,0x64}; uint8_t exp[] = {0x09,0x90,0x3C,0x64}; T("note on", 0, in, exp); }
    { uint8_t in[] = {0x80,0x3C,0x00}; uint8_t exp[] = {0x08,0x80,0x3C,0x00}; T("note off", 0, in, exp); }
    { uint8_t in[] = {0x90,0x3C,0x64,0x40,0x64}; uint8_t exp[] = {0x09,0x90,0x3C,0x64, 0x09,0x90,0x40,0x64}; T("running status", 0, in, exp); }
    { uint8_t in[] = {0xC0,0x05}; uint8_t exp[] = {0x0C,0xC0,0x05,0x00}; T("program change", 0, in, exp); }
    { uint8_t in[] = {0xD3,0x40}; uint8_t exp[] = {0x0D,0xD3,0x40,0x00}; T("channel pressure", 0, in, exp); }
    { uint8_t in[] = {0xE0,0x00,0x40}; uint8_t exp[] = {0x0E,0xE0,0x00,0x40}; T("pitch bend", 0, in, exp); }
    { uint8_t in[] = {0xB0,0x07,0x7F}; uint8_t exp[] = {0x0B,0xB0,0x07,0x7F}; T("control change", 0, in, exp); }
    { uint8_t in[] = {0xF0,0xF7}; uint8_t exp[] = {0x06,0xF0,0xF7,0x00}; T("sysex 2 bytes", 0, in, exp); }
    { uint8_t in[] = {0xF0,0x41,0xF7}; uint8_t exp[] = {0x07,0xF0,0x41,0xF7}; T("sysex 3 bytes", 0, in, exp); }
    { uint8_t in[] = {0xF0,0x41,0x42,0xF7}; uint8_t exp[] = {0x04,0xF0,0x41,0x42, 0x05,0xF7,0x00,0x00}; T("sysex 4 bytes", 0, in, exp); }
    { uint8_t in[] = {0xF0,0x41,0x42,0x43,0xF7}; uint8_t exp[] = {0x04,0xF0,0x41,0x42, 0x06,0x43,0xF7,0x00}; T("sysex 5 bytes", 0, in, exp); }
    { uint8_t in[] = {0xF0,0x7E,0x7F,0x06,0x01,0xF7}; uint8_t exp[] = {0x04,0xF0,0x7E,0x7F, 0x07,0x06,0x01,0xF7}; T("sysex identity request", 0, in, exp); }
    { uint8_t in[] = {0xF0,0x01,0x02,0x03,0x04,0x05,0xF7}; uint8_t exp[] = {0x04,0xF0,0x01,0x02, 0x04,0x03,0x04,0x05, 0x05,0xF7,0x00,0x00}; T("sysex 7 bytes", 0, in, exp); }
    { uint8_t in[] = {0x90,0xF8,0x3C,0x64}; uint8_t exp[] = {0x0F,0xF8,0x00,0x00, 0x09,0x90,0x3C,0x64}; T("realtime inside message", 0, in, exp); }
    { uint8_t in[] = {0xF0,0x41,0xFE,0x42,0xF7}; uint8_t exp[] = {0x0F,0xFE,0x00,0x00, 0x04,0xF0,0x41,0x42, 0x05,0xF7,0x00,0x00}; T("realtime inside sysex", 0, in, exp); }
    { uint8_t in[] = {0xF1,0x05}; uint8_t exp[] = {0x02,0xF1,0x05,0x00}; T("MTC quarter frame", 0, in, exp); }
    { uint8_t in[] = {0xF2,0x01,0x02}; uint8_t exp[] = {0x03,0xF2,0x01,0x02}; T("song position", 0, in, exp); }
    { uint8_t in[] = {0xF6}; uint8_t exp[] = {0x05,0xF6,0x00,0x00}; T("tune request", 0, in, exp); }
    { uint8_t in[] = {0x90,0x3C,0x64}; uint8_t exp[] = {0x19,0x90,0x3C,0x64}; T("cable number 1", 1, in, exp); }
    { uint8_t in[] = {0x3C,0x64,0x90,0x3C,0x64}; uint8_t exp[] = {0x09,0x90,0x3C,0x64}; T("stray data bytes ignored", 0, in, exp); }
    { uint8_t in[] = {0xF0,0x41,0x90,0x3C,0x64}; uint8_t exp[] = {0x09,0x90,0x3C,0x64}; T("unterminated sysex dropped", 0, in, exp); }
    { uint8_t in[] = {0x90,0x3C,0x64,0xF6,0x40,0x64}; uint8_t exp[] = {0x09,0x90,0x3C,0x64, 0x05,0xF6,0x00,0x00}; T("system common cancels running status", 0, in, exp); }

    /* message split across two encode() calls */
    { usbmidi_encoder e; usbmidi_encoder_init(&e, 0); ngot = 0;
      uint8_t a[] = {0x90,0x3C}, b[] = {0x64}; usbmidi_encode(&e, a, 2, collect, NULL); usbmidi_encode(&e, b, 1, collect, NULL);
      uint8_t exp[] = {0x09,0x90,0x3C,0x64}; int ok = ngot == 1 && !memcmp(got[0], exp, 4);
      printf("%s split across calls\n", ok ? "PASS" : "FAIL"); if (!ok) fails++; }

    /* decoder */
    { uint8_t p9[] = {0x09,0x90,0x3C,0x64}, pc[] = {0x0C,0xC0,0x05,0x00}, pf[] = {0x0F,0xF8,0,0}, p0[] = {0,0,0,0}, p4[] = {0x14,0xF0,0x7E,0x7F};
      int ok = usbmidi_decode(p9) == 3 && usbmidi_decode(pc) == 2 && usbmidi_decode(pf) == 1 && usbmidi_decode(p0) == 0 && usbmidi_decode(p4) == 3;
      printf("%s decode lengths\n", ok ? "PASS" : "FAIL"); if (!ok) fails++; }

    printf("%s\n", fails ? "SOME TESTS FAILED" : "all tests passed");
    return fails ? 1 : 0;
}
