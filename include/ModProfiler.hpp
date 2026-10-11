#pragma once

// Mod Profiler's API for other mods: zones, named parts of your code.
//
// No hard dependency: copy this header (or add "zhulis.mod-profiler" to your mod.json's dependencies with
// "required": false). Without Mod Profiler, a zone does nothing and costs a check of a cached pointer.
//
//   void HairNode::update(float dt)
//   {
//     mod_profiler::Zone zone("hair simulation");   // samples taken until the end of the block count for it
//     simulate(dt);
//   }
//
// The table shows a mod's zones under it, in ms a frame. Only the game's main thread is sampled, so zones
// on other threads are ignored. Zones nest: a sample counts for the innermost one. The name must stay
// valid while the zone is open (a string literal is fine)

#include <Geode/loader/Dispatch.hpp>
#include <Geode/loader/Mod.hpp>

#include <atomic>

namespace mod_profiler
{
  namespace detail
  {
    // What Mod Profiler hands out, once: calls through it don't go through events
    struct Api
    {
      // False when the zone isn't counted (not the main thread, too many zones): then no leave for it
      bool (*enter)(char const* modID, char const* name);
      void (*leave)();
    };

    // Asked for until it's there (a mod can open a zone before Mod Profiler loads), a few dozen times at most
    inline Api const* api()
    {
      static std::atomic<Api const*> cached{nullptr};
      static std::atomic<int> tries{0};
      if (auto api = cached.load(std::memory_order_acquire))
        return api;
      if (tries.fetch_add(1, std::memory_order_relaxed) >= 64)
        return nullptr;
      Api const* api = nullptr;
      geode::Dispatch<Api const**>("zhulis.mod-profiler/api").send(&api);
      if (api)
        cached.store(api, std::memory_order_release);
      return api;
    }
  } // namespace detail

  class Zone
  {
  public:
    explicit Zone(char const* name)
    {
      if (auto api = detail::api())
        m_entered = api->enter(geode::getMod()->getID().data(), name);
    }

    Zone(Zone const&) = delete;
    Zone& operator=(Zone const&) = delete;

    ~Zone()
    {
      if (m_entered)
        detail::api()->leave();
    }

  private:
    bool m_entered = false;
  };
} // namespace mod_profiler
