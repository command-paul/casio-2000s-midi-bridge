# casio-midi-bridge

Makes old Casio USB keyboards work as MIDI controllers on modern macOS.

Casio keyboards sold from roughly 2001 to 2010 (LK-90TV, LK-100, CTK-691, WK-3000,
PX-100, AP-series and many more) all share USB ID `07CF:6802` and are **not USB-MIDI
class compliant**. They plug in, they enumerate, and then nothing happens: no port in
Audio MIDI Setup, nothing in GarageBand or Logic. Casio's driver only ever supported
Mac OS X 10.2–10.4 on PowerPC.

`casio-midi-bridge` is a small user-space program that talks to the keyboard directly
over USB and exposes it as a normal CoreMIDI port. No kernel extension, no `sudo`, no
Apple entitlements.

Tested on macOS 26 (Apple silicon) with a Casio LK-90TV.

## Install

Requires Xcode Command Line Tools (`xcode-select --install`).

```bash
git clone https://github.com/<you>/casio-midi-bridge.git
cd casio-midi-bridge
make
make install
```

`make install` copies the binary to `~/.local/bin` and registers a LaunchAgent that starts
the bridge at login and restarts it if it ever dies. Plug in the keyboard and a port named
**"Casio USB MIDI"** appears. Give it a different name with:

```bash
make install PORT_NAME="Casio LK-90TV"
```

Then in GarageBand, add a Software Instrument track and play. In Logic the port is listed
under Settings → MIDI → Inputs. Audio MIDI Setup → Window → Show MIDI Studio shows it too.

Both directions work: notes and controllers go in, and anything you send to the port
(a MIDI file, a DAW track) plays on the keyboard's own sounds.

## Manage

```bash
make status      # is the agent running? last log lines
make log         # follow the log
make restart     # after rebuilding
make uninstall   # remove agent and binary
```

Run it by hand to watch packets go by (stop the agent first, only one process can own
the USB interface):

```bash
./build/casio-midi-bridge -v
```

## Tools

* `build/midimon [port name]` prints decoded MIDI as it arrives from the virtual port.
* `build/usbdesc [vid [pid]]` dumps USB descriptors of attached Casio devices, for adding
  new models (see [CONTRIBUTING.md](CONTRIBUTING.md)).

## How it works

The keyboard's only USB interface is declared as *vendor specific* (class `0xFF`), so
macOS never attaches its built-in USB-MIDI driver. Underneath, the interface carries
ordinary USB-MIDI Streaming descriptors, one bulk OUT endpoint and one interrupt IN
endpoint, and the data on the wire is standard USB-MIDI 1.0 four-byte event packets.
Linux has handled this ID for years with a one-line quirk (`QUIRK_MIDI_YAMAHA` in
`sound/usb/quirks-table.h`).

The bridge:

1. Registers IOKit matching notifications for the known USB IDs, so it reacts to
   plug/unplug without polling.
2. On attach, opens the device with `IOUSBLib`, selects configuration 1, claims interface
   0 and finds the IN/OUT endpoints.
3. Creates a virtual CoreMIDI source and destination with stable unique IDs, so DAWs
   remember the port across reconnects.
4. Streams IN packets to the source and encodes destination traffic into OUT packets.
   The codec (`src/usbmidi.h`) handles SysEx of any length, running status and realtime
   bytes, and is covered by unit tests (`make test`).

## Supported hardware

| USB ID | Devices |
|---|---|
| `07CF:6802` | Most 2001–2010 Casio USB keyboards and digital pianos |
| `07CF:6801` | Casio PL-40R |

Any other device with the same descriptor shape can be tried with
`--vid 0xVVVV --pid 0xPPPP`. Please open a PR to add IDs that work.

## Alternatives

* [francoisferland/casiousbmididriver](https://github.com/francoisferland/casiousbmididriver):
  a CoreMIDI plugin driver derived from Apple's old sample USB MIDI driver. It makes the
  keyboard appear as a real device rather than a virtual port, but needs full Xcode to build
  and a signed build to load on current macOS.
* Apple's MIDIDriverKit (macOS 14+) is the sanctioned way to ship a real driver. It needs a
  paid developer account and a USB transport entitlement granted by Apple.

## License

MIT. See [LICENSE](LICENSE).
