// The other side of include/ModProfiler.hpp: other mods' zones land here
#include "../include/ModProfiler.hpp"

#include "ZonesApi.hpp"

#include <Geode/Geode.hpp>

#include <string>
#include <functional>
#include <thread>
#include <unordered_map>
#include <utility>

using namespace geode::prelude;

namespace
{
  ZoneStack s_stack;
  ZoneRegistry s_registry;
  std::thread::id s_mainThread;
  // The zone a (mod ID, name) pair of pointers was last interned as: zones are entered every frame, the
  // names repeat. The texts are kept too, in case a mod reuses a buffer for another name
  struct Cached
  {
    uint16_t id = 0;
    bool failed = false;
    std::string mod;
    std::string name;
  };
  struct PairHash
  {
    size_t operator()(std::pair<char const*, char const*> const& key) const
    {
      return std::hash<char const*>()(key.first) * 31 + std::hash<char const*>()(key.second);
    }
  };
  std::unordered_map<std::pair<char const*, char const*>, Cached, PairHash> s_cache;
  // Names in buffers that change make a new pair every time: past this the cache starts over
  constexpr size_t MAX_CACHED = 4096;

  bool enter(char const* modID, char const* name)
  {
    if (std::this_thread::get_id() != s_mainThread || !modID || !name)
      return false;
    if (s_cache.size() >= MAX_CACHED)
      s_cache.clear();
    auto& cached = s_cache[{modID, name}];
    if (cached.mod != modID || cached.name != name)
    {
      cached.mod = modID;
      cached.name = name;
      cached.id = s_registry.intern(modID, name);
      // Too many zones: not tried again every time
      cached.failed = cached.id == 0;
    }
    if (cached.failed)
      return false;
    s_stack.push(cached.id);
    return true;
  }

  void leave() { s_stack.pop(); }

  mod_profiler::detail::Api const s_api{enter, leave};
} // namespace

void zones::setup()
{
  s_mainThread = std::this_thread::get_id();
  Dispatch<mod_profiler::detail::Api const**>("zhulis.mod-profiler/api").listen([](mod_profiler::detail::Api const** out)
                                                                                {
                                                                                  *out = &s_api;
                                                                                  return ListenerResult::Stop; })
      .leak();
}

ZoneStack const& zones::stack() { return s_stack; }
ZoneRegistry const& zones::registry() { return s_registry; }
