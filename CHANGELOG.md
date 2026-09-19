# Changelog

## 0.3.1 — 2026-09-18

* Docs: Audio MIDI Setup's MIDI Studio never lists virtual ports, so it will not show the
  bridge's port; corrected the README and troubleshooting guide. Removed the app's
  "Open Audio MIDI Setup" button for the same reason.

## 0.3.0 — 2026-09-18

* Renamed to **casio-2000s-midi-bridge** / **Casio 2000s MIDI Bridge.app** to make clear it
  targets the 2000s-era keyboards. The GitHub URL redirects. The app's bundle identifier
  changed, so "Open at login" and a custom port name need to be set again after upgrading;
  delete the old "Casio 2000s MIDI Bridge.app" from Applications.
* "Not affiliated with Casio" disclaimer in the README, app footer and release notes.

## 0.2.0 — 2026-09-18

* App: menu bar icon and menu; closing the window keeps the bridge running, Quit disconnects.
* App: `--snapshot <png>` renders the window for documentation (`make screenshot`).
* Release workflow pinned by commit, CI runs with read-only token, Dependabot for actions.
* Issue/PR templates, SECURITY.md, CODE_OF_CONDUCT.md, .editorconfig.
* Fixed a leaked IOKit interest notification when a busy keyboard was later acquired.
* docs/TROUBLESHOOTING.md.

## 0.1.0 — 2026-09-18

Initial release.

* **Casio 2000s MIDI Bridge.app**: a one-window macOS app (macOS 13+) showing connection state,
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
