#include "Profile.hpp"

#include <algorithm>
#include <cstdio>

// ! --- Counting --- !

BucketTime& Profile::grow(uint32_t bucket)
{
  if (bucket >= m_buckets.size())
  {
    m_buckets.resize(bucket + 1);
    m_seen.resize(bucket + 1, 0);
  }
  return m_buckets[bucket];
}

void Profile::add(Sample const& sample, ModuleMap const& map)
{
  if (sample.depth == 0)
    return;
  // Sample numbers start at 1, so a zero in m_seen means never
  auto id = ++m_samples;

  grow(map.bucketOf(sample.frames[0])).self++;
  for (int i = 0; i < sample.depth && i < MAX_FRAMES; i++)
  {
    auto bucket = map.bucketOf(sample.frames[i]);
    auto& time = grow(bucket);
    if (m_seen[bucket] == id)
      continue;
    m_seen[bucket] = id;
    time.inclusive++;
  }
}

void Profile::merge(Profile const& other)
{
  for (uint32_t i = 0; i < other.m_buckets.size(); i++)
  {
    auto& time = grow(i);
    time.inclusive += other.m_buckets[i].inclusive;
    time.self += other.m_buckets[i].self;
  }
  m_samples += other.m_samples;
  // Sample numbers go on from the merged count: older marks are lower, so nothing is skipped later
  std::fill(m_seen.begin(), m_seen.end(), 0);
}

void Profile::clear()
{
  m_buckets.clear();
  m_seen.clear();
  m_samples = 0;
}

void SamplerStats::add(Sample const& sample, double microsPerTick)
{
  auto suspendMicros = sample.suspendTicks * microsPerTick;
  auto contextMicros = sample.contextTicks * microsPerTick;
  samples++;
  if (sample.flags & SAMPLE_SCANNED)
    scanned++;
  if (sample.flags & SAMPLE_TRUNCATED)
    truncated++;
  if (sample.flags & SAMPLE_NO_CONTEXT)
    failed++;
  suspendTotalMicros += suspendMicros;
  suspendMaxMicros = std::max(suspendMaxMicros, suspendMicros);
  contextTotalMicros += contextMicros;
  contextMaxMicros = std::max(contextMaxMicros, contextMicros);
}

void SamplerStats::merge(SamplerStats const& other)
{
  samples += other.samples;
  dropped += other.dropped;
  scanned += other.scanned;
  truncated += other.truncated;
  failed += other.failed;
  suspendTotalMicros += other.suspendTotalMicros;
  suspendMaxMicros = std::max(suspendMaxMicros, other.suspendMaxMicros);
  contextTotalMicros += other.contextTotalMicros;
  contextMaxMicros = std::max(contextMaxMicros, other.contextMaxMicros);
}

// ! --- Shares --- !

Share shareOf(Window const& window, uint32_t bucket)
{
  Share share{.bucket = bucket};
  auto samples = window.profile.samples();
  if (samples == 0)
    return share;
  auto time = window.profile.at(bucket);
  share.inclusivePercent = 100.0 * time.inclusive / samples;
  share.selfPercent = 100.0 * time.self / samples;
  if (window.frames > 0)
    share.inclusiveMillisPerFrame = share.inclusivePercent / 100.0 * window.seconds * 1000.0 / window.frames;
  return share;
}

std::vector<Share> topMods(Window const& window, size_t limit)
{
  std::vector<Share> shares;
  auto const& buckets = window.profile.buckets();
  for (uint32_t i = bucket::OTHER + 1; i < buckets.size(); i++)
    if (buckets[i].inclusive > 0)
      shares.push_back(shareOf(window, i));
  std::stable_sort(shares.begin(), shares.end(), [](Share const& a, Share const& b)
                   { return a.inclusivePercent > b.inclusivePercent; });
  if (shares.size() > limit)
    shares.resize(limit);
  return shares;
}

// ! --- Stacks --- !

std::string describeAddress(uintptr_t address, ModuleMap const& map)
{
  char buffer[160];
  auto index = map.findImage(address);
  if (index < 0)
    std::snprintf(buffer, sizeof(buffer), "0x%llx", (unsigned long long)address);
  else
  {
    auto const& module = map.module(index);
    std::snprintf(buffer, sizeof(buffer), "%s+0x%llx", module.name.c_str(), (unsigned long long)(address - module.base));
  }
  return buffer;
}

std::string describeStack(Sample const& sample, ModuleMap const& map)
{
  std::string text;
  for (int i = 0; i < sample.depth && i < MAX_FRAMES; i++)
  {
    if (i)
      text += " < ";
    text += describeAddress(sample.frames[i], map);
  }
  return text.empty() ? "(empty)" : text;
}

// ! --- The diagnostics line --- !

namespace
{
  std::string format(char const* pattern, auto... values)
  {
    char buffer[256];
    auto length = std::snprintf(buffer, sizeof(buffer), pattern, values...);
    return std::string(buffer, length < 0 ? 0 : std::min<size_t>(length, sizeof(buffer) - 1));
  }
} // namespace

std::string diagnosticsLine(Window const& window, Buckets const& buckets, size_t topCount)
{
  auto const& stats = window.stats;
  std::string line = format("%llu samples in %.2f s", (unsigned long long)stats.samples, window.seconds);

  // Only what went wrong, a clean second stays short
  std::string problems;
  auto note = [&](uint64_t count, char const* what)
  {
    if (count == 0)
      return;
    problems += problems.empty() ? " (" : ", ";
    problems += format("%llu %s", (unsigned long long)count, what);
  };
  note(stats.dropped, "dropped");
  note(stats.failed, "failed");
  note(stats.scanned, "scanned");
  note(stats.truncated, "deep");
  if (!problems.empty())
    line += problems + ")";

  line += format(", suspended avg %.1f us, max %.1f us", stats.suspendAverageMicros(), stats.suspendMaxMicros);
  // Waiting for the thread to stop is up to Windows, the rest is the walk
  if (stats.samples)
    line += format(" (stopping avg %.1f us, max %.1f us)", stats.contextAverageMicros(), stats.contextMaxMicros);
  if (window.frames > 0 && window.seconds > 0)
    line += format(", %.0f fps", window.frames / window.seconds);

  if (window.profile.samples() == 0)
    return line + " | no samples";

  // Where the thread itself was: the game, cocos, the rest, and the mods together
  double modsSelf = 100.0;
  line += " | self:";
  for (uint32_t fixed : {bucket::GAME, bucket::COCOS, bucket::OTHER})
  {
    auto share = shareOf(window, fixed);
    modsSelf -= share.selfPercent;
    line += format(" %s %.1f%%,", buckets[fixed].name.c_str(), share.selfPercent);
  }
  line += format(" mods %.1f%%", std::max(0.0, modsSelf));

  auto top = topMods(window, topCount);
  line += " | top:";
  if (top.empty())
    line += " none";
  for (size_t i = 0; i < top.size(); i++)
  {
    auto const& share = top[i];
    auto const& name = share.bucket < buckets.size() ? buckets[share.bucket].id : std::string("?");
    line += format("%s %s %.1f%%", i ? "," : "", name.c_str(), share.inclusivePercent);
    if (share.inclusiveMillisPerFrame > 0)
      line += format(" (%.2f ms)", share.inclusiveMillisPerFrame);
  }
  return line;
}
