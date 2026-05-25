# Home Assistant Voice: Preview Edition

> **This is a customized fork.** It builds the Voice PE as a thin
> client that streams audio directly to a `voice-assistant` backend
> over WebSocket — there is no Home Assistant `voice_assistant`
> pipeline on the audio path. The active config is
> [`home-assistant-voice.va-direct.yaml`](home-assistant-voice.va-direct.yaml).
> See
> [`docs/superpowers/specs/2026-05-25-voice-pe-direct-va-streaming-design.md`](docs/superpowers/specs/2026-05-25-voice-pe-direct-va-streaming-design.md)
> for the design and [`CLAUDE.md`](CLAUDE.md) for current
> implementation notes.

This is the ESPHome source code of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).

See [the documentation](https://voice-pe.home-assistant.io/) for set up and troubleshooting.

If you need to re-install the firmware, [use this installer](https://esphome.github.io/home-assistant-voice-pe/).
