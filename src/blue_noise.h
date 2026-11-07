// edworld — a tile of blue noise: pure arithmetic, no D3D, testable anywhere. Each of the size x size cells gets a
// rank 0..size*size-1 such that the cells of any lowest ranks lie evenly spread, with no clumps and no lines
// (void and cluster, Ulichney 1993, on a torus so the tile repeats without seams). The clouds' march starts each
// pixel's steps by it: the bands of the steps turn into an even fine grain, where a computed pattern showed stripes.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace edworld
  {
  ///\brief the ranks of a size x size tile, row by row; the same for the same size every time
  [[nodiscard]]
  inline auto blue_noise_ranks(int size) -> std::vector<std::uint16_t>
    {
    int const cells{size * size};
    // the energy a cell has from the set cells around it: a gaussian of sigma 1.5, cut at 6 cells, wrapped round
    constexpr int reach{6};
    std::array<float, (2 * reach + 1) * (2 * reach + 1)> kernel{};
    for(int y{-reach}; y <= reach; ++y)
      for(int x{-reach}; x <= reach; ++x)
        kernel[static_cast<std::size_t>((y + reach) * (2 * reach + 1) + x + reach)] = std::exp(-static_cast<float>(x * x + y * y) / (2.f * 1.5f * 1.5f));
    std::vector<std::uint8_t> on(static_cast<std::size_t>(cells));
    std::vector<float> energy(static_cast<std::size_t>(cells));
    auto const add = [&](int cell, float sign)
      {
      int const cx{cell % size};
      int const cy{cell / size};
      for(int y{-reach}; y <= reach; ++y)
        for(int x{-reach}; x <= reach; ++x)
          energy[static_cast<std::size_t>(((cy + y + size) % size) * size + (cx + x + size) % size)]
            += sign * kernel[static_cast<std::size_t>((y + reach) * (2 * reach + 1) + x + reach)];
      };
    // the set cell with the most energy (a cluster's heart) or the free cell with the least (a void's)
    auto const pick = [&](bool set, bool most) -> int
      {
      int best{-1};
      for(int c{}; c != cells; ++c)
        if((on[static_cast<std::size_t>(c)] != 0u) == set
           and (best < 0 or (most ? energy[static_cast<std::size_t>(c)] > energy[static_cast<std::size_t>(best)]
                                  : energy[static_cast<std::size_t>(c)] < energy[static_cast<std::size_t>(best)])))
          best = c;
      return best;
      };

    // a tenth of the cells set at random (a fixed sequence), then spread until moving one changes nothing
    int const first{cells / 10};
    std::uint32_t state{0x9e3779b9u};
    for(int n{}; n != first;)
      {
      state = state * 1664525u + 1013904223u;
      int const c{static_cast<int>((state >> 8) % static_cast<std::uint32_t>(cells))};
      if(on[static_cast<std::size_t>(c)] == 0u)
        {
        on[static_cast<std::size_t>(c)] = 1u;
        add(c, 1.f);
        ++n;
        }
      }
    for(int round{}; round != cells; ++round)
      {
      int const tight{pick(true, true)};
      on[static_cast<std::size_t>(tight)] = 0u;
      add(tight, -1.f);
      int const empty{pick(false, false)};
      on[static_cast<std::size_t>(empty)] = 1u;
      add(empty, 1.f);
      if(empty == tight)
        break;
      }
    auto const start_on{on};
    auto const start_energy{energy};

    std::vector<std::uint16_t> rank(static_cast<std::size_t>(cells));
    // the first ranks: the starting cells taken away, the tightest first, from the top rank of them down
    for(int r{first - 1}; r >= 0; --r)
      {
      int const tight{pick(true, true)};
      on[static_cast<std::size_t>(tight)] = 0u;
      add(tight, -1.f);
      rank[static_cast<std::size_t>(tight)] = static_cast<std::uint16_t>(r);
      }
    on = start_on;
    energy = start_energy;
    // the rest: each next rank into the largest void
    for(int r{first}; r != cells; ++r)
      {
      int const empty{pick(false, false)};
      on[static_cast<std::size_t>(empty)] = 1u;
      add(empty, 1.f);
      rank[static_cast<std::size_t>(empty)] = static_cast<std::uint16_t>(r);
      }
    return rank;
    }
  }  // namespace edworld
