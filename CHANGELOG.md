# Changelog

## Unreleased

- Text frames no longer drop the connection on a short network stall. Every
  text frame the device sends (`start`, `wake`, `interrupt`, `flush`,
  `turn_metrics`, button and flag messages, trigger audio, utterance acks)
  now goes through one FIFO and a sender task that writes with a 1.5 s
  timeout. Before, several were written with a 100-200 ms timeout, and the
  ESP-IDF WebSocket client treats a write that times out as a network error
  and aborts the connection (the mic path was already fixed this way). Other
  sends waited forever on the main loop or the WebSocket task. One FIFO also
  keeps their order.
- `start` now says what the device can do and why the last connection ended:
  `{"type":"start","caps":["ireason1","utt1"],"fw":...,"last_disconnect":
  {"why":"ws_error|ws_closed|wifi|tx_timeout|reboot",...},"prev_utt":{...}}`.
  Add-ons that don't know the fields ignore them.
- Every `interrupt` names its reason (`stop`, `wake`, `button`, `mute`,
  `no_speech`, `residual`) and carries a per-connection `seq`, so an add-on
  can treat a spoken "stop" differently from a barge-in wake or housekeeping.
  yaml: `send_interrupt("stop")` etc.; `send_interrupt()` without a reason
  still works.
- Utterance framing (`utt1`), used only when the add-on sends it:
  - `utt_start{id,kind,turn}` / PCM / `utt_end{id,bytes}` frame each piece of
    speech; the device tracks each utterance over its playback ring and acks
    it with `utt_ack` (`playing` with `wait_ms`, `progress` every second,
    `done`, `cut` or `dropped`, with the bytes fed to the speaker and the
    underruns).
  - After a device interrupt every utterance frame is dropped until the
    add-on's `interrupt_ack{seq}`; the interrupt carries the playing
    utterance and how much of it was fed.
  - Per-utterance start: an utterance that starts on a cold speaker chain
    gets the silence lead (hello `chain_prime_ms`, default 200 ms) and,
    optionally, a start cushion (hello `utt_cushion_ms`, default 0); one that
    follows another back to back gets neither, so a clip flows into its
    result without a gap.
  - The stop-echo guard is anchored at each utterance's first fed byte.
  - "stop" is armed while an `announce` utterance (a timer line) plays and
    for 10 s after it, and is sent as `interrupt{reason:"stop"}`, so the
    add-on knows the announcement was heard. yaml: `on_announce`.
  Without `utt_start` from the add-on, playback is unchanged (prebuffer,
  prime once per turn, incoming audio dropped after an interrupt until the
  next `listening`).
- Short replies are heard. A reply that starts on a cold speaker chain now
  begins with 200 ms of silence instead of 60 ms. With 60 ms a short reply
  ("Done.", "It's on.") was often not heard at all and longer replies lost
  their first word. Measured on a Voice PE over 8 short-reply turns each:
  every reply heard in full with 200 ms (and with 400 ms), against several
  lost or clipped with 60 ms. Cost: about 90 ms more before the first word.
- The device no longer stops itself on the echo of its own short replies
  ("Done.", "It's on.", the "One moment" clip). Two changes:
  - The "stop" cutoff follows the speaker, not the phase: the stricter
    ~0.50 is set when a reply or a timer ring starts and kept whenever reply,
    clip, chime or timer audio is playing and for `stop_echo_hold_ms` (0.8 s)
    after it drains; the sensitive 0.40 applies only once it has been quiet
    that long. The phase went idle while a reply's tail still played, and a
    clip played before "replying", so the 0.40 applied to the device's own
    audio.
  - A "stop" in the first `stop_min_play_ms` (2 s) of a stretch of reply
    audio is ignored (`va_client` `stop_is_reply_echo()`). A stretch starts
    when reply audio reaches the speaker chain after a pause of 0.8 s or more,
    after the prebuffer, so a reply right after the "One moment" clip gets its
    own guard and a live stream's short stalls do not restart it. Measured on
    a Voice PE: the reply's echo scored 0.51-0.69 (sliding average) in its
    first second, real spoken stops 0.57-0.67, so no cutoff separates them; a
    reply that short has nothing to stop, and a longer one still stops after
    2 s. A ringing timer still stops at once.

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
