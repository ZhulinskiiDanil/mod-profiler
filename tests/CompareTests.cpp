// Tests for src/Compare.cpp: a baseline and what changed since
#include "../src/Compare.hpp"
#include "Check.hpp"

#include <cmath>
#include <initializer_list>

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
static bool contains(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }

constexpr uint32_t A = bucket::FIRST_MOD;
constexpr uint32_t B = bucket::FIRST_MOD + 1;
constexpr uint32_t C = bucket::FIRST_MOD + 2;

static ModuleMap sampleMap()
{
  std::vector<MappedModule> modules = {
      {.base = 0x1000, .size = 0x1000, .bucket = bucket::GAME},
      {.base = 0x3000, .size = 0x1000, .bucket = A},
      {.base = 0x4000, .size = 0x1000, .bucket = B},
      {.base = 0x5000, .size = 0x1000, .bucket = C},
  };
  return ModuleMap(std::move(modules), {});
}

static Buckets sampleBuckets()
{
  Buckets buckets;
  buckets.index("zhulis.a", "A");
  buckets.index("zhulis.b", "B");
  buckets.index("zhulis.c", "C");
  return buckets;
}

static Sample stack(std::initializer_list<uintptr_t> frames)
{
  Sample sample;
  for (auto frame : frames)
    sample.frames[sample.depth++] = frame;
  return sample;
}

// 10 frames in a second (100 ms each), 100 samples: `a`, `b` and `c` of them in the mods, the rest the game's
static Window window(int a, int b, int c, int between = 0)
{
  auto map = sampleMap();
  Attribution attribution;
  // The game's 0x1100 is the loop, 0x1200 the frame: a sample in the loop alone is between frames
  attribution.finish();
  Window window;
  window.seconds = 1;
  window.frames = 10;
  auto add = [&](int count, uintptr_t address)
  {
    for (int i = 0; i < count; i++)
      window.profile.add(stack({address, 0x1010}), map, &attribution);
  };
  add(a, 0x3010);
  add(b, 0x4010);
  add(c, 0x5010);
  add(100 - a - b - c - between, 0x1010);
  // Waiting: a sample in a wait stub
  Attribution waiting;
  waiting.waits = {{0x1F00, 0x1F20}};
  waiting.finish();
  for (int i = 0; i < between; i++)
    window.profile.add(stack({0x1F10}), map, &waiting);
  return window;
}

static void testBusy()
{
  CHECK(near(busyMillis(window(0, 0, 0)), 100));
  CHECK(near(busyMillis(window(0, 0, 0, 25)), 75));
  CHECK(near(busyMillis(Window{}), 0));
}

static void testMakeBaseline()
{
  auto baseline = makeBaseline(window(20, 10, 0, 50), sampleBuckets(), "12:00:00");
  CHECK(!baseline.empty());
  CHECK(baseline.saved == "12:00:00");
  CHECK(near(baseline.fps, 10));
  CHECK(near(baseline.frameMillis, 100));
  CHECK(near(baseline.busyMillis, 50));
  CHECK(baseline.mods.size() == 2);
  CHECK(near(baseline.mods["zhulis.a"].ownMillis, 20));
  CHECK(baseline.mods["zhulis.a"].name == "A");
  CHECK(near(baseline.mods["zhulis.b"].inclusivePercent, 10));
  CHECK(Baseline{}.empty());
}

static void testComparison()
{
  auto buckets = sampleBuckets();
  auto baseline = makeBaseline(window(20, 10, 0), buckets, "12:00:00");
  // A got 5 ms more, B is gone, C is new
  auto line = comparisonLine(window(25, 0, 3), buckets, baseline, 5);
  CHECK(contains(line, "vs the baseline of 12:00:00: frame 100.00 ms (+0.00 ms), work 100.00 ms (+0.00 ms)"));
  CHECK(contains(line, "own: B gone, was 10.00 ms, A +5.00 ms, C new, 3.00 ms"));
  CHECK(contains(comparisonLine(window(20, 10, 0), buckets, baseline, 5), "own: no change"));
  CHECK(comparisonLine(window(1, 1, 1), buckets, Baseline{}, 5) == "no baseline");
}

int main()
{
  testBusy();
  testMakeBaseline();
  testComparison();
  return finish("compare");
}
