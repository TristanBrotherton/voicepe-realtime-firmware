#pragma once

// Reassembly of WebSocket frames that esp_websocket_client delivers in
// pieces. The client posts one WEBSOCKET_EVENT_DATA per transport read, and a
// read returns whatever TCP has delivered so far, so one frame can arrive as
// several pieces of any length (payload_offset / payload_len say where each
// piece sits in its frame). Two consequences this file handles:
//
// - A text frame split across reads must be parsed whole. Parsing each piece
//   on its own lost the message: a `utt_end` cut in two never ended its
//   utterance (no `done` ever), and a phase or `interrupt_ack` could vanish.
// - A binary frame split at an odd byte splits a 16-bit sample. Dropping the
//   odd byte cost two bytes per split (the device reported `fed` a few bytes
//   short of `bytes`) and shifted every sample of the next piece by one byte,
//   which plays as a burst of noise.
//
// No ESP-IDF dependencies: tools/ci/test_ws_rx.cpp checks this on the host.

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
namespace va_client {

// Collects the pieces of one text frame.
class WsTextAssembler {
 public:
  explicit WsTextAssembler(size_t max_bytes) : max_bytes_(max_bytes) {}

  // One piece: `offset` is its position in the frame, `total` the frame's
  // payload length. Returns true when `out` holds the whole frame. A frame
  // larger than max_bytes, or one with a piece missing, is dropped
  // (dropped() then counts it) rather than parsed in part.
  bool feed(const char *data, size_t len, size_t offset, size_t total, std::string &out) {
    if (offset == 0) {
      this->buf_.clear();
      this->broken_ = false;
      if (len >= total) {
        // The usual case: the whole frame in one piece.
        out.assign(data, len);
        return true;
      }
    } else if (this->broken_) {
      return this->finish_broken_(offset, len, total);
    } else if (offset != this->buf_.size()) {
      // A piece went missing (or this continues a frame we never saw start).
      this->broken_ = true;
      this->buf_.clear();
      return this->finish_broken_(offset, len, total);
    }
    if (total > this->max_bytes_) {
      this->broken_ = true;
      this->buf_.clear();
      return this->finish_broken_(offset, len, total);
    }
    this->buf_.append(data, len);
    if (offset + len < total)
      return false;
    out.swap(this->buf_);
    this->buf_.clear();
    return true;
  }

  void reset() {
    this->buf_.clear();
    this->broken_ = false;
  }

  uint32_t dropped() const { return this->dropped_; }

 private:
  bool finish_broken_(size_t offset, size_t len, size_t total) {
    if (offset + len >= total) {
      this->broken_ = false;
      this->dropped_++;
    }
    return false;
  }

  std::string buf_;
  size_t max_bytes_;
  bool broken_{false};
  uint32_t dropped_{0};
};

// Keeps 16-bit little-endian samples whole across the pieces of binary
// frames: an odd trailing byte waits for the next piece.
class PcmAligner {
 public:
  // Calls emit(int16_t) for every whole sample in (carried byte + data) and
  // keeps a trailing odd byte. `frame_start`: this piece begins a frame, so
  // a byte still carried from the previous frame (an odd-length frame, which
  // the protocol never sends) is discarded instead of joined to it. Returns
  // the number of samples emitted. Reads bytes, never an unaligned int16.
  template<typename Emit> size_t feed(const uint8_t *data, size_t len, bool frame_start, Emit emit) {
    if (frame_start && this->have_) {
      this->have_ = false;
      this->discarded_++;
    }
    size_t n = 0;
    size_t i = 0;
    if (this->have_ && len > 0) {
      emit(static_cast<int16_t>(static_cast<uint16_t>(this->carry_ | (static_cast<uint16_t>(data[0]) << 8))));
      this->have_ = false;
      i = 1;
      n++;
    }
    for (; i + 1 < len; i += 2) {
      emit(static_cast<int16_t>(static_cast<uint16_t>(data[i] | (static_cast<uint16_t>(data[i + 1]) << 8))));
      n++;
    }
    if (i < len) {
      this->carry_ = data[i];
      this->have_ = true;
    }
    return n;
  }

  void reset() { this->have_ = false; }
  bool carrying() const { return this->have_; }
  uint32_t discarded() const { return this->discarded_; }

 private:
  uint8_t carry_{0};
  bool have_{false};
  uint32_t discarded_{0};
};

}  // namespace va_client
}  // namespace esphome
