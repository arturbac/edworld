// edworld — what a player's log says about the machine the game runs on: Windows or Wine, the processor, the memory,
// the graphics cards with their drivers and displays, the game's graphics options; and for each device the game makes,
// the card it stands on. Names and numbers only: no path and no user name goes into the log
#include "runtime.h"

#include <dxgi.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <exception>
#include <string>
#include <string_view>

namespace edworld
  {
  namespace
    {
    ///\brief a wide text as UTF-8 in a fixed buffer, trailing blanks cut (processor names are padded)
    struct narrow_t
      {
      char text[256]{};

      explicit narrow_t(wchar_t const * wide) noexcept
        {
        if(not wide or WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, sizeof text - 1, nullptr, nullptr) == 0)
          std::snprintf(text, sizeof text, "unknown");
        std::size_t n{std::strlen(text)};
        while(n != 0 and (text[n - 1] == ' ' or text[n - 1] == '\t'))
          text[--n] = '\0';
        }
      };

    auto log_system() noexcept -> void
      {
      HMODULE const ntdll{GetModuleHandleW(L"ntdll.dll")};
      using rtl_get_version_fn = LONG(WINAPI *)(RTL_OSVERSIONINFOW *);
      using wine_version_fn = char const *(CDECL *)();
      using wine_host_fn = void(CDECL *)(char const **, char const **);
      auto const get_version{
        ntdll ? reinterpret_cast<rtl_get_version_fn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr
      };
      auto const wine{ntdll ? reinterpret_cast<wine_version_fn>(GetProcAddress(ntdll, "wine_get_version")) : nullptr};
      auto const host{ntdll ? reinterpret_cast<wine_host_fn>(GetProcAddress(ntdll, "wine_get_host_version")) : nullptr};
      RTL_OSVERSIONINFOW v{};
      v.dwOSVersionInfoSize = sizeof v;
      if(get_version)
        get_version(&v);
      if(wine)
        {
        char const * system{"unknown"};
        char const * release{""};
        if(host)
          host(&system, &release);
        log_line(
          "machine: Wine %s on %s %s, seen as Windows %lu.%lu.%lu",
          wine(),
          system,
          release,
          v.dwMajorVersion,
          v.dwMinorVersion,
          v.dwBuildNumber
        );
        }
      else
        log_line("machine: Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);

      wchar_t name[128]{};
      DWORD bytes{sizeof name};
      if(RegGetValueW(
           HKEY_LOCAL_MACHINE,
           L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
           L"ProcessorNameString",
           RRF_RT_REG_SZ,
           nullptr,
           name,
           &bytes
         )
         != ERROR_SUCCESS)
        name[0] = L'\0';
      MEMORYSTATUSEX memory{};
      memory.dwLength = sizeof memory;
      if(not GlobalMemoryStatusEx(&memory))
        memory.ullTotalPhys = 0;
      log_line(
        "machine: processor %s, %lu logical; memory %.1f GB",
        name[0] ? narrow_t{name}.text : "unknown",
        GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
        static_cast<double>(memory.ullTotalPhys) / 1073741824.0
      );
      }

    auto log_cards() noexcept -> void
      {
      IDXGIFactory1 * factory{};
      if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) or not factory)
        {
        log_line("machine: the graphics cards could not be listed");
        return;
        }
      IDXGIAdapter1 * adapter{};
      for(UINT i{}; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i)
        {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        LARGE_INTEGER umd{};
        char driver[48]{"unknown"};
        if(SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
          std::snprintf(
            driver,
            sizeof driver,
            "%u.%u.%u.%u",
            HIWORD(static_cast<DWORD>(umd.HighPart)),
            LOWORD(static_cast<DWORD>(umd.HighPart)),
            HIWORD(umd.LowPart),
            LOWORD(umd.LowPart)
          );
        log_line(
          "machine: card %u: %s (vendor %04X device %04X)%s, %llu MB own memory, driver %s",
          i,
          narrow_t{d.Description}.text,
          d.VendorId,
          d.DeviceId,
          (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? ", software" : "",
          static_cast<unsigned long long>(d.DedicatedVideoMemory / 1048576u),
          driver
        );
        IDXGIOutput * output{};
        for(UINT o{}; SUCCEEDED(adapter->EnumOutputs(o, &output)); ++o)
          {
          DXGI_OUTPUT_DESC od{};
          output->GetDesc(&od);
          DEVMODEW mode{};
          mode.dmSize = sizeof mode;
          bool const have{EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &mode) != FALSE};
          RECT const & r{od.DesktopCoordinates};
          log_line(
            "machine: card %u display %u: %ldx%ld at %ld,%ld, %lu Hz",
            i,
            o,
            r.right - r.left,
            r.bottom - r.top,
            r.left,
            r.top,
            have ? mode.dmDisplayFrequency : 0ul
          );
          output->Release();
          }
        adapter->Release();
        }
      factory->Release();
      }

    auto read_small_file(std::wstring const & path, std::string & text) -> bool
      {
      HANDLE const file{CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr
      )};
      if(file == INVALID_HANDLE_VALUE)
        return false;
      char buffer[65536];
      DWORD got{};
      bool const ok{ReadFile(file, buffer, sizeof buffer, &got, nullptr) != FALSE};
      CloseHandle(file);
      if(ok)
        text.assign(buffer, got);
      return ok;
      }

    ///\brief each `<Name>value</Name>` of a small XML file as `Name=value`, and the attributes of a tag that has them
    /// as they stand; a log line every 900 characters or so
    auto log_options_file(std::wstring const & dir, wchar_t const * name) -> void
      {
      narrow_t const shown{name};
      std::string text;
      if(not read_small_file(dir + L"\\" + name, text))
        {
        log_line("graphics: %s: none", shown.text);
        return;
        }
      std::string pairs;
      bool logged{};
      auto const flush{[&]
                       {
                       if(not pairs.empty())
                         log_line("graphics: %s:%s", shown.text, pairs.c_str());
                       logged = logged or not pairs.empty();
                       pairs.clear();
                       }};
      std::size_t at{};
      while((at = text.find('<', at)) != std::string::npos)
        {
        std::size_t const close{text.find('>', at)};
        if(close == std::string::npos)
          break;
        std::string_view const tag{text.data() + at + 1, close - at - 1};
        at = close + 1;
        if(tag.empty() or tag.front() == '/' or tag.front() == '?' or tag.front() == '!')
          continue;
        if(auto const space{tag.find_first_of(" \t\r\n")}; space != std::string_view::npos)
          {
          std::string_view attributes{tag.substr(space + 1)};
          if(not attributes.empty() and attributes.back() == '/')
            attributes.remove_suffix(1);
          while(not attributes.empty() and (attributes.back() == ' ' or attributes.back() == '\t'))
            attributes.remove_suffix(1);
          if(not attributes.empty())
            pairs.append(" ").append(attributes);
          }
        else if(tag.back() != '/')
          {
          std::size_t const end{text.find('<', close)};
          if(end == std::string::npos)
            break;
          if(text.compare(end, 2, "</") == 0 and text.compare(end + 2, tag.size(), tag) == 0)
            {
            std::string_view const value{text.data() + close + 1, end - close - 1};
            if(value.find_first_not_of(" \t\r\n") != std::string_view::npos)
              pairs.append(" ").append(tag).append("=").append(value);
            }
          }
        if(pairs.size() > 900)
          flush();
        }
      flush();
      if(not logged)
        log_line("graphics: %s: nothing set", shown.text);
      }

    ///\brief `Custom.4.4.fxcfg` -> 4004; 0 for a name not of that shape
    auto custom_version(wchar_t const * name) noexcept -> unsigned
      {
      unsigned major{};
      unsigned minor{};
      wchar_t tail[8]{};
      if(swscanf_s(name, L"Custom.%u.%u.%7s", &major, &minor, tail, static_cast<unsigned>(std::size(tail))) != 3
         or _wcsicmp(tail, L"fxcfg") != 0)
        return 0;
      return major * 1000u + minor;
      }

    ///\brief the game's own graphics options (%LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\Graphics):
    /// the display, the general options, the preset it starts on and the custom preset of the highest version
    auto log_graphics_options() -> void
      {
      PWSTR local{};
      if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) or not local)
        {
        log_line("graphics: the game's options could not be found");
        return;
        }
      std::wstring const dir{std::wstring{local} + L"\\Frontier Developments\\Elite Dangerous\\Options\\Graphics"};
      CoTaskMemFree(local);
      log_options_file(dir, L"DisplaySettings.xml");
      log_options_file(dir, L"Settings.xml");
      log_options_file(dir, L"GraphicsConfigurationOverride.xml");
      std::string start;
      if(read_small_file(dir + L"\\StartPreset.start", start) and start.size() < 64)
        {
        while(not start.empty() and (start.back() == '\r' or start.back() == '\n' or start.back() == ' '))
          start.pop_back();
        log_line("graphics: StartPreset.start: %s", start.c_str());
        }
      WIN32_FIND_DATAW found{};
      HANDLE const search{FindFirstFileW((dir + L"\\Custom.*.fxcfg").c_str(), &found)};
      if(search == INVALID_HANDLE_VALUE)
        {
        log_line("graphics: no custom preset");
        return;
        }
      std::wstring highest;
      unsigned highest_version{};
      do
        if(unsigned const v{custom_version(found.cFileName)}; v > highest_version)
          {
          highest = found.cFileName;
          highest_version = v;
          }
      while(FindNextFileW(search, &found));
      FindClose(search);
      if(highest.empty())
        log_line("graphics: no custom preset");
      else
        log_options_file(dir, highest.c_str());
      }

    DWORD WINAPI machine_thread(void *)
      {
      try
        {
        log_system();
        log_cards();
        log_graphics_options();
        }
      catch(std::exception const & e)
        {
        log_line("machine: stopped (%s)", e.what());
        }
      catch(...)
        {
        log_line("machine: stopped (an unknown error)");
        }
      return 0;
      }
    }  // namespace

  auto log_machine() noexcept -> void
    {
    static std::atomic<bool> started{};
    if(started.exchange(true))
      return;
    if(HANDLE const t{CreateThread(nullptr, 0, &machine_thread, nullptr, 0, nullptr)})
      CloseHandle(t);
    else
      log_line("machine: the thread could not be made (error %lu)", GetLastError());
    }

  auto log_device(ID3D11Device * device) noexcept -> void
    {
    char card[256]{"unknown"};
    IDXGIDevice * dxgi{};
    if(SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi))) and dxgi)
      {
      IDXGIAdapter * adapter{};
      if(SUCCEEDED(dxgi->GetAdapter(&adapter)) and adapter)
        {
        DXGI_ADAPTER_DESC d{};
        if(SUCCEEDED(adapter->GetDesc(&d)))
          std::snprintf(card, sizeof card, "%s (vendor %04X device %04X)", narrow_t{d.Description}.text, d.VendorId, d.DeviceId);
        adapter->Release();
        }
      dxgi->Release();
      }
    D3D11_FEATURE_DATA_THREADING threading{};
    bool const have_threading{
      SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading, sizeof threading))
    };
    unsigned const level{static_cast<unsigned>(device->GetFeatureLevel())};
    log_line(
      "device %p: card %s, feature level %u_%u, flags 0x%X, driver command lists %s",
      static_cast<void *>(device),
      card,
      level >> 12u,
      (level >> 8u) & 0xfu,
      device->GetCreationFlags(),
      have_threading ? (threading.DriverCommandLists ? "yes" : "no") : "unknown"
    );
    }
  }  // namespace edworld
