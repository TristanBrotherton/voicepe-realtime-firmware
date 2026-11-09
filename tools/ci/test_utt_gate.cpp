// Host check for esphome/components/va_client/utt_gate.h (no ESP-IDF needed):
//   c++ -std=c++17 -Wall -Wextra -Werror -I esphome/components/va_client
//       tools/ci/test_utt_gate.cpp -o build/test_utt_gate && build/test_utt_gate
// (as the Realtime firmware workflow runs it)
//
// The playback hold rules (when a start cushion or a rebuffer after a gap
// ends) and the adaptive cushion's counters, including a simulated
// real-time source: a cushion must be able to build past 300 ms, which the
// old wall-clock start deadline did not allow.

#include "utt_gate.h"

#include <cstdio>

using esphome::va_client::CushionAdapt;
using esphome::va_client::elapsed_ms;
using esphome::va_client::hold_target_ms;
using esphome::va_client::pcm_ms_to_bytes;
using esphome::va_client::playback_hold;

static int failures = 0;

#define CHECK(cond, ...)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);                        \
      std::fprintf(stderr, "\n");                               \
      failures++;                                               \
    }                                                           \
  } while (0)

static void hold_rules() {
  const uint64_t b100 = pcm_ms_to_bytes(100);
  CHECK(pcm_ms_to_bytes(1) == 48, "24 kHz mono 16-bit is 48 bytes/ms");
  // No target: never hold (the default start, utt_cushion_ms 0).
  CHECK(!playback_hold(0, 0, false, 0, 300, 0, 1000), "target 0 holds");
  // Below the target, audio still arriving: hold.
  CHECK(playback_hold(b100, 600, false, 20, 300, 100, 1600), "should hold below target");
  // Target reached: play.
  CHECK(!playback_hold(pcm_ms_to_bytes(600), 600, false, 20, 300, 600, 1600), "target reached holds");
  // Utterance ended: everything is here, play.
  CHECK(!playback_hold(b100, 600, true, 20, 300, 100, 1600), "ended utterance holds");
  // Nothing new for the stall time: play what there is.
  CHECK(!playback_hold(b100, 600, false, 300, 300, 400, 1600), "stalled source holds");
  CHECK(playback_hold(b100, 600, false, 299, 300, 400, 1600), "just under the stall should hold");
  // Safety cap.
  CHECK(!playback_hold(b100, 600, false, 20, 300, 1600, 1600), "hold past the cap");
}

static void targets() {
  CHECK(hold_target_ms(0, 0, 2000) == 0, "no base, no extra: no hold");
  CHECK(hold_target_ms(0, 250, 2000) == 250, "extra alone holds");
  CHECK(hold_target_ms(300, 250, 2000) == 550, "base + extra");
  CHECK(hold_target_ms(1900, 500, 2000) == 2000, "capped");
  CHECK(hold_target_ms(0xFFFFFFFFu, 0xFFFFFFFFu, 2000) == 2000, "no overflow");
}

static void elapsed() {
  CHECK(elapsed_ms(1000, 0) == 0, "unset stamp");
  CHECK(elapsed_ms(1000, 400) == 600, "plain");
  CHECK(elapsed_ms(1000, 1003) == 0, "stamp a hair ahead");
  CHECK(elapsed_ms(5, 0xFFFFFFF0u) == 21, "millis wrap");
}

static void adapt() {
  CushionAdapt a;
  a.configure(0, 1000, 60000);
  a.on_gap(1000);
  CHECK(a.extra_ms() == 0, "step 0 is off, got %u", (unsigned) a.extra_ms());

  a.configure(250, 600, 60000);
  a.on_gap(1000);
  CHECK(a.extra_ms() == 250, "one gap, got %u", (unsigned) a.extra_ms());
  a.on_gap(2000);
  CHECK(a.extra_ms() == 500, "two gaps, got %u", (unsigned) a.extra_ms());
  a.on_gap(3000);
  CHECK(a.extra_ms() == 600, "capped at max, got %u", (unsigned) a.extra_ms());
  a.on_gap(4000);
  CHECK(a.extra_ms() == 600, "stays at max, got %u", (unsigned) a.extra_ms());
  // Decay: a step back per minute without a gap, counted from the last change.
  a.tick(4000 + 59999);
  CHECK(a.extra_ms() == 600, "no decay before a minute, got %u", (unsigned) a.extra_ms());
  a.tick(4000 + 60000);
  CHECK(a.extra_ms() == 350, "one step back, got %u", (unsigned) a.extra_ms());
  a.tick(4000 + 60000 + 30000);
  CHECK(a.extra_ms() == 350, "half a minute later, got %u", (unsigned) a.extra_ms());
  a.tick(4000 + 120000);
  CHECK(a.extra_ms() == 100, "two steps back, got %u", (unsigned) a.extra_ms());
  a.tick(4000 + 180000);
  CHECK(a.extra_ms() == 0, "floor at 0, got %u", (unsigned) a.extra_ms());
  a.tick(4000 + 240000);
  CHECK(a.extra_ms() == 0, "stays at 0, got %u", (unsigned) a.extra_ms());
  // A gap during the decay restarts the minute.
  a.on_gap(300000);
  a.tick(300000 + 30000);
  a.on_gap(300000 + 30000);
  a.tick(300000 + 60000);
  CHECK(a.extra_ms() == 500, "gap restarts the minute, got %u", (unsigned) a.extra_ms());
  a.reset();
  CHECK(a.extra_ms() == 0, "reset (new connection)");
  // Lowering max clamps.
  a.on_gap(1);
  a.on_gap(2);
  a.configure(250, 300, 60000);
  CHECK(a.extra_ms() == 300, "configure clamps, got %u", (unsigned) a.extra_ms());
}

// A source at real time (one 100 ms frame every 100 ms) and a cold start
// with a 600 ms cushion: playback starts once 600 ms is buffered, about
// 600 ms after the first frame. The old rule started at 300 ms whatever.
static void realtime_source_builds_cushion() {
  const uint32_t target = 600, stall = 300;
  uint64_t buffered = 0;
  uint32_t last_rx = 0, first_rx = 0;
  int started_at = -1;
  for (uint32_t t = 0; t <= 3000; t += 10) {
    if (t % 100 == 0) {
      buffered += pcm_ms_to_bytes(100);
      last_rx = t == 0 ? 1 : t;  // millis 0 is "unset"
      if (first_rx == 0)
        first_rx = last_rx;
    }
    if (!playback_hold(buffered, target, false, elapsed_ms(t, last_rx), stall, elapsed_ms(t, first_rx),
                       target + 1000)) {
      started_at = static_cast<int>(t);
      break;
    }
  }
  CHECK(started_at >= 500 && started_at <= 600, "real-time source started at %d ms", started_at);
  CHECK(buffered >= pcm_ms_to_bytes(target), "cushion at start: %u ms",
        (unsigned) (buffered / pcm_ms_to_bytes(1)));

  // Same source, but it stops after 200 ms: the hold ends one stall later.
  buffered = 0;
  last_rx = first_rx = 0;
  started_at = -1;
  for (uint32_t t = 0; t <= 3000; t += 10) {
    if (t % 100 == 0 && t < 200) {
      buffered += pcm_ms_to_bytes(100);
      last_rx = t == 0 ? 1 : t;
      if (first_rx == 0)
        first_rx = last_rx;
    }
    if (!playback_hold(buffered, target, false, elapsed_ms(t, last_rx), stall, elapsed_ms(t, first_rx),
                       target + 1000)) {
      started_at = static_cast<int>(t);
      break;
    }
  }
  CHECK(started_at == 400, "paused source started at %d ms (want 100 + 300)", started_at);
}

int main() {
  hold_rules();
  targets();
  elapsed();
  adapt();
  realtime_source_builds_cushion();
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("utt_gate: all checks passed\n");
  return 0;
}
