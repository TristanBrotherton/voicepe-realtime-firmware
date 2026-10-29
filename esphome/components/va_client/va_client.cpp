#include "va_client.h"
#include "automation.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/network/util.h"

#include <algorithm>
#include <cstring>

#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/base64.h>

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
// Keep identifiers that go into JSON strings to a safe character set (they come
// from substitutions and backend messages, never from free text).
static std::string json_safe(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
        c == '-' || c == '.' || c == ':')
      out.push_back(c);
  }
  return out;
}

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

static bool parse_string_after_key(const std::string &msg, const char *key, std::string &out) {
  size_t p = msg.find(key);
  if (p == std::string::npos)
    return false;
  p += std::strlen(key);
  const size_t end = msg.find('"', p);
  if (end == std::string::npos || end == p)
    return false;
  out = json_safe(msg.substr(p, end - p));
  return !out.empty();
}

void VaClient::setup() {
  ESP_LOGCONFIG(TAG, "Setting up VA Client...");

  // Mic -> WebSocket hand-off (see kMicTxBytes): a PSRAM byte ring and its
  // sender task.
  this->mic_tx_rb_ = xRingbufferCreateWithCaps(kMicTxBytes, RINGBUF_TYPE_BYTEBUF,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (this->mic_tx_rb_ == nullptr ||
      xTaskCreate(&VaClient::mic_tx_task_fn_, "va_mic_tx", 4096, this, 5, &this->mic_tx_task_) != pdPASS) {
    ESP_LOGE(TAG, "Failed to start the mic sender; mic frames will be sent from the mic task");
  }
  // Text -> WebSocket hand-off (see kTextTxBytes): one FIFO for every text
  // frame and its sender task.
  this->text_tx_rb_ = xRingbufferCreateWithCaps(kTextTxBytes, RINGBUF_TYPE_NOSPLIT,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (this->text_tx_rb_ == nullptr ||
      xTaskCreate(&VaClient::text_tx_task_fn_, "va_text_tx", 4096, this, 5, &this->text_tx_task_) != pdPASS) {
    this->text_tx_task_ = nullptr;
    ESP_LOGE(TAG, "Failed to start the text sender; text frames will be sent directly");
  }
  this->disc_reset_reason_ = static_cast<int>(esp_reset_reason());

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

  // Pre-wake trigger snapshot buffer (opt-in false-wake review), PSRAM.
  if (this->preroll_capacity_samples_ > 0) {
    this->snapshot_buf_ = static_cast<int16_t *>(
        heap_caps_malloc(this->preroll_capacity_samples_ * sizeof(int16_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    this->trigger_msg_buf_ = static_cast<char *>(heap_caps_malloc(kTriggerMsgBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  // Turn ids are "<boot id>-<counter>": unique across reboots, no clock needed.
  this->boot_id_ = random_uint32();

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
  // Utterance framing: state transitions, acks and progress ticks. Inert
  // (no descriptors) unless the add-on sends utt_start.
  if (this->speaker_ != nullptr && this->audio_buf_ != nullptr && this->utt_count_ > 0)
    this->utt_service_(millis(), !this->speaker_->has_buffered_data());
  if (this->utt_discarding_.load() && millis() - this->utt_discard_since_ms_ > kUttDiscardMaxMs) {
    this->utt_discarding_ = false;
    ESP_LOGW(TAG, "no interrupt_ack for seq %u after %u ms: accepting audio again",
             (unsigned) this->utt_discard_seq_.load(), (unsigned) kUttDiscardMaxMs);
  }
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
    if (fill > 0 && this->utt_mode_.load()) {
      // Utterance framing: the per-utterance start rule (cold prime from
      // hello, optional cushion, nothing back to back) replaces the
      // once-per-turn prime and the prebuffer below.
      if (this->utt_start_gate_(millis()))
        return;
    } else if (fill > 0) {
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
      // needless prime on an already-warm chain is harmless (kChainPrimeMs of silence); both
      // signals are only ever true at a real cold reply-start, never mid-speech. The
      // real audio waits safely in PSRAM (and builds a small cushion) until priming done.
      {
        const uint32_t now_ms = millis();
        // Startup-only fallback. A long tool pause can exceed kChainColdMs
        // without stopping the resampler; re-priming after real reply audio
        // has started would insert a fabricated gap mid-sentence.
        const bool resampler_cold = !this->reply_audio_started_this_turn_ &&
                                    (this->speaker_->is_stopped() ||
                                     this->last_fed_ms_ == 0 ||
                                     (now_ms - this->last_fed_ms_) > kChainColdMs);
        if (this->chain_prime_remaining_ == 0 && resampler_cold) {
          this->chain_prime_remaining_ =
              (size_t) kChainPrimeMs * (kPlaybackSampleRate / 1000) * 2;  // ms→bytes (mono 16-bit)
          ESP_LOGD(TAG, "resampler cold — priming %u bytes of silence before reply",
                   (unsigned) this->chain_prime_remaining_);
        }
        // Hold real-audio drain until the chain is warmed. Real audio stays in
        // PSRAM. Re-enter loop() next tick to continue/finish priming.
        if (this->feed_prime_(now_ms))
          return;
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
    }
    if (fill > 0) {
      // Detector 3: downstream underrun. If the resampler/mixer/i2s chain
      // ran out of bytes to play while we *still* have PSRAM queued,
      // something hiccupped downstream — the user hears silence or a
      // brief stuck-sample glitch. Log the first occurrence per reply so
      // we know whether bad audio in a turn correlates with this.
      if (!this->underrun_logged_this_turn_ && !this->speaker_->has_buffered_data()) {
        // At reply startup the resampler/mixer counters can already be empty
        // while the cold-prime silence is sitting farther downstream in the
        // I2S leaf buffer. That is not an audible underrun. Only flag a dry
        // chain after real reply audio has started.
        if (this->reply_audio_started_this_turn_) {
          ESP_LOGW(TAG, "downstream underrun: %u bytes queued in PSRAM but speaker chain is dry",
                   (unsigned) fill);
          this->underrun_logged_this_turn_ = true;
        }
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
        // Echo-guard anchor. Utterance framing anchors it at each utterance's
        // first fed byte (utt_service_); without it, a stretch starts after a
        // feeding pause of kAudioStretchGapMs.
        if (!this->utt_mode_.load() &&
            (this->last_reply_fed_ms_ == 0 || this->last_fed_ms_ - this->last_reply_fed_ms_ >= kAudioStretchGapMs))
          this->audible_stretch_start_ms_ = this->last_fed_ms_;
        this->last_reply_fed_ms_ = this->last_fed_ms_;
        this->reply_audio_started_this_turn_ = true;
        this->reply_chain_dry_ = false;
        // First real reply sample accepted by the speaker chain this turn (after
        // any cold-start prime and jitter prebuffer): the device's best view of
        // "first audible" (the I2S/DAC tail adds a further fixed delay). Only
        // real reply audio reaches this path.
        if (this->reply_t_first_audible_ == 0 && this->reply_t_first_audio_ != 0)
          this->reply_t_first_audible_ = this->last_fed_ms_;
        portENTER_CRITICAL(&this->ring_mux_);
        this->audio_head_ = (this->audio_head_ + accepted) % kAudioBufBytes;
        this->audio_fill_ -= accepted;
        this->ring_out_total_ += accepted;
        portEXIT_CRITICAL(&this->ring_mux_);
        static uint32_t dbg_last = 0;
        uint32_t now = millis();
        if (now - dbg_last >= 500) {
          ESP_LOGD(TAG, "drained %u bytes (%u still queued)", (unsigned) accepted,
                   (unsigned) (fill - accepted));
          dbg_last = now;
        }
      }
    } else {
      // Do not inject silence here. ESPHome's I2S speaker keeps the DMA stream
      // clocked and zero-pads at the actual sink. Feeding zero PCM at this
      // producer boundary can run ahead of wall time and interleave silence
      // with real speech that is still buffered in the resampler/mixer/I2S
      // pipeline. Observe a genuinely dry resampler/mixer interval instead so
      // the backend can correlate it with audible faults without altering PCM.
      const Phase phase_now = static_cast<Phase>(this->current_phase_.load());
      const bool reply_active = phase_now == Phase::REPLYING &&
                                this->reply_audio_started_this_turn_ &&
                                !this->suppress_incoming_audio_ && !this->post_stop_guard_;
      const bool chain_dry = reply_active && !this->speaker_->has_buffered_data();
      if (chain_dry && !this->reply_chain_dry_) {
        this->reply_chain_dry_ = true;
        this->reply_chain_dry_events_this_turn_++;
        ESP_LOGW(TAG, "reply chain dry while awaiting more PCM (event %u)",
                 (unsigned) this->reply_chain_dry_events_this_turn_);
      } else if (!chain_dry) {
        this->reply_chain_dry_ = false;
      }
    }
  }
  if (this->snapshot_ready_ && !this->snapshot_wake_turn_.empty() && !this->streaming_ &&
      !this->snapshot_send_pending_) {
    this->snapshot_ready_ = false;
    this->snapshot_turn_ = this->snapshot_wake_turn_;
    this->snapshot_wake_turn_.clear();
    this->snapshot_sent_samples_ = 0;
    this->snapshot_send_pending_ = true;
    ESP_LOGD(TAG, "trigger snippet queued (%u samples)", (unsigned) this->snapshot_samples_);
  }
  if (this->snapshot_send_pending_)
    this->send_trigger_chunks_();

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
    // Already initialised; just (re)start. A synchronous start failure must
    // reschedule — otherwise the reconnect chain stalls silently and the
    // device stays offline until a reboot.
    esp_err_t err = esp_websocket_client_start(static_cast<esp_websocket_client_handle_t>(this->ws_handle_));
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_websocket_client_start (restart) failed: %d — rescheduling", (int) err);
      this->schedule_reconnect_();
    }
    return;
  }

  esp_websocket_client_config_t cfg = {};
  cfg.uri = this->url_.c_str();
  if (!this->token_.empty()) {
    // The add-on's device_token. Kept in a member: the client reads it
    // during every (re)connect handshake.
    this->auth_header_ = "Authorization: Bearer " + this->token_ + "\r\n";
    cfg.headers = this->auth_header_.c_str();
  }
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
      // New connection generation first: text frames still queued for the
      // previous connection are dropped by the sender, never sent on this one.
      this->conn_gen_.fetch_add(1);
      this->connected_at_ms_ = millis();
      // Utterance framing is per connection. Descriptors still playing from
      // the previous connection keep playing but are never acked; the last
      // one without a `done` is reported as prev_utt.
      this->utt_mode_ = false;
      this->utt_discarding_ = false;
      this->interrupt_seq_ = 0;
      {
        bool have = false;
        uint32_t id = 0, fed = 0, bytes = 0;
        UttState st = UttState::DONE;
        portENTER_CRITICAL(&this->ring_mux_);
        for (size_t i = this->utt_count_; i > 0; i--) {
          Utt *u = this->utt_at_(i - 1);
          if (u->orphan)
            continue;
          if (u->st == UttState::QUEUED || u->st == UttState::PLAYING) {
            const uint64_t avail = u->ended ? u->end_off : this->ring_in_total_;
            const uint64_t out = std::min(this->ring_out_total_, avail);
            have = true;
            id = u->id;
            fed = out > u->start_off ? static_cast<uint32_t>(out - u->start_off) : 0;
            bytes = u->ended ? u->declared : static_cast<uint32_t>(avail - u->start_off);
            st = u->st;
          }
          break;  // only the newest utterance of the previous connection counts
        }
        if (!have && this->last_utt_valid_ && !this->last_utt_reported_) {
          have = true;
          id = this->last_utt_id_;
          fed = this->last_utt_fed_;
          bytes = this->last_utt_bytes_;
          st = this->last_utt_st_;
        }
        for (size_t i = 0; i < this->utt_count_; i++)
          this->utt_at_(i)->orphan = true;
        this->open_utt_ = -1;
        this->last_utt_valid_ = false;
        this->last_utt_reported_ = true;
        portEXIT_CRITICAL(&this->ring_mux_);
        this->prev_utt_json_.clear();
        if (have) {
          this->prev_utt_json_ = "{\"id\":" + std::to_string(id) + ",\"fed\":" + std::to_string(fed) +
                                 ",\"bytes\":" + std::to_string(bytes) + ",\"st\":\"" + utt_state_name_(st) +
                                 "\"}";
        }
      }
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

      // Every connection starts with `start`: capabilities, firmware, why the
      // previous connection ended and the utterance it left unfinished.
      const std::string start_msg = this->build_start_msg_();
      this->send_text_(start_msg);
      ESP_LOGI(TAG, "sent %s", start_msg.c_str());
      this->set_phase_("idle");
      this->defer([this]() { this->flush_queued_flags_(); });
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
        // Record why this live connection ended, for the next `start`.
        const uint32_t now = millis();
        const uint32_t tx_fail = this->last_tx_fail_ms_.load();
        int err_type = 0;
        this->disc_esp_err_ = 0;
        this->disc_errno_ = 0;
        if (data != nullptr) {
          err_type = static_cast<int>(data->error_handle.error_type);
          this->disc_esp_err_ = static_cast<int>(data->error_handle.esp_tls_last_esp_err);
          this->disc_errno_ = data->error_handle.esp_transport_sock_errno;
        }
        if (tx_fail != 0 && now - tx_fail < 3000) {
          this->disc_why_ = "tx_timeout";
        } else if (!network::is_connected()) {
          this->disc_why_ = "wifi";
        } else if (event_id == WEBSOCKET_EVENT_CLOSED || err_type == WEBSOCKET_ERROR_TYPE_SERVER_CLOSE) {
          this->disc_why_ = "ws_closed";
        } else {
          this->disc_why_ = "ws_error";
        }
        this->disc_up_s_ = (now - this->connected_at_ms_) / 1000;
        this->disc_up_known_ = true;
        this->disc_at_ms_ = now;
        this->disc_pending_ = true;
        ESP_LOGW(TAG, "WS disconnected (event %d, %s, error type %d, esp_err %d, errno %d, up %u s)",
                 (int) event_id, this->disc_why_, err_type, this->disc_esp_err_, this->disc_errno_,
                 (unsigned) this->disc_up_s_);
      }
      this->ws_connected_ = false;
      // A dropped WS mid-enrollment must not leave the mic pinned open with
      // nobody recording — exit enrollment locally (no notify; link is gone).
      if (this->enroll_mode_)
        this->defer([this]() { this->enroll_stop(false); });
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
    if (msg.find("\"audible\":true") != std::string::npos) {
      // A turn the user started failed and the backend could not explain it
      // out loud: give audible feedback (user-initiated, so never at random).
      this->defer([this]() {
        for (auto *t : this->audible_error_triggers_) {
          t->trigger();
        }
      });
    }
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
    if (parse_uint_after_key(msg, "\"wake_open_delay_ms\":", v)) {
      if (v > kFollowupOpenDelayMaxMs)  // reuse the same 5 s ceiling
        v = kFollowupOpenDelayMaxMs;
      this->wake_open_delay_ms_ = v;
      ESP_LOGI(TAG, "hello: wake mic-open delay = %u ms", (unsigned) v);
    }
    if (parse_uint_after_key(msg, "\"trigger_capture\":", v)) {
      this->backend_trigger_capture_ = (v != 0);
      ESP_LOGI(TAG, "hello: backend %s trigger capture", v ? "accepts" : "does not accept");
    }
    if (parse_uint_after_key(msg, "\"playback_prebuffer_ms\":", v)) {
      if (v > kPlaybackPrebufferMaxMs)
        v = kPlaybackPrebufferMaxMs;
      this->playback_prebuffer_ms_ = v;
      ESP_LOGI(TAG, "hello: playback prebuffer (jitter buffer) = %u ms (%s)", (unsigned) v,
               v == 0 ? "disabled" : "cushion before playback");
    }
    // Per-utterance start rule (used only with utterance framing; legacy
    // playback keeps kChainPrimeMs and the prebuffer above).
    if (parse_uint_after_key(msg, "\"chain_prime_ms\":", v)) {
      this->chain_prime_ms_ = std::min(v, kChainPrimeMaxMs);
      ESP_LOGI(TAG, "hello: utterance cold prime = %u ms", (unsigned) this->chain_prime_ms_);
    }
    if (parse_uint_after_key(msg, "\"utt_cushion_ms\":", v)) {
      this->utt_cushion_ms_ = std::min(v, kUttCushionMaxMs);
      ESP_LOGI(TAG, "hello: utterance start cushion = %u ms", (unsigned) this->utt_cushion_ms_);
    }
    return;
  }

  // Utterance framing (utt1). These never carry a "value" key, so they can't
  // reach the phase parser below.
  if (msg.find("\"type\":\"utt_start\"") != std::string::npos) {
    this->handle_utt_text_(msg, true);
    return;
  }
  if (msg.find("\"type\":\"utt_end\"") != std::string::npos) {
    this->handle_utt_text_(msg, false);
    return;
  }
  if (msg.find("\"type\":\"interrupt_ack\"") != std::string::npos) {
    uint32_t seq = 0;
    if (!parse_uint_after_key(msg, "\"seq\":", seq)) {
      ESP_LOGW(TAG, "interrupt_ack without seq ignored");
      return;
    }
    if (this->utt_discarding_.load() && seq >= this->utt_discard_seq_.load()) {
      this->utt_discarding_ = false;
      ESP_LOGI(TAG, "interrupt_ack %u: accepting utterances again", (unsigned) seq);
    } else {
      ESP_LOGD(TAG, "interrupt_ack %u (nothing to release)", (unsigned) seq);
    }
    return;
  }

  if (msg.find("\"type\":\"output_batch_end\"") != std::string::npos) {
    std::string batch_id;
    uint32_t pcm_bytes = 0;
    if (!parse_string_after_key(msg, "\"output_batch_id\":\"", batch_id) ||
        !parse_uint_after_key(msg, "\"pcm_bytes\":", pcm_bytes)) {
      ESP_LOGW(TAG, "invalid output_batch_end marker ignored");
      this->output_batch_id_.clear();
      return;
    }
    this->output_batch_id_ = batch_id;
    ESP_LOGI(TAG, "output batch %s fenced after %u PCM bytes", batch_id.c_str(),
             (unsigned) pcm_bytes);
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

  if (msg.find("\"type\":\"ack\"") != std::string::npos) {
    // Backend confirms mic audio is flowing for this turn. Cancel the
    // no-speech abort: with semantic VAD the "speech detected" phase can
    // arrive only at utterance COMMIT, which for longer commands lands past
    // any reasonable fixed timeout (community report: aborts mid-sentence).
    // Silent misfires still close via the follow-up window flush timers.
    this->cancel_timeout("va_no_speech");
    ESP_LOGD(TAG, "backend audio ack — no-speech watchdog cancelled");
    return;
  }

  if (msg.find("\"type\":\"enroll\"") != std::string::npos) {
    const bool start = msg.find("\"mode\":\"start\"") != std::string::npos;
    ESP_LOGI(TAG, "enroll control from backend: %s", start ? "start" : "stop");
    this->defer([this, start]() {
      if (start)
        this->enroll_start();
      else
        this->enroll_stop(false);
    });
    return;
  }

  // Substring match on `"value":"<phase>"` — keeps us out of a JSON parser
  // until M3 needs richer payloads.
  static const char *const kPhases[] = {"listening", "thinking", "replying", "idle"};
  for (const char *p : kPhases) {
    std::string needle = std::string("\"value\":\"") + p + "\"";
    if (msg.find(needle) != std::string::npos) {
      // A silent (admission-declined) turn ends with "followup":false: close
      // the session instead of opening a follow-up listening window.
      if (std::strcmp(p, "idle") == 0 && msg.find("\"followup\":false") != std::string::npos)
        this->next_idle_no_followup_ = true;
      this->set_phase_(p);
      return;
    }
  }
}

bool VaClient::stop_is_reply_echo(uint32_t min_play_ms) const {
  if (this->audible_stretch_start_ms_ == 0)
    return false;
  const uint32_t now = millis();
  return now - this->last_reply_fed_ms_ < kEchoTailMs &&
         now - this->audible_stretch_start_ms_ < min_play_ms;
}

void VaClient::handle_binary_(const uint8_t *data, size_t len) {
  if (this->speaker_ == nullptr || len < 2 || this->audio_buf_ == nullptr)
    return;
  // After a "stop"/barge-in (send_interrupt) the backend is still streaming the
  // rest of the already-generated reply. Drop it so the cancelled reply goes
  // silent instead of refilling the PSRAM ring we just flushed. Cleared on the
  // next "idle"/"listening" phase (see set_phase_).
  // Utterance framing: everything between a device interrupt and its
  // interrupt_ack is dropped (the interrupt epoch). Legacy: drop until the
  // next `listening` phase.
  if (this->utt_mode_.load()) {
    if (this->utt_discarding_.load())
      return;
  } else if (this->suppress_incoming_audio_) {
    return;
  }
  const uint32_t now_ms = millis();
  if (this->turn_t_first_audio_out_ == 0 && this->turn_t_wake_ != 0) {
    this->turn_t_first_audio_out_ = now_ms;
  }
  if (this->reply_t_first_audio_ == 0 && this->reply_t_anchor_ != 0)
    this->reply_t_first_audio_ = now_ms;
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
  if (this->utt_discarding_.load()) {
    // An interrupt landed while this frame was being scaled: drop it.
    portEXIT_CRITICAL(&this->ring_mux_);
    return;
  }
  if (this->open_utt_ >= 0 && this->utts_[this->open_utt_].first_rx_ms == 0)
    this->utts_[this->open_utt_].first_rx_ms = now_ms;
  const bool was_empty = (this->audio_fill_ == 0);
  size_t tail = this->audio_tail_;
  size_t first = std::min(len, kAudioBufBytes - tail);
  std::memcpy(this->audio_buf_ + tail, data, first);
  if (first < len) {
    std::memcpy(this->audio_buf_, data + first, len - first);
  }
  this->audio_tail_ = (tail + len) % kAudioBufBytes;
  this->audio_fill_ += len;
  this->ring_in_total_ += len;
  portEXIT_CRITICAL(&this->ring_mux_);
  // Jitter buffer: arm priming only when the ring was empty AND the downstream
  // chain is dry — i.e. a true reply start or a real underflow. Mid-reply the
  // ring routinely flips empty (loop() drains each WS clump on arrival) while
  // the downstream chain still holds ~600 ms of audio; re-arming there did
  // nothing but spam "prebuffer ready" every ~50 ms and could hold a small
  // trailing chunk for the full prebuffer deadline. has_buffered_data() is a
  // counter read, safe enough from the WS task. Only when enabled.
  if (was_empty && !this->utt_mode_.load() && !this->reply_audio_started_this_turn_ &&
      this->playback_prebuffer_ms_ > 0 && !this->playback_priming_ &&
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
  // Copy the ring (oldest first) at the moment of detection: it holds the
  // sound that triggered the wake. Done before the streaming gate because the
  // session may already be open by the next mic frame; the ring is only
  // discarded further down. Mic task is the ring's only writer.
  if (this->snapshot_requested_ && this->snapshot_buf_ != nullptr && !this->snapshot_ready_) {
    const size_t cap = this->preroll_capacity_samples_;
    const size_t count = this->preroll_count_;
    const size_t start = (this->preroll_head_ + cap - count) % cap;
    for (size_t i = 0; i < count; i++)
      this->snapshot_buf_[i] = this->preroll_buf_[(start + i) % cap];
    this->snapshot_samples_ = count;
    this->snapshot_requested_ = false;
    this->snapshot_ready_ = true;
  }
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

  const size_t bytes = this->mono_buf_.size() * sizeof(int16_t);
  if (this->mic_tx_task_ != nullptr) {
    // Queue only: the mic task (shared with wake word detection) never waits
    // on the network. A full ring (over 1 s behind) drops this frame.
    if (xRingbufferSend(this->mic_tx_rb_, this->mono_buf_.data(), bytes, 0) != pdTRUE)
      this->mic_tx_dropped_.fetch_add(1);
    return;
  }
  // Fallback without the sender task: the old direct send.
  esp_websocket_client_send_bin(handle, reinterpret_cast<const char *>(this->mono_buf_.data()),
                                static_cast<int>(bytes), 10 / portTICK_PERIOD_MS);
}

void VaClient::mic_tx_task_fn_(void *arg) { static_cast<VaClient *>(arg)->mic_tx_loop_(); }

void VaClient::mic_tx_loop_() {
  uint32_t last_warn_ms = 0;
  for (;;) {
    size_t len = 0;
    void *item = xRingbufferReceiveUpTo(this->mic_tx_rb_, &len, pdMS_TO_TICKS(200), kMicTxMaxChunk);
    const uint32_t dropped = this->mic_tx_dropped_.exchange(0);
    if (dropped != 0 && millis() - last_warn_ms > 1000) {
      ESP_LOGW(TAG, "mic sender over 1 s behind: dropped %u mic frames", (unsigned) dropped);
      last_warn_ms = millis();
    }
    if (item == nullptr)
      continue;
    // Text frames queued before this mic frame go first: a turn's `wake` and
    // `interrupt` were written synchronously before its first mic frame and
    // the add-on may rely on that order. Bounded wait (about 100 ms).
    for (int i = 0; i < 50 && this->text_tx_task_ != nullptr; i++) {
      UBaseType_t waiting = 0;
      vRingbufferGetInfo(this->text_tx_rb_, nullptr, nullptr, nullptr, nullptr, &waiting);
      if (waiting == 0 && !this->text_tx_sending_.load())
        break;
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
    if (this->ws_connected_ && handle != nullptr) {
      const uint32_t t0 = millis();
      const int sent = esp_websocket_client_send_bin(handle, static_cast<const char *>(item),
                                                     static_cast<int>(len),
                                                     pdMS_TO_TICKS(kMicTxSendTimeoutMs));
      const uint32_t took = millis() - t0;
      if (sent < 0) {
        this->last_tx_fail_ms_ = millis();
        ESP_LOGW(TAG, "mic send failed after %u ms (%u bytes)", (unsigned) took, (unsigned) len);
      } else if (took > 50) {
        ESP_LOGD(TAG, "mic send stalled %u ms (rode it out)", (unsigned) took);
      }
    }
    // Not connected: the frame is discarded (a reconnect starts a new turn).
    vRingbufferReturnItem(this->mic_tx_rb_, item);
  }
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
  if (phase == "enrolling")
    return Phase::ENROLLING;
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
  // Enrollment mode: the mic is pinned open and the backend's phase machine is
  // not driving this device (mic audio is not forwarded to OpenAI). Stray
  // phases — notably the hourly proactive-refresh force-idle — must not close
  // the mic or repaint the LED mid-session.
  if (this->enroll_mode_ && phase != "enrolling") {
    ESP_LOGD(TAG, "ignoring phase '%s' during enrollment", phase.c_str());
    return;
  }
  // Don't dedupe — we want yaml-side control_leds to re-render even on
  // identical phase if other inputs (e.g. va WS connection state) have
  // changed since the last emission.
  const Phase prev = static_cast<Phase>(this->current_phase_.load());
  this->current_phase_.store(static_cast<uint8_t>(phase_from_string_(phase)));
  if (phase != "replying")
    this->reply_chain_dry_ = false;
  ESP_LOGD(TAG, "Phase -> %s", phase.c_str());

  // Post-stop `thinking` guard. After a local "stop" the mic gate is closed
  // (send_interrupt set post_stop_guard_), so no new turn can begin until a
  // wake (start_session clears it). A `thinking` arriving here is the server
  // VAD's end-of-turn for the utterance we just cancelled — acting on it
  // strands the LED in `thinking` until the backend's 15 s watchdog (observed
  // 2026-06-14: "stop" mid-question -> 15 s stuck thinking). Ignore it, keeping
  // the prior phase. Scoped to `thinking`: a web search's replying->thinking
  // has no stop (guard stays false), and a reply-drain emits no `thinking`.
  if (this->post_stop_guard_ && phase == "thinking") {
    this->current_phase_.store(static_cast<uint8_t>(prev));  // don't advance to the stale value
    ESP_LOGI(TAG, "ignoring stale 'thinking' after stop (no wake since)");
    return;
  }

  // Lift the post-"stop" incoming-audio suppression ONLY on "listening" — a
  // genuine fresh user turn whose reply is legitimate. We deliberately do NOT
  // lift on "idle": the backend keeps streaming the cancelled reply's audio in
  // real-time (OpenAI generated it faster than playback; a `response.cancel`
  // after the response already finished is a no-op), so it can drain for many
  // seconds AFTER the stop. An "idle" can arrive mid-drain — notably the
  // backend's thinking-watchdog force-idle — and lifting suppression there
  // un-mutes the still-arriving tail, which then plays as an "answer out of
  // nowhere" (observed live 2026-06-13 10:11). A real next reply is always
  // preceded by "listening" (the user speaks), so that stays the only safe
  // gate; until then the tail keeps being dropped. We also do NOT clear on
  // "thinking"/"replying" — those can still belong to the reply we cancelled.
  if (this->suppress_incoming_audio_ && phase == "listening") {
    this->suppress_incoming_audio_ = false;
    ESP_LOGI(TAG, "incoming-audio suppression lifted on phase=listening");
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
      UttEvent ev[kMaxUtts];
      size_t n = 0;
      portENTER_CRITICAL(&this->ring_mux_);
      n = this->utt_flush_locked_(ev, kMaxUtts);
      this->audio_head_ = 0;
      this->audio_tail_ = 0;
      this->audio_fill_ = 0;
      this->ring_out_total_ = this->ring_in_total_;
      portEXIT_CRITICAL(&this->ring_mux_);
      this->send_utt_events_(ev, n);
      this->idle_emit_pending_ = false;
      ESP_LOGI(TAG, "phase=listening during reply — barge-in, flushed TTS queue");
    }
    if (this->turn_t_listening_ == 0 && this->turn_t_wake_ != 0) {
      this->turn_t_listening_ = millis();
    }
    if (this->reply_t_listening_ == 0 && this->reply_t_anchor_ != 0)
      this->reply_t_listening_ = millis();
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
    if (phase == "thinking" && this->reply_t_thinking_ == 0 && this->reply_t_anchor_ != 0)
      this->reply_t_thinking_ = millis();
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
      // An idle that arrives while the mic gate is still OPEN means an orphaned
      // stream that nothing else will close — shut it. Two real ways to reach
      // here with streaming_ still true:
      //   • idle straight from `listening`: the turn died without a reply (a
      //     backend force-idle on rate-limit / thinking-watchdog — the backend
      //     suppresses `thinking` after declaring a turn dead, so `listening`
      //     is exactly where the device sits — or a WS drop mid-listening).
      //   • idle from `idle` (prev==IDLE) with the mic open: the brief post-wake
      //     "waiting for the first phase" window, or an open follow-up window,
      //     when the WS drops (on_ws_event fires set_phase_("idle") on
      //     disconnect). Without this the gate stays open and the mic resumes
      //     streaming the room the instant we reconnect — and the backend's
      //     mic-resume buffer clear can't help because the stream never paused.
      // Closing here can't cut a LIVE follow-up window short: the only idle that
      // reaches an open window is exactly such a disconnect — the backend sends
      // no idle while it's waiting for the user to answer.
      if (this->streaming_) {
        ESP_LOGI(TAG, "idle while mic open (prev=%s) — closing orphaned mic gate",
                 phase_name_(prev));
        this->streaming_ = false;
        this->cancel_timeout("va_no_speech");
      }
    } else if (this->suppress_followup_ || this->next_idle_no_followup_) {
      // send_interrupt() set suppress_followup_ — the user asked us to stop —
      // or the backend ended a silent turn ("followup":false).
      if (this->next_idle_no_followup_) {
        this->next_idle_no_followup_ = false;
        ESP_LOGI(TAG, "silent turn ended by backend — no follow-up window");
        this->emit_turn_metrics_("silent");
      }
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
      // Stale-`idle` guard. prev==REPLYING with NO audio played since the last
      // wake (turn_t_first_audio_out_==0) means this `idle` belongs to a reply
      // that was stopped and then superseded by a new wake while it was still
      // draining: start_session() reset turn_t_first_audio_out_ and the old
      // tail is suppressed, so nothing actually played in THIS session. Opening
      // a follow-up here lights a spurious `listening` window over the fresh
      // wake session (observed live 2026-06-14: web-search → "stop" → bare wake
      // → ~4 s `listening` flicker; device log "Phase -> idle (was replying)").
      // Ignore it: the wake's no-speech watchdog is still armed (we never
      // reached `listening`, so it was never cancelled) and owns the session —
      // it idles the LED and closes the mic. current_phase_ is already IDLE
      // (set at the top of set_phase_), so the next message's `prev` is fine.
      // Tight by construction: a reply that really played sets
      // turn_t_first_audio_out_ (suppression lifts on `listening`), and a
      // follow-up chain keeps it set (only start_session resets it), so no
      // legitimate follow-up is skipped; the dead-turn path (prev==THINKING,
      // watchdog already cancelled) is deliberately left untouched.
      if (prev == Phase::REPLYING && this->turn_t_first_audio_out_ == 0) {
        ESP_LOGI(TAG, "stale replying-idle after wake (no audio this session) "
                      "— ignoring, no follow-up");
        // Distinguished by the mic gate: streaming_==true is a BARE WAKE (mic
        // still open, never reached `replying` this session) → the no-speech
        // watchdog is armed and owns the idle + mic close, so leave the LED on
        // the wake state. streaming_==false means the turn DID reach `replying`
        // (mic gated) but the audio never played — e.g. suppress_incoming_audio_
        // was still set from an earlier stop, so turn_t_first_audio_out_ stayed
        // 0. The watchdog was cancelled when the turn reached `listening`, so
        // nothing else fires idle → fall through to the LED trigger below, else
        // the ring strands on `replying` (observed live 2026-06-14: rapid stops
        // left suppression on, the search reply was dropped, the LED hung).
        if (this->streaming_) {
          return;  // bare wake: va_no_speech owns the idle + mic
        }
        // else: fall through to fire the idle LED (still no follow-up).
      } else {
        // Server says response.done and the device has actually played out.
        // Open the follow-up window (mic on so user can answer a question).
        this->open_followup_window_(this->followup_ms_);
      }
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
  if (this->enroll_mode_) {
    ESP_LOGW(TAG, "wake ignored — enrollment mode active");
    return;
  }
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
    this->send_interrupt("residual");
  }

  // Discard (do NOT replay) the pre-roll captured before this session. The ring
  // caught the wake chime leaking through the mic during the chime/tail-delay
  // window; replaying it fed the chime back to OpenAI as a phantom "Au!". The
  // mic task does the actual ring reset (its sole owner) when it sees this flag.
  this->preroll_discard_pending_ = true;
  // New turn: id + timing anchors (metrics for the previous turn, if still
  // unsent, go out first so every turn reports exactly once).
  if (this->reply_t_anchor_ != 0 && !this->turn_metrics_sent_)
    this->emit_turn_metrics_("superseded");
  this->turn_counter_++;
  char turn_buf[24];
  snprintf(turn_buf, sizeof(turn_buf), "%08x-%u", (unsigned) this->boot_id_, (unsigned) this->turn_counter_);
  this->turn_id_ = turn_buf;
  this->reply_seq_ = 0;
  this->turn_metrics_sent_ = false;
  this->output_batch_id_.clear();
  ESP_LOGI(TAG, "start_session() — streaming on (turn %s)", this->turn_id_.c_str());
  this->streaming_ = true;
  // Tell the backend a fresh wake started (dangling-VAD guard, A). Sent AFTER
  // the residual-reply interrupt above so the backend sees interrupt → wake in
  // order. The first real mic frame for this turn doesn't flow until after this
  // returns, so the guard's "speech since wake" tracker starts clean.
  this->send_wake_();
  // New wake word starts a fresh session — drop any pending or active
  // follow-up window from the previous turn.
  this->followup_pending_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->request_follow_up_pending_ = false;
  this->followup_armed_ = false;
  this->idle_emit_pending_ = false;
  this->suppress_followup_ = false;
  // A genuine new turn starts here — drop the post-stop `thinking` guard so
  // this turn's `thinking` shows normally. (Set last, AFTER the residual-reply
  // send_interrupt() above re-set it, so the wake always ends with it clear.)
  this->post_stop_guard_ = false;
  this->cancel_timeout("va_followup");
  this->cancel_timeout("va_followup_open");
  this->cancel_timeout("va_tts_tail");
  // Anchor turn-latency timestamps for the new turn.
  this->turn_t_wake_ = millis();
  this->turn_t_listening_ = 0;
  this->turn_t_thinking_ = 0;
  this->turn_t_first_audio_out_ = 0;
  this->last_reply_fed_ms_ = 0;
  this->audible_stretch_start_ms_ = 0;
  this->reply_t_anchor_ = this->turn_t_wake_;
  this->reply_t_listening_ = 0;
  this->reply_t_thinking_ = 0;
  this->reply_t_first_audio_ = 0;
  this->reply_t_first_audible_ = 0;
  // Reset audio-quality detectors for this turn.
  this->last_binary_ms_ = 0;
  this->ws_gap_count_ = 0;
  this->ws_gap_max_ms_ = 0;
  this->clipped_samples_ = 0;
  this->underrun_logged_this_turn_ = false;
  this->reply_audio_started_this_turn_ = false;
  this->reply_chain_dry_ = false;
  this->reply_chain_dry_events_this_turn_ = 0;
  // Watchdog: if server doesn't hear us within kNoSpeechTimeoutMs, abort the
  // session so we're not stuck with the mic open after a misfire.
  this->set_timeout("va_no_speech", kNoSpeechTimeoutMs, [this]() {
    ESP_LOGI(TAG, "no speech detected for %u ms — aborting session",
             (unsigned) kNoSpeechTimeoutMs);
    this->emit_turn_metrics_("no_speech");
    if (this->ws_connected_ && this->ws_handle_ != nullptr) {
      uint32_t utt = 0, fed = 0;
      portENTER_CRITICAL(&this->ring_mux_);
      const bool have_utt = this->utt_mode_.load() && this->utt_interrupt_target_locked_(utt, fed);
      portEXIT_CRITICAL(&this->ring_mux_);
      this->send_interrupt_msg_("no_speech", have_utt, utt, fed);
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
  }
  // Per-turn summary + backend metrics. Used to run only when the idle LED
  // had been deferred, so short replies that drained before phase=idle never
  // reported a timeline. Now every reply reports exactly once.
  this->emit_turn_metrics_("reply");
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
    // A reply in this window is the session's next turn: re-anchor its timing.
    this->reply_seq_++;
    this->turn_metrics_sent_ = false;
    this->output_batch_id_.clear();
    this->reply_t_anchor_ = millis();
    this->reply_t_listening_ = 0;
    this->reply_t_thinking_ = 0;
    this->reply_t_first_audio_ = 0;
    this->reply_t_first_audible_ = 0;
    this->reply_chain_dry_ = false;
    this->reply_chain_dry_events_this_turn_ = 0;
    this->fire_ms_ = 0;
    this->fire_phase_led_("listening");  // blue ring: user may answer now
    this->set_timeout("va_followup", duration_ms, [this]() {
      if (this->streaming_) {
        ESP_LOGI(TAG, "follow-up window expired — mic streaming off");
        this->streaming_ = false;
        this->send_mic_flush_();        // drop any uncommitted partial utterance
        this->fire_phase_led_("idle");  // no answer came; back to idle
        // Nobody spoke: there was no turn to time, so report nothing.
        if (this->reply_t_listening_ == 0)
          this->reply_t_anchor_ = 0;
      }
    });
  });
}

void VaClient::enroll_start() {
  if (this->enroll_mode_)
    return;
  ESP_LOGI(TAG, "enrollment START — mic pinned open, session timers suspended");
  this->cancel_timeout("va_no_speech");
  this->cancel_timeout("va_followup");
  this->cancel_timeout("va_followup_open");
  this->cancel_timeout("va_tts_tail");
  this->followup_pending_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->request_follow_up_pending_ = false;
  this->followup_armed_ = false;
  this->idle_emit_pending_ = false;
  this->suppress_followup_ = false;
  this->post_stop_guard_ = false;
  this->suppress_incoming_audio_ = false;
  this->preroll_discard_pending_ = true;
  this->enroll_mode_ = true;
  this->enroll_started_ms_ = millis();
  this->streaming_ = true;
  this->set_phase_("enrolling");
  // Hard cap: a forgotten/hung session must not stream the household forever.
  this->set_timeout("va_enroll_cap", kEnrollMaxMs, [this]() {
    ESP_LOGW(TAG, "enrollment hit the safety cap — stopping");
    this->enroll_stop(true);
  });
}

void VaClient::enroll_stop(bool notify_backend) {
  if (!this->enroll_mode_)
    return;
  ESP_LOGI(TAG, "enrollment STOP (%s) after %u s",
           notify_backend ? "device-initiated" : "backend-initiated",
           (unsigned) ((millis() - this->enroll_started_ms_) / 1000));
  this->cancel_timeout("va_enroll_cap");
  this->enroll_mode_ = false;
  this->streaming_ = false;
  // Don't let the routine idle transition open a follow-up mic window — the
  // session is over, the device should go fully to rest.
  this->suppress_followup_ = true;
  if (notify_backend)
    this->send_text_("{\"type\":\"enroll_stopped\"}");
  this->set_phase_("idle");
}

bool VaClient::send_text_(const std::string &msg) { return this->enqueue_text_(msg.data(), msg.size()); }

bool VaClient::enqueue_text_(const char *data, size_t len) {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr)
    return false;
  if (this->text_tx_task_ == nullptr) {
    // Fallback without the sender task: a direct send, still with a timeout
    // long enough to ride out a TCP stall.
    auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
    const bool ok = esp_websocket_client_send_text(handle, data, static_cast<int>(len),
                                                   pdMS_TO_TICKS(kTextTxSendTimeoutMs)) >= 0;
    if (!ok)
      this->last_tx_fail_ms_ = millis();
    return ok;
  }
  void *item = nullptr;
  if (xRingbufferSendAcquire(this->text_tx_rb_, &item, len + sizeof(uint32_t), 0) != pdTRUE || item == nullptr) {
    this->text_tx_dropped_.fetch_add(1);
    ESP_LOGW(TAG, "text sender queue full: dropped a %u-byte frame", (unsigned) len);
    return false;
  }
  const uint32_t gen = this->conn_gen_.load();
  std::memcpy(item, &gen, sizeof(gen));
  std::memcpy(static_cast<uint8_t *>(item) + sizeof(gen), data, len);
  xRingbufferSendComplete(this->text_tx_rb_, item);
  return true;
}

void VaClient::text_tx_task_fn_(void *arg) { static_cast<VaClient *>(arg)->text_tx_loop_(); }

void VaClient::text_tx_loop_() {
  for (;;) {
    size_t len = 0;
    void *item = xRingbufferReceive(this->text_tx_rb_, &len, pdMS_TO_TICKS(500));
    if (item == nullptr)
      continue;
    this->text_tx_sending_ = true;
    uint32_t gen = 0;
    if (len >= sizeof(gen))
      std::memcpy(&gen, item, sizeof(gen));
    auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
    if (len > sizeof(gen) && gen == this->conn_gen_.load() && this->ws_connected_ && handle != nullptr) {
      const uint32_t t0 = millis();
      const int sent = esp_websocket_client_send_text(handle, static_cast<const char *>(item) + sizeof(gen),
                                                      static_cast<int>(len - sizeof(gen)),
                                                      pdMS_TO_TICKS(kTextTxSendTimeoutMs));
      const uint32_t took = millis() - t0;
      if (sent < 0) {
        this->last_tx_fail_ms_ = millis();
        ESP_LOGW(TAG, "text send failed after %u ms (%u bytes)", (unsigned) took, (unsigned) (len - sizeof(gen)));
      } else if (took > 100) {
        ESP_LOGD(TAG, "text send stalled %u ms (rode it out)", (unsigned) took);
      }
    }
    // Stale (earlier connection) or not connected: dropped.
    vRingbufferReturnItem(this->text_tx_rb_, item);
    this->text_tx_sending_ = false;
  }
}

void VaClient::send_button_cancel() {
  std::string m = "{\"type\":\"button_cancel\",\"turn\":\"" + json_safe(this->turn_id_) + "\"}";
  if (this->send_text_(m))
    ESP_LOGI(TAG, "button cancel sent");
}

void VaClient::send_false_flag() {
  // Names the turn it flags, so the backend labels exactly that wake on this
  // device. Pressed while offline: queued and sent with its age on reconnect.
  std::string m = "{\"type\":\"false_flag\",\"turn\":\"" + json_safe(this->turn_id_) + "\"}";
  if (this->send_text_(m)) {
    ESP_LOGI(TAG, "explicit false-wake flag sent (double-press, turn %s)", this->turn_id_.c_str());
    return;
  }
  if (this->queued_flags_.size() >= kMaxQueuedFlags)
    this->queued_flags_.erase(this->queued_flags_.begin());
  this->queued_flags_.emplace_back(this->turn_id_, millis());
  ESP_LOGW(TAG, "false-wake flag queued (WS down, %u queued)", (unsigned) this->queued_flags_.size());
}

void VaClient::flush_queued_flags_() {
  while (!this->queued_flags_.empty() && this->ws_connected_) {
    const auto &flag = this->queued_flags_.front();
    std::string m = "{\"type\":\"false_flag\",\"turn\":\"" + json_safe(flag.first) +
                    "\",\"age_ms\":" + std::to_string(millis() - flag.second) + "}";
    if (!this->send_text_(m))
      return;
    ESP_LOGI(TAG, "queued false-wake flag delivered (turn %s)", flag.first.c_str());
    this->queued_flags_.erase(this->queued_flags_.begin());
  }
}

void VaClient::note_wake_fired(bool from_button) {
  this->fire_ms_ = millis();
  this->fire_from_button_ = from_button;
  // Every wake invalidates an unsent snapshot (e.g. one taken for a wake that
  // never opened a session), so audio is only ever sent with its own turn.
  this->snapshot_ready_ = false;
  this->snapshot_requested_ = false;
  this->snapshot_wake_turn_.clear();
  this->snapshot_send_pending_ = false;  // a half-sent older snapshot is dropped
  if (!from_button && this->trigger_capture_enabled_ && this->backend_trigger_capture_ &&
      this->snapshot_buf_ != nullptr && !this->streaming_) {
    this->snapshot_requested_ = true;  // consumed by the mic task
  }
}

void VaClient::send_shadow_detection(const std::string &model) {
  std::string m = "{\"type\":\"shadow_detection\",\"model\":\"" + json_safe(model) + "\"}";
  if (this->send_text_(m))
    ESP_LOGI(TAG, "shadow model detection reported (%s)", model.c_str());
}

void VaClient::send_trigger_chunks_() {
  if (!this->ws_connected_) {
    this->snapshot_send_pending_ = false;  // never resend stale audio later
    return;
  }
  char *buf = this->trigger_msg_buf_;
  const std::string turn = json_safe(this->snapshot_turn_);
  if (buf == nullptr || this->ws_handle_ == nullptr || turn.size() > 128) {
    ESP_LOGW(TAG, "trigger snippet skipped (buffer %s, turn id %u chars)", buf ? "ok" : "missing",
             (unsigned) turn.size());
    this->snapshot_send_pending_ = false;  // skip the clip; never crash for it
    return;
  }
  // Two chunks per loop pass keeps the main loop responsive.
  for (int i = 0; i < 2 && this->snapshot_send_pending_; i++) {
    const size_t remaining = this->snapshot_samples_ - this->snapshot_sent_samples_;
    const size_t n = std::min(remaining, kTriggerChunkSamples);
    const bool last = (n == remaining);
    int head = snprintf(buf, kTriggerMsgBytes, "{\"type\":\"trigger_audio\",\"turn\":\"%s\",\"rate\":16000,\"seq\":%u%s,\"b64\":\"",
                        turn.c_str(), (unsigned) (this->snapshot_sent_samples_ / kTriggerChunkSamples),
                        last ? ",\"last\":1" : "");
    size_t b64_len = 0;
    if (head <= 0 || (size_t) head >= kTriggerMsgBytes ||
        mbedtls_base64_encode(reinterpret_cast<unsigned char *>(buf + head), kTriggerMsgBytes - head - 3, &b64_len,
                              reinterpret_cast<const unsigned char *>(this->snapshot_buf_ + this->snapshot_sent_samples_),
                              n * sizeof(int16_t)) != 0) {
      ESP_LOGW(TAG, "trigger snippet skipped (frame did not fit)");
      this->snapshot_send_pending_ = false;
      return;
    }
    size_t len = head + b64_len;
    buf[len++] = '"';
    buf[len++] = '}';
    if (!this->ws_connected_) {
      ESP_LOGW(TAG, "trigger snippet not sent (chunk %u)", (unsigned) (this->snapshot_sent_samples_ / kTriggerChunkSamples));
      this->snapshot_send_pending_ = false;
      return;
    }
    // Through the text sender. A full queue is back-pressure: retry this
    // chunk on a later loop pass (the sender drains at link speed).
    if (this->text_tx_rb_ != nullptr && this->text_tx_task_ != nullptr &&
        xRingbufferGetCurFreeSize(this->text_tx_rb_) < len + 16)
      return;
    if (!this->enqueue_text_(buf, len)) {
      ESP_LOGW(TAG, "trigger snippet not sent (chunk %u)", (unsigned) (this->snapshot_sent_samples_ / kTriggerChunkSamples));
      this->snapshot_send_pending_ = false;
      return;
    }
    this->snapshot_sent_samples_ += n;
    if (last) {
      this->snapshot_send_pending_ = false;
      ESP_LOGI(TAG, "trigger snippet sent (%u ms)", (unsigned) (this->snapshot_samples_ / 16));
    }
  }
}

void VaClient::emit_turn_metrics_(const char *reason) {
  if (this->reply_t_anchor_ == 0 || this->turn_metrics_sent_)
    return;
  this->turn_metrics_sent_ = true;
  const uint32_t now = millis();
  auto span = [](uint32_t from, uint32_t to) -> int32_t {
    return (from == 0 || to == 0 || to < from) ? -1 : static_cast<int32_t>(to - from);
  };
  const int32_t fire_to_mic = (this->fire_ms_ != 0 && this->reply_seq_ == 0) ? span(this->fire_ms_, this->reply_t_anchor_) : -1;
  const int32_t wake_to_listening = span(this->reply_t_anchor_, this->reply_t_listening_);
  const int32_t listening_to_thinking = span(this->reply_t_listening_, this->reply_t_thinking_);
  const int32_t thinking_to_first_audio = span(this->reply_t_thinking_, this->reply_t_first_audio_);
  const int32_t first_audio_to_audible = span(this->reply_t_first_audio_, this->reply_t_first_audible_);
  const int32_t audible_to_drained = span(this->reply_t_first_audible_, now);
  const int32_t total = span(this->reply_t_anchor_, now);
  auto fmt = [](int32_t v) -> std::string { return v < 0 ? "?" : std::to_string(v) + "ms"; };
  ESP_LOGI(TAG,
           "turn %s seq %u (%s): fire→mic=%s wake→listening=%s listening→thinking=%s "
           "thinking→first_audio=%s first_audio→audible=%s audible→drained=%s total=%s",
           this->turn_id_.c_str(), (unsigned) this->reply_seq_, reason, fmt(fire_to_mic).c_str(),
           fmt(wake_to_listening).c_str(), fmt(listening_to_thinking).c_str(),
           fmt(thinking_to_first_audio).c_str(), fmt(first_audio_to_audible).c_str(),
           fmt(audible_to_drained).c_str(), fmt(total).c_str());
  if (this->ws_gap_count_ > 0 || this->clipped_samples_ > 0 || this->underrun_logged_this_turn_ ||
      this->reply_chain_dry_events_this_turn_ > 0) {
    ESP_LOGW(TAG, "turn audio: ws_gaps=%u (max=%ums) clipped_samples=%u underrun=%s dry_chain_events=%u",
             (unsigned) this->ws_gap_count_, (unsigned) this->ws_gap_max_ms_,
             (unsigned) this->clipped_samples_, this->underrun_logged_this_turn_ ? "yes" : "no",
             (unsigned) this->reply_chain_dry_events_this_turn_);
  }
  std::string m = "{\"type\":\"turn_metrics\",\"turn\":\"" + json_safe(this->turn_id_) +
                  "\",\"seq\":" + std::to_string(this->reply_seq_) + ",\"reason\":\"" + reason + "\"";
  auto add = [&m](const char *key, int32_t v) {
    if (v >= 0)
      m += std::string(",\"") + key + "\":" + std::to_string(v);
  };
  add("fire_to_mic_ms", fire_to_mic);
  add("wake_to_listening_ms", wake_to_listening);
  add("listening_to_thinking_ms", listening_to_thinking);
  add("thinking_to_first_audio_ms", thinking_to_first_audio);
  add("first_audio_to_audible_ms", first_audio_to_audible);
  add("audible_to_drained_ms", audible_to_drained);
  add("total_ms", total);
  add("ws_gaps", static_cast<int32_t>(this->ws_gap_count_));
  add("ws_gap_max_ms", static_cast<int32_t>(this->ws_gap_max_ms_));
  add("underrun", this->underrun_logged_this_turn_ ? 1 : 0);
  add("dry_chain_events", static_cast<int32_t>(this->reply_chain_dry_events_this_turn_));
  if (std::strcmp(reason, "reply") == 0 && !this->output_batch_id_.empty())
    m += ",\"output_batch_id\":\"" + json_safe(this->output_batch_id_) + "\"";
  m += "}";
  this->send_text_(m);
  this->output_batch_id_.clear();
}

void VaClient::send_mic_flush_() {
  // The mic gate just closed mid-stream because a follow-up window timed out.
  // If the user had started (but not finished) speaking, that audio sits
  // UNCOMMITTED in OpenAI's input_audio_buffer; left there, a later wake's
  // audio "completes" it and the model answers a stale half-sentence. Drop it
  // NOW, at the cut-off, so no reactive clear-on-wake is needed (that disturbed
  // the server VAD and caused garbage commits). This timer only fires when the
  // user did NOT trigger speech — `listening` cancels va_followup — so it can
  // never drop a valid command. Cheap no-op when the buffer was empty.
  if (this->send_text_("{\"type\":\"flush\"}"))
    ESP_LOGI(TAG, "follow-up window closed — sent flush (drop uncommitted mic audio)");
}

void VaClient::send_wake_() {
  // Tell the backend a fresh wake started (dangling-VAD guard, A). Until the
  // user actually speaks this turn, OpenAI's server VAD can still fire an
  // end-of-turn for a PREVIOUS utterance that never closed (the reply gated the
  // mic mid-sentence) — committing it auto-creates a garbage answer to an empty
  // turn. The backend uses this signal to suppress that stale thinking + cancel
  // the racing response. Sent on every start_session(); old backends ignore it.
  if (this->ws_connected_ && this->ws_handle_ != nullptr) {
    // Protocol v2: turn id + the exact wake operating point. Old backends
    // match only "type":"wake" and ignore the rest.
    std::string msg = "{\"type\":\"wake\",\"v\":2,\"turn\":\"" + json_safe(this->turn_id_) +
                      "\",\"src\":\"" + (this->fire_from_button_ ? "button" : "wake_word") + "\"";
    if (!this->wake_model_.empty()) {
      msg += ",\"model\":\"" + json_safe(this->wake_model_) + "\"";
      msg += ",\"model_sha\":\"" + json_safe(this->wake_model_sha_) + "\"";
      msg += ",\"cutoff_uint8\":" + std::to_string(this->wake_cutoff_);
      msg += ",\"window\":" + std::to_string(this->wake_window_);
      msg += ",\"tier\":\"" + json_safe(this->wake_tier_) + "\"";
    }
    if (this->fire_ms_ != 0 && millis() - this->fire_ms_ < 30000)
      msg += ",\"fire_age_ms\":" + std::to_string(millis() - this->fire_ms_);
    if (!this->firmware_version_.empty())
      msg += ",\"fw\":\"" + json_safe(this->firmware_version_) + "\"";
    msg += "}";
    this->send_text_(msg);
    ESP_LOGI(TAG, "wake — sent %s", msg.c_str());
    if (this->snapshot_requested_ || this->snapshot_ready_)
      this->snapshot_wake_turn_ = this->turn_id_;  // sent by loop() when ready
  }
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
  // The answer to the model's question is the session's next timed reply.
  this->reply_seq_++;
  this->turn_metrics_sent_ = false;
  this->output_batch_id_.clear();
  this->reply_t_anchor_ = millis();
  this->reply_t_listening_ = 0;
  this->reply_t_thinking_ = 0;
  this->reply_t_first_audio_ = 0;
  this->reply_t_first_audible_ = 0;
  this->reply_chain_dry_ = false;
  this->reply_chain_dry_events_this_turn_ = 0;
  this->fire_ms_ = 0;
  this->set_timeout("va_followup", kRequestFollowUpMs, [this]() {
    if (this->streaming_) {
      ESP_LOGI(TAG, "follow-up window expired — mic streaming off");
      this->streaming_ = false;
      this->send_mic_flush_();  // drop any uncommitted partial utterance
      if (this->reply_t_listening_ == 0)
        this->reply_t_anchor_ = 0;
    }
  });
}

void VaClient::send_interrupt(const std::string &reason) {
  if (this->reply_t_anchor_ != 0 && !this->turn_metrics_sent_ && this->reply_t_first_audio_ != 0)
    this->emit_turn_metrics_("interrupted");
  const bool connected = this->ws_connected_ && this->ws_handle_ != nullptr;
  const bool utt = this->utt_mode_.load();
  // Utterance framing: from here until the add-on's interrupt_ack, every
  // utt_* and binary frame is dropped (set before the flush so a frame being
  // written right now can't land in the flushed ring).
  if (utt && connected) {
    this->utt_discard_seq_ = this->interrupt_seq_ + 1;
    this->utt_discard_since_ms_ = millis();
    this->utt_discarding_ = true;
  }
  // Flush our PSRAM playback queue — what's already been pushed into the
  // resampler/mixer/leaf will still drain (~600 ms residual), but everything
  // we have yet to hand off is dropped. The yaml side stops the resampler
  // explicitly. Reset deferred state too so we don't accidentally hold an
  // "idle" emit waiting for the (now-empty) queue. The ring reset has to
  // happen under the mux: the WS task could be mid-write and seeing
  // head=tail=fill=0 partway through would let it write into a "freshly
  // empty" buffer the user just barge-cancelled.
  UttEvent ev[kMaxUtts];
  size_t n = 0;
  uint32_t target_id = 0, target_fed = 0;
  portENTER_CRITICAL(&this->ring_mux_);
  const bool have_target = utt && this->utt_interrupt_target_locked_(target_id, target_fed);
  n = this->utt_flush_locked_(ev, kMaxUtts);
  this->audio_head_ = 0;
  this->audio_tail_ = 0;
  this->audio_fill_ = 0;
  this->ring_out_total_ = this->ring_in_total_;
  portEXIT_CRITICAL(&this->ring_mux_);
  // Best-effort cancel to the backend — ONLY if the socket is alive. The local
  // cleanup must ALWAYS run: returning early on a dead socket (the old
  // behaviour) left streaming_ on, the PSRAM ring full and the follow-up timers
  // armed after a "stop" on a dead link, so the mic would resume streaming the
  // room the instant we reconnected. The interrupt goes out before the acks
  // for the utterances it cut (one FIFO keeps that order).
  if (connected) {
    this->send_interrupt_msg_(reason.c_str(), have_target, target_id, target_fed);
  } else {
    ESP_LOGW(TAG, "send_interrupt: WS not connected — local cleanup only");
  }
  this->send_utt_events_(ev, n);
  // Legacy: drop further incoming TTS until the backend confirms the turn
  // boundary (`listening`) — it keeps streaming the rest of the
  // (already-generated) reply otherwise. Utterance framing uses the
  // interrupt_ack epoch instead.
  if (!utt)
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
  // Mic gate is now closed: no new turn can begin until a wake. Ignore any
  // `thinking` the backend emits in the meantime — it's the server VAD's
  // end-of-turn for the utterance we just cancelled, not a real new turn.
  // Cleared in start_session() (the next wake). See set_phase_.
  this->post_stop_guard_ = true;
  this->cancel_timeout("va_followup_open");
  ESP_LOGI(TAG, "send_interrupt (%s) — WS msg sent, queue flushed", reason.empty() ? "no reason" : reason.c_str());
}

// ---- Start message and interrupts ---------------------------------------------

std::string VaClient::build_start_msg_() {
  std::string m = "{\"type\":\"start\",\"caps\":[\"ireason1\",\"utt1\"]";
  if (!this->firmware_version_.empty())
    m += ",\"fw\":\"" + json_safe(this->firmware_version_) + "\"";
  if (this->disc_pending_) {
    this->disc_pending_ = false;
    m += ",\"last_disconnect\":{\"why\":\"";
    m += this->disc_why_;
    m += "\"";
    if (!this->disc_up_known_) {
      // First connection after a boot: the reset reason (esp_reset_reason_t).
      m += ",\"reset\":" + std::to_string(this->disc_reset_reason_);
    } else {
      m += ",\"esp_err\":" + std::to_string(this->disc_esp_err_);
      m += ",\"errno\":" + std::to_string(this->disc_errno_);
      m += ",\"up_s\":" + std::to_string(this->disc_up_s_);
    }
    m += ",\"ago_s\":" + std::to_string((millis() - this->disc_at_ms_) / 1000) + "}";
  }
  if (!this->prev_utt_json_.empty()) {
    m += ",\"prev_utt\":" + this->prev_utt_json_;
    this->prev_utt_json_.clear();
  }
  m += "}";
  return m;
}

void VaClient::send_interrupt_msg_(const char *reason, bool have_utt, uint32_t utt, uint32_t fed) {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr)
    return;
  const uint32_t seq = ++this->interrupt_seq_;
  std::string m = "{\"type\":\"interrupt\"";
  if (reason != nullptr && reason[0] != '\0')
    m += ",\"reason\":\"" + json_safe(reason) + "\"";
  m += ",\"seq\":" + std::to_string(seq);
  if (have_utt)
    m += ",\"utt\":" + std::to_string(utt) + ",\"fed\":" + std::to_string(fed);
  m += "}";
  if (this->utt_mode_.load()) {
    // Drop utt_* and binary frames until the add-on acks this interrupt.
    this->utt_discard_seq_ = seq;
    this->utt_discard_since_ms_ = millis();
    this->utt_discarding_ = true;
  }
  this->send_text_(m);
  ESP_LOGI(TAG, "interrupt sent: %s", m.c_str());
}

bool VaClient::announce_stop_window() const {
  if (this->announce_playing_)
    return true;
  return this->announce_end_ms_ != 0 && millis() - this->announce_end_ms_ < kAnnounceStopHoldMs;
}

// ---- Utterance framing ----------------------------------------------------------

VaClient::UttKind VaClient::utt_kind_from_(const std::string &k) {
  if (k == "clip")
    return UttKind::CLIP;
  if (k == "ack")
    return UttKind::ACK;
  if (k == "reply")
    return UttKind::REPLY;
  if (k == "result")
    return UttKind::RESULT;
  if (k == "prompt")
    return UttKind::PROMPT;
  if (k == "announce")
    return UttKind::ANNOUNCE;
  if (k == "coach")
    return UttKind::COACH;
  return UttKind::OTHER;
}

const char *VaClient::utt_state_name_(UttState st) {
  switch (st) {
    case UttState::QUEUED:
      return "queued";
    case UttState::PLAYING:
      return "playing";
    case UttState::DONE:
      return "done";
    case UttState::CUT:
      return "cut";
    default:
      return "dropped";
  }
}

void VaClient::handle_utt_text_(const std::string &msg, bool is_start) {
  uint32_t id = 0;
  if (!parse_uint_after_key(msg, "\"id\":", id)) {
    ESP_LOGW(TAG, "%s without id ignored", is_start ? "utt_start" : "utt_end");
    return;
  }
  if (this->utt_discarding_.load()) {
    ESP_LOGD(TAG, "%s %u discarded (awaiting interrupt_ack)", is_start ? "utt_start" : "utt_end", (unsigned) id);
    return;
  }
  if (is_start) {
    std::string kind;
    parse_string_after_key(msg, "\"kind\":\"", kind);
    const UttKind k = utt_kind_from_(kind);
    bool tracked = false;
    bool implicit_end = false;
    portENTER_CRITICAL(&this->ring_mux_);
    if (this->open_utt_ >= 0) {
      Utt &o = this->utts_[this->open_utt_];
      if (!o.ended) {
        o.ended = true;
        o.end_off = this->ring_in_total_;
        o.declared = static_cast<uint32_t>(o.end_off - o.start_off);
        implicit_end = true;
      }
      this->open_utt_ = -1;
    }
    while (this->utt_count_ > 0) {
      const UttState st = this->utt_at_(0)->st;
      if (st != UttState::DONE && st != UttState::CUT && st != UttState::DROPPED)
        break;
      this->utt_first_ = (this->utt_first_ + 1) % kMaxUtts;
      this->utt_count_--;
    }
    if (this->utt_count_ < kMaxUtts) {
      const size_t idx = (this->utt_first_ + this->utt_count_) % kMaxUtts;
      this->utts_[idx] = Utt{};
      this->utts_[idx].id = id;
      this->utts_[idx].kind = k;
      this->utts_[idx].start_off = this->ring_in_total_;
      this->open_utt_ = static_cast<int>(idx);
      this->utt_count_++;
      tracked = true;
    }
    portEXIT_CRITICAL(&this->ring_mux_);
    if (!this->utt_mode_.exchange(true))
      ESP_LOGI(TAG, "utterance framing on for this connection");
    if (implicit_end)
      ESP_LOGW(TAG, "utt_start %u before the previous utt_end: previous closed", (unsigned) id);
    if (tracked) {
      ESP_LOGD(TAG, "utt %u start (%s)", (unsigned) id, kind.c_str());
    } else {
      ESP_LOGW(TAG, "utt %u not tracked: %u utterances queued", (unsigned) id, (unsigned) kMaxUtts);
    }
    return;
  }
  uint32_t bytes = 0;
  parse_uint_after_key(msg, "\"bytes\":", bytes);
  bool found = false;
  uint32_t received = 0;
  portENTER_CRITICAL(&this->ring_mux_);
  if (this->open_utt_ >= 0 && this->utts_[this->open_utt_].id == id) {
    Utt &u = this->utts_[this->open_utt_];
    u.ended = true;
    u.end_off = this->ring_in_total_;
    u.declared = bytes;
    received = static_cast<uint32_t>(u.end_off - u.start_off);
    this->open_utt_ = -1;
    found = true;
  }
  portEXIT_CRITICAL(&this->ring_mux_);
  if (!found) {
    ESP_LOGW(TAG, "utt_end %u does not match the open utterance; ignored", (unsigned) id);
  } else if (received != bytes) {
    ESP_LOGW(TAG, "utt %u end: %u bytes received, %u announced", (unsigned) id, (unsigned) received,
             (unsigned) bytes);
  } else {
    ESP_LOGD(TAG, "utt %u end (%u bytes)", (unsigned) id, (unsigned) bytes);
  }
}

bool VaClient::feed_prime_(uint32_t now_ms) {
  if (this->chain_prime_remaining_ == 0)
    return false;
  static const uint8_t kSilence[480] = {0};  // 10ms @24k mono16; fed in chunks
  size_t want = std::min(this->chain_prime_remaining_, sizeof(kSilence));
  size_t fed = this->speaker_->play(kSilence, want);
  if (fed > 0) {
    this->chain_prime_remaining_ -= fed;
    this->last_fed_ms_ = now_ms;  // count silence as "fed" so cold-check clears
  }
  return true;
}

bool VaClient::utt_start_gate_(uint32_t now_ms) {
  // Prime silence still going out: hold real audio.
  if (this->feed_prime_(now_ms))
    return true;
  // Cold = the resampler was stopped (speaker.stop) or nothing was fed for
  // kChainColdMs. Checked once per utterance, at its first byte: a
  // back-to-back utterance finds the chain still fed and gets no prime.
  const bool chain_cold = this->speaker_->is_stopped() || this->last_fed_ms_ == 0 ||
                          (now_ms - this->last_fed_ms_) > kChainColdMs;
  bool start_prime = false;
  bool hold = false;
  uint32_t id = 0;
  portENTER_CRITICAL(&this->ring_mux_);
  for (size_t i = 0; i < this->utt_count_; i++) {
    Utt *u = this->utt_at_(i);
    if (u->st != UttState::QUEUED)
      continue;
    // Only the first queued utterance can be at the play head.
    if (u->start_off == this->ring_out_total_) {
      if (!u->start_checked) {
        u->start_checked = true;
        u->cold = chain_cold;
        start_prime = chain_cold && this->chain_prime_ms_ > 0;
        id = u->id;
      }
      if (!start_prime && u->cold && this->utt_cushion_ms_ > 0) {
        // Optional start cushion (hello utt_cushion_ms, default 0): start
        // once that much is buffered, or the utterance is complete, or
        // kUttStartDeadlineMs after its first byte.
        const uint64_t avail = u->ended ? u->end_off : this->ring_in_total_;
        const uint64_t want = (uint64_t) this->utt_cushion_ms_ * (kPlaybackSampleRate / 1000) * 2;
        const bool deadline = u->first_rx_ms != 0 && now_ms - u->first_rx_ms >= kUttStartDeadlineMs;
        if (avail - u->start_off < want && !u->ended && !deadline)
          hold = true;
      }
    }
    break;
  }
  portEXIT_CRITICAL(&this->ring_mux_);
  if (start_prime) {
    this->chain_prime_remaining_ = (size_t) this->chain_prime_ms_ * (kPlaybackSampleRate / 1000) * 2;
    ESP_LOGD(TAG, "utt %u: cold chain — priming %u ms of silence", (unsigned) id,
             (unsigned) this->chain_prime_ms_);
    this->feed_prime_(now_ms);
    return true;
  }
  return hold;
}

void VaClient::utt_service_(uint32_t now_ms, bool chain_dry) {
  UttEvent ev[kMaxUtts * 2];
  size_t n = 0;
  bool anchor = false;
  int announce_change = -1;  // -1 none, 1 started, 0 ended
  portENTER_CRITICAL(&this->ring_mux_);
  const uint64_t out = this->ring_out_total_;
  bool any_playing = false;
  for (size_t i = 0; i < this->utt_count_; i++) {
    Utt *u = this->utt_at_(i);
    const bool announce = u->kind == UttKind::ANNOUNCE;
    if (u->st == UttState::QUEUED) {
      const bool empty = u->ended && u->end_off == u->start_off;
      if (!(out > u->start_off || (empty && out >= u->start_off)))
        break;  // not started; later ones can't have either
      u->st = UttState::PLAYING;
      u->last_progress_ms = now_ms;
      if (!empty)
        anchor = true;
      if (announce) {
        this->announce_playing_ = true;
        announce_change = 1;
      }
      if (!u->orphan && n < kMaxUtts * 2) {
        const uint32_t wait = u->first_rx_ms != 0 ? now_ms - u->first_rx_ms : 0;
        ev[n++] = UttEvent{u->id, UttState::PLAYING, false, announce, 0, wait, 0};
      }
    }
    if (u->st == UttState::PLAYING) {
      const uint64_t avail = u->ended ? u->end_off : this->ring_in_total_;
      const uint32_t fed = static_cast<uint32_t>(std::min(out, avail) - u->start_off);
      if (u->ended && out >= u->end_off) {
        u->st = UttState::DONE;
        if (announce) {
          this->announce_playing_ = false;
          this->announce_end_ms_ = now_ms;
          announce_change = 0;
        }
        if (!u->orphan) {
          if (n < kMaxUtts * 2)
            ev[n++] = UttEvent{u->id, UttState::DONE, false, announce, fed, 0, u->und};
          this->last_utt_valid_ = true;
          this->last_utt_id_ = u->id;
          this->last_utt_fed_ = fed;
          this->last_utt_bytes_ = u->declared;
          this->last_utt_st_ = UttState::DONE;
          this->last_utt_reported_ = true;
        }
      } else {
        // Underruns: the chain ran dry while this utterance still had bytes
        // to come (counted once per dry interval).
        if (!any_playing) {
          if (chain_dry && !this->utt_dry_)
            u->und++;
        }
        any_playing = true;
        if (now_ms - u->last_progress_ms >= 1000) {
          u->last_progress_ms = now_ms;
          if (!u->orphan && n < kMaxUtts * 2)
            ev[n++] = UttEvent{u->id, UttState::PLAYING, true, announce, fed, 0, 0};
        }
      }
    }
  }
  this->utt_dry_ = any_playing && chain_dry;
  // Finished descriptors at the front are no longer needed (their events
  // were collected when they finished).
  while (this->utt_count_ > 0) {
    const UttState st = this->utt_at_(0)->st;
    if (st != UttState::DONE && st != UttState::CUT && st != UttState::DROPPED)
      break;
    this->utt_first_ = (this->utt_first_ + 1) % kMaxUtts;
    this->utt_count_--;
  }
  portEXIT_CRITICAL(&this->ring_mux_);
  if (anchor) {
    // Echo guard anchored at this utterance's first fed byte.
    this->audible_stretch_start_ms_ = now_ms;
    this->last_reply_fed_ms_ = now_ms;
  }
  this->send_utt_events_(ev, n);
  if (announce_change >= 0) {
    const bool playing = announce_change == 1;
    ESP_LOGI(TAG, "announcement %s", playing ? "playing: stop armed" : "finished");
    this->defer([this, playing]() {
      for (auto *t : this->announce_triggers_)
        t->trigger(playing);
    });
  }
}

bool VaClient::utt_interrupt_target_locked_(uint32_t &id, uint32_t &fed) {
  for (size_t i = 0; i < this->utt_count_; i++) {
    Utt *u = this->utt_at_(i);
    if (u->orphan || u->st != UttState::PLAYING)
      continue;
    const uint64_t avail = u->ended ? u->end_off : this->ring_in_total_;
    id = u->id;
    fed = static_cast<uint32_t>(std::min(this->ring_out_total_, avail) - u->start_off);
    return true;
  }
  if (this->last_utt_valid_) {
    id = this->last_utt_id_;
    fed = this->last_utt_fed_;
    return true;
  }
  return false;
}

size_t VaClient::utt_flush_locked_(UttEvent *ev, size_t cap) {
  size_t n = 0;
  const uint64_t out = this->ring_out_total_;
  for (size_t i = 0; i < this->utt_count_; i++) {
    Utt *u = this->utt_at_(i);
    if (u->st != UttState::QUEUED && u->st != UttState::PLAYING)
      continue;
    const bool started = u->st == UttState::PLAYING;
    const uint64_t avail = u->ended ? u->end_off : this->ring_in_total_;
    const uint32_t fed = started ? static_cast<uint32_t>(std::min(out, avail) - u->start_off) : 0;
    u->st = started ? UttState::CUT : UttState::DROPPED;
    if (u->kind == UttKind::ANNOUNCE && this->announce_playing_) {
      this->announce_playing_ = false;
      this->announce_end_ms_ = 0;
    }
    if (!u->orphan) {
      if (n < cap)
        ev[n++] = UttEvent{u->id, u->st, false, u->kind == UttKind::ANNOUNCE, fed, 0, u->und};
      this->last_utt_valid_ = true;
      this->last_utt_id_ = u->id;
      this->last_utt_fed_ = fed;
      this->last_utt_bytes_ = u->ended ? u->declared : static_cast<uint32_t>(avail - u->start_off);
      this->last_utt_st_ = u->st;
      this->last_utt_reported_ = false;
    }
  }
  this->open_utt_ = -1;
  this->utt_dry_ = false;
  return n;
}

void VaClient::send_utt_events_(const UttEvent *ev, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const UttEvent &e = ev[i];
    const char *st = e.progress ? "progress" : utt_state_name_(e.st);
    std::string m = "{\"type\":\"utt_ack\",\"id\":" + std::to_string(e.id) + ",\"st\":\"" + st + "\"";
    if (e.st == UttState::PLAYING && !e.progress) {
      m += ",\"fed\":0,\"wait_ms\":" + std::to_string(e.wait_ms);
    } else {
      m += ",\"fed\":" + std::to_string(e.fed);
    }
    if (e.st == UttState::DONE || e.st == UttState::CUT)
      m += ",\"und\":" + std::to_string(e.und);
    m += "}";
    this->send_text_(m);
    if (e.progress) {
      ESP_LOGV(TAG, "%s", m.c_str());
    } else {
      ESP_LOGD(TAG, "%s", m.c_str());
    }
  }
}

}  // namespace va_client
}  // namespace esphome
