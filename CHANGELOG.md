# Changelog

## Unreleased

- A "stop" heard after the reply has finished (in the follow-up window, often
  a misfire on "thanks") ends the turn quietly: the ring goes back to idle with
  no red flash. The red flash now means a reply that was playing was cut off.

- `va_client` handles the add-on's `output_batch_end` marker: it keeps the
  marker's `output_batch_id` and echoes it in the `turn_metrics` report of the
  reply that drained, so the add-on can confirm which output batch the device
  actually played. The id is cleared at every turn boundary and after each
  report, and only a `reason: "reply"` report carries it. A malformed marker is
  logged and ignored. Without this, add-ons that fence their output wait for a
  drain confirmation that never arrives.

## 1.3.1 — 2026-10-06

- Removed the application-level silence keepalive that could enqueue zero PCM
  ahead of later speech whenever the producer ring briefly emptied.
- Kept cold-chain priming startup-only so a long pause cannot insert a new
  60 ms prime after reply audio has begun.
- Added `dry_chain_events` turn telemetry. It counts transitions where the
  producer ring and resampler/mixer are empty while a reply is active; it does
  not alter the audio stream.
- Validated on Home Assistant Voice Preview Edition with ESPHome 2026.9.0.
  Three fixed short, medium, and long reply prompts played without audible
  static or gaps, including a long turn with hundreds of observed dry-chain
  transitions.

Silence padding remains owned by ESPHome's I2S/DMA sink, the boundary that
knows when output samples are actually unavailable.
