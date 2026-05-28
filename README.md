# Home Assistant Voice PE — OpenAI Realtime fork

> **Customized fork** of `maxmaxme/home-assistant-voice-pe` (itself a fork of
> `esphome/home-assistant-voice-pe`). The Voice PE runs as a **thin client**
> that streams audio over WebSocket to a backend add-on which talks to the
> **OpenAI Realtime API (`gpt-realtime-2`)** and controls Home Assistant via the
> unofficial **[ha-mcp](https://github.com/homeassistant-ai/ha-mcp)** server.
> There is no Home Assistant `voice_assistant` pipeline on the audio path.
>
> Active config: [`home-assistant-voice.va-direct.yaml`](home-assistant-voice.va-direct.yaml).
> Companion backend add-on: **[xandervanerven/ha-openai-realtime](https://github.com/xandervanerven/ha-openai-realtime)**.

## What this fork adds on top of upstream

- **Direct use in ESPHome Builder**: `external_components` and sound/model
  assets are pulled from GitHub, so you can paste the va-direct config into the
  ESPHome dashboard and build without a local checkout.
- **"stop" word interrupt** (kept from the Voice PE design): say *"stop"* while
  the assistant is talking (or press the center button) to cancel the reply.
- **Handsfree barge-in** (`barge_in: true` in the `va_client:` block): the mic
  stays open during the reply so you can simply talk over the assistant; the
  backend's server-VAD cuts the reply off. Best-effort on this hardware — the
  XMOS AEC leaks ~10× speaker→mic, so the stop word/button stay the reliable
  fallback. Set `barge_in: false` for the original turn-based behaviour.

## Setup (ESPHome Builder)

1. Install and configure the **OpenAI Realtime Voice Agent** add-on from
   [xandervanerven/ha-openai-realtime](https://github.com/xandervanerven/ha-openai-realtime)
   (sets your OpenAI key, the model, and the ha-mcp URL/token).
2. In the ESPHome dashboard, create a device from
   [`home-assistant-voice.va-direct.yaml`](home-assistant-voice.va-direct.yaml).
3. Provide these `secrets.yaml` keys: your Wi-Fi credentials (as upstream) and
   `va_device_token` (any non-empty string for now). Optionally override the
   `va_url` substitution if your add-on isn't reachable at
   `ws://homeassistant.local:8080/`.
4. Install/flash. The device connects to the add-on and you're ready.

---

Based on the ESPHome source of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).
See [the upstream documentation](https://voice-pe.home-assistant.io/) for hardware set up and troubleshooting,
and [`CLAUDE.md`](CLAUDE.md) for implementation notes.
