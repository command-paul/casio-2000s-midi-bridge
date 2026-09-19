# Troubleshooting

Answers to the questions people actually search for. If yours isn't here,
[open an issue](https://github.com/command-paul/casio-2000s-midi-bridge/issues/new/choose).

## "Audio MIDI Setup does not show my Casio keyboard"

That's the problem this project exists for. Casio keyboards from roughly 2001–2010 (USB ID
`07CF:6802`: LK, CTK, WK, PX, AP, CDP models with a USB port and no "class compliant" claim)
identify themselves as a vendor-specific USB device, so macOS never creates a MIDI port
for them. System Information → USB shows the device; Audio MIDI Setup shows nothing.
Install [Casio 2000s MIDI Bridge](../README.md#the-app-recommended); the port appears while the
app is running.

Keyboards from about 2011 on (CT-X, CT-S, PX-S, newer Privia, and anything whose manual
says "class compliant") don't need this. They show up on their own.

## The app says "Waiting for keyboard"

The app only lists a keyboard once macOS sees it on USB. Check, in order:

1. The keyboard is switched on. Most Casios don't enumerate on USB until powered.
2. The cable carries data. Some cheap USB-B cables are charge-only. Try another.
3. Adapters and hubs. USB-C to USB-A adapters are fine; a few unpowered hubs are not.
   Plug straight into the Mac to rule that out.
4. System Information (Apple menu → About This Mac → System Report → USB). If there is
   no Casio/vendor `0x07cf` entry, the Mac isn't seeing the keyboard at all and no
   software can help; it's the cable, port or keyboard.
5. If it *is* listed but the app still waits, your model may use a different product ID.
   Build the tools (`make`) and run `./build/usbdesc`, then
   [open an issue](https://github.com/command-paul/casio-2000s-midi-bridge/issues/new?template=keyboard-not-detected.md)
   with the output. You can try it immediately with
   `./build/casio-2000s-midi-bridge --vid 0x07CF --pid 0x<yours> -v`.

## The app says "Keyboard is in use by another program"

Only one program can own the USB interface. Usually it's a second copy of the bridge:
the headless service from `make install`, or a copy of the app you forgot in the menu
bar. The app shows a "Stop background service" button for the first case; for the second,
look for a piano-keys icon in the menu bar and quit it from there. The app retries every
few seconds and connects as soon as the other program lets go.

## GarageBand says "2 MIDI inputs detected" (or 1, or 0)

GarageBand never shows MIDI input names; it just counts sources under Settings →
Audio/MIDI. Expect **1** with the bridge running, **2** while GarageBand has a project open
(it creates its own "GarageBand Virtual Out" port and counts it too). **0** means the
bridge isn't running or the keyboard isn't connected. GarageBand notices new ports live;
you don't need to restart it.

## The keyboard connects but nothing sounds in GarageBand

MIDI is note data, not audio. You need a *Software Instrument* track selected (Track →
New Track → Software Instrument). Audio tracks ignore MIDI. In Logic, also check
Settings → MIDI → Inputs has the port enabled.

## Every note plays twice, or sounds doubled

The keyboard is playing its own built-in sound at the same time as the software
instrument. Turn the keyboard's volume down, or if it has a "Local Control" setting in its
function menu, switch it off so the keys only send MIDI. (Many small Casios don't have
this setting; volume down is the answer.)

## Notes lag behind the keys

Latency comes from the audio side, not the bridge. In GarageBand, Settings → Audio/MIDI →
lower the buffer size, and prefer the Mac's built-in output or a wired interface over
Bluetooth headphones, which add 100–200 ms on their own.

## I want GarageBand to play through the keyboard's speakers

GarageBand can't send MIDI out. Logic, MainStage and most other DAWs can: pick the bridge
port as the track's MIDI output and the keyboard plays the notes with its own sounds.
The app's "Play test notes on keyboard" button demonstrates the path works.

## "Casio 2000s MIDI Bridge cannot be opened because the developer cannot be verified" / "is damaged"

The downloaded app isn't notarized (no paid Apple Developer ID). One-time fix: right-click
the app → Open → Open. If macOS says it's "damaged", remove the quarantine flag instead:

```bash
xattr -d com.apple.quarantine "/Applications/Casio 2000s MIDI Bridge.app"
```

Building from source (`make install-app`) never triggers this.

## It worked, then stopped after macOS updated

Nothing the bridge uses is private API, so updates shouldn't break it, but the app is
rebuilt on every release against the current SDK. Grab the
[latest release](https://github.com/command-paul/casio-2000s-midi-bridge/releases/latest), and
if it's still broken open an issue with the macOS version and the app's log (Console.app,
filter "casio-2000s-midi-bridge").

## Where are the logs?

* App: Console.app → filter "casio-2000s-midi-bridge". The app window also shows the last error.
* Headless service: `~/Library/Logs/casio-2000s-midi-bridge.log`, or `make log`.
* Everything, raw: quit the app and run `./build/casio-2000s-midi-bridge -v` in a terminal to see
  every USB-MIDI packet.

## Does this work on Intel Macs? Older macOS?

The code is plain IOKit and CoreMIDI and builds for both architectures; the release app is
a universal build from GitHub's runners. It needs macOS 13 or later for the app (SwiftUI
`MenuBarExtra`); the command-line tool should build on anything from macOS 11 up. Intel is
untested by the maintainer; reports welcome.
