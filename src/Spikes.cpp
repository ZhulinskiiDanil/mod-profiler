#include "Spikes.hpp"

#include <algorithm>
#include <map>

std::vector<FrameClock::Frame> FrameClock::slowFrames(int64_t threshold)
{
  std::vector<Frame> slow;
  while (m_ends.size() >= 2)
  {
    auto start = m_ends[0];
    auto end = m_ends[1];
    if (end - start > threshold)
      slow.push_back({start, end});
    m_ends.pop_front();
  }
  return slow;
}

Spike makeSpike(FrameClock::Frame frame, double ticksPerMilli, std::vector<Sample const*> const& samples,
                std::vector<Verdict> const& verdicts, size_t stackCount)
{
  Spike spike{.start = frame.start, .end = frame.end, .samples = samples.size()};
  spike.millis = ticksPerMilli > 0 ? (frame.end - frame.start) / ticksPerMilli : 0;
  if (samples.empty())
    return spike;

  std::map<int, uint64_t> owners;
  uint64_t idle = 0, game = 0;
  for (auto const& verdict : verdicts)
  {
    if (verdict.idle || verdict.between)
      idle++;
    else if (verdict.owner >= 0)
      owners[verdict.owner]++;
    else
      game++;
  }
  auto count = static_cast<double>(samples.size());
  spike.gamePercent = 100.0 * game / count;
  spike.idlePercent = 100.0 * idle / count;

  auto busiest = std::max_element(owners.begin(), owners.end(), [](auto const& a, auto const& b)
                                  { return a.second < b.second; });
  if (busiest == owners.end() || busiest->second < CULPRIT_SHARE * count)
    return spike;
  spike.culprit = busiest->first;
  spike.culpritPercent = 100.0 * busiest->second / count;

  // Where in the culprit, and one stack for each of its busiest functions
  std::map<uintptr_t, uint64_t> functions, hooks;
  for (auto const& verdict : verdicts)
  {
    if (verdict.owner != spike.culprit)
      continue;
    functions[verdict.function]++;
    if (verdict.hook)
      hooks[verdict.hook]++;
  }
  auto sorted = [](std::map<uintptr_t, uint64_t> const& counts, size_t limit)
  {
    std::vector<Place> places;
    for (auto const& [address, samples] : counts)
      places.push_back({address, samples});
    std::stable_sort(places.begin(), places.end(), [](Place const& a, Place const& b)
                     { return a.samples > b.samples; });
    if (places.size() > limit)
      places.resize(limit);
    return places;
  };
  spike.functions = sorted(functions, 3);
  spike.hooks = sorted(hooks, 3);

  for (auto const& place : spike.functions)
  {
    if (spike.stacks.size() >= stackCount)
      break;
    for (size_t i = 0; i < samples.size(); i++)
      if (verdicts[i].owner == spike.culprit && verdicts[i].function == place.address)
      {
        spike.stacks.push_back(*samples[i]);
        break;
      }
  }
  return spike;
}
