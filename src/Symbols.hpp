#pragma once

// Function names from PDBs and export tables, with DbgHelp. Windows only, and only ever from the
// analyzer thread: DbgHelp isn't thread safe
#include "ModuleMap.hpp"

#include <string>

namespace symbols
{
  // The name of the function at `address` in `module`, or empty when no PDB or export names it.
  // A module's PDB is loaded the first time one of its addresses is asked about
  std::string name(uintptr_t address, MappedModule const& module);
  void setEnabled(bool enabled);
  // Removes the PDB copies of earlier sessions (a PDB from a build folder is read from a copy, so a mod
  // can be rebuilt while the game runs). Any thread, before the first name
  void cleanUp();
} // namespace symbols
