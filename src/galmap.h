// edworld_eht: the names the galaxy map shows, read off its text draws (2026-11-16). The map writes every label - a
// system's name, the grid's coordinates - with the HUD's text shader, a quad a glyph, into a picture of its labels;
// each glyph's quad takes its rectangle from the font (PS t1, A8). A glyph is told by its shape in the font, not by
// where it lies there: the font is a cache the game fills as it needs glyphs, so where a glyph lies changes from one
// session to the next.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

namespace edworld
  {
  ///\brief the HUD's text shader, which writes the galaxy map's labels too
  inline constexpr std::uint64_t galmap_text_vs{0x1012E00B3CB44469ull};

  inline constexpr std::int32_t galmap_zones_x{6}, galmap_zones_y{9};

  ///\brief a font pixel over this is ink
  inline constexpr std::uint8_t galmap_ink{60};

  ///\brief a glyph whose zones lie closer than this to a template (the sum of the zones' differences) is the template's;
  /// the closest two characters, O and 0, lie 554 apart (2026-11-16); the same glyph is drawn the same each time
  inline constexpr std::uint32_t galmap_glyph_sure{250};

  ///\brief a template: its character ('\0' the blurred copy the game draws under a letter) and its zones
  struct galmap_glyph_t
    {
    char c;
    std::uint8_t zones[galmap_zones_x * galmap_zones_y];
    };
  }  // namespace edworld

#include "galmap_glyphs_generated.h"

namespace edworld
  {
  using galmap_zones_t = std::array<std::uint8_t, galmap_zones_x * galmap_zones_y>;

  ///\brief a glyph's zones: the box of its ink in its rectangle [u0, u1) x [v0, v1) of the font (one byte a pixel, pitch
  /// bytes a row) - of the rows with ink the longest run without a gap (a bit of a neighbour's glyph over or under it
  /// left out), the columns with ink in those rows - cut into galmap_zones_x x galmap_zones_y zones, each zone's mean;
  /// the zone's edges at x0 + w * i / zones (integer division, at least a pixel), as tools/galmap_glyphs.py makes the
  /// templates. So the margins the game leaves about a glyph in its font change nothing
  [[nodiscard]]
  inline auto galmap_zones(std::uint8_t const * font, std::uint32_t pitch, std::int32_t u0, std::int32_t v0, std::int32_t u1,
                           std::int32_t v1) noexcept -> galmap_zones_t
    {
    galmap_zones_t out{};
    auto const at = [&](std::int32_t x, std::int32_t y) noexcept
      { return font[static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x)]; };
    auto const row_lit = [&](std::int32_t y) noexcept
      {
      for(std::int32_t x{u0}; x != u1; ++x)
        if(at(x, y) > galmap_ink)
          return true;
      return false;
      };
    std::int32_t best_from{}, best_to{}, from{-1};
    for(std::int32_t y{v0}; y <= v1; ++y)
      {
      bool const lit{y != v1 and row_lit(y)};
      if(lit and from < 0)
        from = y;
      else if(not lit and from >= 0)
        {
        if(y - from > best_to - best_from)
          {
          best_from = from;
          best_to = y;
          }
        from = -1;
        }
      }
    if(best_to == best_from)
      return out;
    std::int32_t left{u1}, right{u0};
    for(std::int32_t x{u0}; x != u1; ++x)
      for(std::int32_t y{best_from}; y != best_to; ++y)
        if(at(x, y) > galmap_ink)
          {
          left = std::min(left, x);
          right = std::max(right, x + 1);
          break;
          }
    u0 = left;
    u1 = right;
    v0 = best_from;
    v1 = best_to;
    std::int32_t const w{u1 - u0}, h{v1 - v0};
    for(std::int32_t zy{}; zy != galmap_zones_y; ++zy)
      {
      std::int32_t const y0{v0 + h * zy / galmap_zones_y};
      std::int32_t const y1{std::max(y0 + 1, v0 + h * (zy + 1) / galmap_zones_y)};
      for(std::int32_t zx{}; zx != galmap_zones_x; ++zx)
        {
        std::int32_t const x0{u0 + w * zx / galmap_zones_x};
        std::int32_t const x1{std::max(x0 + 1, u0 + w * (zx + 1) / galmap_zones_x)};
        std::uint32_t total{};
        for(std::int32_t y{y0}; y != y1; ++y)
          for(std::int32_t x{x0}; x != x1; ++x)
            total += at(x, y);
        out[static_cast<std::size_t>(zy * galmap_zones_x + zx)] = static_cast<std::uint8_t>(total / static_cast<std::uint32_t>((y1 - y0) * (x1 - x0)));
        }
      }
    return out;
    }

  struct galmap_match_t
    {
    char c;                 ///< the closest template's character, '?' when none is close enough
    std::uint32_t distance; ///< to the closest template
    };

  [[nodiscard]]
  inline auto galmap_match(galmap_zones_t const & zones) noexcept -> galmap_match_t
    {
    galmap_match_t best{'?', ~0u};
    for(galmap_glyph_t const & g: galmap_glyphs)
      {
      std::uint32_t d{};
      for(std::size_t i{}; i != zones.size(); ++i)
        d += static_cast<std::uint32_t>(std::abs(static_cast<int>(zones[i]) - static_cast<int>(g.zones[i])));
      if(d < best.distance)
        best = galmap_match_t{g.c, d};
      }
    if(best.distance >= galmap_glyph_sure)
      best.c = '?';
    return best;
    }

  ///\brief a glyph's quad in its label's own frame (left and right edge, height) and its character ('\0' a blurred copy)
  struct galmap_quad_t
    {
    float x0, x1, height;
    char c;
    };

  ///\brief a gap between two glyphs' quads over this many of their height is a space: letters stand at most 0.3 apart
  /// (a narrow 1 included), words 0.5 (the labels of 2026-11-16)
  inline constexpr float galmap_space{0.4f};

  ///\brief the labels of one text draw, its quads in their order: a label's glyphs run left to right in its own frame,
  /// so the next label begins where x goes back; a space where the gap is over galmap_space of the quads' height. The
  /// game sets digits in one width (a narrow 1 stands in a digit's room), so a digit's quad counts as wide as the
  /// label's widest digit. The blurred copies are no glyphs
  [[nodiscard]]
  inline auto galmap_labels(std::span<galmap_quad_t const> quads) -> std::vector<std::string>
    {
    std::vector<std::string> out;
    std::vector<galmap_quad_t> label;
    auto const finish = [&out, &label]
      {
      if(label.empty())
        return;
      float digit{};
      for(galmap_quad_t const & q: label)
        if(q.c >= '0' and q.c <= '9')
          digit = std::max(digit, q.x1 - q.x0);
      auto const right = [digit](galmap_quad_t const & q) noexcept
        { return q.c >= '0' and q.c <= '9' ? std::max(q.x1, q.x0 + digit) : q.x1; };
      std::string text;
      for(std::size_t i{}; i != label.size(); ++i)
        {
        if(i != 0 and label[i].x0 - right(label[i - 1]) > label[i].height * galmap_space)
          text += ' ';
        text += label[i].c;
        }
      out.push_back(std::move(text));
      label.clear();
      };
    for(galmap_quad_t const & q: quads)
      {
      if(q.c == '\0')
        continue;
      if(not label.empty() and q.x0 < label.back().x0 - 1.f)
        finish();
      label.push_back(q);
      }
    finish();
    return out;
    }
  }  // namespace edworld
