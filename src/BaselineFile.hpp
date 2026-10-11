#pragma once

// The baseline in the mod's save folder, so it's still there after a restart (or a mod update)
#include "Compare.hpp"

namespace baseline_file
{
  // Call on the main thread
  Baseline const& current();
  void load();
  void save(Baseline baseline);
  void clear();
} // namespace baseline_file
