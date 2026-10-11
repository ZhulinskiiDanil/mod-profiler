#include "Symbols.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>

#include <ShlObj.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
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
  // Where copies of PDBs from build folders go, this session's own
  std::wstring s_copies;
  // When a name was last looked up in the PDBs
  std::chrono::steady_clock::time_point s_lastUse;
  constexpr auto KEEP_LOADED = std::chrono::seconds(30);

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

  std::wstring widen(char const* text)
  {
    auto size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 1)
      return {};
    std::wstring result(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), size);
    return result;
  }

  std::wstring folderOf(std::wstring const& path) { return path.substr(0, path.find_last_of(L"\\/") + 1); }

  // The PDB path the linker wrote into the image (its CodeView record), or empty
  std::wstring codeViewPath(MappedModule const& module)
  {
    auto base = module.base;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<uint64_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > module.size)
      return {};
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
      return {};
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG || !directory.VirtualAddress ||
        directory.VirtualAddress + static_cast<uint64_t>(directory.Size) > module.size)
      return {};
    auto entries = reinterpret_cast<IMAGE_DEBUG_DIRECTORY const*>(base + directory.VirtualAddress);
    for (size_t i = 0; i < directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY); i++)
    {
      auto const& entry = entries[i];
      // "RSDS", a GUID, an age, then the path
      if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || !entry.AddressOfRawData || entry.SizeOfData < 25 ||
          entry.AddressOfRawData + static_cast<uint64_t>(entry.SizeOfData) > module.size)
        continue;
      auto data = reinterpret_cast<char const*>(base + entry.AddressOfRawData);
      if (std::memcmp(data, "RSDS", 4) != 0 || data[entry.SizeOfData - 1] != '\0')
        continue;
      return widen(data + 24);
    }
    return {};
  }

  // A PDB in a folder that isn't the module's (where a mod was built) is copied, once, and the copy is
  // what DbgHelp opens: it keeps a PDB open, and a mod rebuilt while the game runs couldn't write its own
  // The copies made this session, by module: loaded again after an unload, they aren't copied again
  std::unordered_map<uintptr_t, std::wstring> s_copied;
  // Modules prepare looked at already
  std::unordered_set<uintptr_t> s_seen;

  enum class Pdb
  {
    // Next to the DLL, or none: loaded as it is
    InPlace,
    // From a build folder, copied: loaded from the copy
    Copied,
    // From a build folder, the copy failed: not loaded at all (exports only), it would be locked
    Skip,
  };

  bool isFolder(std::wstring const& path)
  {
    auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
  }

  // A PDB in a folder that isn't the module's (where a mod was built) is copied, once, and the copy is
  // what DbgHelp opens: it keeps a PDB open, and a mod rebuilt while the game runs couldn't write its own.
  // Done when the module is first seen, before the mod can be rebuilt, so the copy matches the DLL
  Pdb copyOfBuildPdb(MappedModule const& module, std::wstring const& modulePath, std::wstring& folderOut)
  {
    if (auto known = s_copied.find(module.base); known != s_copied.end())
    {
      // Removed meanwhile (another game's clean-up): exports only from now on
      if (!isFolder(known->second))
        return Pdb::Skip;
      folderOut = known->second;
      return Pdb::Copied;
    }
    auto pdb = codeViewPath(module);
    if (pdb.empty() || GetFileAttributesW(pdb.c_str()) == INVALID_FILE_ATTRIBUTES ||
        _wcsicmp(folderOf(pdb).c_str(), folderOf(modulePath).c_str()) == 0)
      return Pdb::InPlace;
    if (s_copies.empty())
    {
      wchar_t temp[MAX_PATH];
      if (!GetTempPathW(MAX_PATH, temp))
        return Pdb::Skip;
      s_copies = std::wstring(temp) + L"zhulis.mod-profiler\\pdb-" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    }
    // One folder a module, named like the PDB inside: DbgHelp finds it there by name
    char base[32];
    std::snprintf(base, sizeof(base), "%llx", static_cast<unsigned long long>(module.base));
    auto folder = s_copies + widen(base) + L"\\";
    auto made = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS)
      return Pdb::Skip;
    auto copy = folder + pdb.substr(pdb.find_last_of(L"\\/") + 1);
    if (!CopyFileW(pdb.c_str(), copy.c_str(), FALSE))
      return Pdb::Skip;
    s_copied[module.base] = folder;
    folderOut = folder;
    return Pdb::Copied;
  }

  bool modulePath(MappedModule const& module, std::wstring& out)
  {
    wchar_t path[MAX_PATH * 2];
    auto length = GetModuleFileNameW(static_cast<HMODULE>(module.handle), path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path))
      return false;
    out.assign(path, length);
    return true;
  }

  void load(MappedModule const& module)
  {
    if (!s_loaded.insert(module.base).second || !module.handle)
      return;
    std::wstring path;
    if (!modulePath(module, path))
      return;

    std::wstring copied;
    auto pdb = copyOfBuildPdb(module, path, copied);
    if (pdb == Pdb::InPlace)
    {
      SymLoadModuleExW(SESSION, nullptr, path.c_str(), nullptr, module.base, static_cast<DWORD>(module.size), nullptr, 0);
      return;
    }
    // The copy only (or no PDB: the build folder's would be locked): the path in the image is ignored,
    // the copy's folder searched, and loaded right now (deferred, it would be looked for later with other
    // options). The options are DbgHelp's for the whole process: changed for this call only
    auto options = SymGetOptions();
    SymSetOptions((options | SYMOPT_IGNORE_CVREC) & ~SYMOPT_DEFERRED_LOADS);
    SymSetSearchPathW(SESSION, pdb == Pdb::Copied ? copied.c_str() : s_copies.c_str());
    SymLoadModuleExW(SESSION, nullptr, path.c_str(), nullptr, module.base, static_cast<DWORD>(module.size), nullptr, 0);
    SymSetSearchPathW(SESSION, L"");
    SymSetOptions(options);
  }
} // namespace

std::string symbols::name(uintptr_t address, MappedModule const& module)
{
  if (!s_enabled.load(std::memory_order_relaxed) || !ready())
    return {};
  // A name looked up again counts as a use: the PDBs stay while the table needs names
  s_lastUse = std::chrono::steady_clock::now();
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

void symbols::prepare(ModuleMap const& map)
{
  if (!s_enabled.load(std::memory_order_relaxed))
    return;
  for (auto const& module : map.modules())
  {
    if (!module.handle || s_copied.contains(module.base) || !s_seen.insert(module.base).second)
      continue;
    std::wstring path, folder;
    if (modulePath(module, path))
      copyOfBuildPdb(module, path, folder);
  }
}

void symbols::tick()
{
  if (!s_ready || s_loaded.empty() || std::chrono::steady_clock::now() - s_lastUse < KEEP_LOADED)
    return;
  for (auto base : s_loaded)
    SymUnloadModule64(SESSION, base);
  s_loaded.clear();
}

void symbols::cleanUp()
{
  // Copies from earlier sessions: this one's folder is made again when needed
  wchar_t temp[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, temp))
    return;
  auto root = std::wstring(temp) + L"zhulis.mod-profiler\\";
  auto realFolder = [](WIN32_FIND_DATAW const& found)
  {
    // Junctions and links are never followed: only what's really in the folder goes
    return (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           found.cFileName[0] != L'.';
  };
  auto stillRunning = [](std::wstring const& name)
  {
    // pdb-<pid>: a game still running has its copies in use
    auto pid = static_cast<DWORD>(std::wcstoul(name.c_str() + 4, nullptr, 10));
    if (!pid || pid == GetCurrentProcessId())
      return pid != 0 && pid == GetCurrentProcessId();
    auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
      return false;
    DWORD code = 0;
    bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
  };

  WIN32_FIND_DATAW found;
  auto search = FindFirstFileW((root + L"pdb-*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE)
    return;
  do
  {
    if (!realFolder(found) || stillRunning(found.cFileName))
      continue;
    auto folder = root + found.cFileName + L"\\";
    WIN32_FIND_DATAW inner;
    auto modules = FindFirstFileW((folder + L"*").c_str(), &inner);
    if (modules != INVALID_HANDLE_VALUE)
    {
      do
      {
        if (!realFolder(inner))
          continue;
        auto moduleFolder = folder + inner.cFileName + L"\\";
        WIN32_FIND_DATAW file;
        auto files = FindFirstFileW((moduleFolder + L"*").c_str(), &file);
        if (files != INVALID_HANDLE_VALUE)
        {
          do
          {
            // Exactly .pdb, not a short-name match of something longer, and only files
            std::wstring name = file.cFileName;
            bool pdb = name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".pdb") == 0;
            if (pdb && !(file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
              DeleteFileW((moduleFolder + name).c_str());
          } while (FindNextFileW(files, &file));
          FindClose(files);
        }
        RemoveDirectoryW(moduleFolder.c_str());
      } while (FindNextFileW(modules, &inner));
      FindClose(modules);
    }
    RemoveDirectoryW(folder.c_str());
  } while (FindNextFileW(search, &found));
  FindClose(search);
}
