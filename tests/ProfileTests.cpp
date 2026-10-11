// Tests for src/Profile.cpp: inclusive and self counts, the shares, the diagnostics line
#include "../src/Profile.hpp"
#include "Check.hpp"

#include <cmath>
#include <initializer_list>

// The first two mods' buckets
constexpr uint32_t A = bucket::FIRST_MOD;
constexpr uint32_t B = bucket::FIRST_MOD + 1;

// GD at 0x1000, cocos at 0x2000, mod A at 0x3000, mod B at 0x4000, a system DLL at 0x5000
static ModuleMap sampleMap()
{
  std::vector<MappedModule> modules = {
      {.base = 0x1000, .size = 0x1000, .bucket = bucket::GAME},
      {.base = 0x2000, .size = 0x1000, .bucket = bucket::COCOS},
      {.base = 0x3000, .size = 0x1000, .bucket = A},
      {.base = 0x4000, .size = 0x1000, .bucket = B},
      {.base = 0x5000, .size = 0x1000, .bucket = bucket::OTHER},
  };
  return ModuleMap(std::move(modules), {});
}

static Buckets sampleBuckets()
{
  Buckets buckets;
  buckets.index("zhulis.a", "A");
  buckets.index("zhulis.b", "B");
  return buckets;
}

static Sample stack(std::initializer_list<uintptr_t> frames)
{
  Sample sample;
  for (auto frame : frames)
    sample.frames[sample.depth++] = frame;
  return sample;
}

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// ! --- Counting --- !

static void testInclusiveOncePerSample()
{
  auto map = sampleMap();
  Profile profile;
  // Mod A is on the stack three times (a recursive call through the game): counted once
  profile.add(stack({0x3010, 0x1010, 0x3020, 0x1020, 0x3030}), map);
  CHECK(profile.samples() == 1);
  CHECK(profile.at(A).inclusive == 1);
  CHECK(profile.at(A).self == 1);
  CHECK(profile.at(bucket::GAME).inclusive == 1);
  CHECK(profile.at(bucket::GAME).self == 0);
  CHECK(profile.at(B).inclusive == 0);
}

static void testSelfIsTheFirstFrame()
{
  auto map = sampleMap();
  Profile profile;
  profile.add(stack({0x5010, 0x4010, 0x1010}), map);
  profile.add(stack({0x2010, 0x3010, 0x1010}), map);
  CHECK(profile.samples() == 2);
  CHECK(profile.at(bucket::OTHER).self == 1);
  CHECK(profile.at(bucket::COCOS).self == 1);
  CHECK(profile.at(B).inclusive == 1);
  CHECK(profile.at(A).inclusive == 1);
  CHECK(profile.at(bucket::GAME).inclusive == 2);
  CHECK(profile.at(A).self == 0);
}

static void testUnknownAddressesAreOther()
{
  auto map = sampleMap();
  Profile profile;
  // A hook handler outside every module
  profile.add(stack({0x9000, 0x3010}), map);
  CHECK(profile.at(bucket::OTHER).self == 1);
  CHECK(profile.at(A).inclusive == 1);
}

static void testEmptySamplesAreSkipped()
{
  auto map = sampleMap();
  Profile profile;
  profile.add(Sample{}, map);
  CHECK(profile.samples() == 0);
  CHECK(profile.buckets().empty());
}

static void testMerge()
{
  auto map = sampleMap();
  Profile a, b;
  a.add(stack({0x3010, 0x1010}), map);
  b.add(stack({0x3010, 0x1010}), map);
  b.add(stack({0x4010, 0x1010}), map);
  a.merge(b);
  CHECK(a.samples() == 3);
  CHECK(a.at(A).inclusive == 2);
  CHECK(a.at(B).inclusive == 1);
  CHECK(a.at(bucket::GAME).inclusive == 3);
  // Counting goes on right after a merge
  a.add(stack({0x3010, 0x3020}), map);
  CHECK(a.at(A).inclusive == 3);
  a.clear();
  CHECK(a.samples() == 0);
  CHECK(a.at(A).inclusive == 0);
}

// ! --- Shares --- !

static Window sampleWindow()
{
  auto map = sampleMap();
  Window window;
  window.seconds = 1.0;
  window.frames = 100;
  for (int i = 0; i < 6; i++)
    window.profile.add(stack({0x1010}), map);
  for (int i = 0; i < 3; i++)
    window.profile.add(stack({0x3010, 0x1010}), map);
  window.profile.add(stack({0x4010, 0x3010, 0x1010}), map);
  return window;
}

static void testShares()
{
  auto window = sampleWindow();
  auto a = shareOf(window, A);
  CHECK(near(a.inclusivePercent, 40));
  CHECK(near(a.selfPercent, 30));
  // 40% of 10 ms a frame
  CHECK(near(a.inclusiveMillisPerFrame, 4));
  auto game = shareOf(window, bucket::GAME);
  CHECK(near(game.inclusivePercent, 100));
  CHECK(near(game.selfPercent, 60));
  // A bucket no sample reached
  CHECK(near(shareOf(window, 42).inclusivePercent, 0));
}

static void testSharesWithoutFrames()
{
  auto window = sampleWindow();
  window.frames = 0;
  CHECK(near(shareOf(window, A).inclusiveMillisPerFrame, 0));
  Window empty;
  CHECK(near(shareOf(empty, A).inclusivePercent, 0));
}

static void testTopMods()
{
  auto window = sampleWindow();
  auto top = topMods(window, 5);
  // Only mods: the game, cocos and other are left out
  CHECK(top.size() == 2);
  CHECK(top[0].bucket == A);
  CHECK(top[1].bucket == B);
  CHECK(topMods(window, 1).size() == 1);
  CHECK(topMods(Window{}, 5).empty());
}

// ! --- Stacks --- !

static void testDescribe()
{
  std::vector<MappedModule> modules = {{.base = 0x1000, .size = 0x1000, .name = "GeometryDash.exe"}};
  ModuleMap map(std::move(modules), {});
  CHECK(describeAddress(0x1abc, map) == "GeometryDash.exe+0xabc");
  CHECK(describeAddress(0x9000, map) == "0x9000");
  CHECK(describeStack(stack({0x1010, 0x9000}), map) == "GeometryDash.exe+0x10 < 0x9000");
  CHECK(describeStack(Sample{}, map) == "(empty)");
}

static void testStatsFromTicks()
{
  Sample sample;
  sample.suspendTicks = 30;
  sample.contextTicks = 10;
  sample.flags = SAMPLE_SCANNED;
  SamplerStats stats;
  // Ticks of 0.5 us
  stats.add(sample, 0.5);
  sample.suspendTicks = 90;
  stats.add(sample, 0.5);
  CHECK(stats.samples == 2);
  CHECK(stats.scanned == 2);
  CHECK(near(stats.suspendAverageMicros(), 30));
  CHECK(near(stats.suspendMaxMicros, 45));
  CHECK(near(stats.contextAverageMicros(), 5));
  SamplerStats total;
  total.merge(stats);
  total.merge(stats);
  CHECK(total.samples == 4);
  CHECK(near(total.suspendMaxMicros, 45));
}

// ! --- The diagnostics line --- !

static bool contains(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }

static void testDiagnosticsLine()
{
  auto window = sampleWindow();
  window.stats.samples = 10;
  window.stats.suspendTotalMicros = 150;
  window.stats.suspendMaxMicros = 40;
  window.stats.contextTotalMicros = 50;
  window.stats.contextMaxMicros = 20;
  auto line = diagnosticsLine(window, sampleBuckets(), 3);
  CHECK(contains(line, "10 samples in 1.00 s"));
  CHECK(contains(line, "suspended avg 15.0 us, max 40.0 us (stopping avg 5.0 us, max 20.0 us)"));
  CHECK(contains(line, "100 fps"));
  CHECK(contains(line, "self: Geometry Dash 60.0%, Cocos2d 0.0%, Other 0.0%, Hook handlers 0.0%, mods 40.0%"));
  CHECK(contains(line, "inclusive: zhulis.a 40.0%, zhulis.b 10.0%"));
  CHECK(contains(line, "idle 0.0%, between frames 0.0%"));
  // Without an attribution nobody has own time
  CHECK(contains(line, "own: none"));
  // Nothing went wrong: no list of problems
  CHECK(!contains(line, "dropped"));
}

static void testDiagnosticsProblems()
{
  auto window = sampleWindow();
  window.stats.samples = 10;
  window.stats.dropped = 2;
  window.stats.scanned = 5;
  auto line = diagnosticsLine(window, sampleBuckets(), 3);
  CHECK(contains(line, "10 samples in 1.00 s (2 dropped, 5 scanned)"));
}

static void testDiagnosticsNoSamples()
{
  Window window;
  window.seconds = 1;
  auto line = diagnosticsLine(window, sampleBuckets(), 3);
  CHECK(contains(line, "0 samples"));
  CHECK(contains(line, "no samples"));
  CHECK(!contains(line, "fps"));
}

static void testDiagnosticsNoMods()
{
  auto map = sampleMap();
  Window window;
  window.seconds = 1;
  window.profile.add(stack({0x1010}), map);
  auto line = diagnosticsLine(window, sampleBuckets(), 3);
  CHECK(contains(line, "inclusive: none"));
}

int main()
{
  testInclusiveOncePerSample();
  testSelfIsTheFirstFrame();
  testUnknownAddressesAreOther();
  testEmptySamplesAreSkipped();
  testMerge();
  testShares();
  testSharesWithoutFrames();
  testTopMods();
  testDescribe();
  testStatsFromTicks();
  testDiagnosticsLine();
  testDiagnosticsProblems();
  testDiagnosticsNoSamples();
  testDiagnosticsNoMods();
  return finish("profile");
}
