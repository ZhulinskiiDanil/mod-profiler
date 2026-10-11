#pragma once

// Other mods' zones (include/ModProfiler.hpp), on the profiler's side
#include "Zones.hpp"

namespace zones
{
  // Call on the main thread, before other mods load: their first zone asks for the API once
  void setup();
  // The main thread's zones, for the sampler
  ZoneStack const& stack();
  ZoneRegistry const& registry();
} // namespace zones
