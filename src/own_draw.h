// edworld - the game's targets as bound at an own draw: whether the draw fits them (the rules in fits.h). For the
// clouds over the game's shell (hooks.cc) and the list's and the spheres' quads after the panel's draw (panel_patch.cc).
#pragma once

#include <d3d11.h>

#include <utility>

#include "fits.h"
#include "runtime.h"

namespace edworld
  {
  ///\brief whether an own draw fits the game's targets as bound now (fits.h): the targets with the pixel shader's
  /// unordered access views in the slots after them
  [[nodiscard]]
  inline auto own_draw_now(ID3D11DeviceContext * ctx) noexcept -> own_draw_e
    {
    ID3D11RenderTargetView * rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, nullptr);
    UINT targets{};
    for(UINT i{}; i != D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
      if(rtv[i])
        targets = i + 1;
    ID3D11UnorderedAccessView * uav[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    if(targets < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)
      ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, targets, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT - targets,
                                                     uav + targets);
    bool any_uav{};
    for(ID3D11UnorderedAccessView * u: uav)
      if(u)
        {
        any_uav = true;
        u->Release();
        }
    D3D11_RENDER_TARGET_VIEW_DESC first{};
    if(rtv[0])
      rtv[0]->GetDesc(&first);
    for(ID3D11RenderTargetView * t: rtv)
      if(t)
        t->Release();
    return own_draw(rtv[0] != nullptr, first.Format, targets > 1, any_uav);
    }

  ///\brief the width and height of a view's 2D texture; 0 x 0 for none or another kind
  [[nodiscard]]
  inline auto view_size(ID3D11View * view) noexcept -> std::pair<UINT, UINT>
    {
    if(not view)
      return {};
    ID3D11Resource * resource{};
    view->GetResource(&resource);
    ID3D11Texture2D * texture{};
    if(resource)
      {
      resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
      resource->Release();
      }
    if(not texture)
      return {};
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    texture->Release();
    return {d.Width, d.Height};
    }

  ///\brief whether part is drawn (m none), and its log as note_misfit says: each reason once, once drawn after one
  inline auto drawn_or_told(char const * part, misfit_log_t & log, misfit_e m) noexcept -> bool
    {
    switch(note_misfit(log, m))
      {
      case misfit_note_e::misfit: log_line("{}: not drawn while {}", part, misfit_text(m)); break;
      case misfit_note_e::drawn:  log_line("{}: drawn where it fits", part); break;
      default:                    break;
      }
    return m == misfit_e::none;
    }
  }  // namespace edworld
