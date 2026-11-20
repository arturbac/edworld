// edworld — the game's shaders edworld knows by name (FNV-1a 64 of the bytecode, as catalog.tsv and the log name them),
// in one place: the code and the settings' defaults take them from here, and eht_private's edworld_install.sh reads
// this file for edshader-catch's watch list (a line per shader, `inline constexpr std::uint64_t <name>_<stage>{0x...ull};`,
// the stage vs, hs, ds, gs, ps or cs), which says when EDHM, placed before edworld, makes one of them as another.
#pragma once

#include <cstdint>

namespace edworld
  {
  ///\brief the cockpit's holo panel family
  inline constexpr std::uint64_t panel_family_vs{0x81216C77F90DEDD6ull};
  ///\brief the panels with the game's "GUI effects" off
  inline constexpr std::uint64_t panel_plain_vs{0x1989E6D3B405FDE0ull};
  ///\brief the one the game switches the same panels to near a war settlement, surface at PS t1 (2026-10-05)
  inline constexpr std::uint64_t panel_settlement_vs{0x925ACEDA0153AA5Bull};
  ///\brief the hologram's models (the own ship's and the target's), each into a 512x512 surface (2026-10-20)
  inline constexpr std::uint64_t hologram_model_vs{0x5B0068AF5630F96Bull};
  ///\brief the quad that lays the holograms' surfaces over the scene among the panels (2026-10-20)
  inline constexpr std::uint64_t hologram_quad_vs{0x5559BD94B6852E83ull};
  ///\brief the HUD's text shader, which writes the galaxy map's labels too (2026-11-16)
  inline constexpr std::uint64_t galmap_text_vs{0x1012E00B3CB44469ull};
  ///\brief the game's shell of the atmosphere over a planet: where the planet is and how large
  inline constexpr std::uint64_t atmosphere_shell_vs{0xFFB551B5E7E79CEAull};
  }  // namespace edworld
