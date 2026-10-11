#pragma once

// The profiler as the rest of the mod sees it: started once, fed the mods list and the frames,
// and asked for the latest numbers. The analysis runs on its own thread once a second
#include "Modules.hpp"
#include "Profile.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace profiler
{
  struct Options
  {
    bool enabled = true;
    double intervalMillis = 1.0;
    bool logDiagnostics = true;
    bool logStacks = false;
    bool symbols = true;
    // Frames longer than this are slow frames, looked into
    double spikeMillis = 50;
    bool logSpikes = true;
  };

  // Call on the main thread
  bool start(Options const& options);
  void setOptions(Options const& options);
  void setMods(std::vector<modules::ModBinary> mods);

  struct HookInfo
  {
    std::string mod;
    uintptr_t target = 0;
    uintptr_t detour = 0;
    // The hooked function, like "MenuLayer::init"
    std::string name;
  };
  void setHooks(std::vector<HookInfo> hooks);
  // The game's loop and the functions of it that make a frame, for telling the time between frames
  void setLoop(uintptr_t loop, std::vector<uintptr_t> frame);
  // One frame drawn, from the main thread
  void frame();
  // The mods are loaded and have made their hooks: generated code can be looked up
  void hooksReady();

  // How many seconds the recent window has
  inline constexpr int RECENT_SECONDS = 5;

  // A slow frame, ready to show
  struct SlowFrame
  {
    // The local time it was found at, "14:03:12"
    std::string when;
    double millis = 0;
    // The mod, or empty when no mod had a fair share
    std::string culprit;
    double culpritPercent = 0;
    double gamePercent = 0;
    double idlePercent = 0;
    // The culprit's busiest function and hook, and a few of its stacks, named
    std::string function;
    std::string hook;
    std::vector<std::string> stacks;
  };
  inline constexpr size_t KEPT_SLOW_FRAMES = 20;

  struct Snapshot
  {
    // The last full second, the last few, and everything since the start
    Window last;
    Window recent;
    Window session;
    Buckets buckets;
    // Names for the busiest places of the recent window: hooked functions and mods' functions
    std::unordered_map<uintptr_t, std::string> names;
    // The latest last
    std::vector<SlowFrame> slowFrames;
  };
  Snapshot snapshot();
} // namespace profiler
