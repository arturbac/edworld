// edworld — a hologram model's sample in its context (edworld_eht, PoC 2026-10-23): what the game's own files said when
// the sample was taken - Status.json as it was (the target chosen, the body, the place, the flags) and the newest
// journal's name, its size then (the place in it the next event goes to) and its last complete line - so the sample can
// be looked up in the journal afterwards and what happened around it read there (tools/holo_db.py). Pure text, testable
// anywhere; the files are read on the dumps' writer thread (game_state.cc).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace edworld
  {
  namespace detail
    {
    [[nodiscard]]
    inline auto trimmed(std::string_view s) noexcept -> std::string_view
      {
      while(not s.empty() and (s.front() == ' ' or s.front() == '\r' or s.front() == '\n' or s.front() == '\t'))
        s.remove_prefix(1);
      while(not s.empty() and (s.back() == ' ' or s.back() == '\r' or s.back() == '\n' or s.back() == '\t'))
        s.remove_suffix(1);
      return s;
      }

    ///\brief a whole JSON object as far as its ends tell (a file the game is writing reads cut)
    [[nodiscard]]
    inline auto json_object(std::string_view s) noexcept -> bool
      {
      return s.size() >= 2u and s.front() == '{' and s.back() == '}';
      }

    inline auto json_string(std::string & out, std::string_view s) -> void
      {
      out += '"';
      for(char const c: s)
        if(c == '"' or c == '\\')
          (out += '\\') += c;
        else if(static_cast<unsigned char>(c) >= 0x20u)
          out += c;
      out += '"';
      }
    }  // namespace detail

  ///\brief the last complete line of a journal's tail (lines end with "\r\n"); empty when there is none whole
  [[nodiscard]]
  inline auto last_complete_line(std::string_view tail) noexcept -> std::string_view
    {
    std::size_t const end{tail.rfind('\n')};
    if(end == std::string_view::npos or end == 0u)
      return {};
    std::size_t const before{tail.rfind('\n', end - 1u)};
    std::size_t const begin{before == std::string_view::npos ? 0u : before + 1u};
    return detail::trimmed(tail.substr(begin, end - begin));
    }

  ///\brief the context file's text: {"sample", "status", "journal": {"file", "size", "last_line"}}; a part not there or
  /// not whole is null
  [[nodiscard]]
  inline auto sample_context_text(std::string_view sample, std::string_view status, std::string_view journal,
                                  std::uint64_t journal_size, std::string_view journal_tail) -> std::string
    {
    std::string out{"{\"sample\": "};
    detail::json_string(out, sample);
    std::string_view const s{detail::trimmed(status)};
    out += ",\n \"status\": ";
    out += detail::json_object(s) ? s : std::string_view{"null"};
    out += ",\n \"journal\": ";
    if(journal.empty())
      out += "null";
    else
      {
      out += "{\"file\": ";
      detail::json_string(out, journal);
      out += ", \"size\": ";
      out += std::to_string(journal_size);
      std::string_view const line{last_complete_line(journal_tail)};
      out += ", \"last_line\": ";
      out += detail::json_object(line) ? line : std::string_view{"null"};
      out += '}';
      }
    out += "}\n";
    return out;
    }
  }  // namespace edworld
