// edworld — the destination's factions as the list under the jump panel shows them, from either source: EHT's
// `target` (edworld_eht) or EDSM's answer (api-system-v1/factions). One source per list, never a mix.
// Plain C++ (no Windows types), so the test builds it as is.
#pragma once

#include "edworld_share.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace edworld
  {
  inline constexpr std::uint32_t max_listed_factions{max_target_factions};

  enum struct list_source_e : std::uint8_t
    {
    none,
    data_source,  ///< EHT's target
    edsm
    };

  ///\brief one row; allegiance and trend as target_faction_t counts them
  struct faction_row_t
    {
    char name[64];
    char states[64];
    float influence;
    std::uint8_t allegiance;
    std::uint8_t trend;
    bool controlling;
    };

  struct faction_list_t
    {
    std::uint64_t system;
    list_source_e source;
    ///\brief EDSM's newest lastUpdate of the factions (unix seconds), 0 for the data source
    std::int64_t updated_unix_s;
    std::uint32_t count;
    faction_row_t rows[max_listed_factions];
    };

  template<std::size_t n>
  inline auto copy_text(char (&field)[n], std::string_view text) noexcept -> void
    {
    std::size_t const length{std::min(text.size(), n - 1u)};
    std::memcpy(field, text.data(), length);
    field[length] = '\0';
    }

  ///\brief the allegiance as target_t counts it: 0 unknown, 1 Federation, 2 Empire, 3 Alliance, 4 Independent, 5 other
  inline auto allegiance_code(std::string_view name) noexcept -> std::uint8_t
    {
    if(name == "Federation")
      return 1u;
    if(name == "Empire")
      return 2u;
    if(name == "Alliance")
      return 3u;
    if(name == "Independent")
      return 4u;
    return name.empty() ? 0u : 5u;
    }

  ///\brief the list from the data source's record; empty unless it carries the factions (size) and says they are known
  inline auto list_from_target(target_t const & t, std::uint32_t record_size) noexcept -> faction_list_t
    {
    faction_list_t list{};
    list.system = t.system_address;
    if(record_size < sizeof(target_t) or t.size < sizeof(target_t) or not t.known or not t.factions_known)
      return list;
    list.source = list_source_e::data_source;
    for(std::uint32_t i{}; i != t.faction_count and i != max_listed_factions; ++i)
      {
      target_faction_t const & f{t.factions[i]};
      faction_row_t & row{list.rows[list.count++]};
      copy_text(row.name, std::string_view{f.name, strnlen(f.name, sizeof f.name)});
      copy_text(row.states, std::string_view{f.states, strnlen(f.states, sizeof f.states)});
      row.influence = f.influence;
      row.allegiance = f.allegiance <= 5u ? f.allegiance : 5u;
      row.trend = f.trend <= 3u ? f.trend : 0u;
      row.controlling = f.controlling != 0u;
      }
    return list;
    }

  // ---- a small JSON reader, enough for EDSM's answer: no exceptions of its own, depth limited ----
  namespace json
    {
    struct value_t
      {
      enum struct kind_e : std::uint8_t
        {
        null,
        boolean,
        number,
        string,
        array,
        object
        };
      kind_e kind{kind_e::null};
      bool boolean{};
      double number{};
      std::string text;
      ///\brief an array's items, or an object's member values (their names in keys, same order)
      std::vector<value_t> items;
      std::vector<std::string> keys;

      [[nodiscard]]
      auto get(std::string_view key) const noexcept -> value_t const *
        {
        for(std::size_t i{}; i != keys.size() and i != items.size(); ++i)
          if(keys[i] == key)
            return &items[i];
        return nullptr;
        }

      [[nodiscard]]
      auto str(std::string_view key) const noexcept -> std::string_view
        {
        value_t const * v{get(key)};
        return v and v->kind == kind_e::string ? std::string_view{v->text} : std::string_view{};
        }

      [[nodiscard]]
      auto num(std::string_view key) const noexcept -> double
        {
        value_t const * v{get(key)};
        return v and v->kind == kind_e::number ? v->number : 0.0;
        }
      };

    struct reader_t
      {
      std::string_view text;
      std::size_t at{};

      auto space() noexcept -> void
        {
        while(at < text.size() and (text[at] == ' ' or text[at] == '\t' or text[at] == '\r' or text[at] == '\n'))
          ++at;
        }

      auto literal(std::string_view word) noexcept -> bool
        {
        if(text.substr(at, word.size()) != word)
          return false;
        at += word.size();
        return true;
        }

      static auto put_utf8(std::string & out, std::uint32_t c) -> void
        {
        if(c < 0x80u)
          out += static_cast<char>(c);
        else if(c < 0x800u)
          {
          out += static_cast<char>(0xc0u | (c >> 6));
          out += static_cast<char>(0x80u | (c & 0x3fu));
          }
        else if(c < 0x10000u)
          {
          out += static_cast<char>(0xe0u | (c >> 12));
          out += static_cast<char>(0x80u | ((c >> 6) & 0x3fu));
          out += static_cast<char>(0x80u | (c & 0x3fu));
          }
        else
          {
          out += static_cast<char>(0xf0u | (c >> 18));
          out += static_cast<char>(0x80u | ((c >> 12) & 0x3fu));
          out += static_cast<char>(0x80u | ((c >> 6) & 0x3fu));
          out += static_cast<char>(0x80u | (c & 0x3fu));
          }
        }

      auto hex4(std::uint32_t & c) noexcept -> bool
        {
        if(at + 4u > text.size())
          return false;
        c = 0u;
        for(int i{}; i != 4; ++i)
          {
          char const h{text[at++]};
          c <<= 4;
          if(h >= '0' and h <= '9')
            c |= static_cast<std::uint32_t>(h - '0');
          else if(h >= 'a' and h <= 'f')
            c |= static_cast<std::uint32_t>(h - 'a' + 10);
          else if(h >= 'A' and h <= 'F')
            c |= static_cast<std::uint32_t>(h - 'A' + 10);
          else
            return false;
          }
        return true;
        }

      auto string(std::string & out) -> bool
        {
        if(at >= text.size() or text[at] != '"')
          return false;
        ++at;
        while(at < text.size())
          {
          char const c{text[at++]};
          if(c == '"')
            return true;
          if(c != '\\')
            {
            out += c;
            continue;
            }
          if(at >= text.size())
            return false;
          switch(char const e{text[at++]}; e)
            {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
              {
              std::uint32_t code{};
              if(not hex4(code))
                return false;
              if(code >= 0xd800u and code < 0xdc00u and literal("\\u"))
                {
                std::uint32_t low{};
                if(not hex4(low) or low < 0xdc00u or low >= 0xe000u)
                  return false;
                code = 0x10000u + ((code - 0xd800u) << 10) + (low - 0xdc00u);
                }
              put_utf8(out, code);
              break;
              }
            default: return false;
            }
          }
        return false;
        }

      auto value(value_t & v, int depth) -> bool
        {
        if(depth > 16)
          return false;
        space();
        if(at >= text.size())
          return false;
        char const c{text[at]};
        if(c == '{')
          {
          v.kind = value_t::kind_e::object;
          ++at;
          space();
          if(at < text.size() and text[at] == '}')
            return ++at, true;
          for(;;)
            {
            space();
            std::string key;
            if(not string(key))
              return false;
            space();
            if(at >= text.size() or text[at++] != ':')
              return false;
            value_t member;
            if(not value(member, depth + 1))
              return false;
            v.keys.push_back(std::move(key));
            v.items.push_back(std::move(member));
            space();
            if(at >= text.size())
              return false;
            if(text[at] == ',')
              {
              ++at;
              continue;
              }
            return text[at++] == '}';
            }
          }
        if(c == '[')
          {
          v.kind = value_t::kind_e::array;
          ++at;
          space();
          if(at < text.size() and text[at] == ']')
            return ++at, true;
          for(;;)
            {
            value_t item;
            if(not value(item, depth + 1))
              return false;
            v.items.push_back(std::move(item));
            space();
            if(at >= text.size())
              return false;
            if(text[at] == ',')
              {
              ++at;
              continue;
              }
            return text[at++] == ']';
            }
          }
        if(c == '"')
          {
          v.kind = value_t::kind_e::string;
          return string(v.text);
          }
        if(literal("true") or literal("false"))
          {
          v.kind = value_t::kind_e::boolean;
          v.boolean = text[at - 1] == 'e' and text[at - 2] == 'u';
          return true;
          }
        if(literal("null"))
          return true;
        // a number: strtod on a bounded copy, the text is not NUL-terminated
        std::size_t const start{at};
        while(at < text.size() and (std::strchr("+-.eE", text[at]) or (text[at] >= '0' and text[at] <= '9')))
          ++at;
        if(at == start or at - start > 64)
          return false;
        char digits[72]{};
        std::memcpy(digits, text.data() + start, at - start);
        char * end{};
        v.kind = value_t::kind_e::number;
        v.number = std::strtod(digits, &end);
        return end == digits + (at - start);
        }
      };

    ///\brief false when the text is not one JSON value
    inline auto parse(std::string_view text, value_t & out) -> bool
      {
      reader_t r{text};
      if(not r.value(out, 0))
        return false;
      r.space();
      return r.at == text.size();
      }
    }  // namespace json

  ///\brief the names of a list of EDSM states ([{"state":"Boom"}, ...]), comma-separated
  inline auto edsm_states(json::value_t const * states) -> std::string
    {
    std::string out;
    if(not states or states->kind != json::value_t::kind_e::array)
      return out;
    for(json::value_t const & s: states->items)
      if(std::string_view const name{s.str("state")}; not name.empty())
        out += (out.empty() ? "" : ", ") + std::string{name};
    return out;
    }

  ///\brief the list from EDSM's answer for the system; false when the body is not that answer. A system EDSM has no
  /// factions for gives an empty list of source edsm (uninhabited, or not known there)
  inline auto list_from_edsm(std::string_view body, std::uint64_t system, faction_list_t & list) -> bool
    {
    list = faction_list_t{};
    list.system = system;
    json::value_t root;
    if(not json::parse(body, root) or root.kind != json::value_t::kind_e::object)
      return false;
    list.source = list_source_e::edsm;
    json::value_t const * factions{root.get("factions")};
    if(not factions or factions->kind != json::value_t::kind_e::array)
      return true;
    std::string_view controlling;
    if(json::value_t const * c{root.get("controllingFaction")}; c and c->kind == json::value_t::kind_e::object)
      controlling = c->str("name");
    std::vector<json::value_t const *> present;
    for(json::value_t const & f: factions->items)
      // a faction that left the system stays in EDSM's list at no influence
      if(f.kind == json::value_t::kind_e::object and f.num("influence") > 0.0 and not f.str("name").empty())
        present.push_back(&f);
    std::stable_sort(present.begin(), present.end(),
                     [](json::value_t const * a, json::value_t const * b) { return a->num("influence") > b->num("influence"); });
    for(json::value_t const * f: present)
      {
      list.updated_unix_s = std::max(list.updated_unix_s, static_cast<std::int64_t>(f->num("lastUpdate")));
      if(list.count == max_listed_factions)
        continue;
      faction_row_t & row{list.rows[list.count++]};
      copy_text(row.name, f->str("name"));
      std::string states{edsm_states(f->get("activeStates"))};
      if(states.empty())
        if(std::string const recovering{edsm_states(f->get("recoveringStates"))}; not recovering.empty())
          states = recovering + " (recovering)";
      copy_text(row.states, states);
      row.influence = static_cast<float>(f->num("influence"));
      row.allegiance = allegiance_code(f->str("allegiance"));
      row.trend = 0u;  // EDSM keeps no tick: which way the last one went is not known from it
      row.controlling = not controlling.empty() and f->str("name") == controlling;
      }
    return true;
    }
  }  // namespace edworld
