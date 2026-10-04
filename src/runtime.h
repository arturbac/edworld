// edworld — what the proxy's parts share: settings, the log, the hooks' entry point.
#pragma once

#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

namespace edworld
  {
  struct settings_t
    {
    ///\brief the next d3d11 in the chain (EDHM, EDVR renamed, ReShade); empty = the system copy
    std::wstring next;
    ///\brief vertex shaders whose draws are panels: the cockpit holo panel family and its "GUI effects off" twin
    std::vector<std::uint64_t> watch_vs{0x81216C77F90DEDD6ull, 0x1989E6D3B405FDE0ull};
    ///\brief the file the panels are published in; empty = no publishing
    std::wstring share{L"Z:\\dev\\shm\\edworld"};
    ///\brief a summary line of the frame's panels at most this often; 0 = never
    std::uint32_t log_interval_ms{1000};
    ///\brief a gap between two panel draws longer than this starts a new frame
    std::uint32_t frame_gap_us{2500};
    ///\brief log the hash of every vertex shader the game creates (discovery)
    bool log_all_vs{false};
    ///\brief the directory edworld.dll lives in; a file `edworld_dump` there asks for one dump of every panel's
    /// interface surface into `edworld_dumps\` (raw rows; size, format and row pitch in the name)
    std::wstring dir;
    };

  auto settings() noexcept -> settings_t const &;
  auto load_settings(std::wstring const & dir) -> void;

  auto log_open(std::wstring const & dir) -> void;
  ///\brief one line, time-stamped (UTC), flushed
  auto log_line(char const * fmt, ...) noexcept -> void;

  ///\brief once per device the game gets from the chain
  auto attach_to_device(ID3D11Device * device) noexcept -> void;
  }  // namespace edworld
