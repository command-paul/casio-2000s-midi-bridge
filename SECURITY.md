# Security

casio-midi-bridge runs entirely in user space, opens only USB devices with the vendor/product
IDs listed in `src/bridge.c`, makes no network connections, and stores nothing but the port
name in the app's own preferences. It needs no administrator rights.

If you find a security problem, please report it privately through GitHub's
"Report a vulnerability" button on the Security tab of this repository rather than in a
public issue. You should hear back within a week.
