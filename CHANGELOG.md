# Changelog

## 0.1.0 — 2026-09-18

Initial release.

* **Casio MIDI Bridge.app**: a one-window macOS app (macOS 13+) showing connection state,
  port name (renamable), live message counters, last message in each direction, a test-notes
  button, and an "Open at login" switch. Quitting disconnects the keyboard. Detects a running
  headless service and offers to stop it.
* The bridge core is a C library (`src/bridge.h`) shared by the app and the CLI; it no
  longer seizes a keyboard owned by another process but reports "in use" and retries.
* User-space bridge exposing non-class-compliant Casio USB keyboards (`07CF:6802`,
  `07CF:6801`) as a virtual CoreMIDI source and destination.
* Hot-plug via IOKit matching notifications; the virtual port appears when the keyboard
  is connected and disappears when it is unplugged.
* Full USB-MIDI 1.0 packet codec with SysEx, running status and realtime handling, covered
  by unit tests.
* `make install` LaunchAgent for start-at-login.
* `midimon` and `usbdesc` diagnostic tools.
