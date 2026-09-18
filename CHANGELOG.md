# Changelog

## 0.1.0 — 2026-09-18

Initial release.

* User-space bridge exposing non-class-compliant Casio USB keyboards (`07CF:6802`,
  `07CF:6801`) as a virtual CoreMIDI source and destination.
* Hot-plug via IOKit matching notifications; the virtual port appears when the keyboard
  is connected and disappears when it is unplugged.
* Full USB-MIDI 1.0 packet codec with SysEx, running status and realtime handling, covered
  by unit tests.
* `make install` LaunchAgent for start-at-login.
* `midimon` and `usbdesc` diagnostic tools.
