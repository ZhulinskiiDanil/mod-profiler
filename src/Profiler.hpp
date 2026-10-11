#pragma once

// The profiler as the rest of the mod sees it: started once, fed the mods list and the frames,
// and asked for the latest numbers. The analysis runs on its own thread once a second
#include "Modules.hpp"
#include "Profile.hpp"

#include <vector>

namespace profiler
{
  struct Options
  {
    bool enabled = true;
    double intervalMillis = 1.0;
    bool logDiagnostics = true;
    bool logStacks = false;
  };

  // Call on the main thread
  bool start(Options const& options);
  void setOptions(Options const& options);
  void setMods(std::vector<modules::ModBinary> mods);
  // One frame drawn, from the main thread
  void frame();

  struct Snapshot
  {
    // The last full second, and everything since the start (or the last reset)
    Window last;
    Window session;
    Buckets buckets;
    std::vector<MappedModule> modules;
  };
  Snapshot snapshot();
} // namespace profiler
