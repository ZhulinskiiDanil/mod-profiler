#pragma once

// The process's loaded modules as a ModuleMap: their code, function tables and buckets. Windows only
#include "ModuleMap.hpp"

#include <filesystem>
#include <memory>
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

  // The loaded modules right now, sorted, to tell when a map is out of date
  std::vector<void*> loaded();

  // A map of the modules in `handles`, each pinned so it stays loaded while the map exists.
  // Mods' binaries get their mod's bucket, added to `buckets` if new
  std::unique_ptr<ModuleMap> build(std::vector<void*> const& handles, std::vector<ModBinary> const& mods, Buckets& buckets);

  // Unpins the modules of a map nothing reads anymore
  void release(ModuleMap const& map);
} // namespace modules
