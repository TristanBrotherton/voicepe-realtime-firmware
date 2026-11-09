#pragma once

// When utterance playback holds audio back to build a cushion, and how the
// cushion adapts to a link that keeps dropping out.
//
// A cushion can only be built by waiting: the add-on streams a live reply at
// about real time, so audio played the moment it arrives has nothing behind
// it, and a Wi-Fi gap longer than what is buffered on the device becomes a
// gap in the speech. Two places hold:
//   - the start of an utterance on a cold chain (hello utt_cushion_ms), and
//   - an utterance that ran dry for an audible gap mid-way ("rebuffer"):
//     once audio comes back, it is held until a cushion is buffered again,
//     so the next gap does not cut the speech straight away.
// A hold ends as soon as the cushion is buffered, the utterance has ended
// (everything there is to play is here), or nothing new arrived for the
// stall time (the source paused: play what there is). A safety cap bounds
// a hold on a source that trickles in slower than real time.
//
// No ESP-IDF dependencies: tools/ci/test_utt_gate.cpp checks this on the host.

#include <cstdint>

namespace esphome {
namespace va_client {

// 24 kHz mono 16-bit PCM.
inline uint64_t pcm_ms_to_bytes(uint32_t ms) { return static_cast<uint64_t>(ms) * 48u; }

// True while playback should keep holding.
//   buffered       bytes waiting in the ring for this utterance (and after it)
//   target_ms      cushion to build; 0 = no hold
//   ended          utt_end received: every byte of the utterance is here
//   since_rx_ms    time since the last PCM arrived
//   stall_ms       nothing new for this long ends the hold
//   since_hold_ms  time since the hold had its first byte
//   max_hold_ms    safety cap on the hold
inline bool playback_hold(uint64_t buffered, uint32_t target_ms, bool ended, uint32_t since_rx_ms,
                          uint32_t stall_ms, uint32_t since_hold_ms, uint32_t max_hold_ms) {
  if (target_ms == 0 || ended)
    return false;
  if (buffered >= pcm_ms_to_bytes(target_ms))
    return false;
  if (since_rx_ms >= stall_ms)
    return false;
  if (since_hold_ms >= max_hold_ms)
    return false;
  return true;
}

// Extra cushion after audible gaps, for the rest of a connection: each gap
// adds step_ms (up to max_ms); each decay_ms without one gives a step back.
// step_ms 0 = off (extra_ms() stays 0).
class CushionAdapt {
 public:
  void configure(uint32_t step_ms, uint32_t max_ms, uint32_t decay_ms) {
    this->step_ms_ = step_ms;
    this->max_ms_ = max_ms;
    this->decay_ms_ = decay_ms;
    if (this->extra_ms_ > max_ms)
      this->extra_ms_ = max_ms;
  }
  void reset() {
    this->extra_ms_ = 0;
    this->last_change_ms_ = 0;
  }
  void on_gap(uint32_t now_ms) {
    if (this->step_ms_ == 0)
      return;
    const uint32_t room = this->max_ms_ > this->extra_ms_ ? this->max_ms_ - this->extra_ms_ : 0;
    this->extra_ms_ += this->step_ms_ < room ? this->step_ms_ : room;
    this->last_change_ms_ = now_ms;
  }
  void tick(uint32_t now_ms) {
    if (this->extra_ms_ == 0 || this->decay_ms_ == 0)
      return;
    if (now_ms - this->last_change_ms_ < this->decay_ms_)
      return;
    this->extra_ms_ = this->extra_ms_ > this->step_ms_ ? this->extra_ms_ - this->step_ms_ : 0;
    this->last_change_ms_ = now_ms;
  }
  uint32_t extra_ms() const { return this->extra_ms_; }

 private:
  uint32_t step_ms_{0};
  uint32_t max_ms_{0};
  uint32_t decay_ms_{0};
  uint32_t extra_ms_{0};
  uint32_t last_change_ms_{0};
};

// The cushion a hold builds: the base, plus the adaptive extra, capped.
inline uint32_t hold_target_ms(uint32_t base_ms, uint32_t extra_ms, uint32_t cap_ms) {
  if (base_ms == 0 && extra_ms == 0)
    return 0;
  const uint64_t t = static_cast<uint64_t>(base_ms) + extra_ms;
  return t > cap_ms ? cap_ms : static_cast<uint32_t>(t);
}

// Time since `then` (millis), 0 when `then` is unset or a hair in the future
// (another task stamped it after `now` was read).
inline uint32_t elapsed_ms(uint32_t now_ms, uint32_t then_ms) {
  if (then_ms == 0)
    return 0;
  const int32_t d = static_cast<int32_t>(now_ms - then_ms);
  return d > 0 ? static_cast<uint32_t>(d) : 0;
}

}  // namespace va_client
}  // namespace esphome
