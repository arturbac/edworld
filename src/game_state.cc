// edworld — Status.json says whether a jump is charging and to which system; EDSM says whose system it is.
// Only the destination's id goes out (api-system-v1/factions?systemId64=), once per new destination.
#include "game_state.h"

#include "edworld_share.h"

#include "runtime.h"

#include <shlobj.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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
    target_t const * target_view{};
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
      if(size.QuadPart < static_cast<LONGLONG>(sizeof(target_t)))
        {
        CloseHandle(file);
        return;
        }
      HANDLE const mapping{CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, sizeof(target_t), nullptr)};
      CloseHandle(file);
      if(not mapping)
        return;
      // the view keeps the mapping alive; neither handle is needed past this point
      target_view = static_cast<target_t const *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(target_t)));
      CloseHandle(mapping);
      if(target_view)
        log_line("state: reading the data source's target from %S", path.c_str());
      }

    ///\brief a consistent copy of the target record; false when there is none or it is not one
    auto read_target(target_t & copy) -> bool
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
        std::memcpy(&copy, target_view, sizeof copy);
        MemoryBarrier();
        if(sequence() == before)
          return copy.magic == target_magic and copy.version == target_version;
        }
      return false;
      }

    DWORD WINAPI poll_thread(LPVOID)
      {
      std::wstring const path{status_path()};
      log_line("state: polling %S", path.c_str());
      std::uint64_t asked{};
      bool last_charging{};
      std::uint64_t target_dest{};
      DWORD target_since{};
      bool target_answered{};
      for(;;)
        {
        std::string const text{read_file(path)};
        if(not text.empty())
          {
          bool ok{};
          std::uint64_t const flags2{number_after(text, "\"Flags2\"", 0, ok)};
          // the game rewrites the file in place: a read caught halfway has no Flags2 and says nothing new
          bool const complete{ok and text.find('}') != std::string::npos and text.back() == '}'};
          bool const now_charging{complete ? (flags2 & (1ull << 19)) != 0 : last_charging};
          charging.store(now_charging, std::memory_order_relaxed);
          auto const dest_at{text.find("\"Destination\"")};
          bool have_dest{};
          std::uint64_t const dest{
            dest_at == std::string::npos ? 0 : number_after(text, "\"System\"", dest_at, have_dest)
          };
          if(have_dest and dest != destination.load())
            {
            destination.store(dest);
            allegiance.store(static_cast<std::uint8_t>(allegiance_e::unknown));
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
        bool const have_target{read_target(t)};
        if(dest and have_target and t.system_address == dest and not target_answered)
          {
          target_answered = true;
          if(t.known)
            {
            allegiance_e const a{t.allegiance <= 5 ? static_cast<allegiance_e>(t.allegiance) : allegiance_e::unknown};
            allegiance.store(static_cast<std::uint8_t>(a));
            asked = dest;  // no EDSM for it
            log_line("state: %llu (%.64s) -> %s, from the data source", static_cast<unsigned long long>(dest), t.name,
                     allegiance_name(a));
            }
          else
            log_line("state: %llu (%.64s) unknown to the data source", static_cast<unsigned long long>(dest), t.name);
          }
        bool const source_silent{not have_target or (not target_answered and GetTickCount() - target_since > 2000)};
        if(dest and dest != asked and settings().edsm and (source_silent or (target_answered and t.system_address == dest and not t.known)))
          {
          asked = dest;
          std::string const body{edsm_factions(dest)};
          auto const controlling{body.find("\"controllingFaction\"")};
          std::string_view const name{
            controlling == std::string::npos ? std::string_view{} : string_after(body, "\"allegiance\"", controlling)
          };
          allegiance_e const a{to_allegiance(name)};
          if(dest == destination.load())
            allegiance.store(static_cast<std::uint8_t>(a));
          log_line("edsm: %llu -> %s (%zu bytes)", static_cast<unsigned long long>(dest), allegiance_name(a), body.size());
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
