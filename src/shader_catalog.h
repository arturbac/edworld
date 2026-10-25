// edworld — the catalog of the game's shaders (PoC, 2026-10-23; edworld_eht, shader_catalog): every shader of every
// stage and every input layout the game creates, kept once per build of the game - its bytecode and a line of its
// shape (shader_dxbc.h) - so that what an update changed is known before anything built on a shader goes wrong.
#pragma once

#include "shader_dxbc.h"

#include <d3d11.h>

#include <cstddef>
#include <cstdint>

namespace edworld
  {
  ///\brief the game's build: the exe's size and last write (hooks.cc, the hologram models' key)
  auto game_build_key() noexcept -> char const *;

  ///\brief a shader the game created (any thread; the creation only notes it, a thread of the catalog writes)
  auto shader_catalog_note(shader_stage_e stage, void const * bytecode, std::size_t length, std::uint64_t hash) noexcept
    -> void;

  ///\brief an input layout the game created: its elements stand for its shape
  auto shader_catalog_note_layout(D3D11_INPUT_ELEMENT_DESC const * elements, UINT count) noexcept -> void;
  }  // namespace edworld
