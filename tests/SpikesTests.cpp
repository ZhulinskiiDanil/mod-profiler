// Tests for src/Spikes.cpp: slow frames and their culprits
#include "../src/Spikes.hpp"
#include "Check.hpp"

#include <cmath>

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// ! --- Frames --- !

static void testSlowFrames()
{
  FrameClock clock;
  for (int64_t end : {0, 10, 20, 95, 105, 300})
    clock.add(end);
  auto slow = clock.slowFrames(50);
  CHECK(slow.size() == 2);
  CHECK(slow[0].start == 20 && slow[0].end == 95);
  CHECK(slow[1].start == 105 && slow[1].end == 300);
  // The last end stays as the next frame's start
  CHECK(clock.oldestStart() == 300);
  CHECK(clock.slowFrames(50).empty());
  clock.add(400);
  auto next = clock.slowFrames(50);
  CHECK(next.size() == 1 && next[0].start == 300);
}

static void testRestart()
{
  // Ends 10 and 300 with lost ones between: not a frame of 290
  FrameClock clock;
  clock.add(0);
  clock.add(10);
  clock.add(300);
  clock.restart();
  CHECK(clock.oldestStart() == 300);
  CHECK(clock.slowFrames(50).empty());
  clock.add(320);
  CHECK(clock.slowFrames(10).size() == 1);
  FrameClock empty;
  empty.restart();
  CHECK(empty.oldestStart() == 0);
}

static void testNoFrames()
{
  FrameClock clock;
  CHECK(clock.slowFrames(1).empty());
  CHECK(clock.oldestStart() == 0);
  clock.add(5);
  CHECK(clock.slowFrames(1).empty());
  CHECK(clock.oldestStart() == 5);
}

// ! --- Culprits --- !

struct Samples
{
  std::vector<Sample> samples;
  std::vector<Verdict> verdicts;

  void add(int count, Verdict verdict)
  {
    for (int i = 0; i < count; i++)
    {
      Sample sample;
      sample.depth = 1;
      sample.frames[0] = verdict.function ? verdict.function + i : 0x1000;
      samples.push_back(sample);
      verdicts.push_back(verdict);
    }
  }

  Spike spike(size_t stacks = 2)
  {
    std::vector<Sample const*> pointers;
    for (auto const& sample : samples)
      pointers.push_back(&sample);
    return makeSpike({0, 80'000}, 1000.0, pointers, verdicts, stacks);
  }
};

static void testCulprit()
{
  Samples frame;
  frame.add(50, {.owner = 4, .function = 0x100, .hook = 0x900});
  frame.add(10, {.owner = 4, .function = 0x200});
  frame.add(20, {.owner = 5, .function = 0x300});
  frame.add(15, {});
  frame.add(5, {.idle = true});
  auto spike = frame.spike();
  CHECK(near(spike.millis, 80));
  CHECK(spike.samples == 100);
  CHECK(spike.culprit == 4);
  CHECK(near(spike.culpritPercent, 60));
  CHECK(near(spike.gamePercent, 15));
  CHECK(near(spike.idlePercent, 5));
  CHECK(spike.functions.size() == 2);
  CHECK(spike.functions[0].address == 0x100 && spike.functions[0].samples == 50);
  CHECK(spike.hooks.size() == 1 && spike.hooks[0].address == 0x900);
  // One stack for each busiest function, at most 2
  CHECK(spike.stacks.size() == 2);
  CHECK(spike.stacks[0].frames[0] == 0x100);
  CHECK(spike.stacks[1].frames[0] == 0x200);
}

static void testNoCulprit()
{
  // The game's own work: no mod has a fair share
  Samples frame;
  frame.add(85, {});
  frame.add(15, {.owner = 4, .function = 0x100});
  auto spike = frame.spike();
  CHECK(spike.culprit == -1);
  CHECK(near(spike.gamePercent, 85));
  CHECK(spike.functions.empty());
  CHECK(spike.stacks.empty());
}

static void testBetweenFramesIsIdle()
{
  Samples frame;
  frame.add(10, {.between = true});
  frame.add(10, {.owner = 4, .function = 0x100});
  auto spike = frame.spike();
  CHECK(near(spike.idlePercent, 50));
  CHECK(spike.culprit == 4);
}

static void testEmpty()
{
  Samples frame;
  auto spike = frame.spike();
  CHECK(spike.samples == 0);
  CHECK(spike.culprit == -1);
  CHECK(near(spike.millis, 80));
}

static void testLockWaits()
{
  std::vector<MappedModule> modules = {
      {.base = 0x1000, .size = 0x1000, .bucket = bucket::GAME},
      {.base = 0x3000, .size = 0x1000, .bucket = bucket::FIRST_MOD},
      {.base = 0x5000, .size = 0x1000, .bucket = bucket::OTHER},
  };
  ModuleMap map(std::move(modules), {});
  Attribution attribution;
  attribution.waits = {{0x5000, 0x5040}};
  attribution.lockWaits = {{0x5000, 0x5020}};
  attribution.finish();
  Sample locked;
  locked.depth = 3;
  locked.frames[0] = 0x5010;
  locked.frames[1] = 0x3010;
  locked.frames[2] = 0x1010;
  Sample sleeping = locked;
  sleeping.frames[0] = 0x5030;
  std::vector<Sample const*> samples = {&locked, &locked, &locked, &sleeping};
  CHECK(lockWaitShare(samples, attribution) == 0.75);
  CHECK(lockWaitShare({}, attribution) == 0);
  CHECK(innermostMod(locked, map) == static_cast<int>(bucket::FIRST_MOD));
  Sample game;
  game.depth = 1;
  game.frames[0] = 0x1010;
  CHECK(innermostMod(game, map) == -1);
  // The mod most of the waiting samples are in, not the last sample's
  std::vector<Sample const*> mixed = {&locked, &locked, &game, &sleeping};
  CHECK(waitingMod(mixed, map, attribution) == static_cast<int>(bucket::FIRST_MOD));
  std::vector<Sample const*> none = {&game, &sleeping};
  CHECK(waitingMod(none, map, attribution) == -1);
}

int main()
{
  testSlowFrames();
  testRestart();
  testNoFrames();
  testCulprit();
  testNoCulprit();
  testBetweenFramesIsIdle();
  testEmpty();
  testLockWaits();
  return finish("spikes");
}
