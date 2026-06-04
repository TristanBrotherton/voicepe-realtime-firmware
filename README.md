# Home Assistant Voice PE — OpenAI Realtime 2 fork

> [!IMPORTANT]
> **This is 1 of 2 repos — you need both halves.** This repo is the **device
> firmware**; on its own it does nothing. It streams audio to a backend add-on that
> runs the OpenAI Realtime session and controls Home Assistant. You must set up both:
> - 🔌 **Device firmware** (this repo) — flashed onto the Voice PE
> - 🧠 **Backend add-on** → **[xandervanerven/ha-openai-realtime](https://github.com/xandervanerven/ha-openai-realtime)** (runs in Home Assistant)
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
   [xandervanerven/ha-openai-realtime](https://github.com/xandervanerven/ha-openai-realtime)
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

---

Based on the ESPHome source of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).
See [the upstream documentation](https://voice-pe.home-assistant.io/) for hardware setup and troubleshooting.
