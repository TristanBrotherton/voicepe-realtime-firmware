// Host check for esphome/components/va_client/ws_rx.h (no ESP-IDF needed):
//   c++ -std=c++17 -Wall -Wextra -Werror -I esphome/components/va_client \
//       tools/ci/test_ws_rx.cpp -o build/test_ws_rx && build/test_ws_rx
//
// esp_websocket_client delivers a frame in as many pieces as TCP reads
// return. These cases split frames at every position and check that text is
// parsed whole and that every PCM byte arrives, in order, as whole samples.

#include "ws_rx.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using esphome::va_client::PcmAligner;
using esphome::va_client::WsTextAssembler;

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

int main() {
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
  std::printf("ws_rx: all checks passed\n");
  return EXIT_SUCCESS;
}
