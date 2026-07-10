# Voice PE Realtime — device firmware

> [!NOTE]
> **This fork** of [TristanBrotherton/voicepe-realtime-firmware](https://github.com/TristanBrotherton/voicepe-realtime-firmware)
> carries a small set of local changes on top of upstream v1.2.0:
> 1. **Custom wake word "hey leonard"** — a self-trained microWakeWord v2 model
>    (`models/hey_leonard.json` + `.tflite` in this repo), trained on 50k synthetic
>    positives across ~900 US + ~109 UK voices plus 4k confusable negatives;
>    calibrated to 97.7% recall at <1 false accept/hour. Swap the `micro_wake_word`
>    model URL back to a stock model if you want okay-nabu/hey-jarvis/alexa.
> 2. **Wake-word model URLs fixed** — the kahrendt `v2.1_models` release URLs 404;
>    models moved to the OHF-Voice org.
> 3. **Louder TTS** — `volume_max` 0.85 → 0.92 (~+6 dB at the DAC) to compensate
>    for OpenAI Realtime audio peaking at −3..−6 dBFS vs ~0 dBFS for stock HA TTS.
> 4. **Double-attenuation fix** — `va_client` output is held at unity gain (0 when
>    muted) instead of mirroring the volume knob into the sample scaler; the DAC
>    hardware volume already scales that lane, so the old behavior made the
>    assistant quieter than the device's own chimes at every knob position below max.
> 5. `va_client` external component + wake model are pulled from **this repo**, so
>    builds don't depend on upstream availability.
>
> All changes are tagged `FORK EDIT` / `LOCAL EDIT` in
> [`home-assistant-voice.realtime.yaml`](home-assistant-voice.realtime.yaml).
> Pair with the backend fork: **[TristanBrotherton/voicepe-realtime-backend](https://github.com/TristanBrotherton/voicepe-realtime-backend)**.
> Upstream docs follow below.


> [!IMPORTANT]
> **This is 1 of 2 repos — you need both halves.** This repo is the **device
> firmware**; on its own it does nothing. It streams audio to a backend add-on that
> runs the OpenAI Realtime session and controls Home Assistant. You must set up both:
> - 🔌 **Device firmware** (this repo) — flashed onto the Voice PE
> - 🧠 **Backend add-on** → **[TristanBrotherton/voicepe-realtime-backend](https://github.com/TristanBrotherton/voicepe-realtime-backend)** (runs in Home Assistant)
>
> 📖 New here? The full **[INSTALL guide](INSTALL.md)** sets up both halves, step by step.

> **Customized fork** of `maxmaxme/home-assistant-voice-pe` (itself a fork of
> `esphome/home-assistant-voice-pe`). The Voice PE runs as a **thin client**: it
> streams microphone audio over a plain WebSocket to a backend add-on, which runs
> an **OpenAI Realtime API** session (`gpt-realtime-2`) for speech-to-speech and
> controls Home Assistant through the official
> **[Home Assistant MCP Server](https://www.home-assistant.io/integrations/mcp_server/)**
> integration. There is no Home Assistant `voice_assistant` pipeline on the audio
> path — STT, TTS and the LLM all live in the Realtime session inside the add-on.
>
> The firmware config is [`home-assistant-voice.realtime.yaml`](home-assistant-voice.realtime.yaml).
> You don't paste it directly — you adopt it via a tiny per-device stub
> ([`esphome-builder.dhcp.yaml`](esphome-builder.dhcp.yaml)) that pulls it as a remote package,
> so updates are **one click** in the ESPHome dashboard (see Setup).

## What this fork changes vs. upstream

- **Custom `va_client` component** replaces the stock `voice_assistant`
  component: a thin WebSocket client (mic up / speaker down + an idle → listening
  → thinking → replying phase/LED state machine). All the voice intelligence
  runs in the backend add-on, not on the device.
- **One-click updates**: instead of pasting the whole config, you adopt a tiny
  per-device stub that pulls the firmware from this repo as a remote ESPHome
  `packages:` include. When a new version ships, the ESPHome dashboard shows
  "Update available" — one click recompiles with the latest. No local checkout,
  no tokens (this repo is public).
- **"stop" word + button interrupt**: say *"stop"* while the assistant is
  talking, or press the center button, to cancel the reply. This is the reliable
  way to interrupt.
- **Conversational, with online answers**: because the brain is `gpt-realtime-2`
  in the backend, you get a natural back-and-forth — and with web search enabled it
  can look things up online (weather, news, facts), not just control your devices.

## Setup (ESPHome Builder)

1. Install and configure the **OpenAI Realtime 2 Voice Agent** add-on from
   [TristanBrotherton/voicepe-realtime-backend](https://github.com/TristanBrotherton/voicepe-realtime-backend)
   (sets your OpenAI API key, the model, and the Home Assistant MCP connection).
2. In **Builder → Secrets**, add the keys from
   [`secrets.yaml.example`](secrets.yaml.example): `wifi_ssid`, `wifi_password`,
   `ota_password`, `api_key` (plus `static_ip`/`gateway`/`subnet`/`dns1`/`dns2`
   only if you want a fixed IP).
3. Create a new device in the dashboard and replace its YAML with a ready-made
   stub — [`esphome-builder.dhcp.yaml`](esphome-builder.dhcp.yaml) for DHCP, or
   [`esphome-builder.static-ip.yaml`](esphome-builder.static-ip.yaml) for a fixed IP.
   Set `name`/`friendly_name` and keep the `packages:`/`dashboard_import:` lines.
   Optionally override the `va_url` substitution if your add-on isn't at
   `ws://homeassistant.local:8080/`.
4. **Install** once (USB, then wireless thereafter). The device adopts the
   firmware and connects to the add-on.

After that, when a new firmware version is released the dashboard shows
**"Update available"** for the device — click it to pull the latest config and
re-flash. No more copy-pasting.

## Known limitations

- **No voice timers or alarms yet** — every other Assist action (lights, switches,
  scenes, climate) and online questions work.
- **A brief reconnect about once an hour** (OpenAI's 60-minute session cap; the
  add-on refreshes proactively during a quiet moment, so it rarely interrupts).
- **Rarely, the assistant may stop itself** on a word in its own reply that sounds
  like "stop" — just ask again.
- **Using "stop":** interrupts the assistant *while it's speaking* (during a reply
  or the short listening window right after one); it has no effect before it has
  started answering.

---

Based on the ESPHome source of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).
See [the upstream documentation](https://voice-pe.home-assistant.io/) for hardware setup and troubleshooting.
