#pragma once

// A saved session to compare a new one with, without Windows or Geode in it
#include "Profile.hpp"

#include <map>
#include <string>

struct BaselineMod
{
  std::string name;
  double ownMillis = 0;
  double inclusivePercent = 0;
};

struct Baseline
{
  // The local time it was saved at
  std::string saved;
  double seconds = 0;
  double fps = 0;
  double frameMillis = 0;
  // The part of a frame that is work: not waiting, not between frames
  double busyMillis = 0;
  // By mod ID: IDs stay the same between launches, bucket numbers don't
  std::map<std::string, BaselineMod> mods;

  bool empty() const { return seconds <= 0; }
};

Baseline makeBaseline(Window const& window, Buckets const& buckets, std::string saved);

// The work part of an average frame, in milliseconds
double busyMillis(Window const& window);

// What changed since the baseline: the frame, then the mods whose own time changed the most
std::string comparisonLine(Window const& window, Buckets const& buckets, Baseline const& baseline, size_t topCount);
