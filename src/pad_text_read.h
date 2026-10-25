// edworld — the pad's number read from the HUD's notice box (2026-10-24): pure arithmetic on the box's pixels, no D3D,
// testable anywhere. The box (panel_math.h pad_box_*) carries the HUD's notices; the tower's is two lines, "PROCEED TO
// LANDING PAD n -" over the grant's countdown "m:ss" (in the game's language). Without words: the text is the pixels whose
// brightest of r, g, b is at least pad_text_bright; lines are runs of rows with text, glyphs runs of columns, words glyphs
// closer than half a line's height (a number's digits stand 7 px apart, words 11 to 15, the lines 18 px high). The pad's
// notice: two lines, the first ending in a lone dash after a word of one or two glyphs, the second four glyphs with a
// colon (narrow, an empty row inside it) second. Each digit seen from afar (2026-10-25): its box in the game's own
// pixels cut into pad_zones_x x pad_zones_y zones, each zone's share of lit pixels in per mille (pixels a zone's edge
// cuts counted in part), matched against pad_digits (tools/pad_text_digits.py: each digit's mean, from samples the data
// source named the pad of) by the sum of differences: the nearest, sure when the next is pad_digit_sure_percent further.
// The box is 12, 13 or 14 px wide by where the number stands; scaled to a grid it moved whole columns, and the lone 8
// and the 3 of 30 were never read sure (2026-10-25). Each pad's notices read by the other pads' templates, as a pad not
// seen yet (2766 samples of two commanders): 939 of 939 right, the least margin 37 % (8 by 18's eight); all on all 964
// of 964 (pad 7 alone has a 7). The structure is the English text's - other languages of the game not checked.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace edworld
  {
  inline constexpr std::int32_t pad_text_width{410}, pad_text_height{81};
  inline constexpr std::uint8_t pad_text_bright{170};
  inline constexpr std::int32_t pad_zones_x{4}, pad_zones_y{6};
  inline constexpr std::uint32_t pad_digit_sure_percent{20};

  ///\brief a digit's template: its zones' shares of lit pixels in per mille, row by row
  struct pad_digit_t
    {
    char digit;
    std::array<std::uint16_t, static_cast<std::size_t>(pad_zones_x) * pad_zones_y> zones;
    };
  }  // namespace edworld

#include "pad_text_digits_generated.h"

namespace edworld
  {
  ///\brief the box's text pixels, row by row
  struct pad_text_bitmap_t
    {
    std::array<std::uint8_t, static_cast<std::size_t>(pad_text_width) * pad_text_height> on;

    [[nodiscard]]
    constexpr auto at(std::int32_t x, std::int32_t y) const noexcept -> bool
      {
      return on[static_cast<std::size_t>(y) * pad_text_width + static_cast<std::size_t>(x)] != 0u;
      }
    };

  ///\brief the text pixels of an area of RGBA8 pixels (row pitch in bytes) copied from the box's place
  inline auto pad_text_bitmap(std::uint8_t const * pixels, std::uint32_t pitch, pad_text_bitmap_t & out) noexcept -> void
    {
    for(std::int32_t y{}; y != pad_text_height; ++y)
      for(std::int32_t x{}; x != pad_text_width; ++x)
        {
        std::uint8_t const * p{pixels + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4u};
        std::uint8_t const top{p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2])};
        out.on[static_cast<std::size_t>(y) * pad_text_width + static_cast<std::size_t>(x)] = top >= pad_text_bright ? 1u : 0u;
        }
    }

  struct pad_text_span_t
    {
    std::int32_t from, to;  ///< [from, to)
    };

  struct pad_text_glyph_t
    {
    std::int32_t x0, x1, y0, y1;  ///< its box: [x0, x1) x [y0, y1)
    };

  inline constexpr std::size_t pad_text_max_lines{4}, pad_text_max_glyphs{48};

  struct pad_text_read_t
    {
    bool notice{};               ///< the box reads as the tower's notice
    bool sure{};                 ///< every digit of the number clear of its next template
    std::uint32_t pad{};         ///< the pad's number
    std::uint32_t seconds_left{};  ///< the countdown's m:ss in seconds
    std::uint32_t margin{100};     ///< the least of its digits' margins, percent (pad_text_digit)
    };

  namespace detail
    {
    ///\brief the rows [top, bottom) holding text, gaps under 3 rows joined, of 8 rows at least; the count found
    inline auto pad_text_lines(pad_text_bitmap_t const & b, std::array<pad_text_span_t, pad_text_max_lines> & out) noexcept
      -> std::size_t
      {
      std::size_t n{};
      std::int32_t start{-1}, end{}, gap{};
      for(std::int32_t y{}; y != pad_text_height + 3; ++y)
        {
        bool any{};
        if(y < pad_text_height)
          for(std::int32_t x{}; x != pad_text_width and not any; ++x)
            any = b.at(x, y);
        if(any)
          {
          if(start < 0)
            start = y;
          gap = 0;
          end = y + 1;
          }
        else if(start >= 0 and ++gap >= 3)
          {
          if(end - start >= 8)
            {
            if(n == out.size())
              return n + 1u;  // more lines than any notice of the tower
            out[n++] = pad_text_span_t{start, end};
            }
          start = -1;
          }
        }
      return n;
      }

    ///\brief the glyphs of a line: runs of columns with text, each with the rows it covers; the count found
    inline auto pad_text_glyphs(pad_text_bitmap_t const & b, pad_text_span_t line,
                                std::array<pad_text_glyph_t, pad_text_max_glyphs> & out) noexcept -> std::size_t
      {
      std::size_t n{};
      std::int32_t start{-1};
      for(std::int32_t x{}; x != pad_text_width + 1; ++x)
        {
        bool any{};
        if(x < pad_text_width)
          for(std::int32_t y{line.from}; y != line.to and not any; ++y)
            any = b.at(x, y);
        if(any and start < 0)
          start = x;
        else if(not any and start >= 0)
          {
          if(n == out.size())
            return n;
          pad_text_glyph_t g{start, x, line.to, line.from};
          for(std::int32_t y{line.from}; y != line.to; ++y)
            for(std::int32_t c{start}; c != x; ++c)
              if(b.at(c, y))
                {
                g.y0 = y < g.y0 ? y : g.y0;
                g.y1 = y + 1 > g.y1 ? y + 1 : g.y1;
                break;
                }
          out[n++] = g;
          start = -1;
          }
        }
      return n;
      }

    [[nodiscard]]
    constexpr auto pad_text_dash(pad_text_glyph_t const & g, std::int32_t line_h) noexcept -> bool
      {
      std::int32_t const h{g.y1 - g.y0};
      return 10 * h < 3 * line_h and g.x1 - g.x0 >= 2 * h;
      }

    ///\brief narrow, two parts one over the other: an empty row inside its box
    inline auto pad_text_colon(pad_text_bitmap_t const & b, pad_text_glyph_t const & g, std::int32_t line_h) noexcept -> bool
      {
      if(4 * (g.x1 - g.x0) > line_h)
        return false;
      for(std::int32_t y{g.y0}; y != g.y1; ++y)
        {
        bool any{};
        for(std::int32_t x{g.x0}; x != g.x1 and not any; ++x)
          any = b.at(x, y);
        if(not any)
          return true;
        }
      return false;
      }

    using pad_zones_t = std::array<std::uint16_t, static_cast<std::size_t>(pad_zones_x) * pad_zones_y>;

    ///\brief the glyph's zones: in units where a pixel is pad_zones_x wide and a zone the box's width (rows alike), each
    /// zone's lit area per mille of its area - integers, as tools/pad_text_digits.py
    inline auto pad_text_zones(pad_text_bitmap_t const & b, pad_text_glyph_t const & g) noexcept -> pad_zones_t
      {
      pad_zones_t out{};
      std::int32_t const w{g.x1 - g.x0}, h{g.y1 - g.y0};
      if(w <= 0 or h <= 0)
        return out;
      for(std::int32_t j{}; j != pad_zones_y; ++j)
        for(std::int32_t i{}; i != pad_zones_x; ++i)
          {
          std::int64_t lit{};
          for(std::int32_t y{}; y != h; ++y)
            {
            std::int32_t const oy{std::min((y + 1) * pad_zones_y, (j + 1) * h) - std::max(y * pad_zones_y, j * h)};
            if(oy <= 0)
              continue;
            for(std::int32_t x{}; x != w; ++x)
              {
              std::int32_t const ox{std::min((x + 1) * pad_zones_x, (i + 1) * w) - std::max(x * pad_zones_x, i * w)};
              if(ox > 0 and b.at(g.x0 + x, g.y0 + y))
                lit += static_cast<std::int64_t>(ox) * oy;
              }
            }
          out[static_cast<std::size_t>(j * pad_zones_x + i)] = static_cast<std::uint16_t>(lit * 1000 / (static_cast<std::int64_t>(w) * h));
          }
      return out;
      }

    struct pad_text_digit_read_t
      {
      char digit;
      bool sure;             ///< the next template pad_digit_sure_percent further at least
      std::uint32_t margin;  ///< the next template's distance over the nearest's, in percent of the next's
      };

    inline auto pad_text_digit(pad_zones_t const & z) noexcept -> pad_text_digit_read_t
      {
      std::uint32_t best{~0u}, next{~0u};
      char digit{'?'};
      for(pad_digit_t const & t: pad_digits)
        {
        std::uint32_t d{};
        for(std::size_t k{}; k != z.size(); ++k)
          d += static_cast<std::uint32_t>(z[k] > t.zones[k] ? z[k] - t.zones[k] : t.zones[k] - z[k]);
        if(d < best)
          {
          next = best;
          best = d;
          digit = t.digit;
          }
        else if(d < next)
          next = d;
        }
      if(next == ~0u or next == 0u)
        return pad_text_digit_read_t{digit, false, 0u};
      return pad_text_digit_read_t{digit, 100u * (next - best) >= pad_digit_sure_percent * next, 100u * (next - best) / next};
      }
    }  // namespace detail

  ///\brief the tower's notice in the box's text pixels: its pad's number and the countdown, or no notice
  inline auto pad_text_read(pad_text_bitmap_t const & b) noexcept -> pad_text_read_t
    {
    std::array<pad_text_span_t, pad_text_max_lines> lines{};
    if(detail::pad_text_lines(b, lines) != 2u)
      return {};
    std::array<pad_text_glyph_t, pad_text_max_glyphs> g1{}, g2{};
    std::size_t const n1{detail::pad_text_glyphs(b, lines[0], g1)}, n2{detail::pad_text_glyphs(b, lines[1], g2)};
    std::int32_t const h1{lines[0].to - lines[0].from}, h2{lines[1].to - lines[1].from};
    // line 1: ... <number> <dash>, words apart by over half the line's height
    if(n1 < 3u or not detail::pad_text_dash(g1[n1 - 1u], h1) or 2 * (g1[n1 - 1u].x0 - g1[n1 - 2u].x1) <= h1)
      return {};
    std::size_t first{n1 - 2u};
    while(first > 0u and 2 * (g1[first].x0 - g1[first - 1u].x1) <= h1)
      --first;
    std::size_t const digits{n1 - 1u - first};
    if(first == 0u or digits < 1u or digits > 2u)
      return {};
    // line 2: m:ss
    if(n2 != 4u or not detail::pad_text_colon(b, g2[1], h2))
      return {};
    pad_text_read_t r{true, true, 0u, 0u};
    for(std::size_t i{first}; i != n1 - 1u; ++i)
      {
      auto const d{detail::pad_text_digit(detail::pad_text_zones(b, g1[i]))};
      r.pad = r.pad * 10u + static_cast<std::uint32_t>(d.digit - '0');
      r.sure = r.sure and d.sure;
      r.margin = std::min(r.margin, d.margin);
      }
    std::uint32_t clock[3]{};
    for(std::size_t i{}, k{}; i != 4u; ++i)
      if(i != 1u)
        clock[k++] = static_cast<std::uint32_t>(detail::pad_text_digit(detail::pad_text_zones(b, g2[i])).digit - '0');
    r.seconds_left = clock[0] * 60u + clock[1] * 10u + clock[2];
    return r;
    }

  ///\brief a pad read again is a new grant when its number differs, or when its countdown has more time left than the
  /// grant before by this much: the tower gives the same pad anew from 9:59 (Artur asks and cancels at a carrier for the
  /// pad he wants); a countdown misread by a digit (9:18 for 9:13, 2026-10-24) is no new grant
  inline constexpr std::int64_t pad_text_regrant_ms{20000};

  [[nodiscard]]
  constexpr auto pad_text_new_grant(std::uint32_t pad, std::int64_t granted_ms, std::uint32_t given_pad, std::int64_t given_ms) noexcept
    -> bool
    {
    return pad != given_pad or granted_ms - given_ms >= pad_text_regrant_ms;
    }

  ///\brief two reads in a row of the same pad, both sure: the pad is taken (a flicker or a 3 read as 8 once is not)
  struct pad_text_vote_t
    {
    std::uint32_t last{};  ///< the pad of the last sure read, 0 = none

    ///\brief the pad taken by this read, 0 = none yet
    constexpr auto add(pad_text_read_t const & r) noexcept -> std::uint32_t
      {
      if(not r.notice or not r.sure or r.pad == 0u)
        {
        last = 0u;
        return 0u;
        }
      std::uint32_t const taken{r.pad == last ? r.pad : 0u};
      last = r.pad;
      return taken;
      }
    };
  }  // namespace edworld
