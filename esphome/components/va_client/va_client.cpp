#include "va_client.h"
#include "automation.h"

#include "esphome/core/log.h"
#include "esphome/components/audio/audio.h"

#include <cstring>

#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_heap_caps.h>

namespace esphome {
namespace va_client {

static const char *const TAG = "va_client";

// Free-function trampoline. esp-idf event registration takes a C function
// pointer; we recover the VaClient* from the user_data slot.
static void va_ws_event_handler(void *handler_args, esp_event_base_t /*base*/, int32_t event_id, void *event_data) {
  auto *self = static_cast<VaClient *>(handler_args);
  if (self == nullptr)
    return;
  self->on_ws_event(event_id, event_data);
}

void VaClient::setup() {
  ESP_LOGCONFIG(TAG, "Setting up VA Client...");

  if (this->mic_ != nullptr) {
    this->mic_->add_data_callback(
        [this](const std::vector<uint8_t> &data) { this->on_mic_data_(data); });
  } else {
    ESP_LOGE(TAG, "Microphone not configured");
  }

  // Allocate the audio ring buffer in PSRAM (8 MB available, internal RAM
  // is only 320 KB and we don't want to starve wifi/mww).
  this->audio_buf_ = static_cast<uint8_t *>(
      heap_caps_malloc(kAudioBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->audio_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-byte audio buffer in PSRAM", (unsigned) kAudioBufBytes);
  } else {
    ESP_LOGCONFIG(TAG, "Allocated %u-byte audio ring buffer in PSRAM", (unsigned) kAudioBufBytes);
  }

  // Tell the resampler what format we'll feed it. The resampler converts to
  // its yaml-configured output format (48k 16-bit) before passing to the
  // mixer → i2s leaf. Start the speaker task once so play() calls just push
  // into its ring buffer instead of racing to re-create the i2s channel.
  if (this->speaker_ != nullptr) {
    audio::AudioStreamInfo info(/*bits_per_sample=*/16, /*channels=*/1, /*sample_rate=*/24000);
    this->speaker_->set_audio_stream_info(info);
    this->speaker_->start();
  }

  this->connect_();
}

void VaClient::loop() {
  // Drain the audio ring buffer into the speaker. speaker.play() accepts
  // only what fits in its own ring (returns the count actually queued).
  if (this->speaker_ == nullptr || this->audio_buf_ == nullptr || this->audio_fill_ == 0)
    return;
  // Contiguous slice we can hand to play() without copying: from head to
  // either the end of the buffer or the tail.
  size_t contiguous = (this->audio_head_ < this->audio_tail_)
                          ? (this->audio_tail_ - this->audio_head_)
                          : (kAudioBufBytes - this->audio_head_);
  if (contiguous > this->audio_fill_)
    contiguous = this->audio_fill_;
  size_t accepted = this->speaker_->play(this->audio_buf_ + this->audio_head_, contiguous);
  if (accepted == 0)
    return;
  this->audio_head_ = (this->audio_head_ + accepted) % kAudioBufBytes;
  this->audio_fill_ -= accepted;
  static uint32_t dbg_last = 0;
  uint32_t now = millis();
  if (now - dbg_last >= 500) {
    ESP_LOGD(TAG, "drained %u bytes (%u still queued)", (unsigned) accepted,
             (unsigned) this->audio_fill_);
    dbg_last = now;
  }
}

void VaClient::connect_() {
  if (this->ws_handle_ != nullptr) {
    // Already initialised; just (re)start.
    esp_websocket_client_start(static_cast<esp_websocket_client_handle_t>(this->ws_handle_));
    return;
  }

  // Keep the header string alive for the entire lifetime of the client; the
  // config struct only stores a pointer.
  this->auth_header_ = "Authorization: Bearer " + this->token_ + "\r\n";

  esp_websocket_client_config_t cfg = {};
  cfg.uri = this->url_.c_str();
  cfg.headers = this->auth_header_.c_str();
  cfg.disable_auto_reconnect = true;  // we drive reconnects ourselves with exponential backoff
  cfg.reconnect_timeout_ms = 5000;    // ignored because disable_auto_reconnect=true

  esp_websocket_client_handle_t handle = esp_websocket_client_init(&cfg);
  if (handle == nullptr) {
    ESP_LOGE(TAG, "esp_websocket_client_init failed");
    this->schedule_reconnect_();
    return;
  }
  this->ws_handle_ = handle;

  esp_err_t err = esp_websocket_register_events(handle, WEBSOCKET_EVENT_ANY, va_ws_event_handler, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_websocket_register_events failed: %d", (int) err);
  }

  ESP_LOGI(TAG, "Connecting to %s", this->url_.c_str());
  err = esp_websocket_client_start(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_websocket_client_start failed: %d", (int) err);
    this->schedule_reconnect_();
  }
}

void VaClient::schedule_reconnect_() {
  // esp_websocket_client emits multiple events per failure (DISCONNECTED,
  // CLOSED, sometimes ERROR). Coalesce them into a single reconnect.
  if (this->reconnect_pending_) {
    return;
  }
  this->reconnect_pending_ = true;

  uint32_t delay = this->reconnect_delay_ms_;
  ESP_LOGI(TAG, "Scheduling reconnect in %u ms", (unsigned) delay);
  // Backoff schedule: 1s -> 2s -> 5s -> 10s (capped).
  if (this->reconnect_delay_ms_ < 2000) {
    this->reconnect_delay_ms_ = 2000;
  } else if (this->reconnect_delay_ms_ < 5000) {
    this->reconnect_delay_ms_ = 5000;
  } else {
    this->reconnect_delay_ms_ = 10000;
  }
  this->set_timeout("va_reconnect", delay, [this]() {
    this->reconnect_pending_ = false;
    this->connect_();
  });
}

void VaClient::on_ws_event(int32_t event_id, void *event_data) {
  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
      ESP_LOGI(TAG, "WS connected");
      this->ws_connected_ = true;
      this->reconnect_delay_ms_ = 1000;  // reset backoff on a clean open

      const char start_msg[] = "{\"type\":\"start\"}";
      auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
      esp_websocket_client_send_text(handle, start_msg, sizeof(start_msg) - 1, portMAX_DELAY);
      this->set_phase_("idle");
      break;
    }
    case WEBSOCKET_EVENT_DATA: {
      if (data == nullptr || data->data_ptr == nullptr || data->data_len <= 0)
        break;
      // op_code: 0x01 = text, 0x02 = binary, 0x00 = continuation of the prior
      // frame. esp_websocket_client splits long messages, so we must track the
      // type from the first chunk and feed continuations to the same handler.
      uint8_t op = data->op_code;
      if (op == 0x01) {
        this->last_data_was_binary_ = false;
        this->handle_text_(data->data_ptr, static_cast<size_t>(data->data_len));
      } else if (op == 0x02) {
        this->last_data_was_binary_ = true;
        this->handle_binary_(reinterpret_cast<const uint8_t *>(data->data_ptr),
                             static_cast<size_t>(data->data_len));
      } else if (op == 0x00) {
        // Continuation. Route based on the type of the in-flight message.
        if (this->last_data_was_binary_) {
          this->handle_binary_(reinterpret_cast<const uint8_t *>(data->data_ptr),
                               static_cast<size_t>(data->data_len));
        } else {
          this->handle_text_(data->data_ptr, static_cast<size_t>(data->data_len));
        }
      }
      break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR: {
      if (this->ws_connected_) {
        ESP_LOGW(TAG, "WS disconnected (event %d)", (int) event_id);
      }
      this->ws_connected_ = false;
      this->set_phase_("idle");
      this->schedule_reconnect_();
      break;
    }
    default:
      break;
  }
}

void VaClient::handle_text_(const char *data, size_t len) {
  std::string msg(data, len);
  ESP_LOGD(TAG, "WS text: %s", msg.c_str());

  if (msg.find("\"type\":\"error\"") != std::string::npos) {
    ESP_LOGW(TAG, "Server reported error: %s", msg.c_str());
    this->set_phase_("idle");
    return;
  }

  // Substring match on `"value":"<phase>"` — keeps us out of a JSON parser
  // until M3 needs richer payloads.
  static const char *const kPhases[] = {"listening", "thinking", "replying", "idle"};
  for (const char *p : kPhases) {
    std::string needle = std::string("\"value\":\"") + p + "\"";
    if (msg.find(needle) != std::string::npos) {
      this->set_phase_(p);
      return;
    }
  }
}

void VaClient::handle_binary_(const uint8_t *data, size_t len) {
  if (this->speaker_ == nullptr || len < 2 || this->audio_buf_ == nullptr)
    return;
  // PCM16 mono @ 24 kHz, append to ring buffer. loop() drains.
  size_t free_space = kAudioBufBytes - this->audio_fill_;
  if (len > free_space) {
    ESP_LOGW(TAG, "audio buffer overflow: dropping %u bytes (have %u free of %u total)",
             (unsigned) (len - free_space), (unsigned) free_space, (unsigned) kAudioBufBytes);
    len = free_space;
    if (len == 0)
      return;
  }
  // Two-part write: from tail to end, then wrap to start.
  size_t first = std::min(len, kAudioBufBytes - this->audio_tail_);
  std::memcpy(this->audio_buf_ + this->audio_tail_, data, first);
  if (first < len) {
    std::memcpy(this->audio_buf_, data + first, len - first);
  }
  this->audio_tail_ = (this->audio_tail_ + len) % kAudioBufBytes;
  this->audio_fill_ += len;
  // No per-chunk log — fires 50+ times per reply at DEBUG and drowns the
  // log. The throttled drain log in loop() gives enough visibility into
  // queue depth.
}

void VaClient::on_mic_data_(const std::vector<uint8_t> &samples) {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr)
    return;
  // Gate streaming on the wake word: if no session is active, drop frames.
  // Otherwise OpenAI Realtime's server VAD would trigger responses to any
  // random speech in the room — wake word would become decoration. The
  // session opens via start_session() (wake-word handler) and closes on
  // "phase":"idle" coming back from the server (response.done).
  if (!this->streaming_)
    return;
  // i2s_mics yields interleaved stereo int32 frames: [L0_low,L0_high, R0_low,R0_high, L1..].
  // Each frame = 8 bytes (2ch × 4 bytes). We want one channel converted to
  // int16 mono. Real audio sits in the high 16 bits (ADC pads up to int32).
  if (samples.size() < 8)
    return;

  const auto *in32 = reinterpret_cast<const int32_t *>(samples.data());
  size_t total_int32 = samples.size() / 4;
  size_t mono_samples = total_int32 / 2;  // half belong to this channel
  size_t offset = this->mic_channel_ & 0x1;

  this->mono_buf_.resize(mono_samples);
  for (size_t i = 0; i < mono_samples; i++) {
    int32_t s = in32[i * 2 + offset];
    this->mono_buf_[i] = static_cast<int16_t>(s >> 16);
  }

  auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
  // 10ms timeout (~portTICK_PERIOD_MS): if WS task is briefly busy we wait
  // a tick rather than dropping the frame and spamming "Could not lock"
  // errors. If we're swamped, we accept dropping rather than blocking mic.
  esp_websocket_client_send_bin(handle, reinterpret_cast<const char *>(this->mono_buf_.data()),
                                static_cast<int>(this->mono_buf_.size() * sizeof(int16_t)),
                                10 / portTICK_PERIOD_MS);
}

void VaClient::set_phase_(const std::string &phase) {
  // Don't dedupe — we want yaml-side control_leds to re-render even on
  // identical phase if other inputs (e.g. va WS connection state) have
  // changed since the last emission.
  this->current_phase_ = phase;
  ESP_LOGD(TAG, "Phase -> %s", phase.c_str());

  // Close the mic streaming window once the server has heard us
  // (phase=thinking) — continuing to send mic frames while TTS audio is
  // streaming back competes for the WS socket on ESP32 and produces choppy
  // playback. Listening for the next wake word doesn't go through the WS,
  // it's local mww.
  if (this->streaming_ && (phase == "thinking" || phase == "replying" || phase == "idle")) {
    ESP_LOGI(TAG, "phase=%s — mic streaming off", phase.c_str());
    this->streaming_ = false;
  }

  // set_phase_ may be called from the websocket task; ESPHome triggers and
  // most component APIs are not thread-safe. Marshal the side effects onto
  // the main loop via defer().
  std::string phase_copy = phase;
  this->defer([this, phase_copy]() {
    // We deliberately do NOT call speaker->stop() on "listening" anymore:
    // the speaker task runs continuously after setup() and play() just
    // appends to its ring buffer. Stop/start churn was creating multiple
    // speaker_task instances racing for the i2s channel ("Parent bus is
    // busy"). For barge-in/interrupt we'll add a buffer-flush API in M3.
    for (auto *t : this->phase_triggers_) {
      t->trigger(phase_copy);
    }
  });
}

void VaClient::start_session() {
  // Open the streaming window. on_mic_data_ will start forwarding frames to
  // the server until "phase":"idle" comes back (response.done). Without this
  // gate, OpenAI Realtime's server VAD would respond to any speech in the
  // room — wake word would be cosmetic.
  ESP_LOGI(TAG, "start_session() — streaming on");
  this->streaming_ = true;
}

void VaClient::send_interrupt() {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr) {
    ESP_LOGW(TAG, "send_interrupt: WS not connected");
    return;
  }
  const char msg[] = "{\"type\":\"interrupt\"}";
  auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
  esp_websocket_client_send_text(handle, msg, sizeof(msg) - 1, portMAX_DELAY);
}

}  // namespace va_client
}  // namespace esphome
