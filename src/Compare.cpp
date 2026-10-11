#include "Compare.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

double busyMillis(Window const& window)
{
  auto const& profile = window.profile;
  if (window.frames == 0 || profile.samples() == 0)
    return 0;
  auto waiting = static_cast<double>(profile.idle() + profile.between()) / profile.samples();
  return std::max(0.0, 1 - waiting) * window.seconds * 1000 / window.frames;
}

Baseline makeBaseline(Window const& window, Buckets const& buckets, std::string saved)
{
  Baseline baseline{.saved = std::move(saved), .seconds = window.seconds};
  if (window.frames > 0 && window.seconds > 0)
  {
    baseline.fps = window.frames / window.seconds;
    baseline.frameMillis = window.seconds * 1000 / window.frames;
  }
  baseline.busyMillis = busyMillis(window);
  for (auto const& share : topMods(window, buckets.size()))
    baseline.mods[buckets[share.bucket].id] = {
        .name = buckets[share.bucket].name,
        .ownMillis = share.ownMillisPerFrame,
        .inclusivePercent = share.inclusivePercent,
    };
  return baseline;
}

namespace
{
  std::string signedMillis(double value)
  {
    char text[32];
    std::snprintf(text, sizeof(text), "%+.2f ms", value);
    return text;
  }
} // namespace

std::string comparisonLine(Window const& window, Buckets const& buckets, Baseline const& baseline, size_t topCount)
{
  if (baseline.empty())
    return "no baseline";
  char text[160];
  auto frameMillis = window.frames ? window.seconds * 1000 / window.frames : 0;
  auto busy = busyMillis(window);
  std::snprintf(text, sizeof(text), "vs the baseline of %s: frame %.2f ms (%s), work %.2f ms (%s)", baseline.saved.c_str(),
                frameMillis, signedMillis(frameMillis - baseline.frameMillis).c_str(), busy,
                signedMillis(busy - baseline.busyMillis).c_str());
  std::string line = text;

  // Every mod in either, by how much its own time moved
  struct Change
  {
    std::string name;
    double now = 0;
    double before = 0;
    bool added = false;
    bool removed = false;
  };
  std::map<std::string, Change> changes;
  for (auto const& share : topMods(window, buckets.size()))
  {
    auto const& bucket = buckets[share.bucket];
    auto& change = changes[bucket.id];
    change.name = bucket.name;
    change.now = share.ownMillisPerFrame;
    change.added = true;
  }
  for (auto const& [id, mod] : baseline.mods)
  {
    auto& change = changes[id];
    if (change.name.empty())
      change.name = mod.name;
    change.before = mod.ownMillis;
    change.removed = !change.added;
    change.added = false;
  }

  std::vector<Change> sorted;
  for (auto const& [id, change] : changes)
    // New or gone with next to no own time is noise, like a change that small
    if (std::fabs(change.now - change.before) >= 0.005)
      sorted.push_back(change);
  std::stable_sort(sorted.begin(), sorted.end(), [](Change const& a, Change const& b)
                   { return std::fabs(a.now - a.before) > std::fabs(b.now - b.before); });
  if (sorted.size() > topCount)
    sorted.resize(topCount);

  line += " | own:";
  if (sorted.empty())
    line += " no change";
  for (size_t i = 0; i < sorted.size(); i++)
  {
    auto const& change = sorted[i];
    line += i ? ", " : " ";
    line += change.name + " ";
    if (change.added)
      line += "new, " + signedMillis(change.now).substr(1);
    else if (change.removed)
      line += "gone, was " + signedMillis(change.before).substr(1);
    else
      line += signedMillis(change.now - change.before);
  }
  return line;
}
