#pragma once

// Counting samples into buckets, and the line the diagnostics log, without Windows or Geode in it
#include "ModuleMap.hpp"
#include "Sample.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct BucketTime
{
  // Samples with the bucket anywhere on the stack, counted once a sample
  uint64_t inclusive = 0;
  // Samples where the thread was in the bucket's own code
  uint64_t self = 0;
};

class Profile
{
public:
  void add(Sample const& sample, ModuleMap const& map);
  void merge(Profile const& other);
  void clear();

  uint64_t samples() const { return m_samples; }
  // Indexed by bucket, may be shorter than the bucket list: the missing ones are zero
  std::vector<BucketTime> const& buckets() const { return m_buckets; }
  BucketTime at(uint32_t bucket) const { return bucket < m_buckets.size() ? m_buckets[bucket] : BucketTime{}; }

private:
  BucketTime& grow(uint32_t bucket);

  std::vector<BucketTime> m_buckets;
  // The last sample each bucket was counted in, so a recursive mod counts once
  std::vector<uint64_t> m_seen;
  uint64_t m_samples = 0;
};

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

struct Share
{
  uint32_t bucket = 0;
  double inclusivePercent = 0;
  double selfPercent = 0;
  // Of a frame on average, 0 when the frame count is unknown
  double inclusiveMillisPerFrame = 0;
};

// Mods sorted by their inclusive share, at most `limit` of them, none with zero samples
std::vector<Share> topMods(Window const& window, size_t limit);
Share shareOf(Window const& window, uint32_t bucket);

// "GeometryDash.exe+0x1a2b3c", or the bare address outside every module
std::string describeAddress(uintptr_t address, ModuleMap const& map);
// The frames of a sample, innermost first, " < " between them
std::string describeStack(Sample const& sample, ModuleMap const& map);

// The once-a-second line of the self-diagnostics
std::string diagnosticsLine(Window const& window, Buckets const& buckets, size_t topCount);
