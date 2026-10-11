#pragma once

// The process's loaded modules as a ModuleMap: their code, function tables and buckets. Windows only
#include "ModuleMap.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace modules
{
  struct ModBinary
  {
    std::string id;
    std::string name;
    std::filesystem::path path;
  };

  // A function of generated code (a hook handler or trampoline) with its unwind info, from the
  // tables registered with RtlInstallFunctionTableCallback. RVAs are from `base`
  struct LearnedFunction
  {
    uintptr_t base = 0;
    // How much memory from `base` on is there to read
    uintptr_t size = 0;
    uint32_t begin = 0;
    uint32_t end = 0;
    uint32_t unwind = 0;
  };

  // Asks Windows for the function at an address outside every module. Call on the main thread:
  // Geode's callback for its hooks reads a map that only the main thread writes to (when hooking)
  std::optional<LearnedFunction> learn(uintptr_t address);

  // The loaded modules right now, sorted, to tell when a map is out of date
  std::vector<void*> loaded();

  // A map of the modules in `handles`, each pinned so it stays loaded while the map exists, and of
  // the learned generated code. Mods' binaries get their mod's bucket, added to `buckets` if new
  std::unique_ptr<ModuleMap> build(std::vector<void*> const& handles, std::vector<ModBinary> const& mods,
                                   std::vector<LearnedFunction> const& learned, Buckets& buckets);

  // Unpins the modules of a map nothing reads anymore
  void release(ModuleMap const& map);

  // The system call stubs a waiting thread sits in (sleeping, waiting for an event, a message, vsync)
  std::vector<std::pair<uintptr_t, uintptr_t>> waitFunctions();
  // Of those, the ones that wait on a lock or an object (another thread), not for time or a message
  std::vector<std::pair<uintptr_t, uintptr_t>> lockWaitFunctions();
} // namespace modules
