// edworld — the d3d11.dll proxy itself: every export forwarded to the next d3d11 in the chain,
// device creation wrapped so the device can be watched.
//
// The chaining discipline is EDVR's (MIT, characterecho-sean/edvr-unofficial-patch, src/d3d11/d3d11_proxy.cpp):
//  - DllMain loads only the system copy (already mapped, no foreign DllMain under the loader lock);
//  - the next proxy is loaded on the first export call;
//  - a chained 3Dmigoto asks for "d3d11.dll" by name, which is this proxy: the second entry on the same thread
//    goes to the system copy, or the chain recurses until the stack runs out.
#include "runtime.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <iterator>
#include <string_view>
#include <mutex>

extern "C"
  {
  extern void * edvr_realProcs_d3d11[];
  void edvr_unresolved_d3d11();
  }

namespace
  {
  char const * const export_names[]{
#include "edvr_exports_d3d11.inc"
  };
  constexpr std::size_t export_count{sizeof(export_names) / sizeof(export_names[0])};

  using create_device_fn = HRESULT(WINAPI *)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, D3D_FEATURE_LEVEL const *, UINT, UINT, ID3D11Device **,
    D3D_FEATURE_LEVEL *, ID3D11DeviceContext **
  );
  using create_device_swap_fn = HRESULT(WINAPI *)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, D3D_FEATURE_LEVEL const *, UINT, UINT,
    DXGI_SWAP_CHAIN_DESC const *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **
  );

  HMODULE self_module{};
  HMODULE system_module{};
  std::wstring module_dir;
  create_device_fn system_create{};
  create_device_swap_fn system_create_swap{};
  create_device_fn next_create{};
  create_device_swap_fn next_create_swap{};

  auto resolve_from(HMODULE preferred, HMODULE fallback) noexcept -> std::size_t
    {
    std::size_t missing{};
    for(std::size_t i{}; i != export_count; ++i)
      {
      void * p{preferred ? reinterpret_cast<void *>(GetProcAddress(preferred, export_names[i])) : nullptr};
      if(not p and fallback)
        p = reinterpret_cast<void *>(GetProcAddress(fallback, export_names[i]));
      if(not p)
        {
        ++missing;
        p = reinterpret_cast<void *>(&edvr_unresolved_d3d11);
        }
      edvr_realProcs_d3d11[i] = p;
      }
    return missing;
    }

  auto loader_phase() noexcept -> void
    {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self_module, path, MAX_PATH);
    module_dir = path;
    if(auto const slash{module_dir.find_last_of(L"\\/")}; slash != std::wstring::npos)
      module_dir.resize(slash);
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring const system_path{std::wstring{sys} + L"\\d3d11.dll"};
    system_module = LoadLibraryW(system_path.c_str());
    resolve_from(system_module, nullptr);
    if(system_module)
      {
      system_create = reinterpret_cast<create_device_fn>(GetProcAddress(system_module, "D3D11CreateDevice"));
      system_create_swap
        = reinterpret_cast<create_device_swap_fn>(GetProcAddress(system_module, "D3D11CreateDeviceAndSwapChain"));
      }
    next_create = system_create;
    next_create_swap = system_create_swap;
    }

  auto chain_next() noexcept -> void
    {
    std::wstring path{edworld::settings().next};
    // under edloader the way on is always edloader's list; a player has nothing to set for it
    if(path.empty() and GetEnvironmentVariableW(L"EDLOADER_CONFIG_DIR", nullptr, 0) != 0)
      path = L"d3d11.dll";
    if(path.empty())
      {
      edworld::log_line("chain: next is the system d3d11.dll");
      return;
      }
    // `d3d11.dll` alone is asked for by name: under edloader that is edloader, which passes the call on down its
    // list; another bare name is a file beside edworld
    bool const by_name{_wcsicmp(path.c_str(), L"d3d11.dll") == 0};
    if(not by_name and path.find(L':') == std::wstring::npos and path.find(L'\\') == std::wstring::npos)
      path = module_dir + L"\\" + path;
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(self_module, self, MAX_PATH);
    wchar_t want[MAX_PATH]{};
    if(not by_name and GetFullPathNameW(path.c_str(), MAX_PATH, want, nullptr) and _wcsicmp(self, want) == 0)
      {
      edworld::log_line("chain: next points at edworld itself, ignored; using the system d3d11.dll");
      return;
      }
    HMODULE const chained{LoadLibraryW(path.c_str())};
    if(by_name and chained == self_module)
      {
      // edworld is the game's d3d11.dll itself: no loader in front of it
      edworld::log_line("chain: next = d3d11.dll is edworld itself (no edloader); using the system d3d11.dll");
      FreeLibrary(chained);
      return;
      }
    if(not chained)
      {
      edworld::log_line("chain: cannot load %S (error %lu); using the system d3d11.dll", path.c_str(), GetLastError());
      return;
      }
    auto const create{reinterpret_cast<create_device_fn>(GetProcAddress(chained, "D3D11CreateDevice"))};
    if(not create)
      {
      edworld::log_line("chain: %S exports no D3D11CreateDevice, ignored", path.c_str());
      FreeLibrary(chained);
      return;
      }
    std::size_t const missing{resolve_from(chained, system_module)};
    next_create = create;
    if(auto const swap{reinterpret_cast<create_device_swap_fn>(GetProcAddress(chained, "D3D11CreateDeviceAndSwapChain"))})
      next_create_swap = swap;
    wchar_t loaded[MAX_PATH]{};
    GetModuleFileNameW(chained, loaded, MAX_PATH);
    edworld::log_line("chain: through %S (%S), %zu export(s) unresolved", path.c_str(), loaded, missing);
    }

  INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

  ///\brief a directory edloader hands its plugins (EDLOADER_CONFIG_DIR, EDLOADER_LOG_DIR); without edloader,
  /// the dll's own
  auto directory_from(wchar_t const * variable) -> std::wstring
    {
    wchar_t value[MAX_PATH]{};
    DWORD const n{GetEnvironmentVariableW(variable, value, MAX_PATH)};
    return n != 0 and n < MAX_PATH ? std::wstring{value, n} : module_dir;
    }

  BOOL CALLBACK init_callback(PINIT_ONCE, PVOID, PVOID *)
    {
    std::wstring const log_dir{directory_from(L"EDLOADER_LOG_DIR")};
    edworld::load_settings(directory_from(L"EDLOADER_CONFIG_DIR"), log_dir);
    edworld::log_open(log_dir);
    edworld::log_line("%S %s, read-only d3d11 observer", edworld::plugin_name, EDWORLD_VERSION);
    edworld::log_machine();
    edworld::complete_settings(directory_from(L"EDLOADER_CONFIG_DIR"));
    edworld::write_settings_doc();
    chain_next();
    return TRUE;
    }

  auto ensure_initialised() noexcept -> void { InitOnceExecuteOnce(&init_once, init_callback, nullptr, nullptr); }

  thread_local int create_depth{};

  struct depth_guard_t
    {
    depth_guard_t() noexcept { ++create_depth; }

    ~depth_guard_t() { --create_depth; }

    [[nodiscard]]
    auto reentrant() const noexcept -> bool
      {
      return create_depth > 1;
      }
    };

  bool loop_reported{};

  enum struct creator_e : std::uint8_t
    {
    game,     ///< the game's exe or a module beside it
    foreign,  ///< a module elsewhere: an overlay (Epic's, Discord's, Steam's) making a device of its own
    unknown   ///< nothing on the stack told: taken as the game's, as before
    };

  struct creator_t
    {
    creator_e kind{creator_e::unknown};
    char name[MAX_PATH]{"unknown"};
    };

  ///\brief the folder of a module's file, with its last backslash
  auto folder_of(wchar_t const * path) noexcept -> std::wstring_view
    {
    std::wstring_view const p{path};
    auto const slash{p.find_last_of(L"\\/")};
    return slash == std::wstring_view::npos ? std::wstring_view{} : p.substr(0, slash + 1);
    }

  ///\brief who asks for a device: the first module on the stack that is neither edworld nor a d3d11 proxy (one that
  /// exports D3D11CreateDevice: edloader, EDHM, EDVR, the system copy); code outside any module (a hook's trampoline) is
  /// passed over. The game's when that module lies in the game exe's folder, foreign elsewhere
  auto find_creator() noexcept -> creator_t
    {
    creator_t c{};
    void * frames[48]{};
    USHORT const n{RtlCaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)), frames, nullptr)};
    for(USHORT i{}; i != n; ++i)
      {
      HMODULE m{};
      if(not GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(frames[i]), &m)
         or not m)
        continue;
      if(m == self_module or GetProcAddress(m, "D3D11CreateDevice"))
        continue;
      wchar_t path[MAX_PATH]{};
      wchar_t exe[MAX_PATH]{};
      if(not GetModuleFileNameW(m, path, MAX_PATH) or not GetModuleFileNameW(nullptr, exe, MAX_PATH))
        return c;
      std::wstring_view const folder{folder_of(path)};
      std::wstring_view const exe_folder{folder_of(exe)};
      bool const beside_game{not exe_folder.empty() and folder.size() == exe_folder.size()
                             and CompareStringOrdinal(folder.data(), static_cast<int>(folder.size()), exe_folder.data(),
                                                      static_cast<int>(exe_folder.size()), TRUE)
                                   == CSTR_EQUAL};
      c.kind = beside_game ? creator_e::game : creator_e::foreign;
      WideCharToMultiByte(CP_UTF8, 0, path + folder.size(), -1, c.name, sizeof c.name - 1, nullptr, nullptr);
      return c;
      }
    return c;
    }

  ///\brief the first few device creations, to see which of them made the device edworld attaches to, and who asked
  auto log_create(char const * what, HRESULT hr, bool asked, bool made, bool nested, creator_t const & by) noexcept -> void
    {
    static int told{};
    if(told++ < 8 or (made and by.kind == creator_e::foreign))
      edworld::log_line("%s: hr 0x%08lX, device %s%s, asked by %s%s", what, static_cast<unsigned long>(hr),
                        made ? "made" : asked ? "asked, none made" : "not asked", nested ? " (nested, not attached)" : "",
                        by.name, by.kind == creator_e::foreign ? " from outside the game's folder: not followed" : "");
    }
  }  // namespace

auto edworld::system_d3d11() noexcept -> HMODULE { return system_module; }

extern "C" HRESULT WINAPI edvr_impl_D3D11CreateDevice(
  IDXGIAdapter * adapter,
  D3D_DRIVER_TYPE driver_type,
  HMODULE software,
  UINT flags,
  D3D_FEATURE_LEVEL const * levels,
  UINT level_count,
  UINT sdk,
  ID3D11Device ** device,
  D3D_FEATURE_LEVEL * level,
  ID3D11DeviceContext ** context
)
  {
  ensure_initialised();
  depth_guard_t const depth;
  create_device_fn target{next_create};
  if(depth.reentrant())
    {
    if(not loop_reported)
      {
      loop_reported = true;
      edworld::log_line("chain: the next proxy called d3d11.dll by name, routed to the system copy");
      }
    target = system_create;
    }
  if(not target)
    return E_FAIL;
  creator_t const by{find_creator()};
  HRESULT const hr{target(adapter, driver_type, software, flags, levels, level_count, sdk, device, level, context)};
  bool const made{SUCCEEDED(hr) and device and *device};
  log_create("D3D11CreateDevice", hr, device != nullptr, made, depth.reentrant(), by);
  if(made and not depth.reentrant() and by.kind != creator_e::foreign)
    edworld::attach_to_device(*device);
  return hr;
  }

extern "C" HRESULT WINAPI edvr_impl_D3D11CreateDeviceAndSwapChain(
  IDXGIAdapter * adapter,
  D3D_DRIVER_TYPE driver_type,
  HMODULE software,
  UINT flags,
  D3D_FEATURE_LEVEL const * levels,
  UINT level_count,
  UINT sdk,
  DXGI_SWAP_CHAIN_DESC const * swap_desc,
  IDXGISwapChain ** swap,
  ID3D11Device ** device,
  D3D_FEATURE_LEVEL * level,
  ID3D11DeviceContext ** context
)
  {
  ensure_initialised();
  depth_guard_t const depth;
  create_device_swap_fn target{depth.reentrant() ? system_create_swap : next_create_swap};
  if(not target)
    return E_FAIL;
  creator_t const by{find_creator()};
  HRESULT const hr{
    target(adapter, driver_type, software, flags, levels, level_count, sdk, swap_desc, swap, device, level, context)
  };
  bool const made{SUCCEEDED(hr) and device and *device};
  log_create("D3D11CreateDeviceAndSwapChain", hr, device != nullptr, made, depth.reentrant(), by);
  if(made and not depth.reentrant() and by.kind != creator_e::foreign)
    edworld::attach_to_device(*device);
  return hr;
  }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
  {
  if(reason == DLL_PROCESS_ATTACH)
    {
    self_module = instance;
    DisableThreadLibraryCalls(instance);
    loader_phase();
    }
  return TRUE;
  }
