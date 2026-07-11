# Voice PE Realtime — device firmware

ESPHome firmware that turns a **Home Assistant Voice PE** into a thin,
low-latency audio client for the
[Voice PE Realtime backend](https://github.com/TristanBrotherton/voicepe-realtime-backend).
Wake-word detection runs on-device; everything else — speech understanding,
replies, smart-home control — streams over a local WebSocket to the backend's
OpenAI Realtime session. There is no Assist pipeline on the audio path.

## The experience

- **Your own wake word.** microWakeWord runs on-device; this repo ships our
  custom model (`models/`) trained on real household voices via the backend's
  enrollment feature — and you can train yours the same way, or drop in any
  stock model (okay nabu / hey jarvis / alexa).
- **Conversation, not commands.** After each reply the mic reopens briefly so
  you can follow up naturally. "Stop" interrupts a reply instantly (with a red
  confirmation flash), tuned per-phase so the assistant's own voice can't
  false-trigger it.
- **Enrollment mode.** When the backend runs a voice-training session, the ring
  breathes cyan, the mic stays open continuously, wake/stop detection disarms,
  and the center button is a physical escape. A 10-minute hard cap and
  WS-drop auto-exit guarantee it can never record unattended.
- **Proper loudness.** OpenAI's audio is mastered quieter than stock TTS; this
  firmware compensates (+6 dB ceiling, single-attenuation volume path) so
  replies match the device's own chimes at every knob position.

## Features

- `va_client` component: raw 16 kHz mic streaming, 24 kHz reply playback,
  jitter buffering, mic pre-roll, reconnect logic
- Phase state machine exposed to HA as a text sensor
  (`idle / waiting / listening / thinking / replying / enrolling`) — trigger
  automations on it (e.g. pause the kitchen speaker the instant a wake fires)
- Per-phase stop-word cutoffs; wake-boundary echo guards tunable from the
  backend without reflashing
- Enrollment mode (backend-controlled), cyan LED effect, button escape
- Wake-word sensitivity select in HA (three calibrated levels)
- Stock niceties preserved: LED ring language, volume dial, mute switch, media
  player for Music Assistant

## Install

1. In the ESPHome dashboard, create a per-device stub from
   [`esphome-builder.dhcp.yaml`](esphome-builder.dhcp.yaml) — it imports
   [`home-assistant-voice.realtime.yaml`](home-assistant-voice.realtime.yaml)
   from this repo as a remote package, so updates are one click.
2. Set your Wi-Fi/API secrets and `va_url` (`ws://<ha-ip>:8080/`, one port per
   device) as substitutions.
3. Install the [backend add-on](https://github.com/TristanBrotherton/voicepe-realtime-backend),
   then flash. First flash of a factory device works over the ESPHome
   dashboard; subsequent updates are OTA.

See [INSTALL.md](INSTALL.md) for the full walk-through.

## Train it on your voice

Enroll through the device itself (say *"teach me my voice"* — the backend
coaches you through it), then retrain microWakeWord with your recordings as
weighted positives and your harvested false wakes as hard negatives. Our
model's trajectory doing exactly this: detection cutoff 0.43 → 0.71 at ~97%
recall in three passes. Training runs on any Apple Silicon or NVIDIA machine
in ~2 hours.

---
*Based on / inspired by xandervanerven's and maxmaxme's Voice PE work and the
official esphome/home-assistant-voice-pe — with thanks.*
