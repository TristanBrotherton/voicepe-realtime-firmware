#pragma once

#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace va_client {

class OnPhaseTrigger;
class OnRepeatedFailureTrigger;

class VaClient : public Component {
 public:
  void set_url(const std::string &url) { url_ = url; }
  void set_token(const std::string &token) { token_ = token; }
  void set_microphone(microphone::Microphone *m) { mic_ = m; }
  void set_mic_channel(uint8_t c) { mic_channel_ = c; }
  void set_speaker(speaker::Speaker *s) { speaker_ = s; }
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

  bool is_connected() const { return ws_connected_; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // YAML-callable actions.
  void start_session();
  void send_interrupt();

  // Called from the static esp-idf event handler trampoline.
  void on_ws_event(int32_t event_id, void *event_data);

 protected:
  void connect_();
  void schedule_reconnect_();
  void on_mic_data_(const std::vector<uint8_t> &samples);
  void handle_text_(const char *data, size_t len);
  void handle_binary_(const uint8_t *data, size_t len);
  void set_phase_(const std::string &phase);
  void open_followup_window_(uint32_t duration_ms = kFollowupMs);

  std::string url_;
  std::string token_;
  // Lifetime-stable storage referenced by esp_websocket_client_config_t.headers.
  std::string auth_header_;
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

  // Scratch buffers reused on the hot path to avoid per-callback heap allocation.
  std::vector<int16_t> mono_buf_;

  // Streaming gate. True while the mic should be forwarded to the server:
  //   - between wake-word start_session() and "listening"/"thinking"
  //   - and again after "idle" for kFollowupMs (in case AI asked a question)
  bool streaming_{false};
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
  // Server sends phase=idle when OpenAI is done generating, but we still
  // have audio queued in PSRAM + downstream rings. If we fire the LED
  // trigger immediately the device looks idle while still speaking. Hold
  // the "idle" emission until the queue drains + kFollowupOpenDelayMs.
  bool idle_emit_pending_{false};
  // Set by send_interrupt() so the phase=idle that follows from the server
  // doesn't trigger a follow-up mic window. The user explicitly asked us to
  // stop — they don't want the device sitting there listening.
  bool suppress_followup_{false};
  // Follow-up dialog window after a real turn ends. 0 disables — mic
  // closes immediately after each reply, like the original turn-based
  // pipeline. Currently 0 because XMOS AEC is too leaky and the mic
  // hears its own TTS tail during this window. Re-enable (e.g. 5000)
  // when AEC is tuned or we add wait_for_user on the server.
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

  // Tracks the opcode of the in-flight WS message so we can route
  // continuation frames (op_code = 0) to the same handler.
  bool last_data_was_binary_{false};

  // Software TTS gain applied in handle_binary_ before queueing audio.
  // OpenAI Realtime output peaks around -3..-6 dBFS — perceptibly quieter
  // than the upstream HA TTS engines which sit near 0 dBFS. 2/1 = +6 dB
  // is loud enough to match the upstream feel and still has headroom
  // because volume_max in the media_player block caps us at 0.85.
  // Raise the numerator carefully — speech that clips sounds buzzy, not
  // just loud.
  static constexpr int32_t kTtsGainNumerator = 2;
  static constexpr int32_t kTtsGainDenominator = 1;

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

  // Per-turn latency anchors (millis()-relative). Captured at each state
  // transition; flushed as one summary line when the deferred phase=idle
  // emit fires (i.e. when the speaker has actually drained). Zero means
  // "not yet hit this milestone this turn".
  uint32_t turn_t_wake_{0};               // start_session() (wake-word handler)
  uint32_t turn_t_listening_{0};          // server's first phase=listening
  uint32_t turn_t_thinking_{0};           // server's phase=thinking (end-of-speech)
  uint32_t turn_t_first_audio_out_{0};    // first binary chunk arrived from server
};

}  // namespace va_client
}  // namespace esphome
