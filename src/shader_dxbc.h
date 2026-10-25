// edworld — a DXBC container's shape (PoC, 2026-10-23), for the catalog of the game's shaders: the stage, the
// signatures' semantics and the declarations of the program. The game's bytecode carries no names of its own (no RDEF,
// no STAT chunk), so the shape is what tells a shader again when an update changes its bytes. Pure arithmetic over
// the bytes, testable anywhere.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace edworld
  {
  enum struct shader_stage_e : std::uint8_t
    {
    pixel,
    vertex,
    geometry,
    hull,
    domain,
    compute,
    layout,  ///< an input layout: no program, its elements
    unknown
    };

  [[nodiscard]]
  constexpr auto stage_name(shader_stage_e s) noexcept -> char const *
    {
    switch(s)
      {
      case shader_stage_e::pixel: return "ps";
      case shader_stage_e::vertex: return "vs";
      case shader_stage_e::geometry: return "gs";
      case shader_stage_e::hull: return "hs";
      case shader_stage_e::domain: return "ds";
      case shader_stage_e::compute: return "cs";
      case shader_stage_e::layout: return "il";
      case shader_stage_e::unknown: break;
      }
    return "??";
    }

  struct dxbc_shape_t
    {
    bool ok{};
    shader_stage_e stage{shader_stage_e::unknown};
    std::uint32_t major{};
    std::uint32_t minor{};
    std::string chunks;   ///< the chunks' tags in order: "ISGN OSGN SHEX"
    std::string inputs;   ///< the input signature: "POSITION0 TEXCOORD0"
    std::string outputs;  ///< the output signature (and the patch constants', after "|")
    std::string decls;    ///< the program's declarations: "cb0[12] t0 s0 u1 tg8x8x1 temps4"
    std::uint32_t instructions{};  ///< the program's tokens that are no declarations
    };

  namespace detail
    {
    [[nodiscard]]
    inline auto dword_at(std::uint8_t const * p) noexcept -> std::uint32_t
      {
      std::uint32_t v{};
      std::memcpy(&v, p, 4);
      return v;
      }

    ///\brief a signature chunk's elements as "NAME<index>", space separated; element bytes 24 (ISGN, OSGN, PCSG),
    /// 28 (OSG5: the stream first) or 32 (ISG1, OSG1, PSG1: the stream first, the min precision last)
    inline auto signature(std::uint8_t const * data, std::uint32_t size, std::uint32_t element, std::uint32_t skip,
                          std::string & out) -> bool
      {
      if(size < 8u)
        return false;
      std::uint32_t const count{dword_at(data)};
      if(count > (size - 8u) / element)
        return false;
      for(std::uint32_t i{}; i != count; ++i)
        {
        std::uint8_t const * const e{data + 8u + i * element + skip};
        std::uint32_t const name{dword_at(e)};
        std::uint32_t const index{dword_at(e + 4)};
        if(name >= size)
          return false;
        std::size_t const length{strnlen(reinterpret_cast<char const *>(data + name), size - name)};
        if(not out.empty() and out.back() != '|')
          out += ' ';
        out.append(reinterpret_cast<char const *>(data + name), length);
        out += std::to_string(index);
        }
      return true;
      }

    inline auto add(std::string & out, char const * text) -> void
      {
      if(not out.empty())
        out += ' ';
      out += text;
      }

    ///\brief the program (SHEX, SHDR): its version token and the declarations a shape needs; the opcodes are D3D10/11's
    /// tokenized program format (d3d11TokenizedProgramFormat.hpp)
    inline auto program(std::uint8_t const * data, std::uint32_t size, dxbc_shape_t & s) -> bool
      {
      if(size < 8u)
        return false;
      std::uint32_t const version{dword_at(data)};
      std::uint32_t const length{dword_at(data + 4)};
      if(length < 2u or length > size / 4u)
        return false;
      std::uint32_t const type{version >> 16 & 0xffffu};
      s.stage = type <= 5u ? static_cast<shader_stage_e>(type) : shader_stage_e::unknown;
      s.major = version >> 4 & 0xfu;
      s.minor = version & 0xfu;
      bool const sm51{s.major > 5u or (s.major == 5u and s.minor >= 1u)};
      char item[48];
      for(std::uint32_t at{2}; at < length;)
        {
        std::uint32_t const token{dword_at(data + at * 4u)};
        std::uint32_t const opcode{token & 0x7ffu};
        std::uint32_t n{token >> 24 & 0x7fu};
        if(opcode == 0x35u)  // customdata: its length in the next token
          n = at + 1u < length ? dword_at(data + (at + 1u) * 4u) : 0u;
        if(n == 0u or at + n > length)
          return false;
        auto const op = [&](std::uint32_t k) noexcept -> std::uint32_t { return k < n ? dword_at(data + (at + k) * 4u) : 0u; };
        bool declaration{true};
        item[0] = '\0';
        switch(opcode)
          {
          case 0x59u:  // dcl_constantbuffer: SM5.1 names a range and its size after it
            if(sm51)
              std::snprintf(item, sizeof item, "cb%u-%u[%u]", op(3), op(4), op(5));
            else
              std::snprintf(item, sizeof item, "cb%u[%u]", op(2), op(3));
            break;
          case 0x58u:  // dcl_resource
          case 0xa1u:  // dcl_resource_raw
          case 0xa2u:  // dcl_resource_structured
            std::snprintf(item, sizeof item, "t%u", sm51 ? op(3) : op(2));
            break;
          case 0x5au:  // dcl_sampler
            std::snprintf(item, sizeof item, "s%u", sm51 ? op(3) : op(2));
            break;
          case 0x9cu:  // dcl_uav_typed
          case 0x9du:  // dcl_uav_raw
          case 0x9eu:  // dcl_uav_structured
            std::snprintf(item, sizeof item, "u%u", sm51 ? op(3) : op(2));
            break;
          case 0x9bu:  // dcl_thread_group
            std::snprintf(item, sizeof item, "tg%ux%ux%u", op(1), op(2), op(3));
            break;
          case 0x68u:  // dcl_temps
            std::snprintf(item, sizeof item, "temps%u", op(1));
            break;
          case 0x9fu:  // dcl_tgsm_raw
          case 0xa0u:  // dcl_tgsm_structured
            std::snprintf(item, sizeof item, "g%u", op(2));
            break;
          case 0x35u:  // customdata (immediate constants and the like): neither
            declaration = false;
            break;
          default:
            // the other declarations (inputs, outputs, globals, the tessellation's) only count
            declaration = (opcode >= 0x5bu and opcode <= 0x6au) or (opcode >= 0x8fu and opcode <= 0xa3u);
            if(not declaration)
              ++s.instructions;
            break;
          }
        if(item[0])
          add(s.decls, item);
        at += n;
        }
      return true;
      }
    }  // namespace detail

  ///\brief the shape of a DXBC container; ok false when the bytes are no DXBC or a chunk does not read
  [[nodiscard]]
  inline auto dxbc_shape(void const * bytes, std::size_t length) -> dxbc_shape_t
    {
    dxbc_shape_t s{};
    auto const * const b{static_cast<std::uint8_t const *>(bytes)};
    if(not b or length < 32u or std::memcmp(b, "DXBC", 4) != 0)
      return s;
    std::uint32_t const total{detail::dword_at(b + 24)};
    std::uint32_t const chunks{detail::dword_at(b + 28)};
    if(total > length or chunks > (total - 32u) / 4u)
      return s;
    bool have_program{};
    for(std::uint32_t i{}; i != chunks; ++i)
      {
      std::uint32_t const at{detail::dword_at(b + 32u + i * 4u)};
      if(at > total - 8u)
        return s;
      std::uint32_t const size{detail::dword_at(b + at + 4u)};
      if(size > total - at - 8u)
        return s;
      std::uint8_t const * const data{b + at + 8u};
      char tag[5]{};
      std::memcpy(tag, b + at, 4);
      detail::add(s.chunks, tag);
      bool read{true};
      std::string_view const t{tag};
      if(t == "ISGN")
        read = detail::signature(data, size, 24u, 0u, s.inputs);
      else if(t == "ISG1")
        read = detail::signature(data, size, 32u, 4u, s.inputs);
      else if(t == "OSGN")
        read = detail::signature(data, size, 24u, 0u, s.outputs);
      else if(t == "OSG5")
        read = detail::signature(data, size, 28u, 4u, s.outputs);
      else if(t == "OSG1")
        read = detail::signature(data, size, 32u, 4u, s.outputs);
      else if(t == "PCSG" or t == "PSG1")
        {
        s.outputs += s.outputs.empty() ? "|" : " |";
        read = detail::signature(data, size, t == "PCSG" ? 24u : 32u, t == "PCSG" ? 0u : 4u, s.outputs);
        }
      else if(t == "SHEX" or t == "SHDR")
        read = have_program = detail::program(data, size, s);
      if(not read)
        return s;
      }
    s.ok = have_program;
    return s;
    }
  }  // namespace edworld
