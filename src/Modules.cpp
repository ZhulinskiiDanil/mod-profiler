#include "Modules.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <cwctype>
#include <string_view>
#include <unordered_map>

namespace
{
  std::wstring lower(std::wstring text)
  {
    for (auto& c : text)
      c = static_cast<wchar_t>(std::towlower(c));
    return text;
  }

  std::wstring pathOf(HMODULE module)
  {
    std::wstring path(MAX_PATH, L'\0');
    while (true)
    {
      auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
      if (length == 0)
        return {};
      if (length < path.size())
      {
        path.resize(length);
        return path;
      }
      path.resize(path.size() * 2);
    }
  }

  std::string narrow(std::wstring const& text)
  {
    if (text.empty())
      return {};
    auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
  }

  // A mod's binary is unpacked into geode\unzipped\<mod id>\: the folder names the mod even before
  // Geode's list of mods is there
  std::string modFromPath(std::wstring const& lowered)
  {
    static constexpr std::wstring_view marker = L"\\geode\\unzipped\\";
    auto at = lowered.find(marker);
    if (at == std::wstring::npos)
      return {};
    auto start = at + marker.size();
    auto end = lowered.find(L'\\', start);
    if (end == std::wstring::npos)
      return {};
    return narrow(lowered.substr(start, end - start));
  }

  // The mod's bucket, with its name from Geode once there is one
  uint32_t named(Buckets& buckets, std::string const& id, std::string const& name)
  {
    auto index = buckets.index(id, name);
    buckets.rename(index, name);
    return index;
  }

  // The mods by their binary's lowered path and by ID, made once for a build
  struct ModIndex
  {
    std::unordered_map<std::wstring, modules::ModBinary const*> byPath;
    std::unordered_map<std::string, modules::ModBinary const*> byID;

    explicit ModIndex(std::vector<modules::ModBinary> const& mods)
    {
      for (auto const& mod : mods)
      {
        if (!mod.path.empty())
          byPath[lower(mod.path.wstring())] = &mod;
        byID[mod.id] = &mod;
      }
    }
  };

  uint32_t bucketFor(std::wstring const& path, ModIndex const& mods, Buckets& buckets)
  {
    auto lowered = lower(path);
    auto file = lowered.substr(lowered.find_last_of(L"\\/") + 1);
    if (file == L"geometrydash.exe")
      return bucket::GAME;
    if (file == L"libcocos2d.dll")
      return bucket::COCOS;
    if (file == L"geode.dll")
      return buckets.index("geode.loader", "Geode");

    if (auto mod = mods.byPath.find(lowered); mod != mods.byPath.end())
      return named(buckets, mod->second->id, mod->second->name);

    auto id = modFromPath(lowered);
    if (!id.empty())
    {
      if (auto mod = mods.byID.find(id); mod != mods.byID.end())
        return named(buckets, mod->second->id, mod->second->name);
      return buckets.index(id, id);
    }
    return bucket::OTHER;
  }
} // namespace

std::optional<modules::LearnedFunction> modules::learn(uintptr_t address)
{
  DWORD64 base = 0;
  auto entry = RtlLookupFunctionEntry(address, &base, nullptr);
  // An entry pointing at another one only happens in images, which the map already has
  if (!entry || !base || (entry->UnwindData & 1))
    return std::nullopt;

  // Generated code lives in memory the hook library allocated: as much of it as is committed
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(reinterpret_cast<void*>(base), &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
      memory.Type == MEM_IMAGE)
    return std::nullopt;
  auto size = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize - static_cast<uintptr_t>(base);
  if (entry->EndAddress <= entry->BeginAddress || entry->EndAddress > size || entry->UnwindData >= size)
    return std::nullopt;

  return LearnedFunction{
      .base = static_cast<uintptr_t>(base),
      .size = size,
      .begin = entry->BeginAddress,
      .end = entry->EndAddress,
      .unwind = entry->UnwindData,
  };
}

std::vector<void*> modules::loaded()
{
  std::vector<HMODULE> handles(512);
  DWORD needed = 0;
  while (true)
  {
    auto bytes = static_cast<DWORD>(handles.size() * sizeof(HMODULE));
    if (!EnumProcessModulesEx(GetCurrentProcess(), handles.data(), bytes, &needed, LIST_MODULES_ALL))
      return {};
    if (needed <= bytes)
      break;
    handles.resize(needed / sizeof(HMODULE) + 32);
  }
  std::vector<void*> result(handles.begin(), handles.begin() + needed / sizeof(HMODULE));
  std::sort(result.begin(), result.end());
  return result;
}

std::unique_ptr<ModuleMap> modules::build(std::vector<void*> const& handles, std::vector<ModBinary> const& mods,
                                          std::vector<LearnedFunction> const& learned, Buckets& buckets)
{
  std::vector<MappedModule> mapped;
  std::vector<CodeRange> code;
  ModIndex index(mods);

  for (auto handle : handles)
  {
    // Pinned: the module can't be unloaded until release(), so its tables stay readable
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, static_cast<LPCWSTR>(handle), &pinned) || pinned != handle)
    {
      if (pinned)
        FreeLibrary(pinned);
      continue;
    }

    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), pinned, &info, sizeof(info)))
    {
      FreeLibrary(pinned);
      continue;
    }

    auto base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
      FreeLibrary(pinned);
      continue;
    }

    auto path = pathOf(pinned);
    MappedModule module{
        .base = base,
        .size = info.SizeOfImage,
        .bucket = bucketFor(path, index, buckets),
        .name = narrow(path.substr(path.find_last_of(L"\\/") + 1)),
        .handle = pinned,
    };

    auto const& exceptions = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION && exceptions.VirtualAddress &&
        exceptions.VirtualAddress + static_cast<uint64_t>(exceptions.Size) <= info.SizeOfImage)
    {
      module.functions = reinterpret_cast<uint32_t const*>(base + exceptions.VirtualAddress);
      module.functionCount = exceptions.Size / sizeof(RUNTIME_FUNCTION);
    }

    auto index = static_cast<uint32_t>(mapped.size());
    auto section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, section++)
    {
      if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
        continue;
      auto begin = base + section->VirtualAddress;
      auto size = std::max<uintptr_t>(section->Misc.VirtualSize, 1);
      code.push_back({begin, std::min(begin + size, base + info.SizeOfImage), index});
    }

    mapped.push_back(std::move(module));
  }

  // Generated code, one module for each block of it, with the functions learned in it
  auto sorted = learned;
  std::sort(sorted.begin(), sorted.end(), [](auto const& a, auto const& b)
            { return a.base != b.base ? a.base < b.base : a.begin < b.begin; });
  for (size_t i = 0; i < sorted.size();)
  {
    auto index = static_cast<uint32_t>(mapped.size());
    MappedModule block{.base = sorted[i].base, .size = sorted[i].size, .bucket = bucket::HOOKS, .name = "hooks"};
    for (; i < sorted.size() && sorted[i].base == block.base; i++)
    {
      auto const& function = sorted[i];
      // The same function learned twice, from two addresses in it
      if (!block.ownFunctions.empty() && block.ownFunctions[block.ownFunctions.size() - 3] == function.begin)
        continue;
      block.ownFunctions.insert(block.ownFunctions.end(), {function.begin, function.end, function.unwind});
      code.push_back({block.base + function.begin, block.base + function.end, index});
    }
    mapped.push_back(std::move(block));
  }

  return std::make_unique<ModuleMap>(std::move(mapped), std::move(code));
}

std::vector<std::pair<uintptr_t, uintptr_t>> modules::waitFunctions()
{
  // A stub is a few instructions around `syscall`: the thread is right after it while in the kernel
  constexpr uintptr_t STUB_SIZE = 0x20;
  static constexpr std::pair<wchar_t const*, char const*> functions[] = {
      {L"ntdll.dll", "NtWaitForSingleObject"},
      {L"ntdll.dll", "NtWaitForMultipleObjects"},
      {L"ntdll.dll", "NtDelayExecution"},
      {L"ntdll.dll", "NtWaitForAlertByThreadId"},
      {L"ntdll.dll", "NtWaitForKeyedEvent"},
      {L"ntdll.dll", "NtSignalAndWaitForSingleObject"},
      {L"ntdll.dll", "NtRemoveIoCompletion"},
      {L"ntdll.dll", "NtRemoveIoCompletionEx"},
      {L"win32u.dll", "NtUserMsgWaitForMultipleObjectsEx"},
      {L"win32u.dll", "NtUserWaitMessage"},
      {L"win32u.dll", "NtUserGetMessage"},
      {L"win32u.dll", "NtGdiDdDDIWaitForVerticalBlankEvent"},
      {L"win32u.dll", "NtGdiDdDDIWaitForVerticalBlankEvent2"},
      {L"win32u.dll", "NtGdiDdDDIWaitForSynchronizationObjectFromCpu"},
  };
  std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
  for (auto [library, name] : functions)
  {
    auto module = GetModuleHandleW(library);
    if (!module)
      continue;
    if (auto address = reinterpret_cast<uintptr_t>(GetProcAddress(module, name)))
      ranges.push_back({address, address + STUB_SIZE});
  }
  return ranges;
}

void modules::release(ModuleMap const& map)
{
  for (auto const& module : map.modules())
    if (module.handle)
      FreeLibrary(static_cast<HMODULE>(module.handle));
}
