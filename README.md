# Voice PE Realtime — device firmware

ESPHome firmware that turns a **Home Assistant Voice PE** into the thin,
low-latency audio client half of
**[Voice PE Realtime](https://github.com/TristanBrotherton/voicepe-realtime)**.
Wake-word detection (default: **"Hey Leonard"**, switchable to Hey Jarvis /
Okay Nabu from Home Assistant) runs on-device; everything else — speech
understanding, replies, smart-home control — streams over a local WebSocket to
the backend add-on's OpenAI Realtime or GPT-Live session.

## 📖 All documentation lives in the main repo

**→ [github.com/TristanBrotherton/voicepe-realtime](https://github.com/TristanBrotherton/voicepe-realtime)**

- [Getting Started](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/docs/getting-started.md) — flashing this firmware + installing the backend, step by step
- [Configuration Reference](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/docs/configuration.md) — including this firmware's substitutions (wake word model, sensitivity cutoffs, device name, `va_url`)
- [Features](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/docs/features.md) — wake words & the retrain flywheel, speaker recognition, timers, and more
- [FAQ](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/docs/faq.md) — including how to revert to the stock firmware
- [GPT-Live runtime](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/openai_realtime_voice_agent/DOCS.md#12-gpt-live-runtime-experimental) — the backend's experimental, opt-in `gpt-live-1` runtime (0.19.0) and its [cost safety options](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/docs/configuration.md#-gpt-live-cost-safety)

## Which firmware for which backend

| Firmware (`main`) | Backend add-on | Realtime | GPT-Live (`voice_runtime: live`) |
|---|---|---|---|
| 1.3.1 | 0.19.0 and later | yes | yes — no firmware change needed; tested on 1.3.1, whose per-turn playback reports (added after 1.2.0) the backend's Live confirmation gate relies on |
| 1.3.1 | 0.17.x | yes | n/a (the backend has no Live runtime) |
| 1.2.0 and older | any | yes | not supported: update the firmware first (one click in ESPHome Builder) |

GPT-Live is a backend option; the device does the same job either way: wake
word on-device, 16 kHz mic audio up, 24 kHz reply audio down.

## Building from source

You don't build anything by hand for a normal install — the per-device stub
([`esphome-builder.dhcp.yaml`](esphome-builder.dhcp.yaml) or
[`esphome-builder.static-ip.yaml`](esphome-builder.static-ip.yaml)) pulls
[`home-assistant-voice.realtime.yaml`](home-assistant-voice.realtime.yaml) from
this repo as a remote package, and ESPHome Builder compiles and flashes it (one
click for updates). To hack on the firmware itself, point the stub's `packages:`
block at your fork/branch instead. Wake-word models live in
[`models/`](models/) with metrics-only evaluation manifests and the release
gate ([`models/README.md`](models/README.md)); the previous model is kept in
`models/previous/` for rollback.

CI ([`.github/workflows/realtime.yml`](.github/workflows/realtime.yml)) compiles
this realtime firmware from the checkout under review
(`tools/ci/make_ci_config.py`), runs the wake-word gate and consistency checks
(`tools/wakeword/`), lints the YAML, and scans for private data.

Release changes are recorded in [`CHANGELOG.md`](CHANGELOG.md).

The [`static/`](static/) web installer restores the **stock** firmware; it does
not install Voice PE Realtime.

---
*Based on / inspired by xandervanerven's and maxmaxme's Voice PE work and the
official esphome/home-assistant-voice-pe — with thanks.*
