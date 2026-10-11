#include "Symbols.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace
{
  // Not the real process handle: Geode's crash handler calls SymInitialize on that one, and a second
  // session on the same handle would break its stack traces. Modules are loaded from their files
  HANDLE const SESSION = reinterpret_cast<HANDLE>(0x5052'4f46);

  std::atomic<bool> s_enabled{true};
  bool s_ready = false;
  bool s_failed = false;
  std::unordered_set<uintptr_t> s_loaded;
  std::unordered_map<uintptr_t, std::string> s_names;

  std::string narrow(wchar_t const* text)
  {
    auto size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
      return {};
    std::string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
  }

  bool ready()
  {
    if (s_ready || s_failed)
      return s_ready;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    // No symbol server: only the PDB next to the module or where it was built
    s_ready = SymInitializeW(SESSION, L"", FALSE);
    s_failed = !s_ready;
    return s_ready;
  }

  void load(MappedModule const& module)
  {
    if (!s_loaded.insert(module.base).second || !module.handle)
      return;
    wchar_t path[MAX_PATH * 2];
    auto length = GetModuleFileNameW(static_cast<HMODULE>(module.handle), path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path))
      return;
    SymLoadModuleExW(SESSION, nullptr, path, nullptr, module.base, static_cast<DWORD>(module.size), nullptr, 0);
  }
} // namespace

std::string symbols::name(uintptr_t address, MappedModule const& module)
{
  if (!s_enabled.load(std::memory_order_relaxed) || !ready())
    return {};
  if (auto known = s_names.find(address); known != s_names.end())
    return known->second;

  load(module);
  alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)] = {};
  auto symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
  symbol->MaxNameLen = MAX_SYM_NAME;
  DWORD64 displacement = 0;
  std::string name;
  if (SymFromAddrW(SESSION, address, &displacement, symbol))
  {
    name = narrow(symbol->Name);
    // An export far before the address is some other function: without a PDB, exports are all there is
    if (displacement > 0x4000)
      name.clear();
    else if (displacement)
    {
      char offset[32];
      std::snprintf(offset, sizeof(offset), "+0x%llx", static_cast<unsigned long long>(displacement));
      name += offset;
    }
  }
  s_names[address] = name;
  return name;
}

void symbols::setEnabled(bool enabled) { s_enabled = enabled; }
