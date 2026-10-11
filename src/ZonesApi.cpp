// The other side of include/ModProfiler.hpp: other mods' zones land here
#include "../include/ModProfiler.hpp"

#include "ZonesApi.hpp"

#include <Geode/Geode.hpp>

#include <string>
#include <thread>
#include <unordered_map>

using namespace geode::prelude;

namespace
{
  ZoneStack s_stack;
  ZoneRegistry s_registry;
  std::thread::id s_mainThread;
  // The zone a name pointer was last interned as: zones are entered every frame, the names repeat. The
  // name is kept too, in case a mod reuses a buffer for another name
  struct Cached
  {
    uint16_t id = 0;
    std::string name;
  };
  std::unordered_map<char const*, Cached> s_byPointer;

  bool enter(char const* modID, char const* name)
  {
    if (std::this_thread::get_id() != s_mainThread || !modID || !name)
      return false;
    auto& cached = s_byPointer[name];
    if (cached.id == 0 || cached.name != name)
    {
      cached.id = s_registry.intern(modID, name);
      cached.name = name;
    }
    if (cached.id == 0)
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
