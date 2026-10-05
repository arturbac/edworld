// edworld — the d3d11.dll proxy itself: every export forwarded to the next d3d11 in the chain,
// device creation wrapped so the device can be watched.
//
// The chaining discipline is EDVR's (MIT, characterecho-sean/edvr-unofficial-patch, src/d3d11/d3d11_proxy.cpp):
//  - DllMain loads only the system copy (already mapped, no foreign DllMain under the loader lock);
//  - the next proxy is loaded on the first export call;
//  - a chained 3Dmigoto asks for "d3d11.dll" by name, which is us: the second entry on the same thread
//    goes to the system copy, or the chain recurses until the stack runs out.
#include "runtime.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
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
    if(path.empty())
      {
      edworld::log_line("chain: next is the system d3d11.dll");
      return;
      }
    if(path.find(L':') == std::wstring::npos and path.find(L'\\') == std::wstring::npos)
      path = module_dir + L"\\" + path;
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(self_module, self, MAX_PATH);
    wchar_t want[MAX_PATH]{};
    if(GetFullPathNameW(path.c_str(), MAX_PATH, want, nullptr) and _wcsicmp(self, want) == 0)
      {
      edworld::log_line("chain: next points at edworld itself, ignored; using the system d3d11.dll");
      return;
      }
    HMODULE const chained{LoadLibraryW(path.c_str())};
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
    edworld::log_line("chain: through %S, %zu export(s) unresolved", path.c_str(), missing);
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
    edworld::log_line("edworld %s, read-only d3d11 observer", EDWORLD_VERSION);
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
  }  // namespace

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
  HRESULT const hr{target(adapter, driver_type, software, flags, levels, level_count, sdk, device, level, context)};
  if(SUCCEEDED(hr) and device and *device and not depth.reentrant())
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
  HRESULT const hr{
    target(adapter, driver_type, software, flags, levels, level_count, sdk, swap_desc, swap, device, level, context)
  };
  if(SUCCEEDED(hr) and device and *device and not depth.reentrant())
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
