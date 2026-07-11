# Contributing

Same rules as the main repo — read
[voicepe-realtime/CONTRIBUTING.md](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/CONTRIBUTING.md).

Firmware-specific bar:

- **You flashed it.** State the device revision and that wake word, timers,
  voice enrollment, and the phase LED states still work after your change.
- The firmware stays a **thin audio client** — session logic, tools, and
  memory belong in the backend add-on. PRs that grow a second brain on the
  device will be redirected there.
- Substitutions are the public API of this package: renaming or removing one
  breaks every user's device stub. Add, don't rename.
