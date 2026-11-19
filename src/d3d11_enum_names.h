// edworld — the bounds of the D3D11 enumerations edworld logs, so simple_enum writes their names (D3D11_BLEND_ONE)
// in place of numbers. Found by argument-dependent lookup: in the enumerations' own (the global) namespace.
#pragma once

#include <d3d11.h>

#include <simple_enum/simple_enum.hpp>

consteval auto adl_enum_bounds(D3D11_BLEND) -> simple_enum::adl_info<D3D11_BLEND>
  {
  return {D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC1_ALPHA};
  }

consteval auto adl_enum_bounds(D3D11_BLEND_OP) -> simple_enum::adl_info<D3D11_BLEND_OP>
  {
  return {D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_MAX};
  }

consteval auto adl_enum_bounds(D3D11_COMPARISON_FUNC) -> simple_enum::adl_info<D3D11_COMPARISON_FUNC>
  {
  return {D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS};
  }

consteval auto adl_enum_bounds(D3D11_DEPTH_WRITE_MASK) -> simple_enum::adl_info<D3D11_DEPTH_WRITE_MASK>
  {
  return {D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_DEPTH_WRITE_MASK_ALL};
  }
