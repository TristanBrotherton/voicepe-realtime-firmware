#include "va_client.h"
#include "automation.h"

#include "esphome/core/log.h"
#include "esphome/components/audio/audio.h"

#include <algorithm>
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

// Parse the unsigned integer immediately following `key` in `msg` (key includes
// the quotes + colon, e.g. "\"follow_up_ms\":"). Returns true and sets `out` if a
// run of digits was found right after the key. Keeps us out of a JSON parser for
// the few small ints the backend sends in `hello`.
static bool parse_uint_after_key(const std::string &msg, const char *key, uint32_t &out) {
  size_t p = msg.find(key);
  if (p == std::string::npos)
    return false;
  p += std::strlen(key);
  uint32_t v = 0;
  bool any = false;
  while (p < msg.size() && msg[p] >= '0' && msg[p] <= '9') {
    v = v * 10u + static_cast<uint32_t>(msg[p] - '0');
    p++;
    any = true;
  }
  if (!any)
    return false;
  out = v;
  return true;
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

  // Allocate the mic pre-roll ring in PSRAM (kPreRollMs of 16 kHz int16 mono).
  this->preroll_capacity_samples_ = (size_t) kPreRollMs * (kMicSampleRate / 1000);
  this->preroll_buf_ = static_cast<int16_t *>(
      heap_caps_malloc(this->preroll_capacity_samples_ * sizeof(int16_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->preroll_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-sample mic pre-roll buffer in PSRAM",
             (unsigned) this->preroll_capacity_samples_);
    this->preroll_capacity_samples_ = 0;
  } else {
    ESP_LOGCONFIG(TAG, "Allocated %u ms mic pre-roll buffer in PSRAM (%u samples)",
                  (unsigned) kPreRollMs, (unsigned) this->preroll_capacity_samples_);
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
  if (this->speaker_ != nullptr && this->audio_buf_ != nullptr) {
    // Snapshot ring state under the lock — head/tail/fill are all
    // mutated from the WS task on the other core.
    portENTER_CRITICAL(&this->ring_mux_);
    size_t head = this->audio_head_;
    size_t tail = this->audio_tail_;
    size_t fill = this->audio_fill_;
    portEXIT_CRITICAL(&this->ring_mux_);
    if (fill > 0) {
      // Resampler cold-start SILENCE-PRIME (crackle fix). The resampler does NOT
      // idle-timeout (verified vs ESPHome source): resample(stop_gracefully=false)
      // never returns FINISHED, and its output mixer-source is timeout:never, so the
      // chain stays WARM between normal replies. It goes COLD only after an explicit
      // `speaker.stop: media_resampling_speaker` (yaml interrupt / "stop" word / wake /
      // follow-up), which tears the task down to STATE_STOPPED. The next reply then
      // cold-starts a fresh AudioResampler whose windowed-sinc FIR begins from a zero
      // state → a startup-transient click. A PSRAM prebuffer can't fix it (the transient
      // is downstream of the ring). Fix: when cold, feed kChainPrimeMs of silence BEFORE
      // the first real sample so the FIR settles to a clean zero output first. We detect
      // "cold" two ways: the resampler actually reporting is_stopped() (true exactly
      // post-speaker.stop — the precise signal) OR, as a backup, nothing fed for
      // > kChainColdMs. is_stopped() closes the window the timer alone misses: a reply
      // whose audio lands < kChainColdMs after a speaker.stop (the residual click). A
      // needless prime on an already-warm chain is harmless (60ms of silence); both
      // signals are only ever true at a real cold reply-start, never mid-speech. The
      // real audio waits safely in PSRAM (and builds a small cushion) until priming done.
      {
        const uint32_t now_ms = millis();
        const bool resampler_cold = this->speaker_->is_stopped() ||
                                    this->last_fed_ms_ == 0 ||
                                    (now_ms - this->last_fed_ms_) > kChainColdMs;
        if (this->chain_prime_remaining_ == 0 && resampler_cold) {
          this->chain_prime_remaining_ =
              (size_t) kChainPrimeMs * (kPlaybackSampleRate / 1000) * 2;  // ms→bytes (mono 16-bit)
          ESP_LOGD(TAG, "resampler cold — priming %u bytes of silence before reply",
                   (unsigned) this->chain_prime_remaining_);
        }
        if (this->chain_prime_remaining_ > 0) {
          static const uint8_t kSilence[480] = {0};  // 10ms @24k mono16; fed in chunks
          size_t want = std::min(this->chain_prime_remaining_, sizeof(kSilence));
          size_t fed = this->speaker_->play(kSilence, want);
          if (fed > 0) {
            this->chain_prime_remaining_ -= fed;
            this->last_fed_ms_ = now_ms;  // count silence as "fed" so cold-check clears
          }
          // Hold real-audio drain until the chain is warmed. Real audio stays in
          // PSRAM. Re-enter loop() next tick to continue/finish priming.
          return;
        }
      }
      // Jitter buffer priming gate. After the ring was empty (reply start or a
      // post-underflow gap) hold playback until either the prebuffer cushion has
      // accumulated (fill >= target) or a deadline elapses (so real-time, non-
      // burst audio still starts promptly). Holding here lets the downstream
      // chain start with a cushion so a network gap doesn't dry it out → no
      // crackle. Skipped entirely when playback_prebuffer_ms_ == 0 (disabled).
      if (this->playback_priming_) {
        const size_t target =
            (size_t) this->playback_prebuffer_ms_ * (kPlaybackSampleRate / 1000) * 2;
        if (fill >= target ||
            (millis() - this->prime_started_ms_) >= this->playback_prebuffer_ms_) {
          this->playback_priming_ = false;
          ESP_LOGD(TAG, "prebuffer ready (%u bytes) — playback start", (unsigned) fill);
        } else {
          return;  // keep accumulating; don't drain (and don't false-flag underrun)
        }
      }
      // Detector 3: downstream underrun. If the resampler/mixer/i2s chain
      // ran out of bytes to play while we *still* have PSRAM queued,
      // something hiccupped downstream — the user hears silence or a
      // brief stuck-sample glitch. Log the first occurrence per reply so
      // we know whether bad audio in a turn correlates with this.
      if (!this->underrun_logged_this_turn_ && !this->speaker_->has_buffered_data()) {
        ESP_LOGW(TAG, "downstream underrun: %u bytes queued in PSRAM but speaker chain is dry",
                 (unsigned) fill);
        this->underrun_logged_this_turn_ = true;
      }
      // Contiguous slice we can hand to play() without copying: from head
      // to either the end of the buffer or the tail.
      size_t contiguous = (head < tail) ? (tail - head) : (kAudioBufBytes - head);
      if (contiguous > fill)
        contiguous = fill;
      // play() runs OUTSIDE the critical section: it can take milliseconds
      // (resampler ring may be full, mixer blocks). Holding ring_mux_
      // across it would block the writer and cause audio underrun.
      size_t accepted = this->speaker_->play(this->audio_buf_ + head, contiguous);
      if (accepted > 0) {
        this->last_fed_ms_ = millis();  // keep the chain "warm" for cold-detection
        portENTER_CRITICAL(&this->ring_mux_);
        this->audio_head_ = (this->audio_head_ + accepted) % kAudioBufBytes;
        this->audio_fill_ -= accepted;
        portEXIT_CRITICAL(&this->ring_mux_);
        static uint32_t dbg_last = 0;
        uint32_t now = millis();
        if (now - dbg_last >= 500) {
          ESP_LOGD(TAG, "drained %u bytes (%u still queued)", (unsigned) accepted,
                   (unsigned) (fill - accepted));
          dbg_last = now;
        }
      }
    }
  }
  // If a follow-up window was deferred while audio was draining, wait for
  // the downstream chain (resampler + mixer + i2s + DAC tail) to actually
  // finish playing before firing the deferred LED-idle / chime trigger.
  // Just because our PSRAM queue is empty doesn't mean the user has heard
  // the audio yet — and an "сейчас посмотрю" preamble before a tool call
  // would drain the ring mid-turn, so we can't act on audio_fill_==0
  // alone.
  //
  // Primary signal: speaker_->is_stopped(). The resampling speaker
  // transitions to STOPPED only after every byte we wrote has actually
  // gone out through the i2s pipeline.
  //
  // Fallback: kSpeakerStopTimeoutMs (3 s). If something wedges and the
  // speaker never reports STOPPED, we still progress so the LED doesn't
  // lock in `replying`.
  if (this->followup_pending_ && this->audio_fill_ == 0 &&
      !this->waiting_for_speaker_stop_) {
    this->waiting_for_speaker_stop_ = true;
    this->speaker_stop_wait_started_ms_ = millis();
  }
  if (this->waiting_for_speaker_stop_) {
    // Use has_buffered_data() instead of is_stopped(): the resampler only
    // transitions to STATE_STOPPED once its downstream (mixer source)
    // reports stopped, but our mixer sources are configured `timeout:
    // never` and stay RUNNING forever, so is_stopped() would never fire
    // and we'd always hit the fallback. has_buffered_data() walks the
    // chain (resampler ring + mixer source ring) and returns false as
    // soon as both have drained — exactly what we want.
    //
    // Note: this does *not* cover the i2s 500ms ring + ~100ms DAC tail
    // downstream of the mixer. We fire ~500ms before true silence. For
    // the LED that's imperceptible; for the request_follow_up chime,
    // yaml's wait_until !is_announcing + i2s tail delay already absorbs
    // any small overlap with the fading TTS tail.
    const bool speaker_drained =
        (this->speaker_ != nullptr) && !this->speaker_->has_buffered_data();
    const bool timed_out =
        (millis() - this->speaker_stop_wait_started_ms_) >= kSpeakerStopTimeoutMs;
    if (speaker_drained || timed_out) {
      if (timed_out && !speaker_drained) {
        ESP_LOGW(TAG,
                 "speaker still had buffered data after %u ms — "
                 "proceeding anyway (fallback)",
                 (unsigned) kSpeakerStopTimeoutMs);
      }
      this->waiting_for_speaker_stop_ = false;
      const bool was_request = this->request_follow_up_pending_;
      this->followup_pending_ = false;
      this->request_follow_up_pending_ = false;
      if (was_request) {
        // Request-driven path: speaker has drained, now hand off to yaml
        // for the chime → wait_until !is_announcing → commit_followup_mic
        // sequence (announcement lane is separate from the TTS lane we
        // just waited on, so the chime won't collide with our tail).
        this->open_followup_window_(0);  // emit deferred LED idle + latency log; no mic
        this->followup_armed_ = true;
        for (auto *t : this->followup_opened_triggers_) {
          t->trigger();
        }
      } else {
        // Natural-idle path: emit the deferred LED idle, and — if the backend
        // configured a follow-up window (followup_ms_ > 0) — open the mic for
        // that long so the user can answer back without a wake word.
        this->open_followup_window_(this->followup_ms_);
      }
    }
  }
}

void VaClient::connect_() {
  if (this->ws_handle_ != nullptr) {
    // Already initialised; just (re)start.
    esp_websocket_client_start(static_cast<esp_websocket_client_handle_t>(this->ws_handle_));
    return;
  }

  esp_websocket_client_config_t cfg = {};
  cfg.uri = this->url_.c_str();
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
    // Without an audible cue the user just sees the LED go idle and
    // assumes the assistant ignored them. Reuse the on_repeated_failure
    // trigger — it already plays error_cloud_expired and the failure
    // mode is identical from the user's perspective ("something went
    // wrong, try again"). We deliberately don't bump consecutive_failures_
    // here; that counter is for WS reachability, not server-side errors.
    // Marshalled via defer(): handle_text_ runs on the WS task and ESPHome
    // triggers (→ the yaml play_sound script) are not thread-safe.
    this->defer([this]() {
      for (auto *t : this->repeated_failure_triggers_) {
        t->trigger();
      }
    });
    this->set_phase_("idle");
    return;
  }

  if (msg.find("\"type\":\"hello\"") != std::string::npos) {
    // Handshake ack from the backend. It may carry follow-up tuning so the
    // device behaviour is configurable from the add-on (no reflash): a reconnect
    // after an add-on restart re-reads these.
    //   "follow_up_ms":N            — how long the mic stays open after a reply
    //                                 so the user can answer without a wake word.
    //   "follow_up_open_delay_ms":N — delay before that mic opens, to let the
    //                                 reply's i2s/DAC tail finish playing out.
    uint32_t v = 0;
    if (parse_uint_after_key(msg, "\"follow_up_ms\":", v)) {
      if (v > kFollowupMsMax)
        v = kFollowupMsMax;
      this->followup_ms_ = v;
      ESP_LOGI(TAG, "hello: follow-up window = %u ms (%s)", (unsigned) v,
               v == 0 ? "disabled, turn-based" : "mic stays open after replies");
    }
    if (parse_uint_after_key(msg, "\"follow_up_open_delay_ms\":", v)) {
      if (v > kFollowupOpenDelayMaxMs)
        v = kFollowupOpenDelayMaxMs;
      this->followup_open_delay_ms_ = v;
      ESP_LOGI(TAG, "hello: follow-up mic-open delay = %u ms", (unsigned) v);
    }
    if (parse_uint_after_key(msg, "\"playback_prebuffer_ms\":", v)) {
      if (v > kPlaybackPrebufferMaxMs)
        v = kPlaybackPrebufferMaxMs;
      this->playback_prebuffer_ms_ = v;
      ESP_LOGI(TAG, "hello: playback prebuffer (jitter buffer) = %u ms (%s)", (unsigned) v,
               v == 0 ? "disabled" : "cushion before playback");
    }
    return;
  }

  if (msg.find("\"type\":\"request_follow_up\"") != std::string::npos) {
    // Server's model called the request_follow_up tool — it asked a
    // question and wants the user to answer without saying a wake word.
    // Defer to loop()'s waiting_for_speaker_stop_ logic so we only fire
    // the chime + arm the mic after speaker_->is_stopped() returns true
    // (i.e. the i2s pipeline has finished playing the question's audio).
    // Setting both pending flags is idempotent — loop() handles both the
    // "already drained" and "still queued" cases uniformly via the
    // speaker-state poll.
    ESP_LOGI(TAG, "request_follow_up — waiting for speaker drain (%u bytes queued)",
             (unsigned) this->audio_fill_);
    this->followup_pending_ = true;
    this->request_follow_up_pending_ = true;
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
  // After a "stop"/barge-in (send_interrupt) the backend is still streaming the
  // rest of the already-generated reply. Drop it so the cancelled reply goes
  // silent instead of refilling the PSRAM ring we just flushed. Cleared on the
  // next "idle"/"listening" phase (see set_phase_).
  if (this->suppress_incoming_audio_)
    return;
  const uint32_t now_ms = millis();
  if (this->turn_t_first_audio_out_ == 0 && this->turn_t_wake_ != 0) {
    this->turn_t_first_audio_out_ = now_ms;
  }
  // Detector 1: WS frame inter-arrival jitter. Normal cadence is ~20 ms
  // per frame (OpenAI streams realtime). A gap > kWsGapWarnMs means the
  // bridge stalled, network blip, or OpenAI burst late — anything that
  // could starve the downstream chain. Log immediately so the gap is
  // adjacent to whatever the user reports hearing.
  if (this->last_binary_ms_ != 0) {
    const uint32_t gap = now_ms - this->last_binary_ms_;
    if (gap > kWsGapWarnMs) {
      this->ws_gap_count_++;
      if (gap > this->ws_gap_max_ms_) this->ws_gap_max_ms_ = gap;
      ESP_LOGW(TAG, "ws audio gap: %u ms (ring fill %u bytes)",
               (unsigned) gap, (unsigned) this->audio_fill_);
    }
  }
  this->last_binary_ms_ = now_ms;
  // PCM16 mono @ 24 kHz, append to ring buffer. loop() drains.
  // Snapshot audio_fill_ under the lock — it's modified by loop() on the
  // other core and we can't trust a torn read.
  size_t free_space;
  portENTER_CRITICAL(&this->ring_mux_);
  free_space = kAudioBufBytes - this->audio_fill_;
  portEXIT_CRITICAL(&this->ring_mux_);
  if (len > free_space) {
    ESP_LOGW(TAG, "audio buffer overflow: dropping %u bytes (have %u free of %u total)",
             (unsigned) (len - free_space), (unsigned) free_space, (unsigned) kAudioBufBytes);
    len = free_space;
    if (len == 0)
      return;
  }
  // Apply user-controlled volume from external_media_player before writing to
  // the ring. volume_ is set from yaml on every media_player volume / mute
  // change. With vol ≤ 1 there is no mathematical way to overflow int16, but
  // we keep a defensive saturation + clipped_samples_ counter so any future
  // gain re-introduction shows up in the per-turn summary instead of silently
  // distorting.
  size_t pairs = len / 2;
  if (pairs > 0) {
    auto *in = reinterpret_cast<const int16_t *>(data);
    // Scale into tts_buf_ — NOT mono_buf_: that vector is owned by the mic
    // task (on_mic_data_ refills it on every mic frame, and the mic never
    // stops while mWW runs), whereas we're on the WS task. Sharing one vector
    // raced the two tasks (concurrent resize/realloc + interleaved writes),
    // putting mic samples / freed memory into the TTS ring — audible as hiss.
    this->tts_buf_.resize(pairs);
    float vol = this->volume_;
    if (vol < 0.0f) vol = 0.0f;
    else if (vol > 1.0f) vol = 1.0f;
    // Q15 fixed point so the inner loop stays integer-only.
    int32_t scale = static_cast<int32_t>(vol * 32768.0f);
    uint32_t clipped = 0;
    for (size_t i = 0; i < pairs; i++) {
      int32_t v = (static_cast<int32_t>(in[i]) * scale) >> 15;
      if (v > 32767) { v = 32767; clipped++; }
      else if (v < -32768) { v = -32768; clipped++; }
      this->tts_buf_[i] = static_cast<int16_t>(v);
    }
    this->clipped_samples_ += clipped;
    data = reinterpret_cast<const uint8_t *>(this->tts_buf_.data());
    // len is unchanged (pairs * 2 == len rounded down; trailing odd byte ignored).
    len = pairs * 2;
  }
  // Two-part write: from tail to end, then wrap to start.
  // We need a stable snapshot of audio_tail_ for the memcpy destination,
  // then commit tail + fill atomically with the writes so the reader on
  // the other core never sees a new tail before the memcpy completed.
  // Doing the memcpy *inside* the critical section is the simplest way
  // to guarantee that ordering — len is at most a few KB per WS frame
  // and PSRAM memcpy is ~10–20 µs, well under any audio deadline.
  portENTER_CRITICAL(&this->ring_mux_);
  const bool was_empty = (this->audio_fill_ == 0);
  size_t tail = this->audio_tail_;
  size_t first = std::min(len, kAudioBufBytes - tail);
  std::memcpy(this->audio_buf_ + tail, data, first);
  if (first < len) {
    std::memcpy(this->audio_buf_, data + first, len - first);
  }
  this->audio_tail_ = (tail + len) % kAudioBufBytes;
  this->audio_fill_ += len;
  portEXIT_CRITICAL(&this->ring_mux_);
  // Jitter buffer: arm priming only when the ring was empty AND the downstream
  // chain is dry — i.e. a true reply start or a real underflow. Mid-reply the
  // ring routinely flips empty (loop() drains each WS clump on arrival) while
  // the downstream chain still holds ~600 ms of audio; re-arming there did
  // nothing but spam "prebuffer ready" every ~50 ms and could hold a small
  // trailing chunk for the full prebuffer deadline. has_buffered_data() is a
  // counter read, safe enough from the WS task. Only when enabled.
  if (was_empty && this->playback_prebuffer_ms_ > 0 && !this->playback_priming_ &&
      !this->speaker_->has_buffered_data()) {
    this->prime_started_ms_ = now_ms;
    this->playback_priming_ = true;
  }
  // No per-chunk log — fires 50+ times per reply at DEBUG and drowns the
  // log. The throttled drain log in loop() gives enough visibility into
  // queue depth.
}

void VaClient::on_mic_data_(const std::vector<uint8_t> &samples) {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr)
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

  // Streaming gate. When no session is active we don't forward frames to the
  // server (otherwise OpenAI's VAD would respond to any room speech — the wake
  // word would be decoration). We keep a rolling pre-roll ring while closed,
  // but it is DISCARDED on session open (see below), so this is just a cheap
  // rolling buffer kept around for a possible future capture-gating approach.
  // The session opens via start_session() (wake handler) and closes on
  // "phase":"idle" from the server (response.done).
  if (!this->streaming_) {
    this->preroll_push_(this->mono_buf_.data(), this->mono_buf_.size());
    return;
  }

  auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);

  // First frame of a fresh session: DISCARD the pre-roll instead of replaying
  // it. The ring caught the wake chime leaking through the mic (XMOS AEC leaves
  // ~10x) during the chime + tail-delay window; replaying it fed the chime back
  // to OpenAI as a phantom utterance ("Au!"). Resetting here (the ring's only
  // owner is this mic task) avoids any cross-task race — the main loop just sets
  // the flag. Matches marcinnowak79 gemini_proxy's ring_buffer_->reset() on
  // start. Trade-off: a word spoken *during* the chime is lost (the user speaks
  // after the listening ring lights up).
  if (this->preroll_discard_pending_) {
    this->preroll_discard_pending_ = false;
    this->preroll_count_ = 0;
    this->preroll_head_ = 0;
  }

  // 10ms timeout (~portTICK_PERIOD_MS): if WS task is briefly busy we wait
  // a tick rather than dropping the frame and spamming "Could not lock"
  // errors. If we're swamped, we accept dropping rather than blocking mic.
  esp_websocket_client_send_bin(handle, reinterpret_cast<const char *>(this->mono_buf_.data()),
                                static_cast<int>(this->mono_buf_.size() * sizeof(int16_t)),
                                10 / portTICK_PERIOD_MS);
}

void VaClient::preroll_push_(const int16_t *data, size_t n) {
  if (this->preroll_buf_ == nullptr || this->preroll_capacity_samples_ == 0)
    return;
  const size_t cap = this->preroll_capacity_samples_;
  for (size_t i = 0; i < n; i++) {
    this->preroll_buf_[this->preroll_head_] = data[i];
    if (++this->preroll_head_ >= cap)
      this->preroll_head_ = 0;
    if (this->preroll_count_ < cap)
      this->preroll_count_++;
  }
}

VaClient::Phase VaClient::phase_from_string_(const std::string &phase) {
  if (phase == "listening")
    return Phase::LISTENING;
  if (phase == "thinking")
    return Phase::THINKING;
  if (phase == "replying")
    return Phase::REPLYING;
  return Phase::IDLE;
}

const char *VaClient::phase_name_(Phase p) {
  switch (p) {
    case Phase::LISTENING:
      return "listening";
    case Phase::THINKING:
      return "thinking";
    case Phase::REPLYING:
      return "replying";
    default:
      return "idle";
  }
}

void VaClient::set_phase_(const std::string &phase) {
  // Don't dedupe — we want yaml-side control_leds to re-render even on
  // identical phase if other inputs (e.g. va WS connection state) have
  // changed since the last emission.
  const Phase prev = static_cast<Phase>(this->current_phase_.load());
  this->current_phase_.store(static_cast<uint8_t>(phase_from_string_(phase)));
  ESP_LOGD(TAG, "Phase -> %s (was %s)", phase.c_str(), phase_name_(prev));

  // Lift the post-"stop" incoming-audio suppression once the backend confirms a
  // turn boundary: "idle" (the cancelled reply truly ended) or "listening" (a
  // fresh turn's audio is now legitimate). We deliberately do NOT clear on
  // "thinking"/"replying" — those can still belong to the reply we cancelled.
  if (this->suppress_incoming_audio_ && (phase == "idle" || phase == "listening")) {
    this->suppress_incoming_audio_ = false;
    ESP_LOGI(TAG, "incoming-audio suppression lifted on phase=%s", phase.c_str());
  }

  // Streaming gate state machine:
  //   listening  → mic on (user is being heard)
  //   thinking   → mic stays on. `thinking` only means "the VAD thinks the user
  //                stopped", but with semantic_vad it can flap listening↔thinking
  //                at the start of a turn while the user is still talking. No bot
  //                audio plays during thinking (no echo risk), so keep streaming
  //                until the reply genuinely begins — otherwise a spurious
  //                `thinking` cuts the mic, the backend's input watchdog
  //                force-ends the turn with no transcript, and the turn hangs.
  //   replying   → mic off (bot is speaking; gate to avoid picking up our own
  //                TTS in case the XMOS AEC isn't perfect). barge_in keeps it on.
  //   idle       → mic on for kFollowupMs so the user can answer a question
  //                without re-triggering the wake word. Timer expiry closes
  //                the session.
  if (phase == "listening") {
    if (!this->streaming_) {
      ESP_LOGI(TAG, "phase=listening — mic streaming on");
      this->streaming_ = true;
    }
    // Handsfree barge-in cut-over: a `listening` arriving while we still have
    // TTS queued means the backend's server VAD heard the user talk over the
    // reply and already cancelled the OpenAI response. Drop the audio still in
    // our PSRAM ring so playback stops immediately instead of finishing the
    // now-cancelled sentence. We do NOT send a WS interrupt here — the backend
    // initiated this — we just stop local playback.
    if (this->barge_in_ && this->audio_fill_ > 0) {
      portENTER_CRITICAL(&this->ring_mux_);
      this->audio_head_ = 0;
      this->audio_tail_ = 0;
      this->audio_fill_ = 0;
      portEXIT_CRITICAL(&this->ring_mux_);
      this->idle_emit_pending_ = false;
      ESP_LOGI(TAG, "phase=listening during reply — barge-in, flushed TTS queue");
    }
    if (this->turn_t_listening_ == 0 && this->turn_t_wake_ != 0) {
      this->turn_t_listening_ = millis();
    }
    // Server heard us — watchdog no longer needed.
    this->cancel_timeout("va_no_speech");
    this->cancel_timeout("va_followup");
  } else if (phase == "thinking" || phase == "replying") {
    // Gate the mic off only once the bot actually starts speaking (`replying`).
    // With handsfree barge-in we don't gate at all (the server VAD + XMOS AEC
    // arbitrate talk-over). Crucially we do NOT gate on `thinking`: semantic_vad
    // can flap listening↔thinking at the start of a turn while the user is still
    // speaking, and cutting the mic on those spurious flaps starves the backend
    // of audio → its input watchdog force-ends the turn with no transcript → the
    // turn hangs in thinking. No bot audio plays during thinking, so there's no
    // echo cost to keeping the mic open until the reply genuinely starts.
    if (phase == "replying" && this->streaming_ && !this->barge_in_) {
      ESP_LOGI(TAG, "phase=replying — mic streaming off");
      this->streaming_ = false;
    }
    if (phase == "thinking" && this->turn_t_thinking_ == 0 && this->turn_t_wake_ != 0) {
      this->turn_t_thinking_ = millis();
    }
    this->cancel_timeout("va_followup");
    this->cancel_timeout("va_followup_open");
    this->cancel_timeout("va_tts_tail");
    this->cancel_timeout("va_no_speech");
    this->followup_pending_ = false;
    this->waiting_for_speaker_stop_ = false;
    this->request_follow_up_pending_ = false;
    this->followup_armed_ = false;
    this->idle_emit_pending_ = false;  // new turn began, drop any held idle
  } else if (phase == "idle") {
    // Turn boundary: reset the WS-gap reference so the silence between THIS
    // reply and the NEXT turn's reply (~7 s across a follow-up exchange, where
    // start_session() — the other reset point — is never called) isn't logged
    // as a bogus "ws audio gap". Only intra-reply gaps are real signal.
    this->last_binary_ms_ = 0;
    // Only open a follow-up window if we just finished a real turn —
    // i.e. the previous phase was thinking or replying. Otherwise we'd
    // open the window for every spurious idle (initial WS hello, post-
    // disconnect idle, etc), spamming "follow-up window open" logs and
    // opening the mic for 5s every time the device just reconnects.
    const bool turn_just_ended = prev == Phase::THINKING || prev == Phase::REPLYING;
    if (!turn_just_ended) {
      // Plain idle (boot, reconnect, etc) — no follow-up. Fall through to
      // the regular trigger fire so the LED updates.
      //
      // One plain-idle case DOES need work: idle straight from `listening`
      // means the turn died without a reply — a backend force-idle (rate
      // limit / thinking-watchdog; the backend suppresses `thinking` after
      // declaring a turn dead, so `listening` is exactly where the device
      // sits then) or a WS drop mid-listening. Nothing else ever closes the
      // mic gate in that state: no `replying` follows and the no-speech
      // watchdog was cancelled when `listening` arrived — the mic would
      // stream the room indefinitely (and the backend's mic-resume buffer
      // clear never fires, because the stream never pauses). prev==IDLE is
      // deliberately left alone: that's the open follow-up window
      // (fire_phase_led_ doesn't change current_phase_), and closing the
      // gate there would cut the window short.
      if (prev == Phase::LISTENING && this->streaming_) {
        ESP_LOGI(TAG, "idle from listening — turn died, mic streaming off");
        this->streaming_ = false;
      }
    } else if (this->suppress_followup_) {
      // send_interrupt() set this — user explicitly asked us to stop.
      // Close the session cleanly: streaming off, no follow-up, fall through
      // to the regular trigger fire so the LED goes idle.
      this->suppress_followup_ = false;
      this->streaming_ = false;
      this->followup_pending_ = false;
      this->waiting_for_speaker_stop_ = false;
      this->request_follow_up_pending_ = false;
      this->followup_armed_ = false;
      this->cancel_timeout("va_tts_tail");
      this->idle_emit_pending_ = false;
    } else if (this->audio_fill_ == 0) {
      // Server says response.done and the device has actually played out.
      // Open the follow-up window (mic on so user can answer a question).
      this->open_followup_window_(this->followup_ms_);
      // fall through to fire the trigger normally below (LED -> idle); the
      // follow-up window, if any, fires its own `listening` LED after its delay.
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

  // Belt-and-suspenders barge-in. The yaml wake handler calls send_interrupt()
  // when it observes voice_assistant_phase == replying, but two windows slip
  // past that check:
  //   1) server already sent phase=idle yet PSRAM still has seconds of TTS
  //      queued (idle_emit_pending_). yaml's voice_assistant_phase has been
  //      reset to idle and the wake handler takes the "fresh session" path —
  //      no interrupt — so the new reply overlaps with the tail of the old.
  //   2) wake fires mid-reply on a long answer where the server is still
  //      generating tokens; without an interrupt, OpenAI keeps streaming TTS
  //      we'll never play, burning tokens.
  // The bridge treats interrupt as cheap when there's nothing to cancel
  // (response_cancel_not_active is in its benignCodes set), and
  // input_audio_buffer.clear is safe here because mic frames for the new turn
  // don't start flowing until after this function returns.
  const Phase phase_now = static_cast<Phase>(this->current_phase_.load());
  const bool residual_reply =
      this->audio_fill_ > 0 ||
      this->idle_emit_pending_ ||
      phase_now == Phase::REPLYING ||
      phase_now == Phase::THINKING;
  if (residual_reply) {
    ESP_LOGI(TAG, "start_session: interrupting residual reply (phase=%s, fill=%u)",
             phase_name_(phase_now), (unsigned) this->audio_fill_);
    this->send_interrupt();
  }

  // Discard (do NOT replay) the pre-roll captured before this session. The ring
  // caught the wake chime leaking through the mic during the chime/tail-delay
  // window; replaying it fed the chime back to OpenAI as a phantom "Au!". The
  // mic task does the actual ring reset (its sole owner) when it sees this flag.
  this->preroll_discard_pending_ = true;
  ESP_LOGI(TAG, "start_session() — streaming on");
  this->streaming_ = true;
  // New wake word starts a fresh session — drop any pending or active
  // follow-up window from the previous turn.
  this->followup_pending_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->request_follow_up_pending_ = false;
  this->followup_armed_ = false;
  this->idle_emit_pending_ = false;
  this->suppress_followup_ = false;
  this->cancel_timeout("va_followup");
  this->cancel_timeout("va_followup_open");
  this->cancel_timeout("va_tts_tail");
  // Anchor turn-latency timestamps for the new turn.
  this->turn_t_wake_ = millis();
  this->turn_t_listening_ = 0;
  this->turn_t_thinking_ = 0;
  this->turn_t_first_audio_out_ = 0;
  // Reset audio-quality detectors for this turn.
  this->last_binary_ms_ = 0;
  this->ws_gap_count_ = 0;
  this->ws_gap_max_ms_ = 0;
  this->clipped_samples_ = 0;
  this->underrun_logged_this_turn_ = false;
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
      // Audio-quality summary: only logged if anything anomalous fired.
      // A clean turn produces no line — keeps the noise floor low.
      if (this->ws_gap_count_ > 0 || this->clipped_samples_ > 0 ||
          this->underrun_logged_this_turn_) {
        ESP_LOGW(TAG,
                 "turn audio: ws_gaps=%u (max=%ums) clipped_samples=%u underrun=%s",
                 (unsigned) this->ws_gap_count_,
                 (unsigned) this->ws_gap_max_ms_,
                 (unsigned) this->clipped_samples_,
                 this->underrun_logged_this_turn_ ? "yes" : "no");
      }
      this->turn_t_wake_ = 0;  // mark turn as logged
    }
  }
  if (duration_ms == 0) {
    // Follow-up disabled for this call: turn-based behaviour like the
    // original pipeline. Leave the mic closed; user must say a wake word
    // for the next turn. (The LED idle was already emitted above / by the
    // set_phase_ tail.)
    this->streaming_ = false;
    return;
  }
  // Follow-up dialog window. We do NOT open the mic immediately: has_buffered_
  // data() (the drain signal that got us here) goes false ~500 ms before true
  // silence — there's still the i2s ring + DAC tail playing out. Opening the
  // mic now would let that tail leak back in (XMOS AEC ~10x) and false-trigger
  // the server VAD. So wait followup_open_delay_ms_ (from the backend) for the
  // tail to clear, THEN open the mic and show `listening` so the user can see
  // the device is waiting for them to answer (without a wake word). The LED
  // stays idle (emitted just above) during this short gap. Any new turn / wake /
  // stop / interrupt cancels "va_followup_open" before it fires (see set_phase_,
  // start_session, send_interrupt), so a new turn won't reopen the mic under it.
  const uint32_t open_delay = this->followup_open_delay_ms_;
  ESP_LOGI(TAG, "follow-up: mic opens in %u ms, then listening for %u ms",
           (unsigned) open_delay, (unsigned) duration_ms);
  this->set_timeout("va_followup_open", open_delay, [this, duration_ms]() {
    if (!this->ws_connected_) {
      // The reply drained into a dead connection (WS dropped mid-reply).
      // Opening the mic would show a "listening" LED while on_mic_data_
      // drops every frame — a window the user talks into for nothing.
      // Leave the LED on idle; a fresh wake after reconnect starts clean.
      ESP_LOGI(TAG, "follow-up window skipped — WS disconnected");
      return;
    }
    ESP_LOGI(TAG, "follow-up window open (mic on, listening for %u ms)", (unsigned) duration_ms);
    this->streaming_ = true;
    this->fire_phase_led_("listening");  // blue ring: user may answer now
    this->set_timeout("va_followup", duration_ms, [this]() {
      if (this->streaming_) {
        ESP_LOGI(TAG, "follow-up window expired — mic streaming off");
        this->streaming_ = false;
        this->fire_phase_led_("idle");  // no answer came; back to idle
      }
    });
  });
}

void VaClient::fire_phase_led_(const std::string &phase) {
  // Drive the yaml on_phase automation (LED ring + voice_assistant_phase global)
  // from a device-side timer, not a server message. Marshalled via defer() so it
  // runs on the main loop even if called from another task.
  std::string phase_copy = phase;
  this->defer([this, phase_copy]() {
    for (auto *t : this->phase_triggers_) {
      t->trigger(phase_copy);
    }
  });
}

void VaClient::commit_followup_mic() {
  // Called from yaml's on_followup_opened automation once the chime has
  // finished playing AND the i2s tail has cleared (wait_until + delay).
  // If anything pre-empted us between trigger fire and here (a fresh
  // wake word, a Stop, send_interrupt, or a new turn starting) the
  // armed flag was cleared — silently no-op so we don't reopen the mic
  // out of nowhere.
  if (!this->followup_armed_) {
    ESP_LOGD(TAG, "commit_followup_mic: not armed, ignoring");
    return;
  }
  this->followup_armed_ = false;
  ESP_LOGI(TAG, "follow-up mic armed by yaml (window %u ms)",
           (unsigned) kRequestFollowUpMs);
  // Discard the pre-roll: the request_follow_up chime just played and leaked
  // into the ring; don't replay it to OpenAI. (Same "Au!" guard as the wake
  // path; consumed by the mic task on the next frame.)
  this->preroll_discard_pending_ = true;
  this->streaming_ = true;
  this->set_timeout("va_followup", kRequestFollowUpMs, [this]() {
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
  // "idle" emit waiting for the (now-empty) queue. The ring reset has to
  // happen under the mux: the WS task could be mid-write and seeing
  // head=tail=fill=0 partway through would let it write into a "freshly
  // empty" buffer the user just barge-cancelled.
  size_t flushed_bytes;
  portENTER_CRITICAL(&this->ring_mux_);
  flushed_bytes = this->audio_fill_;
  this->audio_head_ = 0;
  this->audio_tail_ = 0;
  this->audio_fill_ = 0;
  portEXIT_CRITICAL(&this->ring_mux_);
  // Drop further incoming TTS until the backend confirms the turn boundary —
  // it keeps streaming the rest of the (already-generated) reply otherwise.
  this->suppress_incoming_audio_ = true;
  // Ring was just flushed; re-arm the jitter buffer fresh for the next reply.
  this->playback_priming_ = false;
  // Abandon any in-progress cold-start silence-prime; the next reply will detect
  // cold and re-prime cleanly.
  this->chain_prime_remaining_ = 0;
  // Close the mic gate. An interrupt during the OPEN follow-up window would
  // otherwise leave streaming_ true while the va_followup close-timer gets
  // cancelled just below — mic open + streaming to OpenAI indefinitely, so any
  // later room speech becomes an unprompted turn. Callers that start a fresh
  // turn (start_session) re-open it themselves right after.
  this->streaming_ = false;
  this->followup_pending_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->request_follow_up_pending_ = false;
  this->followup_armed_ = false;
  this->idle_emit_pending_ = false;
  this->cancel_timeout("va_no_speech");
  this->cancel_timeout("va_followup");
  this->cancel_timeout("va_tts_tail");
  // The phase=idle the server is about to send shouldn't open a follow-up
  // mic window — the user said "stop", not "wait for me to keep talking".
  this->suppress_followup_ = true;
  this->cancel_timeout("va_followup_open");
  // Report how much already-buffered TTS we just dropped — i.e. how much of the
  // (burst-complete) reply the user did NOT hear. If stale audio ever bleeds into
  // the next turn, this number + the next "Phase ->" tell the story.
  ESP_LOGI(TAG, "send_interrupt — WS msg sent; flushed %u bytes (~%u ms) of queued TTS, suppressing further audio until next turn",
           (unsigned) flushed_bytes,
           (unsigned) (flushed_bytes / (kPlaybackSampleRate / 1000 * 2)));
}

}  // namespace va_client
}  // namespace esphome
