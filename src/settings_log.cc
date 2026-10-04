// edworld — edworld.ini beside the dll, and edworld.log beside it.
#include "panel_math.h"
#include "runtime.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
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
    }  // namespace

  auto settings() noexcept -> settings_t const & { return current; }

  auto load_settings(std::wstring const & dir) -> void
    {
    std::wstring const path{dir + L"\\edworld.ini"};
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
      else if(key == "share")
        current.share = widen(value);
      else if(key == "log_interval_ms")
        current.log_interval_ms = to_uint(value, current.log_interval_ms);
      else if(key == "frame_gap_us")
        current.frame_gap_us = to_uint(value, current.frame_gap_us);
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

  auto log_open(std::wstring const & dir) -> void
    {
    std::wstring const path{dir + L"\\edworld.log"};
    log_file = _wfopen(path.c_str(), L"ab");
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
