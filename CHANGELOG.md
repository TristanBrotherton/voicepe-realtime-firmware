# Changelog

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
