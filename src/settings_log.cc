// edworld — <plugin>.ini and <plugin>.log (edworld or edworld_eht, by build): in the directories edloader hands its
// plugins, else beside the dll; the names do not follow the dll's file name, which may be d3d11.dll.
#include "game_state.h"
#include "panel_math.h"
#include "runtime.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <iterator>
#include <mutex>
#include <cstdlib>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

    auto narrow(std::wstring_view s) -> std::string
      {
      if(s.empty())
        return {};
      int const n{WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr)};
      std::string text(static_cast<std::size_t>(n), '\0');
      WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), text.data(), n, nullptr, nullptr);
      return text;
      }

    struct hash_field_t
      {
      std::uint64_t settings_t::* field;
      };

    ///\brief 0xRRGGBB
    struct colour_field_t
      {
      std::uint32_t settings_t::* field;
      };

    struct list_field_t
      {
      std::vector<std::uint64_t> settings_t::* field;
      };

    ///\brief WIDTHxHEIGHT
    struct surface_field_t
      {
      std::uint32_t settings_t::* width;
      std::uint32_t settings_t::* height;
      };

    using field_t = std::variant<
      bool settings_t::*,
      std::uint32_t settings_t::*,
      float settings_t::*,
      std::wstring settings_t::*,
      hash_field_t,
      colour_field_t,
      list_field_t,
      surface_field_t>;

    struct key_t
      {
      std::string_view name;
      field_t field;
      ///\brief the comment over the key in the ini; the keys a player looks for first have one
      char const * note{};
      };

    // every setting once: read from the ini and written to it by this list, in the order the ini gets them
    constexpr key_t keys[]{
      {"next", &settings_t::next, "Where edworld goes on to, the next d3d11 mod; empty = the system's own. Through edloader, with other mods after edworld in edloader.txt: d3d11.dll. On its own, a mod's dll you renamed: d3d11_edhm.dll"},
      {"status_file", &settings_t::status_file, "The game's Status.json, the journals beside it; change it only if edworld looks in the wrong place"},
      {"clouds", &settings_t::clouds, "Clouds over planets with an atmosphere: 0 = none"},
      {"clouds_height", &settings_t::clouds_height, "the layer's height over the surface, as a part of the planet's radius"},
      {"clouds_alpha", &settings_t::clouds_alpha, "how much they cover what is under them, 0 to 1"},
      {"clouds_cover", &settings_t::clouds_cover, "how much of the sky they take, 0 clear to 1 overcast"},
      {"clouds_light", &settings_t::clouds_light, "how much of the sun they give back"},
      {"clouds_volume", &settings_t::clouds_volume, "Clouds with depth you fly through: 0 = the flat layer, lighter for a weaker graphics card"},
      {"clouds_bottom", &settings_t::clouds_bottom},
      {"clouds_top", &settings_t::clouds_top},
      {"clouds_steps", &settings_t::clouds_steps},
      {"clouds_density", &settings_t::clouds_density},
      {"clouds_opacity", &settings_t::clouds_opacity},
      {"clouds_min_light", &settings_t::clouds_min_light, "the star's light the clouds with depth get at least, so a faint star does not lose them; 0 = as the game gives it"},
      {"clouds_under_gain", &settings_t::clouds_under_gain, "how much lighter the clouds with depth are seen from under them, 1 = as they are"},
      {"clouds_top_gain", &settings_t::clouds_top_gain, "how much lighter the clouds with depth are seen from above, 1 = as from under them"},
      {"compass", &settings_t::compass, "The navballs beside the radar: 0 = none, the game's compass is not read at all"},
      {"edsm", &settings_t::edsm, "The destination's factions under the jump panel: 0 = never ask EDSM"},
      {"list_alpha", &settings_t::list_alpha, "how much the list's box covers what is behind it, 0 to 1"},
#if defined(EDWORLD_EHT)
      {"shm_dir", &settings_t::shm_dir, "The folder shared with EHT, as the game sees it"},
      {"shader_catalog", &settings_t::shader_catalog},
#endif
      {"patch", &settings_t::patch, "The rest, all in doc/users.md"},
      {"patch_x", &settings_t::patch_x},
      {"patch_y", &settings_t::patch_y},
      {"patch_width", &settings_t::patch_width},
      {"patch_height", &settings_t::patch_height},
      {"patch_emblem_height", &settings_t::patch_emblem_height},
      {"patch_surface", surface_field_t{&settings_t::patch_surface_width, &settings_t::patch_surface_height}},
      {"patch_ground", colour_field_t{&settings_t::patch_ground}},
      {"independent_colour", colour_field_t{&settings_t::independent_colour}},
      {"patch_force", &settings_t::patch_force},
      {"list", &settings_t::list},
      {"list_x", &settings_t::list_x},
      {"list_top", &settings_t::list_top},
      {"list_width", &settings_t::list_width},
      {"list_rows", &settings_t::list_rows},
      {"list_text", &settings_t::list_text},
      {"list_gain", &settings_t::list_gain},
      {"compass_surface", surface_field_t{&settings_t::compass_surface_width, &settings_t::compass_surface_height}},
      {"compass_x", &settings_t::compass_x},
      {"compass_y", &settings_t::compass_y},
      {"compass_radius", &settings_t::compass_radius},
      {"compass_text_x", &settings_t::compass_text_x},
      {"compass_text_y", &settings_t::compass_text_y},
      {"compass_text_size", &settings_t::compass_text_size},
      {"compass_info_x", &settings_t::compass_info_x},
      {"compass_info_y", &settings_t::compass_info_y},
      {"compass_info_size", &settings_t::compass_info_size},
      {"compass_spheres", &settings_t::compass_spheres},
      {"compass_sphere_height", &settings_t::compass_sphere_height},
      {"compass_sphere_aspect", &settings_t::compass_sphere_aspect},
      {"compass_sphere_a_x", &settings_t::compass_sphere_a_x},
      {"compass_sphere_a_y", &settings_t::compass_sphere_a_y},
      {"compass_sphere_c_x", &settings_t::compass_sphere_c_x},
      {"compass_sphere_c_y", &settings_t::compass_sphere_c_y},
      {"compass_should_dive", &settings_t::compass_should_dive},
      {"compass_sphere_gamma", &settings_t::compass_sphere_gamma},
      {"compass_sphere_gain", &settings_t::compass_sphere_gain},
      {"compass_sphere_text_above", &settings_t::compass_sphere_text_above},
      {"compass_anchor", &settings_t::compass_anchor},
      {"compass_anchor_c_px", &settings_t::compass_anchor_c_px},
      {"compass_anchor_c_py", &settings_t::compass_anchor_c_py},
      {"compass_anchor_scale", &settings_t::compass_anchor_scale},
      {"compass_anchor_a_dx", &settings_t::compass_anchor_a_dx},
      {"compass_anchor_a_dy", &settings_t::compass_anchor_a_dy},
      {"compass_anchor_c_dx", &settings_t::compass_anchor_c_dx},
      {"compass_anchor_c_dy", &settings_t::compass_anchor_c_dy},
      {"compass_text", &settings_t::compass_text},
      {"compass_hide_game", &settings_t::compass_hide_game},
      {"compass_colour_disc", colour_field_t{&settings_t::compass_colour_disc}},
      {"compass_colour_grid", colour_field_t{&settings_t::compass_colour_grid}},
      {"compass_colour_rim", colour_field_t{&settings_t::compass_colour_rim}},
      {"compass_colour_target", colour_field_t{&settings_t::compass_colour_target}},
      {"compass_colour_behind", colour_field_t{&settings_t::compass_colour_behind}},
      {"compass_colour_nose", colour_field_t{&settings_t::compass_colour_nose}},
      {"compass_colour_text_left", colour_field_t{&settings_t::compass_colour_text_left}},
      {"compass_colour_text_right", colour_field_t{&settings_t::compass_colour_text_right}},
      {"compass_colour_ok", colour_field_t{&settings_t::compass_colour_ok}},
      {"compass_colour_wrong", colour_field_t{&settings_t::compass_colour_wrong}},
      {"compass_colour_pad_base", colour_field_t{&settings_t::compass_colour_pad_base}},
      {"compass_colour_top", colour_field_t{&settings_t::compass_colour_top}},
      {"compass_approach_below", &settings_t::compass_approach_below},
      {"compass_approach_smooth_s", &settings_t::compass_approach_smooth_s},
      {"compass_approach_km", &settings_t::compass_approach_km},
      {"compass_approach_radius", &settings_t::compass_approach_radius},
      {"compass_stale_s", &settings_t::compass_stale_s},
      {"compass_max_rate_dps", &settings_t::compass_max_rate_dps},
      {"compass_filled_min", &settings_t::compass_filled_min},
      {"compass_filled_max", &settings_t::compass_filled_max},
      {"compass_hollow_min", &settings_t::compass_hollow_min},
      {"compass_hollow_max", &settings_t::compass_hollow_max},
      {"compass_log_ms", &settings_t::compass_log_ms},
      {"gate_dive", &settings_t::gate_dive},
      {"gate_dive_height", &settings_t::gate_dive_height},
      {"gate_arc", &settings_t::gate_arc},
      {"gate_dive_steepest", &settings_t::gate_dive_steepest},
      {"station_axes", &settings_t::station_axes},
      {"pad_tilt_green_deg", &settings_t::pad_tilt_green_deg},
      {"pad_tilt_red_deg", &settings_t::pad_tilt_red_deg},
      {"pad_text_samples", &settings_t::pad_text_samples},
      {"pad_text_read", &settings_t::pad_text_read},
      {"pad_text_unsure_sample", &settings_t::pad_text_unsure_sample},
      {"station_vs", hash_field_t{&settings_t::station_vs}},
      {"log_interval_ms", &settings_t::log_interval_ms},
      {"frame_gap_us", &settings_t::frame_gap_us},
      {"log_all_vs", &settings_t::log_all_vs},
      {"watch_vs", list_field_t{&settings_t::watch_vs}},
      {"dump_vs", list_field_t{&settings_t::dump_vs}},
    };

    template<typename... functions_t>
    struct overloaded_t : functions_t...
      {
      using functions_t::operator()...;
      };

    auto read_value(settings_t & s, field_t const & field, std::string_view value) -> void
      {
      std::visit(
        overloaded_t{
          [&](bool settings_t::* f) { s.*f = value == "1" or value == "true"; },
          [&](std::uint32_t settings_t::* f) { s.*f = to_uint(value, s.*f); },
          [&](float settings_t::* f) { s.*f = to_float(value, s.*f); },
          [&](std::wstring settings_t::* f) { s.*f = widen(value); },
          [&](hash_field_t f) { s.*f.field = parse_hash(value).value_or(0); },
          [&](colour_field_t f)
          {
            if(auto const v{parse_hash(value)}; v and *v <= 0xffffffu)
              s.*f.field = static_cast<std::uint32_t>(*v);
          },
          [&](list_field_t f)
          {
            std::vector<std::uint64_t> & list{s.*f.field};
            list.clear();
            std::string_view rest{value};
            while(not rest.empty())
              {
              auto const comma{rest.find(',')};
              if(auto const h{parse_hash(rest.substr(0, comma))}; h)
                list.push_back(*h);
              if(comma == std::string_view::npos)
                break;
              rest.remove_prefix(comma + 1);
              }
          },
          [&](surface_field_t f)
          {
            auto const x{value.find('x')};
            if(x == std::string_view::npos)
              return;
            s.*f.width = to_uint(trim(value.substr(0, x)), s.*f.width);
            s.*f.height = to_uint(trim(value.substr(x + 1)), s.*f.height);
          }
        },
        field
      );
      }

    auto value_text(settings_t const & s, field_t const & field) -> std::string
      {
      char text[32];
      return std::visit(
        overloaded_t{
          [&](bool settings_t::* f) -> std::string { return s.*f ? "1" : "0"; },
          [&](std::uint32_t settings_t::* f) -> std::string { return std::to_string(s.*f); },
          [&](float settings_t::* f) -> std::string
          {
            std::snprintf(text, sizeof text, "%g", static_cast<double>(s.*f));
            return text;
          },
          [&](std::wstring settings_t::* f) -> std::string { return narrow(s.*f); },
          [&](hash_field_t f) -> std::string
          {
            std::snprintf(text, sizeof text, "%016llX", static_cast<unsigned long long>(s.*f.field));
            return text;
          },
          [&](colour_field_t f) -> std::string
          {
            std::snprintf(text, sizeof text, "%06X", s.*f.field);
            return text;
          },
          [&](list_field_t f) -> std::string
          {
            std::string list;
            for(std::uint64_t const h: s.*f.field)
              {
              std::snprintf(text, sizeof text, "%s%016llX", list.empty() ? "" : ", ", static_cast<unsigned long long>(h));
              list += text;
              }
            return list;
          },
          [&](surface_field_t f) -> std::string
          {
            std::snprintf(text, sizeof text, "%ux%u", s.*f.width, s.*f.height);
            return text;
          }
        },
        field
      );
      }

    ///\brief the setting a line names, as a comment too: ";clouds = 1" -> clouds; empty for a line of words
    auto named_key(std::string_view line) -> std::string_view
      {
      line = trim(line);
      while(not line.empty() and (line.front() == ';' or line.front() == '#' or line.front() == ' '))
        line.remove_prefix(1);
      auto const eq{line.find('=')};
      return eq == std::string_view::npos ? std::string_view{} : trim(line.substr(0, eq));
      }

    auto read_whole(std::wstring const & path) -> std::string
      {
      std::string text;
      if(std::FILE * const f{_wfopen(path.c_str(), L"rb")})
        {
        char block[4096];
        while(std::size_t const n{std::fread(block, 1, sizeof block, f)})
          text.append(block, n);
        std::fclose(f);
        }
      return text;
      }

    ///\brief the lines for the settings `ini` names nowhere, with the values of `s`
    auto missing_lines(std::string_view ini, settings_t const & s, std::size_t & count) -> std::string
      {
      std::vector<std::string_view> named;
      for(std::string_view rest{ini}; not rest.empty();)
        {
        auto const end{rest.find('\n')};
        named.push_back(named_key(rest.substr(0, end)));
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        }
      std::string lines;
      for(key_t const & key: keys)
        {
        if(std::find(named.begin(), named.end(), key.name) != named.end())
          continue;
        if(key.note)
          (lines += "\n; ") += key.note, lines += '\n';
        ((lines += ';') += key.name) += " = ";
        lines += value_text(s, key.field);
        lines += '\n';
        ++count;
        }
      return lines;
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
      auto const found{std::find_if(std::begin(keys), std::end(keys), [&](key_t const & k) { return k.name == key; })};
      if(found != std::end(keys))
        read_value(current, found->field, value);
      }
    std::fclose(f);
    }

  auto complete_settings(std::wstring const & config_dir) noexcept -> void
    {
    try
      {
      std::wstring const path{config_dir + L"\\" + plugin_name + L".ini"};
      std::string const ini{read_whole(path)};
      settings_t running{current};
      running.status_file = status_path();
      std::size_t count{};
      std::string const lines{missing_lines(ini, running, count)};
      if(count == 0)
        return;
      std::FILE * const f{_wfopen(path.c_str(), L"ab")};
      if(not f)
        {
        log_line("settings: %S cannot be written (error %d); edworld runs on as it is", path.c_str(), errno);
        return;
        }
      std::string text;
      if(ini.empty())
        text = "; edworld's settings, written at its first start. Take the ';' off the start of a line to change it;\n"
               "; a change takes a new start of the game. A line left as a comment follows edworld's default.\n";
      else
        text = (ini.back() == '\n' ? "\n" : "\n\n") + std::string{"; Settings this file did not name yet, added by edworld.\n"};
      text += lines;
      bool const written{std::fwrite(text.data(), 1, text.size(), f) == text.size()};
      bool const closed{std::fclose(f) == 0};
      if(written and closed)
        log_line("settings: %zu added to %S", count, path.c_str());
      else
        log_line("settings: writing %S failed; edworld runs on as it is", path.c_str());
      }
    catch(std::exception const & e)
      {
      log_line("settings: the ini not completed: %s", e.what());
      }
    catch(...)
      {
      log_line("settings: the ini not completed");
      }
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
