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
  // If a follow-up window was deferred while audio was draining, schedule
  // it to open AFTER the downstream buffers (resampler ring + mixer ring +
  // i2s_audio_speaker 500ms ring) finish playing. Just because our PSRAM
  // queue is empty doesn't mean the user has heard the audio yet —
  // opening the mic too early lets it pick up the device's own tail, and
  // the server VAD interprets it as user speech.
  if (this->followup_pending_ && this->audio_fill_ == 0) {
    const bool was_request = this->request_follow_up_pending_;
    this->followup_pending_ = false;
    this->request_follow_up_pending_ = false;
    const uint32_t duration = was_request ? kRequestFollowUpMs : kFollowupMs;
    this->set_timeout("va_followup_open", kFollowupOpenDelayMs, [this, duration]() {
      this->open_followup_window_(duration);
    });
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

  // One per *failure* (coalesced), not per individual WS event. Once we
  // cross the threshold fire the audible-error trigger exactly once until
  // a successful connect resets the counter.
  this->consecutive_failures_++;
  if (this->consecutive_failures_ >= kRepeatedFailureThreshold &&
      !this->repeated_failure_fired_) {
    this->repeated_failure_fired_ = true;
    ESP_LOGW(TAG, "%u consecutive reconnect failures — firing on_repeated_failure",
             (unsigned) this->consecutive_failures_);
    this->defer([this]() {
      for (auto *t : this->repeated_failure_triggers_) {
        t->trigger();
      }
    });
  }

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
      // Don't reset the failure counter / fired flag yet — a flap-and-die
      // link would spam chimes. Only re-arm after the connection has held
      // for kStableConnectionMs without a disconnect.
      this->set_timeout("va_stable_connection", kStableConnectionMs, [this]() {
        if (this->ws_connected_) {
          this->consecutive_failures_ = 0;
          this->repeated_failure_fired_ = false;
          ESP_LOGD(TAG, "WS stable for %u ms — error chime re-armed",
                   (unsigned) kStableConnectionMs);
        }
      });

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
      // Connection broke before the stability window elapsed — keep the
      // failure counter and the fired flag. A flapping link won't earn
      // a fresh chime.
      this->cancel_timeout("va_stable_connection");
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

  if (msg.find("\"type\":\"request_follow_up\"") != std::string::npos) {
    // Server's model called the request_follow_up tool — it asked a
    // question and wants the user to answer without saying a wake word.
    // BUT this message arrives with the question's audio still queued in
    // PSRAM + downstream rings — opening the mic now means it picks up
    // the AI's own question. Defer to loop()'s drain handler, same as
    // the natural-idle path; mark this as a request-driven follow-up so
    // the longer kRequestFollowUpMs window applies once it fires.
    if (this->audio_fill_ == 0) {
      ESP_LOGI(TAG, "request_follow_up — opening %u ms mic window",
               (unsigned) kRequestFollowUpMs);
      this->open_followup_window_(kRequestFollowUpMs);
    } else {
      ESP_LOGI(TAG, "request_follow_up but %u bytes still queued; mic window deferred",
               (unsigned) this->audio_fill_);
      this->followup_pending_ = true;
      this->request_follow_up_pending_ = true;
    }
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
  if (this->turn_t_first_audio_out_ == 0 && this->turn_t_wake_ != 0) {
    this->turn_t_first_audio_out_ = millis();
  }
  // PCM16 mono @ 24 kHz, append to ring buffer. loop() drains.
  size_t free_space = kAudioBufBytes - this->audio_fill_;
  if (len > free_space) {
    ESP_LOGW(TAG, "audio buffer overflow: dropping %u bytes (have %u free of %u total)",
             (unsigned) (len - free_space), (unsigned) free_space, (unsigned) kAudioBufBytes);
    len = free_space;
    if (len == 0)
      return;
  }
  // Apply a small software gain before writing to the ring. OpenAI Realtime
  // TTS peaks around -3..-6 dBFS, which sounds noticeably quieter than the
  // upstream HA TTS engines (which sit near 0 dBFS). We multiply by
  // kTtsGainNumerator / kTtsGainDenominator with saturation. Keep the gain
  // modest — clipping a 16 kHz speech signal is audible as a buzzy rasp.
  // Tune by ear; bump kTtsGainNumerator if still too quiet.
  size_t pairs = len / 2;
  if (pairs > 0) {
    auto *in = reinterpret_cast<const int16_t *>(data);
    // Reuse mono_buf_ as a scratch — it's already int16_t.
    this->mono_buf_.resize(pairs);
    // Combine the +3.5 dB gain (compensates OpenAI's -3..-6 dBFS output)
    // with the user-controlled volume coming from external_media_player.
    // volume_ is set from yaml on every media_player volume / mute change.
    float vol = this->volume_;
    if (vol < 0.0f) vol = 0.0f;
    else if (vol > 1.0f) vol = 1.0f;
    // Scale factor in fixed-point Q15 so we stay int-only in the inner loop.
    int32_t scale = static_cast<int32_t>(
        vol * (32768.0f * kTtsGainNumerator / kTtsGainDenominator));
    for (size_t i = 0; i < pairs; i++) {
      int32_t v = (static_cast<int32_t>(in[i]) * scale) >> 15;
      if (v > 32767) v = 32767;
      else if (v < -32768) v = -32768;
      this->mono_buf_[i] = static_cast<int16_t>(v);
    }
    data = reinterpret_cast<const uint8_t *>(this->mono_buf_.data());
    // len is unchanged (pairs * 2 == len rounded down; trailing odd byte ignored).
    len = pairs * 2;
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
  const std::string prev_phase = this->current_phase_;
  this->current_phase_ = phase;
  ESP_LOGD(TAG, "Phase -> %s", phase.c_str());

  // Streaming gate state machine:
  //   listening  → mic on (user is being heard)
  //   thinking   → mic off (server processing; sending more burns WS bandwidth
  //                that TTS needs, and OpenAI ignores audio while a response
  //                is in flight)
  //   replying   → mic off (also avoids picking up our own TTS in case XMOS
  //                AEC isn't perfect)
  //   idle       → mic on for kFollowupMs so the user can answer a question
  //                without re-triggering the wake word. Timer expiry closes
  //                the session.
  if (phase == "listening") {
    if (!this->streaming_) {
      ESP_LOGI(TAG, "phase=listening — mic streaming on");
      this->streaming_ = true;
    }
    if (this->turn_t_listening_ == 0 && this->turn_t_wake_ != 0) {
      this->turn_t_listening_ = millis();
    }
    // Server heard us — watchdog no longer needed.
    this->cancel_timeout("va_no_speech");
    this->cancel_timeout("va_followup");
  } else if (phase == "thinking" || phase == "replying") {
    if (this->streaming_) {
      ESP_LOGI(TAG, "phase=%s — mic streaming off", phase.c_str());
      this->streaming_ = false;
    }
    if (phase == "thinking" && this->turn_t_thinking_ == 0 && this->turn_t_wake_ != 0) {
      this->turn_t_thinking_ = millis();
    }
    this->cancel_timeout("va_followup");
    this->cancel_timeout("va_followup_open");
    this->cancel_timeout("va_no_speech");
    this->followup_pending_ = false;
    this->request_follow_up_pending_ = false;
    this->idle_emit_pending_ = false;  // new turn began, drop any held idle
  } else if (phase == "idle") {
    // Only open a follow-up window if we just finished a real turn —
    // i.e. the previous phase was thinking or replying. Otherwise we'd
    // open the window for every spurious idle (initial WS hello, post-
    // disconnect idle, etc), spamming "follow-up window open" logs and
    // opening the mic for 5s every time the device just reconnects.
    const bool turn_just_ended = prev_phase == "thinking" || prev_phase == "replying";
    if (!turn_just_ended) {
      // Plain idle (boot, reconnect, etc) — no follow-up. Fall through to
      // the regular trigger fire so the LED updates.
    } else if (this->suppress_followup_) {
      // send_interrupt() set this — user explicitly asked us to stop.
      // Close the session cleanly: streaming off, no follow-up, fall through
      // to the regular trigger fire so the LED goes idle.
      this->suppress_followup_ = false;
      this->streaming_ = false;
      this->followup_pending_ = false;
      this->request_follow_up_pending_ = false;
      this->idle_emit_pending_ = false;
    } else if (this->audio_fill_ == 0) {
      // Server says response.done and the device has actually played out.
      // Open the follow-up window (mic on so user can answer a question).
      this->open_followup_window_();
      // fall through to fire the trigger normally below
    } else {
      // Server says response.done, but we still have seconds of TTS queued
      // in PSRAM + downstream rings. Two things wait on the queue:
      //   1) the LED transition to idle (otherwise it goes off while the
      //      device is still speaking)
      //   2) opening the follow-up mic window (echo + false VAD trigger)
      // Mark both pending; the drain handler in loop() releases them
      // together after the speaker actually finishes.
      ESP_LOGI(TAG, "phase=idle but %u bytes still queued; LED + follow-up deferred",
               (unsigned) this->audio_fill_);
      this->followup_pending_ = true;
      this->idle_emit_pending_ = true;
      return;  // suppress immediate trigger fire — open_followup_window_ will fire it later
    }
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
  // New wake word starts a fresh session — drop any pending or active
  // follow-up window from the previous turn.
  this->followup_pending_ = false;
  this->request_follow_up_pending_ = false;
  this->idle_emit_pending_ = false;
  this->suppress_followup_ = false;
  this->cancel_timeout("va_followup");
  this->cancel_timeout("va_followup_open");
  // Anchor turn-latency timestamps for the new turn.
  this->turn_t_wake_ = millis();
  this->turn_t_listening_ = 0;
  this->turn_t_thinking_ = 0;
  this->turn_t_first_audio_out_ = 0;
  // Watchdog: if server doesn't hear us within kNoSpeechTimeoutMs, abort the
  // session so we're not stuck with the mic open after a misfire.
  this->set_timeout("va_no_speech", kNoSpeechTimeoutMs, [this]() {
    ESP_LOGI(TAG, "no speech detected for %u ms — aborting session",
             (unsigned) kNoSpeechTimeoutMs);
    if (this->ws_connected_ && this->ws_handle_ != nullptr) {
      const char m[] = "{\"type\":\"interrupt\"}";
      auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
      esp_websocket_client_send_text(handle, m, sizeof(m) - 1, portMAX_DELAY);
    }
    this->streaming_ = false;
    this->turn_t_wake_ = 0;
    // Force LED back to idle from yaml side.
    this->defer([this]() {
      for (auto *t : this->phase_triggers_) {
        t->trigger("idle");
      }
    });
  });
}

void VaClient::open_followup_window_(uint32_t duration_ms) {
  // If a phase=idle LED transition was held back while audio drained, fire
  // it now so the LED goes to idle in sync with the speaker actually going
  // quiet (instead of as soon as the server emitted response.done).
  if (this->idle_emit_pending_) {
    this->idle_emit_pending_ = false;
    this->defer([this]() {
      for (auto *t : this->phase_triggers_) {
        t->trigger("idle");
      }
    });
    // Per-turn latency summary. Anchors are zero if we skipped a milestone
    // (e.g. interrupt mid-reply); show "?" so the line stays readable.
    if (this->turn_t_wake_ != 0) {
      uint32_t now = millis();
      auto fmt = [](uint32_t from, uint32_t to) -> std::string {
        if (from == 0 || to == 0 || to < from)
          return "?";
        return std::to_string(to - from) + "ms";
      };
      ESP_LOGI(TAG,
               "turn latency: wake→listening=%s listening→thinking=%s "
               "thinking→first_audio=%s first_audio→played_out=%s "
               "total=%s",
               fmt(this->turn_t_wake_, this->turn_t_listening_).c_str(),
               fmt(this->turn_t_listening_, this->turn_t_thinking_).c_str(),
               fmt(this->turn_t_thinking_, this->turn_t_first_audio_out_).c_str(),
               fmt(this->turn_t_first_audio_out_, now).c_str(),
               fmt(this->turn_t_wake_, now).c_str());
      this->turn_t_wake_ = 0;  // mark turn as logged
    }
  }
  if (duration_ms == 0) {
    // Follow-up disabled for this call: turn-based behaviour like the
    // original pipeline. Leave the mic closed; user must say a wake word
    // for the next turn.
    this->streaming_ = false;
    return;
  }
  ESP_LOGI(TAG, "follow-up window open (mic on for %u ms)", (unsigned) duration_ms);
  this->streaming_ = true;
  this->set_timeout("va_followup", duration_ms, [this]() {
    if (this->streaming_) {
      ESP_LOGI(TAG, "follow-up window expired — mic streaming off");
      this->streaming_ = false;
    }
  });
}

void VaClient::send_interrupt() {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr) {
    ESP_LOGW(TAG, "send_interrupt: WS not connected");
    return;
  }
  const char msg[] = "{\"type\":\"interrupt\"}";
  auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
  esp_websocket_client_send_text(handle, msg, sizeof(msg) - 1, portMAX_DELAY);
  // Flush our PSRAM playback queue — what's already been pushed into the
  // resampler/mixer/leaf will still drain (~600 ms residual), but everything
  // we have yet to hand off is dropped. The yaml side stops the resampler
  // explicitly. Reset deferred state too so we don't accidentally hold an
  // "idle" emit waiting for the (now-empty) queue.
  this->audio_head_ = 0;
  this->audio_tail_ = 0;
  this->audio_fill_ = 0;
  this->followup_pending_ = false;
  this->request_follow_up_pending_ = false;
  this->idle_emit_pending_ = false;
  this->cancel_timeout("va_no_speech");
  this->cancel_timeout("va_followup");
  // The phase=idle the server is about to send shouldn't open a follow-up
  // mic window — the user said "stop", not "wait for me to keep talking".
  this->suppress_followup_ = true;
  this->cancel_timeout("va_followup_open");
  ESP_LOGI(TAG, "send_interrupt — WS msg sent, queue flushed");
}

}  // namespace va_client
}  // namespace esphome
