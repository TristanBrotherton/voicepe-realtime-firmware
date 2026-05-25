#include "va_client.h"
#include "automation.h"

#include "esphome/core/log.h"

#include <cstring>

#include <esp_websocket_client.h>
#include <esp_event.h>

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

  this->connect_();
}

void VaClient::loop() {
  // All work is event-driven (esp-idf WS task + microphone callback).
  // loop() is intentionally empty; reconnects are scheduled via set_timeout().
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
      // op_code: 0x01 = text frame, 0x02 = binary frame.
      if (data->op_code == 0x02) {
        this->handle_binary_(reinterpret_cast<const uint8_t *>(data->data_ptr),
                             static_cast<size_t>(data->data_len));
      } else if (data->op_code == 0x01) {
        this->handle_text_(data->data_ptr, static_cast<size_t>(data->data_len));
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
  if (this->speaker_ == nullptr || len < 2)
    return;

  // Server frame is PCM16 mono @ 24 kHz. Upsample to 48 kHz stereo with
  // zero-order hold (each input sample S -> S,S,S,S). Blocky but cheap;
  // M4 will replace with proper interpolation if needed.
  const auto *in = reinterpret_cast<const int16_t *>(data);
  size_t in_samples = len / 2;

  this->stereo_48k_buf_.resize(in_samples * 4);
  int16_t *out = this->stereo_48k_buf_.data();
  for (size_t i = 0; i < in_samples; i++) {
    int16_t s = in[i];
    out[i * 4 + 0] = s;
    out[i * 4 + 1] = s;
    out[i * 4 + 2] = s;
    out[i * 4 + 3] = s;
  }

  this->speaker_->play(reinterpret_cast<const uint8_t *>(out), this->stereo_48k_buf_.size() * sizeof(int16_t));
}

void VaClient::on_mic_data_(const std::vector<uint8_t> &samples) {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr)
    return;
  // Incoming buffer is interleaved stereo int16 [L0,R0,L1,R1,...]. We need
  // mic_channel_ (0 = L, 1 = R) as mono.
  if (samples.size() < 4)
    return;

  const auto *in = reinterpret_cast<const int16_t *>(samples.data());
  size_t total_int16 = samples.size() / 2;
  size_t mono_samples = total_int16 / 2;  // half are this channel

  this->mono_buf_.resize(mono_samples);
  size_t offset = this->mic_channel_ & 0x1;
  for (size_t i = 0; i < mono_samples; i++) {
    this->mono_buf_[i] = in[i * 2 + offset];
  }

  auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
  esp_websocket_client_send_bin(handle, reinterpret_cast<const char *>(this->mono_buf_.data()),
                                static_cast<int>(this->mono_buf_.size() * sizeof(int16_t)), 0);
}

void VaClient::set_phase_(const std::string &phase) {
  if (this->current_phase_ == phase)
    return;
  this->current_phase_ = phase;
  ESP_LOGD(TAG, "Phase -> %s", phase.c_str());

  // set_phase_ may be called from the websocket task; ESPHome triggers and
  // most component APIs are not thread-safe. Marshal the side effects onto
  // the main loop via defer().
  std::string phase_copy = phase;
  this->defer([this, phase_copy]() {
    // Drop any lingering TTS audio when we move into listening so we don't
    // talk over the user.
    if (phase_copy == "listening" && this->speaker_ != nullptr) {
      this->speaker_->stop();
    }
    for (auto *t : this->phase_triggers_) {
      t->trigger(phase_copy);
    }
  });
}

void VaClient::start_session() {
  // Mic streaming is always-on while WS is connected; this is mostly a hook
  // for the wake-word handler. M3 will use it to flush state.
  ESP_LOGD(TAG, "start_session() called (no-op stub)");
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
