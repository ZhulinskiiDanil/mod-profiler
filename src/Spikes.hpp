#pragma once

// Frames that took too long, and whose fault they were, without Windows or Geode in it
#include "Attribution.hpp"
#include "Profile.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Turns the times frames ended at into the frames over a threshold
class FrameClock
{
public:
  struct Frame
  {
    int64_t start = 0;
    int64_t end = 0;
  };

  void add(int64_t end) { m_ends.push_back(end); }
  // Frames that ended and took longer than `threshold` ticks, oldest first. Frames that were
  // looked at are forgotten, the last end stays as the next frame's start
  std::vector<Frame> slowFrames(int64_t threshold);
  // When the oldest frame not looked at yet started, or 0: samples before it aren't needed anymore
  int64_t oldestStart() const { return m_ends.empty() ? 0 : m_ends.front(); }

private:
  std::deque<int64_t> m_ends;
};

struct Spike
{
  // Ticks, like the samples
  int64_t start = 0;
  int64_t end = 0;
  double millis = 0;
  uint64_t samples = 0;
  // The mod with the most own time in it, or -1 when no mod had a fair share of it
  int culprit = -1;
  double culpritPercent = 0;
  // The game's own share (samples nobody's own, not idle or between frames)
  double gamePercent = 0;
  double idlePercent = 0;
  // The culprit's busiest functions and hooks, and a few of its stacks
  std::vector<Place> functions;
  std::vector<Place> hooks;
  std::vector<Sample> stacks;
};

// A mod needs this much of a spike's samples to be its culprit
inline constexpr double CULPRIT_SHARE = 0.2;

// The spike made of these samples (those of one frame) and their verdicts
Spike makeSpike(FrameClock::Frame frame, double ticksPerMilli, std::vector<Sample const*> const& samples,
                std::vector<Verdict> const& verdicts, size_t stackCount);
