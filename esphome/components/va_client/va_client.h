#pragma once

#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"

#include <cstdint>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace esphome {
namespace va_client {

class OnPhaseTrigger;
class OnRepeatedFailureTrigger;
class OnFollowupOpenedTrigger;

class VaClient : public Component {
 public:
  void set_url(const std::string &url) { url_ = url; }
  void set_microphone(microphone::Microphone *m) { mic_ = m; }
  void set_mic_channel(uint8_t c) { mic_channel_ = c; }
  void set_speaker(speaker::Speaker *s) { speaker_ = s; }
  // Enables handsfree barge-in: when true the mic keeps streaming through the
  // `thinking`/`replying` phases instead of being gated off, so the backend's
  // server VAD can hear the user talk over the assistant. On the `listening`
  // transition that follows (server confirmed a barge-in) we flush the PSRAM
  // playback queue so the old TTS stops immediately. When false the firmware
  // keeps the original turn-based behaviour (mic off while the assistant
  // speaks). Relies on the XMOS AEC to suppress speaker→mic echo; see the
  // ~10x leak caveat in CLAUDE.md.
  void set_barge_in(bool v) { barge_in_ = v; }
  // Sets the output-volume multiplier applied to TTS in handle_binary_.
  // Driven from yaml by external_media_player's volume / mute state so the
  // device's physical +/- buttons and mute switch scale our TTS the same
  // way they scale chime announcements played through media_player. (No
  // HA api: block on this firmware — there's no remote slider.) Range
  // [0, 1]; values are clamped on read so callers don't have to bounds-check.
  void set_volume(float v) { volume_ = v; }
  void add_on_phase_trigger(OnPhaseTrigger *t) { phase_triggers_.push_back(t); }
  void add_on_repeated_failure_trigger(OnRepeatedFailureTrigger *t) {
    repeated_failure_triggers_.push_back(t);
  }
  void add_on_followup_opened_trigger(OnFollowupOpenedTrigger *t) {
    followup_opened_triggers_.push_back(t);
  }

  bool is_connected() const { return ws_connected_; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // YAML-callable actions.
  void start_session();
  void send_interrupt();
  // Called from yaml's on_followup_opened automation AFTER the chime has
  // finished announcing through the speaker (wait_until !is_announcing +
  // i2s tail). Opens the mic for kRequestFollowUpMs. No-op if the device
  // is no longer armed (e.g. user already pressed wake before the chime
  // finished — the new session takes priority).
  void commit_followup_mic();

  // Called from the static esp-idf event handler trampoline.
  void on_ws_event(int32_t event_id, void *event_data);

 protected:
  void connect_();
  void schedule_reconnect_();
  void on_mic_data_(const std::vector<uint8_t> &samples);
  // Mic pre-roll helper (mic-task only, no lock). push appends to the rolling
  // ring while the session is closed; the ring is DISCARDED (not replayed) on
  // session open — see preroll_discard_pending_.
  void preroll_push_(const int16_t *data, size_t n);
  void handle_text_(const char *data, size_t len);
  void handle_binary_(const uint8_t *data, size_t len);
  void set_phase_(const std::string &phase);
  // Marshal a phase-LED trigger fire onto the main loop (used to drive the LED
  // ring to `listening`/`idle` from timer callbacks during the follow-up window,
  // independently of a server-sent phase).
  void fire_phase_led_(const std::string &phase);
  void open_followup_window_(uint32_t duration_ms);

  std::string url_;
  uint8_t mic_channel_{0};

  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *speaker_{nullptr};

  // esp_websocket_client_handle_t kept opaque to avoid leaking esp-idf into the header.
  void *ws_handle_{nullptr};
  bool ws_connected_{false};

  uint32_t reconnect_delay_ms_{1000};
  // Set when a reconnect timer is in flight. esp_websocket_client emits both
  // DISCONNECTED and CLOSED (and sometimes ERROR) per failure; without this
  // guard we'd double-bump the backoff delay and double-log.
  bool reconnect_pending_{false};

  std::string current_phase_{"idle"};
  std::vector<OnPhaseTrigger *> phase_triggers_;
  std::vector<OnRepeatedFailureTrigger *> repeated_failure_triggers_;
  std::vector<OnFollowupOpenedTrigger *> followup_opened_triggers_;

  // Counts consecutive failed reconnect attempts. Reset to 0 on a clean
  // WS_CONNECTED event. When it hits kRepeatedFailureThreshold we fire the
  // on_repeated_failure trigger exactly once (until the count resets) — yaml
  // plays an audible error chime so the user knows the link is dead.
  uint32_t consecutive_failures_{0};
  bool repeated_failure_fired_{false};
  static constexpr uint32_t kRepeatedFailureThreshold = 5;
  // Don't reset the failure counter/flag the moment WS reconnects — a
  // flapping link (connect → 2 s later disconnect → 5 more fails → another
  // chime) would spam the user. Require kStableConnectionMs of unbroken
  // uptime before declaring "we're properly back" and re-arming the chime.
  static constexpr uint32_t kStableConnectionMs = 30000;

  // Scratch buffers reused on the hot paths to avoid per-callback heap
  // allocation. Each is OWNED BY ONE TASK — never share them across tasks:
  //   mono_buf_  — mic task only (on_mic_data_): i2s stereo-int32 → mono-int16.
  //   tts_buf_   — WS task only (handle_binary_): TTS volume-scaling scratch.
  // They used to be one shared vector; the mic task kept refilling it while
  // the WS task scaled TTS through it → cross-task realloc/write races that
  // dropped garbage samples into the playback ring (audible as hiss).
  std::vector<int16_t> mono_buf_;
  std::vector<int16_t> tts_buf_;

  // Mic pre-roll ring (int16 mono @ kMicSampleRate), allocated in PSRAM. The
  // rolling ring continuously retains the most recent kPreRollMs of mic audio
  // while the session is closed. We DO NOT replay it on session open: during
  // the wake-chime + tail-delay window the ring inevitably captures the chime
  // leaking through the mic (XMOS AEC leaves ~10x), and replaying it fed the
  // chime back to OpenAI as a phantom utterance ("Au!"). Instead, on session
  // open we DISCARD the ring (preroll_discard_pending_), matching marcinnowak79
  // gemini_proxy's ring_buffer_->reset() on start. Trade-off: a word spoken
  // *during* the chime is lost; the user speaks once the listening ring lights.
  // Touched ONLY by the mic task (on_mic_data_) — no lock needed.
  // preroll_discard_pending_ is set by start_session()/commit_followup_mic()
  // (main loop) and consumed by the mic task: a plain bool like streaming_.
  static constexpr uint32_t kMicSampleRate = 16000;  // i2s_mics rate (16 samples/ms)
  static constexpr uint32_t kPreRollMs = 600;
  int16_t *preroll_buf_{nullptr};
  size_t preroll_capacity_samples_{0};
  size_t preroll_head_{0};   // next write index
  size_t preroll_count_{0};  // valid samples (<= capacity)
  bool preroll_discard_pending_{false};

  // Streaming gate. True while the mic should be forwarded to the server:
  //   - between wake-word start_session() and "listening"/"thinking"
  //   - and again after "idle" for kFollowupMs (in case AI asked a question)
  bool streaming_{false};
  // Handsfree barge-in toggle, set from yaml (`barge_in:`). See set_barge_in().
  bool barge_in_{true};
  // Set on phase=idle when there's still TTS audio queued — we can't open
  // the mic until the speaker drains, otherwise it picks up its own output.
  // loop() flips this to a live followup window once audio_fill_ hits 0.
  bool followup_pending_{false};
  // Tracks whether the pending follow-up was requested by the server's
  // request_follow_up tool (model asked a question) vs the natural
  // post-reply path. The former wants a longer mic window
  // (kRequestFollowUpMs); the latter uses kFollowupMs (which is 0 by
  // default — no auto-follow-up).
  bool request_follow_up_pending_{false};
  // Set when on_followup_opened has fired and we're waiting on yaml to
  // play the chime + call commit_followup_mic(). Cleared on commit or
  // when a new session preempts. Without this flag a stale
  // commit_followup_mic() call (e.g. delayed lambda after a `Stop` wake
  // word already reset state) would re-open the mic out of nowhere.
  bool followup_armed_{false};
  // Server sends phase=idle when OpenAI is done generating, but we still
  // have audio queued in PSRAM + downstream rings. If we fire the LED
  // trigger immediately the device looks idle while still speaking. Hold
  // the "idle" emission until the queue drains + kFollowupOpenDelayMs.
  bool idle_emit_pending_{false};
  // Set by send_interrupt() so the phase=idle that follows from the server
  // doesn't trigger a follow-up mic window. The user explicitly asked us to
  // stop — they don't want the device sitting there listening.
  bool suppress_followup_{false};
  // Set by send_interrupt() ("stop" word / barge-in). OpenAI bursts the whole
  // reply faster than real-time, so by the time the user says "stop" the audio
  // is already buffered (backend + our PSRAM) and the backend keeps streaming
  // the rest. Flushing our queue isn't enough — handle_binary_ just refills it
  // from the in-flight frames. While this is true we DROP incoming audio so the
  // cancelled reply actually goes silent. Cleared in set_phase_ on the next
  // "idle" (reply ended) or "listening" (a fresh turn's audio is legitimate).
  bool suppress_incoming_audio_{false};
  // Live duration (ms) of the post-reply follow-up window: how long the mic
  // stays open after the assistant finishes so the user can answer back
  // WITHOUT re-saying the wake word. Pushed from the backend add-on in the
  // `hello` handshake (`"follow_up_ms":N`) so it's tunable from the add-on
  // config without reflashing; 0 = disabled (turn-based, mic closes after each
  // reply). Clamped to kFollowupMsMax on parse. The window only opens AFTER the
  // speaker chain drains + kFollowupOpenDelayMs so the assistant's own TTS tail
  // can't leak into the open mic (XMOS AEC ~10x leak).
  uint32_t followup_ms_{0};
  static constexpr uint32_t kFollowupMsMax = 60000;
  // Live delay (ms) between the speaker draining and the follow-up mic opening,
  // also pushed from the backend `hello` ("follow_up_open_delay_ms":N). Covers
  // the i2s ring + DAC tail that plays out after has_buffered_data() goes false,
  // so the mic doesn't catch the reply's own tail. Defaults to the conservative
  // kFollowupOpenDelayMs but is tunable from the add-on (lower = snappier, risk
  // of hearing the tail; higher = safer). Clamped to kFollowupOpenDelayMaxMs.
  uint32_t followup_open_delay_ms_{kFollowupOpenDelayMs};
  static constexpr uint32_t kFollowupOpenDelayMaxMs = 5000;
  // Playback jitter buffer ("prebuffer"). Before starting/resuming playback we
  // hold audio in the PSRAM ring until at least this many ms have accumulated
  // (or a short deadline elapses), so the downstream resampler/mixer/i2s chain
  // starts with a cushion and a network jitter gap (we see 100-340 ms gaps)
  // doesn't dry it out → audible crackle. Pushed from the backend `hello`
  // ("playback_prebuffer_ms":N) so it's tunable without reflashing; clamped to
  // kPlaybackPrebufferMaxMs. 0 = disabled (play immediately, old behaviour).
  // Re-armed whenever the ring drains to empty (reply start AND post-underflow).
  uint32_t playback_prebuffer_ms_{0};
  static constexpr uint32_t kPlaybackPrebufferMaxMs = 2000;
  static constexpr uint32_t kPlaybackSampleRate = 24000;  // incoming TTS PCM rate
  // True while we're accumulating the prebuffer cushion (holding playback).
  // Touched by handle_binary_ (WS task, arms it) + loop() (main task, releases);
  // plain flag like streaming_, the tiny race is harmless.
  bool playback_priming_{false};
  // millis() when priming started (first byte after the ring was empty); used
  // for the prime deadline so real-time (non-burst) audio still starts promptly.
  uint32_t prime_started_ms_{0};

  // Resampler cold-start SILENCE-PRIME (crackle fix). The resampler does NOT
  // idle-timeout (verified vs ESPHome source): resample(stop_gracefully=false)
  // never returns FINISHED and its output mixer-source is timeout:never, so the
  // chain stays WARM between normal replies. It goes COLD only after an explicit
  // `speaker.stop: media_resampling_speaker` (yaml interrupt / "stop" / wake /
  // follow-up), which tears the task down (is_stopped()==true). The next reply
  // then cold-starts a fresh AudioResampler whose windowed-sinc FIR begins from a
  // zero state → a startup-transient click. A PSRAM prebuffer can't fix it (the
  // transient is downstream of the ring). Fix: when cold, feed kChainPrimeMs of
  // SILENCE first so the FIR settles to a clean zero output before real audio.
  // Cold = resampler is_stopped() (precise, true exactly post-speaker.stop) OR,
  // as a backup, nothing fed for > kChainColdMs. Both are only ever true at a
  // real cold reply-start, never mid-speech; a needless prime on a warm chain is
  // harmless (60ms silence).
  static constexpr uint32_t kChainPrimeMs = 60;   // silence burst to warm the filter
  static constexpr uint32_t kChainColdMs = 600;   // backup timer; is_stopped() is the primary signal
  // Bytes of silence still to feed this cold-start (24kHz mono 16-bit). >0 while
  // priming; loop() feeds silence and holds real-audio drain until it reaches 0.
  size_t chain_prime_remaining_{0};
  // millis() of the last time we fed the resampler ANYTHING (silence or real).
  // Used to detect a cold chain: now - last_fed_ms_ > kChainColdMs. 0 = never fed.
  uint32_t last_fed_ms_{0};
  // Legacy compile-time default, kept for reference. The live value now comes
  // from the backend (followup_ms_); this stays 0 so a device talking to an
  // old backend that doesn't send follow_up_ms keeps the turn-based behaviour.
  static constexpr uint32_t kFollowupMs = 0;
  // Used when the server explicitly requests a follow-up via the
  // request_follow_up tool — overrides kFollowupMs for a single turn.
  // Longer than the default because the model asked a real question
  // and the user might pause before answering.
  static constexpr uint32_t kRequestFollowUpMs = 10000;
  // After start_session() we wait this long for the server to emit
  // phase=listening (i.e. server VAD heard speech). If nothing comes, the
  // user pressed wake/button and stayed silent — close the session so we
  // don't sit there with the mic open eating OpenAI minutes.
  static constexpr uint32_t kNoSpeechTimeoutMs = 7000;
  // After our PSRAM queue drains there's still audio in flight:
  //   resampler ring 4800 B (≈ 50 ms)
  //   mixer source buffer 100 ms
  //   i2s_audio_speaker buffer_duration 500 ms
  //   XMOS DSP pipeline / DAC analog tail ≈ 100 ms
  // Sum ≈ 750 ms. We add headroom so a slow drain doesn't leak TTS into
  // the mic — every leak triggers the server VAD because the XMOS AEC
  // doesn't fully cancel our own speaker output (M3.2 measured ~10× leak).
  static constexpr uint32_t kFollowupOpenDelayMs = 1500;
  // Hard ceiling on how long we'll wait for the speaker chain to drain
  // (resampler ring + mixer source ring, via has_buffered_data()) after
  // PSRAM hits 0 before giving up and proceeding anyway. Should be >
  // the worst-case downstream buffer (resampler + mixer source ~150 ms,
  // plus play-out time of whatever was in flight) by a comfortable
  // margin, but short enough that a wedged speaker doesn't lock the
  // LED in `replying` forever.
  static constexpr uint32_t kSpeakerStopTimeoutMs = 3000;

  // True while we're waiting for the downstream speaker chain to actually
  // finish playing the TTS we wrote into it. Entered when audio_fill_
  // hits 0 with followup_pending_ set; exited when
  // !speaker_->has_buffered_data() OR kSpeakerStopTimeoutMs elapses.
  bool waiting_for_speaker_stop_{false};
  // millis() snapshot from when waiting_for_speaker_stop_ went true.
  // Used to fire the fallback timeout if the chain never drains.
  uint32_t speaker_stop_wait_started_ms_{0};

  // Tracks the opcode of the in-flight WS message so we can route
  // continuation frames (op_code = 0) to the same handler.
  bool last_data_was_binary_{false};

  // Output volume multiplier in [0, 1], updated from yaml whenever
  // external_media_player.volume / mute changes. Defaults to 1.0 so a
  // stand-alone va_client (no media_player wiring) still plays audibly.
  float volume_{1.0f};

  // Ring buffer for pending TTS audio, allocated in PSRAM. The server can
  // burst the entire response in ~200 ms; we buffer here and drain into
  // speaker.play() from loop() to keep playback smooth.
  //
  // 2 MB / (24000 Hz × 2 B) ≈ 43 s of headroom. A 30 s monologue arriving
  // in ~1 s would peak at ~1.4 MB; this size gives ~40 % overhead on top.
  // PSRAM is 8 MB on the Voice PE so cost is negligible.
  uint8_t *audio_buf_{nullptr};
  static constexpr size_t kAudioBufBytes = 2 * 1024 * 1024;
  size_t audio_head_{0};  // read pos (next byte to play)
  size_t audio_tail_{0};  // write pos (next byte to fill)
  size_t audio_fill_{0};  // bytes currently queued (audio_tail_ ≥ audio_head_ when not wrapped)
  // ESP32-S3 is dual-core: handle_binary_ runs in the esp-idf
  // websocket task (background) while loop() runs in the main app task,
  // typically on the other core. Both touch audio_head_/tail_/fill_
  // and the PSRAM data they index; without sync we hit races where
  // fill_ goes inconsistent, the reader sees the new tail before the
  // memcpy is visible, or two non-atomic increments lose each other.
  // The DAC then plays whatever bytes happened to be at those PSRAM
  // offsets — audible as "speech drops into hiss" mid-utterance.
  // portMUX is the cheapest cross-core primitive: a spinlock that
  // also masks interrupts on the holding core. Critical sections
  // are tiny (a few field updates + ≤2 memcpys of at most a few KB
  // per WS frame), so contention is negligible.
  portMUX_TYPE ring_mux_ = portMUX_INITIALIZER_UNLOCKED;

  // Per-turn latency anchors (millis()-relative). Captured at each state
  // transition; flushed as one summary line when the deferred phase=idle
  // emit fires (i.e. when the speaker has actually drained). Zero means
  // "not yet hit this milestone this turn".
  uint32_t turn_t_wake_{0};               // start_session() (wake-word handler)
  uint32_t turn_t_listening_{0};          // server's first phase=listening
  uint32_t turn_t_thinking_{0};           // server's phase=thinking (end-of-speech)
  uint32_t turn_t_first_audio_out_{0};    // first binary chunk arrived from server

  // Diagnostics for the "speech sometimes drops into hiss / noise"
  // symptom. We don't know the cause yet, so we measure three things
  // simultaneously and let the logs tell us which one (if any) fires
  // during a bad reply.
  //
  //  1) WS frame inter-arrival time. If the bridge stalls and audio
  //     arrives in bursts with > kWsGapWarnMs silence between, the
  //     downstream chain may underrun and inject silence/noise. We log
  //     each large gap with the duration and how full the PSRAM ring
  //     was at the time.
  //  2) TTS clipping. Software gain is disabled, so clipping is
  //     mathematically impossible while volume_ ≤ 1. Counter stays as
  //     a tripwire — if anyone reintroduces a >1 scale factor, the
  //     per-turn summary will surface it before users hear the rasp.
  //  3) Downstream underrun. speaker_->has_buffered_data() = false
  //     while audio_fill_ > 0 means the resampler/mixer/i2s chain ran
  //     dry while we still had PSRAM to feed it — bug or stall in the
  //     downstream side. We log the first underrun per reply.
  uint32_t last_binary_ms_{0};
  uint32_t ws_gap_count_{0};       // # gaps > kWsGapWarnMs in this turn
  uint32_t ws_gap_max_ms_{0};      // largest gap observed this turn
  uint32_t clipped_samples_{0};    // clipped samples in this turn
  bool underrun_logged_this_turn_{false};
  static constexpr uint32_t kWsGapWarnMs = 80;  // > ~3× normal 20 ms frame
};

}  // namespace va_client
}  // namespace esphome
