// edworld — <plugin>.ini and <plugin>.log (edworld or edworld_eht, by build): in the directories edloader hands its
// plugins, else beside the dll; the names do not follow the dll's file name, which may be d3d11.dll.
#include "panel_math.h"
#include "runtime.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <cstdlib>
#include <string>
#include <string_view>

namespace edworld
  {
  namespace
    {
    settings_t current;
    std::FILE * log_file{};
    std::mutex log_mutex;

    auto trim(std::string_view s) -> std::string_view
      {
      while(not s.empty() and (s.front() == ' ' or s.front() == '\t'))
        s.remove_prefix(1);
      while(not s.empty() and (s.back() == ' ' or s.back() == '\t' or s.back() == '\r' or s.back() == '\n'))
        s.remove_suffix(1);
      return s;
      }

    auto widen(std::string_view s) -> std::wstring
      {
      if(s.empty())
        return {};
      int const n{MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0)};
      std::wstring w(static_cast<std::size_t>(n), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
      return w;
      }

    auto to_uint(std::string_view s, std::uint32_t fallback) -> std::uint32_t
      {
      std::uint32_t v{};
      if(s.empty())
        return fallback;
      for(char const c: s)
        {
        if(c < '0' or c > '9')
          return fallback;
        v = v * 10u + static_cast<std::uint32_t>(c - '0');
        }
      return v;
      }
    auto to_float(std::string_view s, float fallback) -> float
      {
      std::string const text{s};
      char * end{};
      float const v{std::strtof(text.c_str(), &end)};
      return end and end != text.c_str() ? v : fallback;
      }
    }  // namespace

  auto settings() noexcept -> settings_t const & { return current; }

  auto load_settings(std::wstring const & config_dir, std::wstring const & output_dir) -> void
    {
    current.dir = output_dir;
    std::wstring const path{config_dir + L"\\" + plugin_name + L".ini"};
    std::FILE * f{_wfopen(path.c_str(), L"rb")};
    if(not f)
      return;
    char line[1024];
    while(std::fgets(line, sizeof line, f))
      {
      std::string_view const text{trim(line)};
      if(text.empty() or text.front() == ';' or text.front() == '#' or text.front() == '[')
        continue;
      auto const eq{text.find('=')};
      if(eq == std::string_view::npos)
        continue;
      std::string_view const key{trim(text.substr(0, eq))};
      std::string_view const value{trim(text.substr(eq + 1))};
      if(key == "next")
        current.next = widen(value);
#if defined(EDWORLD_EHT)
      else if(key == "shm_dir")
        current.shm_dir = widen(value);
#endif
      else if(key == "log_interval_ms")
        current.log_interval_ms = to_uint(value, current.log_interval_ms);
      else if(key == "frame_gap_us")
        current.frame_gap_us = to_uint(value, current.frame_gap_us);
      else if(key == "patch")
        current.patch = to_uint(value, current.patch);
      else if(key == "patch_surface")
        {
        auto const x{value.find('x')};
        if(x != std::string_view::npos)
          {
          current.patch_surface_width = to_uint(trim(value.substr(0, x)), current.patch_surface_width);
          current.patch_surface_height = to_uint(trim(value.substr(x + 1)), current.patch_surface_height);
          }
        }
      else if(key == "patch_x")
        current.patch_x = to_float(value, current.patch_x);
      else if(key == "patch_y")
        current.patch_y = to_float(value, current.patch_y);
      else if(key == "patch_width")
        current.patch_width = to_float(value, current.patch_width);
      else if(key == "patch_height")
        current.patch_height = to_float(value, current.patch_height);
      else if(key == "patch_emblem_height")
        current.patch_emblem_height = to_float(value, current.patch_emblem_height);
      else if(key == "patch_ground")
        {
        if(auto const v{parse_hash(value)}; v)
          current.patch_ground = static_cast<std::uint32_t>(*v);
        }
      else if(key == "independent_colour")
        {
        if(auto const v{parse_hash(value)}; v)
          current.independent_colour = static_cast<std::uint32_t>(*v);
        }
      else if(key == "patch_force")
        current.patch_force = to_uint(value, current.patch_force);
      else if(key == "list")
        current.list = to_uint(value, current.list);
      else if(key == "list_x")
        current.list_x = to_float(value, current.list_x);
      else if(key == "list_top")
        current.list_top = to_float(value, current.list_top);
      else if(key == "list_width")
        current.list_width = to_float(value, current.list_width);
      else if(key == "list_rows")
        current.list_rows = to_uint(value, current.list_rows);
      else if(key == "list_text")
        current.list_text = to_float(value, current.list_text);
      else if(key == "list_gain")
        current.list_gain = to_float(value, current.list_gain);
      else if(key == "compass")
        current.compass = to_uint(value, current.compass);
      else if(key == "compass_surface")
        {
        auto const x{value.find('x')};
        if(x != std::string_view::npos)
          {
          current.compass_surface_width = to_uint(trim(value.substr(0, x)), current.compass_surface_width);
          current.compass_surface_height = to_uint(trim(value.substr(x + 1)), current.compass_surface_height);
          }
        }
      else if(key == "compass_x")
        current.compass_x = to_float(value, current.compass_x);
      else if(key == "compass_y")
        current.compass_y = to_float(value, current.compass_y);
      else if(key == "compass_radius")
        current.compass_radius = to_float(value, current.compass_radius);
      else if(key == "compass_text_x")
        current.compass_text_x = to_float(value, current.compass_text_x);
      else if(key == "compass_text_y")
        current.compass_text_y = to_float(value, current.compass_text_y);
      else if(key == "compass_text_size")
        current.compass_text_size = to_float(value, current.compass_text_size);
      else if(key == "compass_info_x")
        current.compass_info_x = to_float(value, current.compass_info_x);
      else if(key == "compass_info_y")
        current.compass_info_y = to_float(value, current.compass_info_y);
      else if(key == "compass_info_size")
        current.compass_info_size = to_float(value, current.compass_info_size);
      else if(key == "compass_log_ms")
        current.compass_log_ms = to_uint(value, current.compass_log_ms);
      else if(key == "edsm")
        current.edsm = value == "1" or value == "true";
      else if(key == "log_all_vs")
        current.log_all_vs = value == "1" or value == "true";
      else if(key == "watch_vs")
        {
        current.watch_vs.clear();
        std::string_view rest{value};
        while(not rest.empty())
          {
          auto const comma{rest.find(',')};
          if(auto const h{parse_hash(rest.substr(0, comma))}; h)
            current.watch_vs.push_back(*h);
          if(comma == std::string_view::npos)
            break;
          rest.remove_prefix(comma + 1);
          }
        }
      }
    std::fclose(f);
    }

  // One file per game session: the last session's log is set aside under the time it was last written,
  // <plugin>.<UTC>.log, before this one starts - the tool beside the game takes those away (EHT's backup).
  auto log_open(std::wstring const & dir) -> void
    {
    std::wstring const path{dir + L"\\" + plugin_name + L".log"};
    bool set_aside_failed{};
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) and (a.nFileSizeHigh != 0 or a.nFileSizeLow != 0))
      {
      SYSTEMTIME t{};
      FileTimeToSystemTime(&a.ftLastWriteTime, &t);
      wchar_t aside[64];
      std::swprintf(aside, 64, L"\\%ls.%04u%02u%02uT%02u%02u%02uZ.log", plugin_name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
      set_aside_failed = not MoveFileExW(path.c_str(), (dir + aside).c_str(), 0);
      }
    log_file = _wfopen(path.c_str(), L"ab");
    if(set_aside_failed)
      log_line("log: the last session's log could not be set aside (error %lu); appending to it", GetLastError());
    }

  auto log_line(char const * fmt, ...) noexcept -> void
    {
    if(not log_file)
      return;
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[2048];
    int n{std::snprintf(
      buf,
      sizeof buf,
      "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ ",
      t.wYear,
      t.wMonth,
      t.wDay,
      t.wHour,
      t.wMinute,
      t.wSecond,
      t.wMilliseconds
    )};
    va_list args;
    va_start(args, fmt);
    int const m{std::vsnprintf(buf + n, sizeof buf - static_cast<std::size_t>(n) - 2, fmt, args)};
    va_end(args);
    n = m < 0 ? n : std::min<int>(n + m, static_cast<int>(sizeof buf) - 2);
    buf[n++] = '\n';
    std::lock_guard const lock{log_mutex};
    std::fwrite(buf, 1, static_cast<std::size_t>(n), log_file);
    std::fflush(log_file);
    }
  }  // namespace edworld
