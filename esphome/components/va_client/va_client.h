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

class VaClient : public Component {
 public:
  void set_url(const std::string &url) { url_ = url; }
  void set_token(const std::string &token) { token_ = token; }
  void set_microphone(microphone::Microphone *m) { mic_ = m; }
  void set_mic_channel(uint8_t c) { mic_channel_ = c; }
  void set_speaker(speaker::Speaker *s) { speaker_ = s; }
  void add_on_phase_trigger(OnPhaseTrigger *t) { phase_triggers_.push_back(t); }

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

  // Scratch buffers reused on the hot path to avoid per-callback heap allocation.
  std::vector<int16_t> mono_buf_;

  // Streaming gate: only true between wake-word `start_session()` and the
  // server's `phase:idle` (response.done). on_mic_data_ checks this before
  // forwarding frames.
  bool streaming_{false};

  // Tracks the opcode of the in-flight WS message so we can route
  // continuation frames (op_code = 0) to the same handler.
  bool last_data_was_binary_{false};

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
};

}  // namespace va_client
}  // namespace esphome
