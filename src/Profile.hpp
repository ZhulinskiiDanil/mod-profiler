#pragma once

// Counting samples into buckets, and the line the diagnostics log, without Windows or Geode in it
#include "Attribution.hpp"
#include "ModuleMap.hpp"
#include "Sample.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct BucketTime
{
  // Samples with the bucket anywhere on the stack, counted once a sample
  uint64_t inclusive = 0;
  // Samples where the thread was in the bucket's own code
  uint64_t self = 0;
  // Samples that are the mod's own work, see Attribution.hpp
  uint64_t own = 0;
};

class Profile
{
public:
  // Without an attribution there's no own time and no idle
  void add(Sample const& sample, ModuleMap const& map, Attribution const* attribution = nullptr);
  void merge(Profile const& other);
  void clear();

  uint64_t samples() const { return m_samples; }
  // Samples where the thread was waiting, and where it was in the loop between frames
  uint64_t idle() const { return m_idle; }
  uint64_t between() const { return m_between; }
  // Indexed by bucket, may be shorter than the bucket list: the missing ones are zero
  std::vector<BucketTime> const& buckets() const { return m_buckets; }
  BucketTime at(uint32_t bucket) const { return bucket < m_buckets.size() ? m_buckets[bucket] : BucketTime{}; }

  // Own samples by (mod, function start) and by (mod, hooked function)
  using Places = std::map<std::pair<uint32_t, uintptr_t>, uint64_t>;
  Places const& functions() const { return m_functions; }
  Places const& hooks() const { return m_hooks; }

private:
  BucketTime& grow(uint32_t bucket);

  std::vector<BucketTime> m_buckets;
  // The last sample each bucket was counted in, so a recursive mod counts once
  std::vector<uint64_t> m_seen;
  uint64_t m_samples = 0;
  uint64_t m_idle = 0;
  uint64_t m_between = 0;
  Places m_functions;
  Places m_hooks;
};

struct Place
{
  uintptr_t address = 0;
  uint64_t samples = 0;
};

// The places of one mod with the most samples, most first
std::vector<Place> topPlaces(Profile::Places const& places, uint32_t bucket, size_t limit);

// How the sampler itself did
struct SamplerStats
{
  uint64_t samples = 0;
  // Slots the ring had no room for
  uint64_t dropped = 0;
  uint64_t scanned = 0;
  uint64_t truncated = 0;
  uint64_t failed = 0;
  double suspendTotalMicros = 0;
  double suspendMaxMicros = 0;
  double contextTotalMicros = 0;
  double contextMaxMicros = 0;
  // CPU time of the profiler's own threads meanwhile
  double samplerCpuMillis = 0;
  double analyzerCpuMillis = 0;

  void add(Sample const& sample, double microsPerTick);
  void merge(SamplerStats const& other);
  double suspendAverageMicros() const { return samples ? suspendTotalMicros / samples : 0; }
  double contextAverageMicros() const { return samples ? contextTotalMicros / samples : 0; }
};

struct Window
{
  Profile profile;
  SamplerStats stats;
  double seconds = 0;
  // Frames the game drew meanwhile, 0 when unknown
  uint64_t frames = 0;
};

// What the profiler costs, from a window's stats
struct Cost
{
  // The main thread standing still, a second
  double stoppedMillisPerSecond = 0;
  // Of one CPU core
  double samplerPercent = 0;
  double analyzerPercent = 0;
};
Cost costOf(Window const& window);

struct Share
{
  uint32_t bucket = 0;
  double inclusivePercent = 0;
  double selfPercent = 0;
  double ownPercent = 0;
  // Of a frame on average, 0 when the frame count is unknown
  double inclusiveMillisPerFrame = 0;
  double ownMillisPerFrame = 0;
};

enum class Order
{
  Inclusive,
  Own,
};

// Mods sorted by a share, at most `limit` of them, none that never was on the stack
std::vector<Share> topMods(Window const& window, size_t limit, Order order = Order::Inclusive);
Share shareOf(Window const& window, uint32_t bucket);

// "GeometryDash.exe+0x1a2b3c", or the bare address outside every module
std::string describeAddress(uintptr_t address, ModuleMap const& map);
// The frames of a sample, innermost first, " < " between them
std::string describeStack(Sample const& sample, ModuleMap const& map);

// The once-a-second line of the self-diagnostics
std::string diagnosticsLine(Window const& window, Buckets const& buckets, size_t topCount);
