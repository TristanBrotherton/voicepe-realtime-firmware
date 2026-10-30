// Host check for esphome/components/va_client/ws_rx.h and ws_disc.h (no ESP-IDF needed):
//   c++ -std=c++17 -Wall -Wextra -Werror -I esphome/components/va_client
//       tools/ci/test_ws_rx.cpp -o build/test_ws_rx && build/test_ws_rx
// (as the Realtime firmware workflow runs it)
//
// esp_websocket_client delivers a frame in as many pieces as TCP reads
// return. These cases split frames at every position and check that text is
// parsed whole and that every PCM byte arrives, in order, as whole samples.
// The ws_disc cases check what the device reports about a lost connection.

#include "ws_disc.h"
#include "ws_rx.h"

#include <cstring>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using esphome::va_client::PcmAligner;
using esphome::va_client::WsFailWhere;
using esphome::va_client::WsTextAssembler;
using esphome::va_client::ws_disconnect_why;
using esphome::va_client::ws_error_codes_valid;
using esphome::va_client::ws_fail_where;
using esphome::va_client::ws_fail_where_name;

static int failures = 0;

#define CHECK(cond, ...)                         \
  do {                                           \
    if (!(cond)) {                               \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);         \
      std::fprintf(stderr, "\n");                \
      failures++;                                \
    }                                            \
  } while (0)

// Feeds `frame` to the assembler in pieces cut at `cuts` (offsets, ascending).
static std::vector<std::string> feed_text(WsTextAssembler &rx, const std::string &frame,
                                          const std::vector<size_t> &cuts) {
  std::vector<std::string> got;
  size_t start = 0;
  std::vector<size_t> ends(cuts);
  ends.push_back(frame.size());
  for (size_t end : ends) {
    std::string out;
    if (rx.feed(frame.data() + start, end - start, start, frame.size(), out))
      got.push_back(out);
    start = end;
  }
  return got;
}

static void text_split_anywhere() {
  const std::string frame = "{\"type\":\"utt_end\",\"id\":45,\"bytes\":43200}";
  WsTextAssembler rx(16 * 1024);
  for (size_t cut = 1; cut < frame.size(); cut++) {
    auto got = feed_text(rx, frame, {cut});
    CHECK(got.size() == 1 && got[0] == frame, "split at %zu: %zu messages", cut, got.size());
  }
  for (size_t a = 1; a < frame.size(); a++)
    for (size_t b = a + 1; b < frame.size(); b++) {
      auto got = feed_text(rx, frame, {a, b});
      CHECK(got.size() == 1 && got[0] == frame, "split at %zu,%zu", a, b);
    }
  auto whole = feed_text(rx, frame, {});
  CHECK(whole.size() == 1 && whole[0] == frame, "a whole frame passes through");
}

static void text_back_to_back() {
  // Pieces of consecutive frames never mix.
  WsTextAssembler rx(16 * 1024);
  const std::string a = "{\"type\":\"phase\",\"value\":\"replying\"}";
  const std::string b = "{\"type\":\"utt_start\",\"id\":46,\"kind\":\"result\",\"turn\":\"t\"}";
  auto got_a = feed_text(rx, a, {10});
  auto got_b = feed_text(rx, b, {3, 30});
  CHECK(got_a.size() == 1 && got_a[0] == a, "first frame");
  CHECK(got_b.size() == 1 && got_b[0] == b, "second frame");
}

static void text_missing_piece_and_oversize() {
  WsTextAssembler rx(64);
  const std::string frame = "{\"type\":\"utt_end\",\"id\":7,\"bytes\":4800}";
  std::string out;
  CHECK(!rx.feed(frame.data(), 10, 0, frame.size(), out), "first piece alone is not a message");
  // The piece at 10..20 is lost; 20..end arrives.
  CHECK(!rx.feed(frame.data() + 20, frame.size() - 20, 20, frame.size(), out),
        "a frame with a hole is never parsed");
  CHECK(rx.dropped() == 1, "and it is counted");
  auto next = feed_text(rx, frame, {5});
  CHECK(next.size() == 1 && next[0] == frame, "the next frame is whole again");
  std::string big(100, 'x');
  auto got = feed_text(rx, big, {40, 80});
  CHECK(got.empty() && rx.dropped() == 2, "an oversize frame is dropped");
  auto after = feed_text(rx, frame, {});
  CHECK(after.size() == 1 && after[0] == frame, "and the next one passes");
}

// Feeds a binary frame in pieces; returns the bytes of the samples emitted.
static std::vector<uint8_t> feed_pcm(PcmAligner &rx, const std::vector<uint8_t> &frame,
                                     const std::vector<size_t> &cuts) {
  std::vector<uint8_t> got;
  size_t start = 0;
  std::vector<size_t> ends(cuts);
  ends.push_back(frame.size());
  for (size_t end : ends) {
    rx.feed(frame.data() + start, end - start, start == 0, [&](int16_t s) {
      got.push_back(static_cast<uint8_t>(static_cast<uint16_t>(s) & 0xff));
      got.push_back(static_cast<uint8_t>(static_cast<uint16_t>(s) >> 8));
    });
    start = end;
  }
  return got;
}

static std::vector<uint8_t> pcm_frame(size_t bytes, unsigned seed) {
  std::vector<uint8_t> f(bytes);
  for (size_t i = 0; i < bytes; i++)
    f[i] = static_cast<uint8_t>((i * 37 + seed * 101 + (i >> 3)) & 0xff);
  return f;
}

static void pcm_split_anywhere() {
  // A 4800-byte frame (100 ms) cut at odd and even offsets: every byte comes
  // out, in order, as whole samples (fed == bytes; no byte-shifted noise).
  PcmAligner rx;
  const auto frame = pcm_frame(4800, 1);
  for (size_t cut = 1; cut < 64; cut++) {
    auto got = feed_pcm(rx, frame, {cut, 1460 + cut, 2921, 4799});
    CHECK(got == frame, "cuts at %zu: %zu of %zu bytes", cut, got.size(), frame.size());
  }
  for (size_t cut = 1; cut < frame.size(); cut += 97) {
    auto got = feed_pcm(rx, frame, {cut});
    CHECK(got == frame, "cut at %zu", cut);
  }
  auto ones = std::vector<size_t>();
  for (size_t i = 1; i < 31; i++)
    ones.push_back(i);
  auto small = pcm_frame(32, 2);
  CHECK(feed_pcm(rx, small, ones) == small, "one byte at a time");
}

static void pcm_utterance_total() {
  // 57600 bytes in 4800-byte frames, each split at an odd position: the
  // total that reaches the ring equals what was sent (the device used to
  // report 57596 of 57600).
  PcmAligner rx;
  size_t total = 0;
  for (unsigned k = 0; k < 12; k++) {
    auto frame = pcm_frame(4800, k);
    auto got = feed_pcm(rx, frame, {1023 + 2 * k, 3001});
    CHECK(got == frame, "frame %u", k);
    total += got.size();
  }
  CHECK(total == 57600, "total %zu", total);
}

static void pcm_frame_start_discards_a_dangling_byte() {
  // An odd-length frame (never sent by the add-on) must not shift the next.
  PcmAligner rx;
  std::vector<uint8_t> odd = {1, 2, 3};
  feed_pcm(rx, odd, {});
  CHECK(rx.carrying(), "the odd byte waits");
  const auto frame = pcm_frame(8, 3);
  CHECK(feed_pcm(rx, frame, {3}) == frame, "the next frame starts aligned");
  CHECK(rx.discarded() == 1, "the dangling byte is counted");
}

static WsFailWhere where_of(const char *msg) { return ws_fail_where(msg, std::strlen(msg)); }

static void disc_where_from_client_error_text() {
  // The texts esp_websocket_client 1.7.0 posts with WEBSOCKET_EVENT_ERROR.
  CHECK(where_of("esp_transport_write() returned 0, transport_error=ESP_OK, tls_error_code=0, tls_flags=0, "
                 "errno=0") == WsFailWhere::WRITE,
        "write");
  CHECK(where_of("esp_transport_read() failed with -1, transport_error=ESP_OK, tls_error_code=0, tls_flags=0, "
                 "errno=104") == WsFailWhere::READ,
        "read");
  CHECK(where_of("esp_transport_poll_read() returned -1, errno=113") == WsFailWhere::POLL, "poll");
  CHECK(where_of("Error, no PONG received for more than 120 seconds after PING") == WsFailWhere::PONG, "pong");
  CHECK(where_of("something else") == WsFailWhere::OTHER, "other");
  CHECK(ws_fail_where(nullptr, 0) == WsFailWhere::NONE, "no text");
  // Not NUL-terminated: only data_len bytes count.
  const char cut[] = "esp_transport_write() returned 0";
  CHECK(ws_fail_where(cut, 9) == WsFailWhere::OTHER, "length respected");
  CHECK(std::strcmp(ws_fail_where_name(WsFailWhere::WRITE), "write") == 0, "write name");
  CHECK(ws_fail_where_name(WsFailWhere::NONE) == nullptr, "no name for none");
}

static void disc_why_values() {
  // A write that timed out is a tx_timeout even though the client reports it
  // from inside the send call, before the sender could note its failure.
  CHECK(std::strcmp(ws_disconnect_why(true, false, WsFailWhere::WRITE, false), "tx_timeout") == 0, "write");
  CHECK(std::strcmp(ws_disconnect_why(true, false, WsFailWhere::NONE, true), "tx_timeout") == 0, "tx fail");
  CHECK(std::strcmp(ws_disconnect_why(true, false, WsFailWhere::READ, false), "ws_error") == 0, "read");
  CHECK(std::strcmp(ws_disconnect_why(true, true, WsFailWhere::NONE, false), "ws_closed") == 0, "closed");
  CHECK(std::strcmp(ws_disconnect_why(false, false, WsFailWhere::WRITE, true), "wifi") == 0, "wifi first");
  // Only the values the add-on accepts.
  const char *allowed[] = {"ws_error", "ws_closed", "wifi", "tx_timeout"};
  for (int wifi = 0; wifi < 2; wifi++)
    for (int closed = 0; closed < 2; closed++)
      for (int w = 0; w <= static_cast<int>(WsFailWhere::OTHER); w++)
        for (int tx = 0; tx < 2; tx++) {
          const char *why = ws_disconnect_why(wifi, closed, static_cast<WsFailWhere>(w), tx);
          bool ok = false;
          for (const char *a : allowed)
            ok = ok || std::strcmp(why, a) == 0;
          CHECK(ok, "unexpected why %s", why);
        }
}

static void disc_error_codes_only_from_disconnected() {
  // The ERROR event's codes are uninitialised stack (a device reported
  // errno 1008406854); only a TCP transport DISCONNECTED carries them.
  CHECK(!ws_error_codes_valid(true, true), "never from ERROR");
  CHECK(!ws_error_codes_valid(false, false), "not without a transport error");
  CHECK(ws_error_codes_valid(false, true), "DISCONNECTED after a transport error");
}

int main() {
  disc_where_from_client_error_text();
  disc_why_values();
  disc_error_codes_only_from_disconnected();
  text_split_anywhere();
  text_back_to_back();
  text_missing_piece_and_oversize();
  pcm_split_anywhere();
  pcm_utterance_total();
  pcm_frame_start_discards_a_dangling_byte();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("ws_rx, ws_disc: all checks passed\n");
  return EXIT_SUCCESS;
}
