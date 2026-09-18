---
name: Keyboard not detected
about: The app or CLI does not see my Casio keyboard
labels: hardware
---

**Keyboard model:** (e.g. Casio CTK-810)

**macOS version and Mac:** (e.g. macOS 15.3, MacBook Air M2)

**What the app shows:** (Waiting for keyboard / in use by another program / error text)

**USB descriptor dump** — build with `make`, plug the keyboard in, run `./build/usbdesc`
and paste the output here:

```
(paste here)
```

If `usbdesc` prints "no USB device with vendor 0x07CF found", also paste the output of
`system_profiler SPUSBDataType`.
