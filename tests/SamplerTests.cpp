// Tests for src/Sampler.cpp on Windows: the real sampler suspends a thread stuck in a known chain of
// functions, and the stacks it walks have to show that chain, in order, every time
#include "../src/Modules.hpp"
#include "../src/Profile.hpp"
#include "../src/Sampler.hpp"
#include "Check.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// ! --- The thread to sample --- !

static std::atomic<int> s_phase{0};
static volatile uint64_t s_sink = 0;

// A leaf without a frame: an optimizing compiler gives it no function table entry
extern "C" __declspec(noinline) void spin()
{
  for (int i = 0; i < 1000; i++)
    s_sink = s_sink + i;
}

// The innermost of the chain: the thread is in it or in the leaf it calls
extern "C" __declspec(noinline) void level3()
{
  int phase = s_phase.load();
  while (s_phase.load() == phase)
    spin();
}

extern "C" __declspec(noinline) void level2()
{
  level3();
  s_sink = s_sink + 1;
}

extern "C" __declspec(noinline) void level1()
{
  level2();
  s_sink = s_sink + 1;
}

// Deeper than the walk goes
extern "C" __declspec(noinline) void deep(int depth)
{
  if (depth == 0)
    level3();
  else
    deep(depth - 1);
  s_sink = s_sink + 1;
}

// A function's address, past an incremental-linking jump to it
static uintptr_t address(void (*function)())
{
  auto at = reinterpret_cast<uintptr_t>(function);
  if (*reinterpret_cast<uint8_t const*>(at) == 0xE9)
    at += 5 + *reinterpret_cast<int32_t const*>(at + 1);
  return at;
}

// ! --- Sampling it --- !

struct Session
{
  Ring<Sample> ring{4096};
  std::atomic<ModuleMap const*> current{nullptr};
  std::unique_ptr<ModuleMap> map;
  Buckets buckets;

  std::vector<Sample> collect(std::chrono::milliseconds duration)
  {
    std::vector<Sample> samples;
    auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until)
    {
      while (auto sample = ring.peek())
      {
        samples.push_back(*sample);
        ring.pop();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return samples;
  }
};

// Where the sample's frames start, innermost first
static std::vector<uintptr_t> starts(Sample const& sample, ModuleMap const& map)
{
  std::vector<uintptr_t> result;
  for (int i = 0; i < sample.depth; i++)
    result.push_back(map.functionStart(i == 0 ? sample.frames[i] : sample.frames[i] - 1));
  return result;
}

int main()
{
  // The thread captures itself as "the main thread", then waits in the chain
  std::atomic<bool> captured{false};
  std::thread target([&]
                     {
                       sampler::captureMainThread();
                       captured = true;
                       level1();
                       deep(200); });
  while (!captured)
    std::this_thread::yield();

  Session session;
  session.map = modules::build(modules::loaded(), {}, {}, session.buckets);
  session.current = session.map.get();
  CHECK(sampler::start(session.ring, session.current, 1.0));

  auto f1 = address(level1), f2 = address(level2), f3 = address(level3), fdeep = address(reinterpret_cast<void (*)()>(deep));

  // ! --- The chain, in order --- !
  auto samples = session.collect(std::chrono::milliseconds(500));
  size_t found = 0;
  double suspendTicks = 0;
  for (auto const& sample : samples)
  {
    suspendTicks += sample.suspendTicks;
    // The chain may be under a helper level3 calls (an atomic's load in a build without optimizations)
    auto frames = starts(sample, *session.map);
    auto at = std::find(frames.begin(), frames.end(), f3);
    if (at - frames.begin() <= 2 && frames.end() - at >= 3 && at[1] == f2 && at[2] == f1)
      found++;
    else
    {
      std::printf("  not the chain (flags %d, depth %d):", sample.flags, sample.depth);
      for (int i = 0; i < sample.depth && i < 6; i++)
        std::printf(" %s", describeAddress(sample.frames[i], *session.map).c_str());
      std::printf("\n");
    }
  }
  std::printf("chain: %zu of %zu samples, suspended %.1f us on average\n", found, samples.size(),
              samples.empty() ? 0.0 : suspendTicks / samples.size() * 1e6 / sampler::ticksPerSecond());
  // At 1 ms for half a second: a few hundred, every one of them the chain
  CHECK(samples.size() > 200);
  CHECK(found == samples.size());
  // And below the chain, the thread's start: the walk goes all the way down
  bool reachedStart = false;
  if (!samples.empty())
  {
    auto const& last = samples.back();
    auto ntdll = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ntdll.dll"));
    auto image = session.map->findImage(last.frames[last.depth - 1]);
    reachedStart = image >= 0 && session.map->module(image).base == ntdll && !(last.flags & SAMPLE_TRUNCATED);
  }
  CHECK(reachedStart);

  // ! --- Deeper than the walk goes --- !
  s_phase = 1;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  while (session.ring.peek())
    session.ring.pop();
  samples = session.collect(std::chrono::milliseconds(300));
  size_t truncated = 0;
  for (auto const& sample : samples)
  {
    auto frames = starts(sample, *session.map);
    auto at = std::find(frames.begin(), frames.end(), f3);
    bool allDeep = frames.size() == MAX_FRAMES && at - frames.begin() <= 2;
    for (auto frame = at + 1; frame < frames.end() && allDeep; frame++)
      allDeep = *frame == fdeep;
    if (allDeep && (sample.flags & SAMPLE_TRUNCATED))
      truncated++;
  }
  std::printf("deep: %zu of %zu samples cut at %d frames\n", truncated, samples.size(), MAX_FRAMES);
  CHECK(samples.size() > 100);
  CHECK(truncated == samples.size());

  // ! --- Stopping --- !
  sampler::stop();
  CHECK(!sampler::running());
  CHECK(!sampler::alive());
  // No more samples once stopped, and the thread runs on
  while (session.ring.peek())
    session.ring.pop();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(session.ring.peek() == nullptr);
  s_phase = 2;
  target.join();
  modules::release(*session.map);
  return finish("sampler");
}
