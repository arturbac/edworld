// edworld — Status.json says whether a jump is charging and to which system; EHT's `target` (edworld_eht only)
// or EDSM says whose system it is. Only the destination's id goes out (api-system-v1/factions?systemId64=), once per
// new destination.
#include "game_state.h"

#include "compass_math.h"
#include "edworld_share.h"

#include "runtime.h"

#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace edworld
  {
  namespace
    {
    std::atomic<bool> started{};
    std::atomic<bool> charging{};
    std::atomic<std::uint64_t> destination{};
    std::atomic<std::uint8_t> allegiance{};
    // the list is written by the state thread, read by the patch on the render thread
    SRWLOCK list_lock = SRWLOCK_INIT;
    faction_list_t list{};
    SRWLOCK flight_lock = SRWLOCK_INIT;
    flight_t flight_now{};
    ULONGLONG last_fix_ms{};  ///< when the last fix that differed was read (the state thread's own)

    auto set_list(faction_list_t const & value) -> void
      {
      AcquireSRWLockExclusive(&list_lock);
      list = value;
      ReleaseSRWLockExclusive(&list_lock);
      }

    auto status_path() -> std::wstring
      {
      PWSTR saved{};
      if(FAILED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved)) or not saved)
        return {};
      std::wstring path{saved};
      CoTaskMemFree(saved);
      return path + L"\\Frontier Developments\\Elite Dangerous\\Status.json";
      }

    auto read_file(std::wstring const & path) -> std::string
      {
      std::string text;
      std::FILE * f{_wfopen(path.c_str(), L"rb")};
      if(not f)
        return text;
      char buf[4096];
      std::size_t n;
      while((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        text.append(buf, n);
      std::fclose(f);
      return text;
      }

    ///\brief the number after "key": in text, from position from; nullopt-like 0 with ok=false
    auto number_after(std::string_view text, std::string_view key, std::size_t from, bool & ok) -> std::uint64_t
      {
      ok = false;
      auto const at{text.find(key, from)};
      if(at == std::string_view::npos)
        return 0;
      std::size_t i{at + key.size()};
      while(i < text.size() and (text[i] == ' ' or text[i] == ':'))
        ++i;
      std::uint64_t v{};
      std::size_t const start{i};
      while(i < text.size() and text[i] >= '0' and text[i] <= '9')
        v = v * 10u + static_cast<std::uint64_t>(text[i++] - '0');
      ok = i > start;
      return v;
      }

    auto string_after(std::string_view text, std::string_view key, std::size_t from) -> std::string_view
      {
      auto const at{text.find(key, from)};
      if(at == std::string_view::npos)
        return {};
      auto const open{text.find('"', at + key.size())};
      if(open == std::string_view::npos)
        return {};
      auto const close{text.find('"', open + 1)};
      if(close == std::string_view::npos)
        return {};
      return text.substr(open + 1, close - open - 1);
      }

    ///\brief the decimal number (sign, fraction, exponent) after "key":; ok false when there is none
    auto real_after(std::string_view text, std::string_view key, std::size_t from, bool & ok) -> double
      {
      ok = false;
      auto const at{text.find(key, from)};
      if(at == std::string_view::npos)
        return 0.0;
      std::size_t i{at + key.size()};
      while(i < text.size() and (text[i] == ' ' or text[i] == ':'))
        ++i;
      char buf[48]{};
      std::size_t n{};
      while(i < text.size() and n + 1 < sizeof buf and std::strchr("+-.0123456789eE", text[i]))
        buf[n++] = text[i++];
      char * end{};
      double const v{std::strtod(buf, &end)};
      ok = n != 0 and end == buf + n;
      return ok ? v : 0.0;
      }

    ///\brief the flight fields of a complete Status.json; the path angle from the previous fix that differed
    auto read_flight(std::string_view text, std::uint64_t flags, std::uint64_t flags2, flight_t & last) -> void
      {
      flight_t f{};
      f.flags = flags;
      f.flags2 = flags2;
      bool la{}, lo{}, al{}, pr{}, hd{};
      f.latitude = real_after(text, "\"Latitude\"", 0, la);
      f.longitude = real_after(text, "\"Longitude\"", 0, lo);
      f.altitude = real_after(text, "\"Altitude\"", 0, al);
      f.planet_radius = real_after(text, "\"PlanetRadius\"", 0, pr);
      f.heading = real_after(text, "\"Heading\"", 0, hd);
      f.has_position = la and lo and al and pr;
      f.path_angle = std::nan("");
      f.vertical_speed = std::nan("");
      ULONGLONG const now{GetTickCount64()};
      if(f.has_position and last.has_position)
        {
        bool const moved{f.latitude != last.latitude or f.longitude != last.longitude or f.altitude != last.altitude};
        f.path_angle = moved ? flight_path_angle(last.latitude, last.longitude, last.altitude, f.latitude, f.longitude,
                                                 f.altitude, f.planet_radius)
                             : last.path_angle;
        double const seconds{static_cast<double>(now - last_fix_ms) / 1000.0};
        f.vertical_speed = moved and seconds > 0.05 ? (f.altitude - last.altitude) / seconds : last.vertical_speed;
        }
      auto const dest_at{text.find("\"Destination\"")};
      if(dest_at != std::string_view::npos)
        {
        bool ok{};
        f.destination_body = static_cast<std::uint32_t>(number_after(text, "\"Body\"", dest_at, ok));
        std::string_view const name{string_after(text, "\"Name\"", dest_at)};
        std::size_t const n{std::min(name.size(), sizeof f.destination_name - 1)};
        std::memcpy(f.destination_name, name.data(), n);
        }
      // the previous fix kept until the position changes, so the angle spans a real move
      bool const same{f.has_position and last.has_position and f.latitude == last.latitude and f.longitude == last.longitude and
                      f.altitude == last.altitude};
      if(not same)
        {
        last = f;
        last_fix_ms = now;
        }
      else
        {
        last.flags = f.flags;
        last.flags2 = f.flags2;
        last.heading = f.heading;
        last.destination_body = f.destination_body;
        std::memcpy(last.destination_name, f.destination_name, sizeof f.destination_name);
        }
      AcquireSRWLockExclusive(&flight_lock);
      flight_now = last;
      ReleaseSRWLockExclusive(&flight_lock);
      }

    auto to_allegiance(std::string_view name) -> allegiance_e
      {
      if(name == "Federation")
        return allegiance_e::federation;
      if(name == "Empire")
        return allegiance_e::empire;
      if(name == "Alliance")
        return allegiance_e::alliance;
      if(name == "Independent")
        return allegiance_e::independent;
      return name.empty() ? allegiance_e::unknown : allegiance_e::other;
      }

    ///\brief GET https://www.edsm.net/api-system-v1/factions?systemId64=<id>; empty on any failure (logged)
    auto edsm_factions(std::uint64_t system) -> std::string
      {
      std::string body;
      HINTERNET const session{WinHttpOpen(L"edworld/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
      if(not session)
        return body;
      WinHttpSetTimeouts(session, 2000, 2000, 2000, 3000);
      HINTERNET const connect{WinHttpConnect(session, L"www.edsm.net", INTERNET_DEFAULT_HTTPS_PORT, 0)};
      wchar_t path[128];
      std::swprintf(path, 128, L"/api-system-v1/factions?systemId64=%llu", static_cast<unsigned long long>(system));
      HINTERNET const request{
        connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr
      };
      DWORD status{};
      if(request and WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) and WinHttpReceiveResponse(request, nullptr))
        {
        DWORD size{sizeof status};
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
        DWORD avail{};
        while(WinHttpQueryDataAvailable(request, &avail) and avail and body.size() < 1u << 20)
          {
          std::string chunk(avail, '\0');
          DWORD got{};
          if(not WinHttpReadData(request, chunk.data(), avail, &got) or not got)
            break;
          body.append(chunk.data(), got);
          }
        }
      else
        log_line("edsm: request for %llu failed (error %lu)", static_cast<unsigned long long>(system), GetLastError());
      if(status != 200 and not body.empty())
        {
        log_line("edsm: status %lu for %llu", status, static_cast<unsigned long long>(system));
        body.clear();
        }
      if(request)
        WinHttpCloseHandle(request);
      if(connect)
        WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return body;
      }

    // ---- the data source's target (EHT): read before EDSM is asked ----
#if defined(EDWORLD_EHT)
    target_t const * target_view{};
    std::uint32_t target_mapped{};  ///< bytes of the view: the writer's layout, the first one or this one
    DWORD next_target_try{};

    auto map_target() -> void
      {
      if(target_view or settings().shm_dir.empty())
        return;
      DWORD const now{GetTickCount()};
      if(static_cast<LONG>(now - next_target_try) < 0)
        return;
      next_target_try = now + 2000;
      std::wstring const path{settings().shm_dir + L"\\target"};
      HANDLE const file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
      if(file == INVALID_HANDLE_VALUE)
        return;
      LARGE_INTEGER size{};
      GetFileSizeEx(file, &size);
      // a writer of the first layout (no factions) is read as far as it goes
      if(size.QuadPart < static_cast<LONGLONG>(target_size_first))
        {
        CloseHandle(file);
        return;
        }
      DWORD const bytes{size.QuadPart < static_cast<LONGLONG>(sizeof(target_t)) ? static_cast<DWORD>(size.QuadPart)
                                                                                 : static_cast<DWORD>(sizeof(target_t))};
      HANDLE const mapping{CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, bytes, nullptr)};
      CloseHandle(file);
      if(not mapping)
        return;
      // the view keeps the mapping alive; neither handle is needed past this point
      target_view = static_cast<target_t const *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, bytes));
      CloseHandle(mapping);
      if(target_view)
        {
        target_mapped = bytes;
        log_line("state: reading the data source's target from %S (%lu bytes%s)", path.c_str(), bytes,
                 bytes < sizeof(target_t) ? ", without factions" : "");
        }
      }

    ///\brief a consistent copy of the target record (what the view lacks left zero); false when there is none or
    /// it is not one
    auto read_target(target_t & copy, std::uint32_t & copied) -> bool
      {
      map_target();
      if(not target_view)
        return false;
      for(int attempt{}; attempt != 8; ++attempt)
        {
        // plain loads only: the view is read-only, and an interlocked operation writes even when it changes
        // nothing - an access violation on this page
        auto const sequence{[]() noexcept -> std::uint32_t
          { return *static_cast<std::uint32_t const volatile *>(&target_view->sequence); }};
        std::uint32_t const before{sequence()};
        MemoryBarrier();
        if(before & 1u)
          continue;
        copy = target_t{};
        std::memcpy(&copy, target_view, target_mapped);
        MemoryBarrier();
        if(sequence() != before)
          continue;
        if(copy.magic != target_magic or copy.version != target_version)
          return false;
        // the writer grew to the layout with factions (the tool restarted in a newer version): mapped again
        if(copy.size > target_mapped and target_mapped < sizeof(target_t))
          {
          UnmapViewOfFile(target_view);
          target_view = nullptr;
          next_target_try = GetTickCount();
          log_line("state: the data source's target grew to %u bytes, mapping it again", copy.size);
          }
        copied = target_mapped;
        return true;
        }
      return false;
      }
#else
    ///\brief edworld alone has no data source: EDSM is asked at once
    auto read_target(target_t &, std::uint32_t &) -> bool { return false; }
#endif

    DWORD WINAPI poll_thread(LPVOID)
      {
      std::wstring const path{status_path()};
      log_line("state: polling %S", path.c_str());
      std::uint64_t asked{};
      bool last_charging{};
      std::uint64_t target_dest{};
      DWORD target_since{};
      bool target_answered{};
      bool target_known{};
      bool target_listed{};
      flight_t last_fix{};
      for(;;)
        {
        std::string const text{read_file(path)};
        if(not text.empty())
          {
          bool ok{};
          std::uint64_t const flags2{number_after(text, "\"Flags2\"", 0, ok)};
          bool flags_ok{};
          std::uint64_t const flags{number_after(text, "\"Flags\"", 0, flags_ok)};
          // the game rewrites the file in place: a read caught halfway has no Flags2 and says nothing new
          // the game ends the file with "}\r\n"
          auto const last{text.find_last_not_of(" \t\r\n")};
          bool const complete{ok and flags_ok and last != std::string::npos and text[last] == '}'};
          // Flags2 bit 19 (hyperdrive charging) stays set through the witchspace tunnel, until FSDJump;
          // Flags bit 30 (FSD jump) is set from StartJump on, when the jump panel is already gone
          bool const now_charging{
            complete ? (flags2 & (1ull << 19)) != 0 and (flags & (1ull << 30)) == 0 : last_charging
          };
          charging.store(now_charging, std::memory_order_relaxed);
          if(complete)
            read_flight(text, flags, flags2, last_fix);
          auto const dest_at{text.find("\"Destination\"")};
          bool have_dest{};
          std::uint64_t const dest{
            dest_at == std::string::npos ? 0 : number_after(text, "\"System\"", dest_at, have_dest)
          };
          if(have_dest and dest != destination.load())
            {
            destination.store(dest);
            allegiance.store(static_cast<std::uint8_t>(allegiance_e::unknown));
            faction_list_t none{};
            none.system = dest;
            set_list(none);
            }
          if(now_charging != last_charging)
            {
            last_charging = now_charging;
            log_line("state: charging %s, destination %llu", now_charging ? "on" : "off", static_cast<unsigned long long>(destination.load()));
            }
          }
        std::uint64_t const dest{destination.load()};
        // the data source first: when it knows the destination, it is the answer; when it says it does not,
        // or says nothing of this destination for two seconds, EDSM is asked
        if(dest != target_dest)
          {
          target_dest = dest;
          target_since = GetTickCount();
          target_answered = false;
          }
        target_t t{};
        std::uint32_t copied{};
        bool const have_target{read_target(t, copied)};
        if(dest and have_target and t.system_address == dest and not target_answered)
          {
          target_answered = true;
          target_known = t.known != 0u;
          target_listed = false;
          if(t.known)
            {
            allegiance_e const a{t.allegiance <= 5 ? static_cast<allegiance_e>(t.allegiance) : allegiance_e::unknown};
            allegiance.store(static_cast<std::uint8_t>(a));
            faction_list_t const from_source{list_from_target(t, copied)};
            target_listed = from_source.source == list_source_e::data_source;
            if(target_listed)
              {
              set_list(from_source);
              asked = dest;  // no EDSM for it
              }
            log_line("state: %llu (%.64s) -> %s, from the data source; factions %s", static_cast<unsigned long long>(dest),
                     t.name, allegiance_name(a), target_listed ? "listed by it" : "not known to it, EDSM asked for them");
            }
          else
            log_line("state: %llu (%.64s) unknown to the data source", static_cast<unsigned long long>(dest), t.name);
          }
        bool const source_silent{not have_target or (not target_answered and GetTickCount() - target_since > 2000)};
        // the list comes from one source whole: what the data source does not list, EDSM lists, the emblem stays the
        // data source's when it knows the system
        bool const source_lacks{target_answered and t.system_address == dest and not target_listed};
        if(dest and dest != asked and settings().edsm and (source_silent or source_lacks))
          {
          asked = dest;
          std::string const body{edsm_factions(dest)};
          auto const controlling{body.find("\"controllingFaction\"")};
          std::string_view const name{
            controlling == std::string::npos ? std::string_view{} : string_after(body, "\"allegiance\"", controlling)
          };
          allegiance_e const a{to_allegiance(name)};
          bool const still{dest == destination.load()};
          if(still and not(target_answered and target_known))
            allegiance.store(static_cast<std::uint8_t>(a));
          faction_list_t from_edsm{};
          bool parsed{};
          try
            {
            parsed = not body.empty() and list_from_edsm(body, dest, from_edsm);
            }
          catch(std::exception const & e)
            {
            log_line("edsm: the answer for %llu could not be read (%s)", static_cast<unsigned long long>(dest), e.what());
            }
          if(still and parsed)
            set_list(from_edsm);
          log_line("edsm: %llu -> %s, %u faction(s)%s (%zu bytes)", static_cast<unsigned long long>(dest), allegiance_name(a),
                   from_edsm.count, parsed ? "" : ", answer not read", body.size());
          }
        Sleep(100);
        }
      }
    }  // namespace

  auto start_game_state() noexcept -> void
    {
    if(started.exchange(true))
      return;
    if(HANDLE const t{CreateThread(nullptr, 0, &poll_thread, nullptr, 0, nullptr)})
      CloseHandle(t);
    }

  auto game_state() noexcept -> game_state_t
    {
    return game_state_t{
      charging.load(std::memory_order_relaxed),
      destination.load(std::memory_order_relaxed),
      static_cast<allegiance_e>(allegiance.load(std::memory_order_relaxed))
    };
    }

  auto flight() noexcept -> flight_t
    {
    AcquireSRWLockShared(&flight_lock);
    flight_t const copy{flight_now};
    ReleaseSRWLockShared(&flight_lock);
    return copy;
    }

  auto destination_factions() noexcept -> faction_list_t
    {
    AcquireSRWLockShared(&list_lock);
    faction_list_t const copy{list};
    ReleaseSRWLockShared(&list_lock);
    return copy;
    }

  auto allegiance_name(allegiance_e a) noexcept -> char const *
    {
    switch(a)
      {
      case allegiance_e::federation:  return "Federation";
      case allegiance_e::empire:      return "Empire";
      case allegiance_e::alliance:    return "Alliance";
      case allegiance_e::independent: return "Independent";
      case allegiance_e::other:       return "other";
      default:                        return "unknown";
      }
    }
  }  // namespace edworld
