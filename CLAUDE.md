# CLAUDE.md

Guidance for Claude Code (and humans) working in this repo.

## Project

This is a **customized fork** of
[`esphome/home-assistant-voice-pe`](https://github.com/esphome/home-assistant-voice-pe).
It is **not drop-in compatible with the stock Home Assistant voice
pipeline.** The fork replaces the standard HA `voice_assistant` ESPHome
component with a custom `va_client` component that streams PCM16 audio
over a plain WebSocket to a backend running OpenAI's Realtime API
(the `voice-assistant` Node service in the sibling repo).

In the smart-home stack the device is therefore a **thin client**:

```
Voice PE (this firmware)
  │   wake word (micro_wake_word) runs locally on the ESP32
  │   XMOS DSP does AEC / NS / IC / AGC (configured via voice_kit over I2C)
  │
  └── WebSocket /voice (bearer VA_DEVICE_TOKEN)
        ──▶ voice-assistant container :3001
              ──▶ OpenAI Realtime API (gpt-realtime-2)
              ──▶ HA MCP server (tools only, no voice pipeline)
```

There is no HA `voice_assistant` integration on the audio path
anymore. STT/TTS happen inside the Realtime session in the backend;
the device just streams mic audio up and plays speaker audio down.

## Configs

| File | Purpose |
| --- | --- |
| `home-assistant-voice.va-direct.yaml` | **Active config — this is what gets flashed.** Uses the custom `va_client` component. |
| `home-assistant-voice.yaml` | Original upstream config. Kept for reference / upstream sync. Not built. |
| `home-assistant-voice.8mb.yaml`, `home-assistant-voice.factory.yaml` | Other upstream variants — unused here. |
| `voice-kit.yaml` | XMOS voice-kit component. Shared between configs. |
| `secrets.yaml` | **Gitignored.** Holds `va_url` (e.g. `ws://va.local:3001/voice`) and `va_device_token` (must match the backend's `VA_DEVICE_TOKEN`). |

## Custom component: `esphome/components/va_client/`

| File | Role |
| --- | --- |
| `__init__.py` | ESPHome codegen + YAML schema. Configurable: `url`, `token`, `microphone`, `mic_channel`, `speaker`, `on_phase` (automation), `on_repeated_failure` (automation). |
| `va_client.h`, `va_client.cpp` | The actual client. Built on `esp_websocket_client` (esp-idf). |
| `automation.h` | `OnPhaseTrigger : Trigger<std::string>` (fires on every phase transition with the new phase name) and `OnRepeatedFailureTrigger : Trigger<>` (fires when the failure counter trips). |

### What `va_client.cpp` does

- **Mic stream**: pulls int32 stereo frames from the microphone,
  drops to int16 mono via `>>16` on the selected `mic_channel`, and
  ships PCM16 over the WS as binary frames.
- **Speaker playback**: incoming binary frames are PCM16 audio. A
  **2 MB PSRAM ring buffer** smooths jitter and lets us defer "ready"
  LED state until the buffer actually drains.
- **Phase machine**: `idle → listening → thinking → replying → idle`,
  driven by `phase` JSON messages from the server (`src/realtime/protocol.ts`
  in voice-assistant). Each transition fires `on_phase`.
- **Reconnect**: exponential backoff (1s / 2s / 5s / 10s, capped).
- **No-speech watchdog**: 7 s after entering `listening` with no audio
  flowing → tear the session down and return to `idle`.
- **Repeated-failure handling**: 5 consecutive failed connect/handshake
  attempts trip `on_repeated_failure` (used to play the error chime).
  After a 30 s stable connection the counter re-arms.

### Wire protocol

JSON messages (text frames) interleaved with binary PCM16:

- **server → device**: `hello` (handshake ack), `phase` (state transition),
  `error`, `pong`.
- **device → server**: `start` (begin a turn), `interrupt` (barge-in),
  `ping`.

Defined in voice-assistant's `src/realtime/protocol.ts` — keep both
sides in lockstep when changing it.

## yaml-side glue (`home-assistant-voice.va-direct.yaml`)

- `va_client:` block wires the component to `secrets: va_url` /
  `va_device_token`, the i2s microphone (mic channel 0 — XMOS already
  outputs cleaned audio there), and the speaker output.
- `on_phase:` maps the phase string to the global
  `voice_assistant_phase` so the original `control_leds` script can
  drive the LED ring without modification. The original script came
  from upstream; we just keep feeding it.
- **LED idle deferral**: the yaml waits until the speaker ring buffer
  has actually drained before emitting the final `idle` LED state, so
  the LED stays in `replying` while audio is still playing out.
- `on_repeated_failure:` plays the error chime.

### Known TODOs

Several yaml blocks are marked `TODO va-direct` — features that were
tied to the upstream `voice_assistant` component and don't apply here:

- Timers (HA-managed timer UI / chimes).
- Group media player (multi-room).
- Music ducking (this firmware no longer plays music — voice-only).

If/when these come back, they'll need a custom path through `va_client`
or a sidecar HA integration.

## Secrets

`secrets.yaml` is gitignored. Required keys:

- `va_url` — backend WebSocket URL, e.g. `ws://va.local:3001/voice`.
- `va_device_token` — bearer token; must equal `VA_DEVICE_TOKEN` in
  the voice-assistant container's env on the Pi.
- All upstream secrets (WiFi creds, etc.) stay as in stock.

If the token mismatches, the device gets `401` on the WS upgrade and
the failure counter trips after a few retries.

## Backend

This fork pairs with the **ha-openai-realtime** Home Assistant add-on
(`xandervanerven/ha-openai-realtime`, a fork of `fjfricke/ha-openai-realtime`).
The add-on terminates the device WebSocket, runs the OpenAI Realtime session
(`gpt-realtime-2`), bridges Home Assistant control through the unofficial
`homeassistant-ai/ha-mcp` MCP server, and emits the `phase` messages this
firmware consumes. (The original `maxmaxme` `voice-assistant` Node backend is
not public; the add-on speaks the same wire protocol.)

## Handsfree barge-in (`barge_in:`)

`va_client` now has a `barge_in` option (default `true`, wired in the
va-direct yaml). When enabled, the mic keeps streaming through the
`thinking`/`replying` phases instead of being gated off, so the backend's
server VAD can hear the user talk over the assistant; the `listening`
transition that follows flushes the PSRAM playback queue to stop the old TTS.

### Critical caveat: XMOS AEC isn't perfect

Measured on M3.2 hardware: ~10× speaker → mic leak survives AEC, so handsfree
barge-in is **best-effort** — echo can trip the server VAD and cut a reply
short. The **"stop" word and the center button remain the reliable interrupt**.
Raise `vad_threshold` in the add-on (and rely on the XMOS AEC) to reduce false
barge-ins, or set `barge_in: false` for the original turn-based behaviour.

The post-reply follow-up window is still gated by `kFollowupMs` in
`va_client.h` (default `0` = mic closes after each reply; the user says a wake
word for the next turn). Bump it to re-open the mic automatically once AEC is
tuned.

## Building / flashing

Stock ESPHome workflow against `home-assistant-voice.va-direct.yaml`:

```bash
esphome run home-assistant-voice.va-direct.yaml
```

The custom component under `esphome/components/va_client/` is picked
up automatically because the yaml `external_components:` block points
at it.

## Relation to upstream

Stay close to `esphome/home-assistant-voice-pe` so we can pull in
upstream fixes for the XMOS / voice-kit / LED stack. The minimal
delta is:

- The `va_client` component.
- The va-direct yaml (the original yaml is untouched).
- `secrets.yaml` keys.

Avoid editing upstream files unless you're cherry-picking from
upstream or fixing something that genuinely needs a fork-side change.

## Keep this file up to date

When the wire protocol changes (in lockstep with voice-assistant's
`src/realtime/protocol.ts`), when phase semantics shift, when the
`kFollowupMs` workaround is removed, or when a TODO comes off the
list — update this file in the same change.
