// edworld — the jump panel patch. While a hyperspace jump charges, just before the game composites the
// interface surface that carries the charge panel into the cockpit, the panel's wrong superpower emblem is
// painted over on that surface with the panel's own black and the right emblem. Drawn onto the surface, the
// patch then rides the panel through the cockpit like the rest of its text: camera lag, head look, the
// hologram's own effects.
//
// The only place edworld changes what the game draws. Everything the game had bound is read back first and
// put back after (the ImGui D3D11 backend's discipline), and our own draws go through our own objects.
#include "panel_patch.h"

#include "emblems.h"
#include "game_state.h"
#include "patch_ps.h"
#include "patch_vs.h"

#include <cstring>

namespace edworld
  {
  namespace
    {
    struct resources_t
      {
      ID3D11Device * device{};
      ID3D11VertexShader * vs{};
      ID3D11PixelShader * ps{};
      ID3D11Buffer * cb{};
      ID3D11SamplerState * sampler{};
      ID3D11BlendState * opaque{};
      ID3D11BlendState * over{};
      ID3D11RasterizerState * raster{};
      ID3D11DepthStencilState * no_depth{};
      ID3D11ShaderResourceView * emblem[3]{};
      std::uint32_t emblem_width[3]{};
      std::uint32_t emblem_height[3]{};
      bool failed{};
      };

    resources_t r;

    struct rtv_cache_t
      {
      void * texture;
      ID3D11RenderTargetView * rtv;
      };

    rtv_cache_t rtvs[8]{};
    std::uint32_t rtv_next{};
    std::uint64_t patched_frame{~0ull};
    bool told_drawing{};

    struct constants_t
      {
      float rect[4];
      float colour[4];
      float mode[4];
      };

    auto make_mask(std::uint8_t const * mask, std::uint32_t w, std::uint32_t h, std::uint32_t slot) -> bool
      {
      D3D11_TEXTURE2D_DESC d{};
      d.Width = w;
      d.Height = h;
      d.MipLevels = 1;
      d.ArraySize = 1;
      d.Format = DXGI_FORMAT_R8_UNORM;
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_IMMUTABLE;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA init{mask, w, 0};
      ID3D11Texture2D * tex{};
      if(FAILED(r.device->CreateTexture2D(&d, &init, &tex)) or not tex)
        return false;
      HRESULT const hr{r.device->CreateShaderResourceView(tex, nullptr, &r.emblem[slot])};
      tex->Release();
      r.emblem_width[slot] = w;
      r.emblem_height[slot] = h;
      return SUCCEEDED(hr);
      }

    auto create(ID3D11Device * device) -> bool
      {
      if(r.device == device)
        return not r.failed;
      r = resources_t{};
      r.device = device;
      bool ok{SUCCEEDED(device->CreateVertexShader(g_patch_vs, sizeof g_patch_vs, nullptr, &r.vs))};
      ok = ok and SUCCEEDED(device->CreatePixelShader(g_patch_ps, sizeof g_patch_ps, nullptr, &r.ps));
      D3D11_BUFFER_DESC cbd{sizeof(constants_t), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
      ok = ok and SUCCEEDED(device->CreateBuffer(&cbd, nullptr, &r.cb));
      D3D11_SAMPLER_DESC sd{};
      sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.MaxLOD = D3D11_FLOAT32_MAX;
      ok = ok and SUCCEEDED(device->CreateSamplerState(&sd, &r.sampler));
      D3D11_BLEND_DESC bd{};
      bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      ok = ok and SUCCEEDED(device->CreateBlendState(&bd, &r.opaque));
      bd.RenderTarget[0].BlendEnable = TRUE;
      bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
      bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
      bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
      bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
      bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
      ok = ok and SUCCEEDED(device->CreateBlendState(&bd, &r.over));
      D3D11_RASTERIZER_DESC rd{};
      rd.FillMode = D3D11_FILL_SOLID;
      rd.CullMode = D3D11_CULL_NONE;
      rd.DepthClipEnable = TRUE;
      ok = ok and SUCCEEDED(device->CreateRasterizerState(&rd, &r.raster));
      D3D11_DEPTH_STENCIL_DESC dd{};
      dd.DepthEnable = FALSE;
      dd.StencilEnable = FALSE;
      ok = ok and SUCCEEDED(device->CreateDepthStencilState(&dd, &r.no_depth));
      ok = ok and make_mask(emblems::federation_mask, emblems::federation_width, emblems::federation_height, 0);
      ok = ok and make_mask(emblems::empire_mask, emblems::empire_width, emblems::empire_height, 1);
      ok = ok and make_mask(emblems::alliance_mask, emblems::alliance_width, emblems::alliance_height, 2);
      r.failed = not ok;
      log_line(ok ? "patch: resources ready" : "patch: resources could not be made; no patch");
      return ok;
      }

    ///\brief the surface's render target view, made once per surface; the typeless surface is written as UNORM
    auto rtv_for(ID3D11Texture2D * tex) -> ID3D11RenderTargetView *
      {
      for(rtv_cache_t const & c: rtvs)
        if(c.texture == tex)
          return c.rtv;
      D3D11_TEXTURE2D_DESC d{};
      tex->GetDesc(&d);
      if(not(d.BindFlags & D3D11_BIND_RENDER_TARGET))
        return nullptr;
      D3D11_RENDER_TARGET_VIEW_DESC vd{};
      vd.Format = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : d.Format;
      vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      ID3D11RenderTargetView * rtv{};
      if(FAILED(r.device->CreateRenderTargetView(tex, &vd, &rtv)))
        return nullptr;
      rtv_cache_t & slot{rtvs[rtv_next++ % 8]};
      if(slot.rtv)
        slot.rtv->Release();
      slot = {tex, rtv};
      return rtv;
      }

    ///\brief everything our draws touch, as the game left it
    struct backup_t
      {
      UINT viewports{D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE};
      D3D11_VIEWPORT viewport[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
      ID3D11RasterizerState * raster{};
      ID3D11BlendState * blend{};
      float blend_factor[4]{};
      UINT sample_mask{};
      ID3D11DepthStencilState * depth{};
      UINT stencil_ref{};
      ID3D11RenderTargetView * rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
      ID3D11DepthStencilView * dsv{};
      ID3D11ShaderResourceView * ps_srv[4]{};
      ID3D11SamplerState * ps_sampler{};
      ID3D11PixelShader * ps{};
      ID3D11ClassInstance * ps_instances[256]{};
      UINT ps_instance_count{256};
      ID3D11VertexShader * vs{};
      ID3D11ClassInstance * vs_instances[256]{};
      UINT vs_instance_count{256};
      ID3D11Buffer * vs_cb{};
      ID3D11Buffer * ps_cb{};
      D3D11_PRIMITIVE_TOPOLOGY topology{};
      ID3D11InputLayout * layout{};
      };

    auto save(ID3D11DeviceContext * ctx, backup_t & b) -> void
      {
      ctx->RSGetViewports(&b.viewports, b.viewport);
      ctx->RSGetState(&b.raster);
      ctx->OMGetBlendState(&b.blend, b.blend_factor, &b.sample_mask);
      ctx->OMGetDepthStencilState(&b.depth, &b.stencil_ref);
      ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b.rtv, &b.dsv);
      ctx->PSGetShaderResources(0, 4, b.ps_srv);
      ctx->PSGetSamplers(0, 1, &b.ps_sampler);
      ctx->PSGetShader(&b.ps, b.ps_instances, &b.ps_instance_count);
      ctx->VSGetShader(&b.vs, b.vs_instances, &b.vs_instance_count);
      ctx->VSGetConstantBuffers(0, 1, &b.vs_cb);
      ctx->PSGetConstantBuffers(0, 1, &b.ps_cb);
      ctx->IAGetPrimitiveTopology(&b.topology);
      ctx->IAGetInputLayout(&b.layout);
      }

    template<typename T>
    auto release(T *& p) -> void
      {
      if(p)
        p->Release();
      p = nullptr;
      }

    auto restore(ID3D11DeviceContext * ctx, backup_t & b) -> void
      {
      ctx->RSSetViewports(b.viewports, b.viewport);
      ctx->RSSetState(b.raster);
      ctx->OMSetBlendState(b.blend, b.blend_factor, b.sample_mask);
      ctx->OMSetDepthStencilState(b.depth, b.stencil_ref);
      // the targets first: the surface goes back to being a shader resource only once it is no target
      ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b.rtv, b.dsv);
      ctx->PSSetShaderResources(0, 4, b.ps_srv);
      ctx->PSSetSamplers(0, 1, &b.ps_sampler);
      ctx->PSSetShader(b.ps, b.ps_instances, b.ps_instance_count);
      ctx->VSSetShader(b.vs, b.vs_instances, b.vs_instance_count);
      ctx->VSSetConstantBuffers(0, 1, &b.vs_cb);
      ctx->PSSetConstantBuffers(0, 1, &b.ps_cb);
      ctx->IASetPrimitiveTopology(b.topology);
      ctx->IASetInputLayout(b.layout);
      release(b.raster);
      release(b.blend);
      release(b.depth);
      for(auto & v: b.rtv)
        release(v);
      release(b.dsv);
      for(auto & v: b.ps_srv)
        release(v);
      release(b.ps_sampler);
      release(b.ps);
      for(UINT i{}; i != b.ps_instance_count; ++i)
        release(b.ps_instances[i]);
      release(b.vs);
      for(UINT i{}; i != b.vs_instance_count; ++i)
        release(b.vs_instances[i]);
      release(b.vs_cb);
      release(b.ps_cb);
      release(b.layout);
      }

    auto draw_rect(ID3D11DeviceContext * ctx, float const (&rect)[4], std::uint32_t rgb, float alpha, float mode) -> void
      {
      D3D11_MAPPED_SUBRESOURCE m{};
      if(FAILED(ctx->Map(r.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) or not m.pData)
        return;
      constants_t c{
        {rect[0], rect[1], rect[2], rect[3]},
        {static_cast<float>((rgb >> 16) & 0xffu) / 255.f, static_cast<float>((rgb >> 8) & 0xffu) / 255.f,
         static_cast<float>(rgb & 0xffu) / 255.f, alpha},
        {mode, 0.f, 0.f, 0.f}
      };
      std::memcpy(m.pData, &c, sizeof c);
      ctx->Unmap(r.cb, 0);
      ctx->Draw(4, 0);
      }

    ///\brief surface pixels -> the surface's normalised device coordinates (y up)
    auto to_ndc(float cx, float cy, float w, float h, float sw, float sh, float (&rect)[4]) -> void
      {
      rect[0] = (cx - w / 2.f) / sw * 2.f - 1.f;
      rect[2] = (cx + w / 2.f) / sw * 2.f - 1.f;
      rect[1] = 1.f - (cy + h / 2.f) / sh * 2.f;
      rect[3] = 1.f - (cy - h / 2.f) / sh * 2.f;
      }

    auto colour_of(allegiance_e a) -> std::uint32_t
      {
      switch(a)
        {
        case allegiance_e::federation: return 0xd9534fu;
        case allegiance_e::empire:     return 0x4a90d9u;
        case allegiance_e::alliance:   return 0x3cb371u;
        default:                       return 0xffffffu;
        }
      }
    }  // namespace

  auto panel_patch(ID3D11DeviceContext * ctx, ID3D11Device * device, std::uint64_t frame) noexcept -> void
    {
    settings_t const & s{settings()};
    if(s.patch == 0 or frame == patched_frame)
      return;
    game_state_t const g{game_state()};
    bool const test{s.patch == 2};
    int emblem{-1};
    allegiance_e const allegiance{
      s.patch_force >= 1 and s.patch_force <= 3 ? static_cast<allegiance_e>(s.patch_force) : g.allegiance
    };
    switch(allegiance)
      {
      case allegiance_e::federation: emblem = 0; break;
      case allegiance_e::empire:     emblem = 1; break;
      case allegiance_e::alliance:   emblem = 2; break;
      default:                       break;
      }
    if(not g.charging or (not test and emblem < 0))
      return;

    // the surface this composite draw samples, and whether it is the panel's
    ID3D11ShaderResourceView * srv{};
    ctx->PSGetShaderResources(2, 1, &srv);
    if(not srv)
      ctx->PSGetShaderResources(1, 1, &srv);
    if(not srv)
      return;
    ID3D11Resource * res{};
    srv->GetResource(&res);
    srv->Release();
    if(not res)
      return;
    D3D11_RESOURCE_DIMENSION dim{};
    res->GetType(&dim);
    ID3D11Texture2D * tex{};
    if(dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
    res->Release();
    if(not tex)
      return;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if(d.Width != s.patch_surface_width or d.Height != s.patch_surface_height or not create(device))
      {
      tex->Release();
      return;
      }
    ID3D11RenderTargetView * const rtv{rtv_for(tex)};
    tex->Release();
    if(not rtv)
      return;
    patched_frame = frame;

    backup_t b;
    save(ctx, b);
    D3D11_VIEWPORT const vp{0.f, 0.f, static_cast<float>(d.Width), static_cast<float>(d.Height), 0.f, 1.f};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(r.raster);
    ctx->OMSetDepthStencilState(r.no_depth, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(r.vs, nullptr, 0);
    ctx->PSSetShader(r.ps, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &r.cb);
    ctx->PSSetConstantBuffers(0, 1, &r.cb);
    ctx->PSSetSamplers(0, 1, &r.sampler);

    float const sw{static_cast<float>(d.Width)}, sh{static_cast<float>(d.Height)};
    float rect[4];
    to_ndc(s.patch_x, s.patch_y, s.patch_width, s.patch_height, sw, sh, rect);
    float const no_factor[4]{};
    ctx->OMSetBlendState(r.opaque, no_factor, 0xffffffffu);
    if(test)
      {
      draw_rect(ctx, rect, 0xff00ffu, 1.f, 0.f);  // magenta: where the patch goes
      to_ndc(s.patch_x, s.patch_y, s.patch_width - 8.f, s.patch_height - 8.f, sw, sh, rect);
      }
    draw_rect(ctx, rect, s.patch_ground, 1.f, 0.f);
    if(emblem >= 0)
      {
      float const eh{s.patch_emblem_height};
      float const ew{eh * static_cast<float>(r.emblem_width[emblem]) / static_cast<float>(r.emblem_height[emblem])};
      to_ndc(s.patch_x, s.patch_y, ew, eh, sw, sh, rect);
      ctx->OMSetBlendState(r.over, no_factor, 0xffffffffu);
      ctx->PSSetShaderResources(0, 1, &r.emblem[emblem]);
      draw_rect(ctx, rect, colour_of(allegiance), 1.f, 1.f);
      }
    restore(ctx, b);
    if(not told_drawing)
      {
      told_drawing = true;
      log_line("patch: first drawn (%s, %s%s, surface %ux%u)", test ? "test" : "emblem", allegiance_name(allegiance),
               s.patch_force ? " forced" : "", d.Width, d.Height);
      }
    }
  }  // namespace edworld
