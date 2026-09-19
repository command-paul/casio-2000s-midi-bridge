# Contributing

Thanks for helping keep old keyboards alive.

## Adding a keyboard

Most 2001–2010 Casio USB keyboards share USB ID `07CF:6802` and already work. If yours
doesn't show up:

1. Plug it in, then run `./build/usbdesc` (build with `make`). It prints the vendor/product
   ID and the full configuration descriptor of every Casio device on the bus. Paste that
   output into an issue.
2. If the descriptor shows one interface with a bulk/interrupt IN and OUT endpoint and
   `CS_INTERFACE` descriptors (subtypes 0x01–0x03), the wire format is almost certainly
   standard USB-MIDI. Try `./build/casio-2000s-midi-bridge --vid 0x07CF --pid 0xXXXX -v`.
3. If that works, add the ID to `known_ids[]` in `src/main.c` with a short description
   and open a pull request. Please mention the model name in the PR.

## Development

```bash
make            # builds build/casio-2000s-midi-bridge, build/midimon, build/usbdesc
make app        # builds "build/Casio 2000s MIDI Bridge.app" (swiftc, no Xcode project)
make icon       # regenerates app/AppIcon.icns from app/icon/make-icon.swift
make screenshot # re-renders docs/app-screenshot.png from the running app (keyboard connected)
make test       # unit tests for the USB-MIDI packet codec (no hardware needed)
./build/casio-2000s-midi-bridge -v      # run in the foreground, print every packet
./build/midimon "Casio USB MIDI"  # print decoded MIDI arriving from the virtual port
```

Stop the LaunchAgent (`make uninstall` or `launchctl bootout gui/$(id -u)/com.github.casio-2000s-midi-bridge`)
before running the bridge by hand; only one process can own the USB interface.

## Code layout

* `src/usbmidi.h` — pure USB-MIDI 1.0 packet encoder/decoder. No OS dependencies; keep it
  that way so it stays unit-testable.
* `src/bridge.c` / `src/bridge.h` — the core: IOKit device discovery/hot-plug, endpoint I/O,
  CoreMIDI virtual ports, live stats. Plain C API used by both front ends.
* `src/cli.c` — command-line front end.
* `app/` — SwiftUI app front end, Info.plist template, icon.
* `launchd/` — LaunchAgent template for the headless install.
* `tools/` — diagnostics.
* `tests/` — codec tests, run by CI on macOS.

## Style

Plain C and Swift, `clang -Wall -Wextra` and `swiftc` warning-free, no external dependencies. Keep it small.
