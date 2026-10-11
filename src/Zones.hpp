#pragma once

// Named parts of a mod's code (zones), without Windows or Geode in it. A mod marks a zone with
// mod_profiler::Zone (include/ModProfiler.hpp), and samples taken in it count for the zone.
// Only the main thread's zones count: that's the thread sampled
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The main thread's zones, innermost last. The sampler reads it while the thread is suspended: the
// thread writes an id, then the depth, so a depth read is never ahead of its ids
struct ZoneStack
{
  static constexpr uint32_t MAX_DEPTH = 32;
  std::atomic<uint32_t> depth{0};
  // Deeper than MAX_DEPTH: counted, not kept, so the pops still match
  uint32_t overflow = 0;
  uint16_t ids[MAX_DEPTH] = {};

  void push(uint16_t id) noexcept;
  void pop() noexcept;
  // The innermost zone, 0 for none. Safe to call while the writing thread is suspended
  uint16_t top() const noexcept;
};

struct ZoneName
{
  std::string mod;
  std::string name;
};

// Zone names to small ids, 1 and up. Written by the main thread, read by the analyzer
class ZoneRegistry
{
public:
  static constexpr size_t MAX_ZONES = 4000;
  // The id for this mod's zone of this name, made if it's new. 0 when there are too many
  uint16_t intern(std::string const& mod, std::string const& name);
  ZoneName nameOf(uint16_t id) const;
  size_t size() const;

private:
  mutable std::mutex m_mutex;
  std::unordered_map<std::string, uint16_t> m_ids;
  std::vector<ZoneName> m_names;
};
