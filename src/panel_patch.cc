// edworld — the jump panel patch. While a hyperspace jump charges, just before the game composites the
// interface surface that carries the charge panel into the cockpit, the panel's wrong superpower emblem is
// painted over on that surface with the panel's own black and the right emblem. Drawn onto the surface, the
// patch then rides the panel through the cockpit like the rest of its text: camera lag, head look, the
// hologram's own effects. The patch shows only where the game itself drew on the surface this frame (its
// alpha under each pixel, copied just before): when the game hides the panel while the ship still aligns, Status.json
// still says charging, and the patch must go with the panel.
//
// Under the panel the destination's factions are listed (from EHT's target in edworld_eht when it has them, else from
// EDSM, said so in the list's last row). Not on the surface: the panel shows only its own rectangle of it (rows 2-493
// of 3072x660), and the rows below belong to other panels. The list is drawn into a texture of its own, and right
// after the game's draw of the jump panel a quad of edworld's own carries it under the panel, in the panel's plane:
// the panel's local-to-surface map (read once from its vertices) takes the list's box, given in the surface's pixels
// past its edge, back to local points, which the list's vertex shader places as the game's shader places the panel.
// It shows while the game shows the panel (the surface's alpha at one point of it, the gate).
//
// The only place edworld changes what the game draws. The patch is a Dear ImGui draw list rendered by ImGui's
// D3D11 backend (its own context, no input, no files). Everything the game had bound is read back first and put
// back after, and edworld's own draws go through its own objects.
#include "panel_patch.h"

#include <d3d11_1.h>

#include "emblems.h"
#include "game_state.h"
#include "list_font.h"
#include "list_ps.h"
#include "list_vs.h"
#include "compass_math.h"
#include "hologram.h"
#include "panel_math.h"
#include "pad_text_read.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>

#ifndef EDWORLD_VERSION
#define EDWORLD_VERSION "dev"
#endif

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace edworld
  {
  auto imgui_check_failed(char const * what, char const * file, int line) noexcept -> void
    {
    static std::atomic<std::uint32_t> failures{};
    if(failures.fetch_add(1) == 0)  // the first one tells; a check failing every frame would flood the log
      log_line("imgui: check failed: %s (%s:%d); later failures not logged", what, file, line);
    }

  namespace
    {
    struct resources_t
      {
      ID3D11Device * device{};
      ImGuiContext * imgui{};
      ID3D11ShaderResourceView * emblem[4]{};  ///< Federation, Empire, Alliance, Independent
      std::uint32_t emblem_width[4]{};
      std::uint32_t emblem_height[4]{};
      ///\brief the panel's text area copied for reading its SUPERPOWER row (superpower_area on the surface)
      ID3D11Texture2D * text_copy{};
      // a copy of the panel's surface, only the patch's box written: what the game drew there this frame
      ID3D11Texture2D * mask{};
      ID3D11ShaderResourceView * mask_srv{};
      std::uint32_t mask_width{}, mask_height{};
      ///\brief the pixel shader's b0: the patch's box (alpha from under the pixel) and the gate (alpha elsewhere)
      ID3D11Buffer * gate{};
      ///\brief the list's font; none when it could not be made (no list then, the emblem still)
      ImFont * font{};
      ///\brief the spheres' lines' font, made at their size (a small font enlarged comes out soft); none: the list's
      ImFont * sphere_font{};
      // the list: its texture (premultiplied, drawn by ImGui), and what carries it under the panel
      ID3D11Texture2D * list_tex{};
      ID3D11RenderTargetView * list_rtv{};
      ID3D11ShaderResourceView * list_srv{};
      std::uint32_t list_width{}, list_height{};
      ID3D11ShaderResourceView * white{};  ///< 1x1 opaque: the ImGui shader's gate texture when drawing the list's
      ID3D11Buffer * white_gate{};
      ID3D11VertexShader * list_vs{};
      ID3D11PixelShader * list_ps{};
      ID3D11InputLayout * list_layout{};
      ID3D11Buffer * list_cb{};
      ID3D11SamplerState * list_sampler{};
      ID3D11BlendState * list_blend{};
      ID3D11DepthStencilState * list_depth{};
      ID3D11RasterizerState * list_raster{};
      bool list_failed{};
      bool failed{};
      };

    struct gate_t
      {
      std::int32_t box[4];
      std::int32_t point[4];
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
    std::uint64_t told_list_for{};
    bool told_list_quad{};

    ///\brief the emblem as white with its coverage as alpha, so ImGui's colour x texture tints it
    auto make_emblem(std::uint8_t const * mask, std::uint32_t w, std::uint32_t h, std::uint32_t slot) -> bool
      {
      std::vector<std::uint32_t> rgba(std::size_t{w} * h);
      for(std::size_t i{}; i != rgba.size(); ++i)
        rgba[i] = 0x00ffffffu | (std::uint32_t{mask[i]} << 24);
      D3D11_TEXTURE2D_DESC d{};
      d.Width = w;
      d.Height = h;
      d.MipLevels = 1;
      d.ArraySize = 1;
      d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_IMMUTABLE;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA init{rgba.data(), w * 4u, 0};
      ID3D11Texture2D * tex{};
      if(FAILED(r.device->CreateTexture2D(&d, &init, &tex)) or not tex)
        return false;
      HRESULT const hr{r.device->CreateShaderResourceView(tex, nullptr, &r.emblem[slot])};
      tex->Release();
      r.emblem_width[slot] = w;
      r.emblem_height[slot] = h;
      return SUCCEEDED(hr);
      }

    ///\brief ImGui (its own context, nothing but drawing) and the emblems, once per device
    auto create(ID3D11Device * device, ID3D11DeviceContext * ctx) -> bool
      {
      if(r.device == device)
        return not r.failed;
      if(r.imgui)
        {
        ImGui::SetCurrentContext(r.imgui);
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(r.imgui);
        }
      for(auto & e: r.emblem)
        if(e)
          e->Release();
      if(r.mask_srv)
        r.mask_srv->Release();
      if(r.mask)
        r.mask->Release();
      if(r.gate)
        r.gate->Release();
      if(r.text_copy)
        r.text_copy->Release();
      for(IUnknown * u: std::initializer_list<IUnknown *>{r.list_tex, r.list_rtv, r.list_srv, r.white, r.white_gate, r.list_vs, r.list_ps,
                                                         r.list_layout, r.list_cb, r.list_sampler, r.list_blend, r.list_depth,
                                                         r.list_raster})
        if(u)
          u->Release();
      r = resources_t{};
      r.device = device;
      r.imgui = ImGui::CreateContext();
      ImGuiIO & io{ImGui::GetIO()};
      io.IniFilename = nullptr;
      io.LogFilename = nullptr;
      io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
      // the font before the backend makes its texture; Latin-1 and Latin Extended-A cover the factions' names
      {
      ImFontConfig cfg;
      cfg.FontDataOwnedByAtlas = false;
      static ImWchar const ranges[]{0x0020, 0x017F, 0};
      r.font = io.Fonts->AddFontFromMemoryTTF(const_cast<std::uint8_t *>(font::list_font), static_cast<int>(font::list_font_size),
                                              std::clamp(settings().list_text, 8.f, 64.f), &cfg, ranges);
      if(not r.font)
        log_line("patch: the list's font could not be made; no list");
      if(settings().compass_spheres)
        r.sphere_font = io.Fonts->AddFontFromMemoryTTF(const_cast<std::uint8_t *>(font::list_font), static_cast<int>(font::list_font_size),
                                                       84.f, &cfg, ranges);
      }
      creating_own.store(true);
      bool ok{ImGui_ImplDX11_Init(device, ctx)};
      ok = ok and ImGui_ImplDX11_CreateDeviceObjects();
      creating_own.store(false);
      ok = ok and make_emblem(emblems::federation_mask, emblems::federation_width, emblems::federation_height, 0);
      ok = ok and make_emblem(emblems::empire_mask, emblems::empire_width, emblems::empire_height, 1);
      ok = ok and make_emblem(emblems::alliance_mask, emblems::alliance_width, emblems::alliance_height, 2);
      ok = ok and make_emblem(emblems::independent_mask, emblems::independent_width, emblems::independent_height, 3);
      if(ok)
        {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(gate_t);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &r.gate)) and r.gate;
        }
      r.failed = not ok;
      log_line(ok ? "patch: resources ready (imgui %s)" : "patch: resources could not be made (imgui %s); no patch", IMGUI_VERSION);
      return ok;
      }

    ///\brief the mask copy, as large as the surface and of its format, made again when the size changes
    auto mask_for(D3D11_TEXTURE2D_DESC const & surface) -> ID3D11ShaderResourceView *
      {
      if(r.mask and r.mask_width == surface.Width and r.mask_height == surface.Height)
        return r.mask_srv;
      if(r.mask_srv)
        r.mask_srv->Release();
      if(r.mask)
        r.mask->Release();
      r.mask = nullptr;
      r.mask_srv = nullptr;
      D3D11_TEXTURE2D_DESC d{};
      d.Width = surface.Width;
      d.Height = surface.Height;
      d.MipLevels = 1;
      d.ArraySize = 1;
      d.Format = surface.Format;
      d.SampleDesc.Count = 1;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if(FAILED(r.device->CreateTexture2D(&d, nullptr, &r.mask)) or not r.mask)
        return nullptr;
      D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
      vd.Format = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : d.Format;
      vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      vd.Texture2D.MipLevels = 1;
      if(FAILED(r.device->CreateShaderResourceView(r.mask, &vd, &r.mask_srv)))
        {
        r.mask->Release();
        r.mask = nullptr;
        return nullptr;
        }
      r.mask_width = surface.Width;
      r.mask_height = surface.Height;
      return r.mask_srv;
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

    ///\brief everything edworld's draws touch, as the game left it
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

    ///\brief 0xRRGGBB -> ImGui's packed colour, opaque
    auto im_colour(std::uint32_t rgb) -> ImU32
      {
      return IM_COL32((rgb >> 16) & 0xffu, (rgb >> 8) & 0xffu, rgb & 0xffu, 0xffu);
      }

    ///\brief a w x h box centred on (cx, cy), in surface pixels
    auto box(ImDrawList * dl, float cx, float cy, float w, float h, ImU32 colour) -> void
      {
      dl->AddRectFilled(ImVec2{cx - w / 2.f, cy - h / 2.f}, ImVec2{cx + w / 2.f, cy + h / 2.f}, colour);
      }

    auto colour_of(allegiance_e a) -> std::uint32_t
      {
      switch(a)
        {
        case allegiance_e::federation:  return 0xd9534fu;
        case allegiance_e::empire:      return 0x4a90d9u;
        case allegiance_e::alliance:    return 0x3cb371u;
        case allegiance_e::independent: return settings().independent_colour;
        default:                        return 0xffffffu;
        }
      }

    auto unix_now() -> std::int64_t
      {
      FILETIME ft{};
      GetSystemTimeAsFileTime(&ft);
      std::uint64_t const t{(std::uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime};
      return static_cast<std::int64_t>((t - 116444736000000000ull) / 10000000ull);
      }

    ///\brief the destination's factions in a box under the panel: the trend at the last tick, the controlling one
    /// starred, name, influence, states; EDSM's list says so in its last row, with the age of its data. In test mode
    /// with nothing to list, placeholder rows show where the box goes
    ///\brief the EDSM source line (and the test's), empty when the list is the data source's
    auto source_line(faction_list_t const & list, bool placeholder, std::uint32_t rows, float size, char (&buf)[128]) -> bool
      {
      if(placeholder)
        std::snprintf(buf, sizeof buf, "list test: %u rows at most, text %.0f px", rows, static_cast<double>(size));
      else if(list.source != list_source_e::edsm)
        return false;
      else if(list.updated_unix_s <= 0)
        std::snprintf(buf, sizeof buf, "from EDSM");
      else
        {
        std::int64_t const age{std::max<std::int64_t>(0, unix_now() - list.updated_unix_s)};
        if(age < 3600)
          std::snprintf(buf, sizeof buf, "from EDSM, updated %lld min ago", static_cast<long long>(age / 60));
        else if(age < 48 * 3600)
          std::snprintf(buf, sizeof buf, "from EDSM, updated %lld h ago", static_cast<long long>(age / 3600));
        else
          std::snprintf(buf, sizeof buf, "from EDSM, updated %lld days ago", static_cast<long long>(age / 86400));
        }
      return true;
      }

    ///\brief the destination's factions in a black box, as wide as its rows and centred on centre_x: the trend at the
    /// last tick, the controlling one starred, name, influence, states; EDSM's list says so in its last row, with the
    /// age of its data. In test mode with nothing to list, placeholder rows show where the box goes. The box is at
    /// most s.list_width wide. Returns its height
    auto draw_list(ImDrawList * dl, settings_t const & s, faction_list_t const & list, bool test, float centre_x, float top) -> float
      {
      if(not r.font)
        return 0.f;
      float const size{r.font->FontSize};
      float const line{std::round(size * 1.15f)};
      float const pad{std::round(size * 0.3f)};
      bool const placeholder{test and list.count == 0};
      list_lines_t const lines{list_lines(list.count, list.source == list_source_e::edsm, placeholder, s.list_rows)};
      std::uint32_t const source_rows{lines.source};
      std::uint32_t const rows{lines.factions};
      std::uint32_t const shown{lines.factions};
      if(shown == 0)
        return 0.f;
      auto const width = [&](char const * text) { return r.font->CalcTextSizeA(size, FLT_MAX, 0.f, text).x; };
      float const cw{width("0")};
      char buf[128];

      // the columns as wide as what they hold: names (at most 32 characters), influence, states (at most 32)
      float name_w{}, influence_w{}, states_w{};
      for(std::uint32_t i{}; i != shown; ++i)
        {
        if(placeholder)
          {
          std::snprintf(buf, sizeof buf, "list test row %u of %u", i + 1u, shown);
          name_w = std::max(name_w, width(buf));
          continue;
          }
        faction_row_t const & row{list.rows[i]};
        name_w = std::max(name_w, std::min(width(row.name), 32.f * cw));
        std::snprintf(buf, sizeof buf, "%.1f%%", static_cast<double>(row.influence) * 100.0);
        influence_w = std::max(influence_w, width(buf));
        if(row.states[0] != '\0')
          states_w = std::max(states_w, std::min(width(row.states), 32.f * cw));
        }
      float inner{2.f * cw + name_w};
      if(influence_w > 0.f)
        inner += 2.f * cw + influence_w;
      if(states_w > 0.f)
        inner += 2.f * cw + states_w;
      char source[128];
      bool const with_source{source_line(list, placeholder, rows, size, source) and source_rows != 0};
      if(with_source)
        inner = std::max(inner, 2.f * cw + width(source));
      // the footer: edworld's name and version, small and grey in the box's bottom right corner
      char footer[64];
      std::snprintf(footer, sizeof footer, "edworld %s", EDWORLD_VERSION);
      float const footer_size{std::round(size * 0.55f)};
      float const footer_w{r.font->CalcTextSizeA(footer_size, FLT_MAX, 0.f, footer).x};
      inner = std::max(inner, footer_w);
      float const box_w{std::min(inner + 2.f * pad, s.list_width)};
      float const left{std::round(centre_x - box_w / 2.f)}, right{left + box_w};
      float const bottom{top + list_height(lines, line, pad, footer_size)};
      dl->AddRectFilled(ImVec2{left, top}, ImVec2{right, bottom}, IM_COL32(0, 0, 0, 255));
      if(test)
        dl->AddRect(ImVec2{left, top}, ImVec2{right, bottom}, im_colour(0xff00ffu), 0.f, 0, 2.f);

      float const x_star{left + pad + cw};
      float const x_name{left + pad + 2.f * cw};
      float const x_influence{x_name + name_w + 2.f * cw + influence_w};  // the influence's right edge
      float const x_states{x_influence + 2.f * cw};
      ImU32 const grey{im_colour(0x9aa0a6u)};
      for(std::uint32_t i{}; i != shown; ++i)
        {
        float const y{top + pad + static_cast<float>(i) * line};
        if(placeholder)
          {
          std::snprintf(buf, sizeof buf, "list test row %u of %u", i + 1u, shown);
          dl->AddText(r.font, size, ImVec2{x_name, y}, im_colour(0xe0e0e0u), buf);
          continue;
          }
        faction_row_t const & row{list.rows[i]};
        float const cx{left + pad + cw * 0.5f}, mid{y + line * 0.5f}, half{cw * 0.4f};
        if(row.trend == 1u)
          dl->AddTriangleFilled(ImVec2{cx - half, mid + half}, ImVec2{cx + half, mid + half}, ImVec2{cx, mid - half}, im_colour(0x5cd65cu));
        else if(row.trend == 3u)
          dl->AddTriangleFilled(ImVec2{cx - half, mid - half}, ImVec2{cx, mid + half}, ImVec2{cx + half, mid - half}, im_colour(0xff6060u));
        else if(row.trend == 2u)
          dl->AddRectFilled(ImVec2{cx - half, mid - std::max(1.f, size * 0.05f)}, ImVec2{cx + half, mid + std::max(1.f, size * 0.05f)}, grey);
        if(row.controlling)
          dl->AddText(r.font, size, ImVec2{x_star, y}, im_colour(0xe0e0e0u), "*");
        std::uint32_t const tint{row.allegiance >= 1u and row.allegiance <= 3u ? colour_of(static_cast<allegiance_e>(row.allegiance)) : 0xc8c8c8u};
        ImVec4 const name_clip{x_name, y, x_name + name_w, y + line};
        dl->AddText(r.font, size, ImVec2{x_name, y}, im_colour(tint), row.name, nullptr, 0.f, &name_clip);
        std::snprintf(buf, sizeof buf, "%.1f%%", static_cast<double>(row.influence) * 100.0);
        dl->AddText(r.font, size, ImVec2{x_influence - width(buf), y}, im_colour(0xe0e0e0u), buf);
        if(row.states[0] != '\0')
          {
          ImVec4 const states_clip{x_states, y, right - pad, y + line};
          dl->AddText(r.font, size, ImVec2{x_states, y}, grey, row.states, nullptr, 0.f, &states_clip);
          }
        }
      if(with_source)
        dl->AddText(r.font, size, ImVec2{x_name, top + pad + static_cast<float>(shown) * line}, grey, source);
      dl->AddText(r.font, footer_size, ImVec2{right - pad - footer_w, bottom - pad - footer_size}, im_colour(0x6e7378u), footer);
      return bottom - top;
      }

    // ---- the list under the panel: its texture, the panel's map, the quad ----
    struct list_cb_t
      {
      float corner[4][4];
      float uv_extent[4];
      std::int32_t gate[4];
      float gain[4];
      float colour[4];  ///< x: the texture's gamma (0: as it is)
      };

    std::uint64_t list_frame{~0ull};  ///< the frame the list's texture was drawn in
    float list_used{};                ///< its height in that frame, in pixels

    ///\brief the list's shaders and states, once per device; none = no list (logged once)
    auto make_list_objects() -> bool
      {
      if(r.list_vs)
        return true;
      if(r.list_failed)
        return false;
      creating_own.store(true);
      bool ok{SUCCEEDED(r.device->CreateVertexShader(g_list_vs, sizeof g_list_vs, nullptr, &r.list_vs))};
      ok = ok and SUCCEEDED(r.device->CreatePixelShader(g_list_ps, sizeof g_list_ps, nullptr, &r.list_ps));
      creating_own.store(false);
      // the instance index from the game's own instance stream (VB0), as its panel shader reads it
      D3D11_INPUT_ELEMENT_DESC const element{"INSTANCEINDEX", 0, DXGI_FORMAT_R32G32_UINT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1};
      ok = ok and SUCCEEDED(r.device->CreateInputLayout(&element, 1, g_list_vs, sizeof g_list_vs, &r.list_layout));
      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = sizeof(list_cb_t);
      bd.Usage = D3D11_USAGE_DYNAMIC;
      bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      ok = ok and SUCCEEDED(r.device->CreateBuffer(&bd, nullptr, &r.list_cb));
      D3D11_SAMPLER_DESC sd{};
      sd.Filter = D3D11_FILTER_ANISOTROPIC;
      sd.MaxAnisotropy = 8;
      // the cockpit shows the list at about 0.7 of its texture (the game's supersampling below 1): half a mip level
      // sharper than the filter would pick, so its letters do not go soft
      sd.MipLODBias = -0.5f;
      sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.MaxLOD = D3D11_FLOAT32_MAX;
      ok = ok and SUCCEEDED(r.device->CreateSamplerState(&sd, &r.list_sampler));
      D3D11_BLEND_DESC blend{};
      blend.RenderTarget[0].BlendEnable = TRUE;
      blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;  // the texture is premultiplied
      blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
      blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
      blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
      blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
      blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      ok = ok and SUCCEEDED(r.device->CreateBlendState(&blend, &r.list_blend));
      D3D11_DEPTH_STENCIL_DESC depth{};
      depth.DepthEnable = FALSE;
      depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
      depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
      ok = ok and SUCCEEDED(r.device->CreateDepthStencilState(&depth, &r.list_depth));
      D3D11_RASTERIZER_DESC raster{};
      raster.FillMode = D3D11_FILL_SOLID;
      raster.CullMode = D3D11_CULL_NONE;
      raster.DepthClipEnable = TRUE;
      ok = ok and SUCCEEDED(r.device->CreateRasterizerState(&raster, &r.list_raster));
      // the ImGui shader multiplies by a gate texture's alpha: for the list's own texture, an opaque pixel
      std::uint32_t const white{0xffffffffu};
      D3D11_TEXTURE2D_DESC td{};
      td.Width = td.Height = 1;
      td.MipLevels = td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_IMMUTABLE;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA const init{&white, 4u, 0};
      ID3D11Texture2D * tex{};
      ok = ok and SUCCEEDED(r.device->CreateTexture2D(&td, &init, &tex)) and tex;
      ok = ok and SUCCEEDED(r.device->CreateShaderResourceView(tex, nullptr, &r.white));
      if(tex)
        tex->Release();
      gate_t const zero{};
      D3D11_BUFFER_DESC gd{};
      gd.ByteWidth = sizeof(gate_t);
      gd.Usage = D3D11_USAGE_IMMUTABLE;
      gd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA const gate_init{&zero, 0, 0};
      ok = ok and SUCCEEDED(r.device->CreateBuffer(&gd, &gate_init, &r.white_gate));
      if(not ok)
        {
        r.list_failed = true;
        log_line("list: its shaders or states could not be made; no list (the emblem still)");
        }
      return ok;
      }

    ///\brief the list's texture, made again when its size changes
    auto list_target(std::uint32_t w, std::uint32_t h) -> bool
      {
      if(r.list_tex and r.list_width == w and r.list_height == h)
        return true;
      for(IUnknown * u: std::initializer_list<IUnknown *>{r.list_srv, r.list_rtv, r.list_tex})
        if(u)
          u->Release();
      r.list_srv = nullptr;
      r.list_rtv = nullptr;
      r.list_tex = nullptr;
      // a full mip chain: the cockpit shows the list smaller than it is drawn, and without mips its text breaks up
      D3D11_TEXTURE2D_DESC d{};
      d.Width = w;
      d.Height = h;
      d.MipLevels = 0;
      d.ArraySize = 1;
      d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.SampleDesc.Count = 1;
      d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
      if(FAILED(r.device->CreateTexture2D(&d, nullptr, &r.list_tex)) or not r.list_tex
         or FAILED(r.device->CreateRenderTargetView(r.list_tex, nullptr, &r.list_rtv))
         or FAILED(r.device->CreateShaderResourceView(r.list_tex, nullptr, &r.list_srv)))
        return false;
      r.list_width = w;
      r.list_height = h;
      return true;
      }

    ///\brief one panel draw as the list knows it: which rectangle of the surface it shows, read once from its vertices
    struct panel_draw_t
      {
      enum struct state_e : std::uint8_t
        {
        empty,
        pending,
        other,
        jump_panel
        };
      void * ib;
      UINT ib_offset;
      std::uint32_t start_index;
      void * vb;
      UINT vb_offset;
      std::int32_t base_vertex;
      state_e state;
      std::uint32_t index_bytes;
      ID3D11Buffer * staged_ib;
      ID3D11Buffer * staged_vb;
      LONGLONG queued;
      panel_map_t map;
      };

    constexpr std::uint32_t max_panel_draws{32};
    constexpr std::uint32_t staged_vertices{16};
    panel_draw_t panel_draws[max_panel_draws]{};
    std::uint32_t panel_draw_next{};

    auto qpc() -> LONGLONG
      {
      LARGE_INTEGER t{};
      QueryPerformanceCounter(&t);
      return t.QuadPart;
      }

    auto staging_copy(ID3D11DeviceContext * ctx, ID3D11Buffer * source, std::uint64_t from, std::uint32_t bytes) -> ID3D11Buffer *
      {
      D3D11_BUFFER_DESC bd{};
      source->GetDesc(&bd);
      if(from + bytes > bd.ByteWidth)
        return nullptr;
      D3D11_BUFFER_DESC sd{};
      sd.ByteWidth = (bytes + 15u) & ~15u;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Buffer * staging{};
      if(FAILED(r.device->CreateBuffer(&sd, nullptr, &staging)) or not staging)
        return nullptr;
      D3D11_BOX const box{static_cast<UINT>(from), 0, 0, static_cast<UINT>(from + bytes), 1, 1};
      ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);
      return staging;
      }

    auto drop_staged(panel_draw_t & e) -> void
      {
      if(e.staged_ib)
        e.staged_ib->Release();
      if(e.staged_vb)
        e.staged_vb->Release();
      e.staged_ib = nullptr;
      e.staged_vb = nullptr;
      }

    ///\brief the copied vertices read: the draw's map, and whether its rectangle holds the patch's centre (the jump panel)
    auto judge(ID3D11DeviceContext * ctx, panel_draw_t & e, settings_t const & s) -> void
      {
      D3D11_MAPPED_SUBRESOURCE mi{}, mv{};
      HRESULT const hi{ctx->Map(e.staged_ib, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mi)};
      if(hi == DXGI_ERROR_WAS_STILL_DRAWING)
        return;
      HRESULT const hv{SUCCEEDED(hi) ? ctx->Map(e.staged_vb, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mv) : hi};
      if(hv == DXGI_ERROR_WAS_STILL_DRAWING)
        {
        ctx->Unmap(e.staged_ib, 0);
        return;
        }
      e.state = panel_draw_t::state_e::other;
      if(SUCCEEDED(hi) and SUCCEEDED(hv) and mi.pData and mv.pData)
        {
        std::uint32_t const size{e.index_bytes};
        std::uint32_t idx[3]{};
        for(std::uint32_t i{}; i != 3; ++i)
          {
          if(size == 2u)
            {
            std::uint16_t v{};
            std::memcpy(&v, static_cast<std::uint8_t const *>(mi.pData) + i * 2u, 2);
            idx[i] = v;
            }
          else
            std::memcpy(&idx[i], static_cast<std::uint8_t const *>(mi.pData) + i * 4u, 4);
          }
        if(idx[0] < staged_vertices and idx[1] < staged_vertices and idx[2] < staged_vertices)
          {
          vec3_t p[3];
          float px[3][2];
          for(std::uint32_t i{}; i != 3; ++i)
            {
            std::uint32_t w[5];
            std::memcpy(w, static_cast<std::uint8_t const *>(mv.pData) + idx[i] * vertex_stride, sizeof w);
            p[i] = decode_local(w[0], w[1], w[2]);
            float u, v;
            decode_uv(w[4], u, v);
            px[i][0] = u * static_cast<float>(s.patch_surface_width);
            px[i][1] = v * static_cast<float>(s.patch_surface_height);
            }
          if(auto const m{map_from(p, px)}; m)
            {
            e.map = *m;
            // the rectangle the draw shows: the first triangle's box (a quad's two triangles share it)
            float const x0{std::min({px[0][0], px[1][0], px[2][0]})}, x1{std::max({px[0][0], px[1][0], px[2][0]})};
            float const y0{std::min({px[0][1], px[1][1], px[2][1]})}, y1{std::max({px[0][1], px[1][1], px[2][1]})};
            bool const holds{s.patch_x >= x0 and s.patch_x <= x1 and s.patch_y >= y0 and s.patch_y <= y1};
            if(holds)
              e.state = panel_draw_t::state_e::jump_panel;
            log_line("list: a draw (base vertex %d) shows x %.0f..%.0f, y %.0f..%.0f of the surface%s", e.base_vertex,
                     static_cast<double>(x0), static_cast<double>(x1), static_cast<double>(y0), static_cast<double>(y1),
                     holds ? ": the jump panel" : "");
            }
          }
        }
      if(SUCCEEDED(hi))
        ctx->Unmap(e.staged_ib, 0);
      if(SUCCEEDED(hv))
        ctx->Unmap(e.staged_vb, 0);
      drop_staged(e);
      }

    ///\brief everything the list's quad sets, as the game left it (the game's buffers and records stay bound)
    struct list_backup_t
      {
      ID3D11VertexShader * vs{};
      ID3D11ClassInstance * vs_instances[256]{};
      UINT vs_instance_count{256};
      ID3D11PixelShader * ps{};
      ID3D11ClassInstance * ps_instances[256]{};
      UINT ps_instance_count{256};
      ID3D11InputLayout * layout{};
      D3D11_PRIMITIVE_TOPOLOGY topology{};
      ID3D11Buffer * vs_cb{};
      ID3D11Buffer * ps_cb{};
      ID3D11ShaderResourceView * ps_srv[2]{};
      ID3D11SamplerState * ps_sampler{};
      ID3D11BlendState * blend{};
      float blend_factor[4]{};
      UINT sample_mask{};
      ID3D11DepthStencilState * depth{};
      UINT stencil_ref{};
      ID3D11RasterizerState * raster{};
      };

    constexpr UINT list_cb_slot{4};

    // ---- the superpower, read from the panel's own text ----
    constexpr std::int32_t text_x{superpower_label_x0}, text_y{superpower_area_y0};
    constexpr std::int32_t text_width{superpower_value_x1 - superpower_label_x0}, text_height{superpower_area_y1 - superpower_area_y0};
    constexpr std::uint32_t text_tries{240};  ///< frames of an unreadable panel (hidden while aligning, no superpower) before giving up

    struct reading_t
      {
      std::uint64_t destination{~0ull};
      superpower_reading_t result{};
      bool done{};
      bool copied{};  ///< a copy waits in r.text_copy
      std::uint32_t tries{};
      };

    reading_t reading;

    auto superpower_allegiance(superpower_e p) -> allegiance_e
      {
      switch(p)
        {
        case superpower_e::federation:  return allegiance_e::federation;
        case superpower_e::empire:      return allegiance_e::empire;
        case superpower_e::alliance:    return allegiance_e::alliance;
        case superpower_e::independent: return allegiance_e::independent;
        default:                        return allegiance_e::unknown;
        }
      }

    ///\brief the area under reading, as it was copied, into edworld_dumps (an unknown word: to learn it)
    auto dump_text(D3D11_MAPPED_SUBRESOURCE const & m, std::uint64_t destination) -> void
      {
      CreateDirectoryW((settings().dir + L"\\edworld_dumps").c_str(), nullptr);
      wchar_t name[MAX_PATH];
      std::swprintf(name, MAX_PATH, L"%ls\\edworld_dumps\\superpower_%llu_%dx%d_pitch%u.raw", settings().dir.c_str(),
                    static_cast<unsigned long long>(destination), text_width, text_height, m.RowPitch);
      if(std::FILE * f{_wfopen(name, L"wb")})
        {
        std::fwrite(m.pData, 1, static_cast<std::size_t>(m.RowPitch) * text_height, f);
        std::fclose(f);
        log_line("superpower: the area read written to %S", name);
        }
      }

    ///\brief once a frame while a jump charges: the panel's text copied (this frame) and read (a few frames later,
    /// when the copy is done); per destination, until its SUPERPOWER row is found or the tries run out
    auto read_text(ID3D11DeviceContext * ctx, ID3D11Texture2D * surface, D3D11_TEXTURE2D_DESC const & d, std::uint64_t destination) -> void
      {
      if(d.Width < static_cast<UINT>(superpower_value_x1) or d.Height < static_cast<UINT>(superpower_area_y1))
        return;  // not the 3072x660 surface the rows were measured on
      if(destination != reading.destination)
        reading = reading_t{destination, {}, false, false, 0};
      if(reading.done)
        return;
      if(not r.text_copy)
        {
        D3D11_TEXTURE2D_DESC c{};
        c.Width = static_cast<UINT>(text_width);
        c.Height = static_cast<UINT>(text_height);
        c.MipLevels = c.ArraySize = 1;
        c.Format = d.Format;
        c.SampleDesc.Count = 1;
        c.Usage = D3D11_USAGE_STAGING;
        c.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if(FAILED(r.device->CreateTexture2D(&c, nullptr, &r.text_copy)) or not r.text_copy)
          {
          reading.done = true;
          log_line("superpower: no copy of the panel's text could be made; no emblem");
          return;
          }
        }
      if(reading.copied)
        {
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT const hr{ctx->Map(r.text_copy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)};
        if(hr == DXGI_ERROR_WAS_STILL_DRAWING)
          return;
        reading.copied = false;
        if(SUCCEEDED(hr) and m.pData)
          {
          reading.result = read_superpower(static_cast<std::uint8_t const *>(m.pData), m.RowPitch, text_width, text_height, text_x, text_y);
          if(reading.result.label_found)
            {
            reading.done = true;
            superpower_reading_t const & x{reading.result};
            if(x.superpower != superpower_e::none)
              log_line("superpower: %llu reads %s on the panel (label %d px, word %d px, row %d..%d); emblem at y %.0f",
                       static_cast<unsigned long long>(destination), allegiance_name(superpower_allegiance(x.superpower)),
                       x.label_width, x.value_width, x.row_top, x.row_bottom, static_cast<double>(emblem_centre_y(x)));
            else
              {
              log_line("superpower: %llu: a SUPERPOWER row with a word of no known width (label %d px, word %d px, row %d..%d); no emblem",
                       static_cast<unsigned long long>(destination), x.label_width, x.value_width, x.row_top, x.row_bottom);
              dump_text(m, destination);
              }
            }
          ctx->Unmap(r.text_copy, 0);
          }
        if(not reading.done and ++reading.tries >= text_tries)
          {
          reading.done = true;
          log_line("superpower: %llu: no SUPERPOWER row on the panel in %u frames (no superpower, or the panel hidden); no emblem",
                   static_cast<unsigned long long>(destination), text_tries);
          }
        if(reading.done)
          return;
        }
      D3D11_BOX const area{static_cast<UINT>(text_x), static_cast<UINT>(text_y), 0, static_cast<UINT>(text_x + text_width),
                           static_cast<UINT>(text_y + text_height), 1};
      ctx->CopySubresourceRegion(r.text_copy, 0, 0, 0, 0, surface, 0, &area);
      reading.copied = true;
      }

    float patch_centre_y{};  ///< where the patch stands this frame (the gate of the list's quad)
    }  // namespace

  auto panel_patch(ID3D11DeviceContext * ctx, ID3D11Device * device, std::uint64_t frame) noexcept -> void
    {
    settings_t const & s{settings()};
    if(s.patch == 0 or frame == patched_frame)
      return;
    game_state_t const g{game_state()};
    if(not g.charging)
      return;
    bool const test{s.patch == 2};

    // the surface this composite draw samples, and whether it is the panel's: at PS t2 for the panel family,
    // at t1 for the shader the game uses near a war settlement
    ID3D11Texture2D * tex{};
    D3D11_TEXTURE2D_DESC d{};
    for(UINT const slot: {2u, 1u})
      {
      ID3D11ShaderResourceView * srv{};
      ctx->PSGetShaderResources(slot, 1, &srv);
      if(not srv)
        continue;
      ID3D11Resource * res{};
      srv->GetResource(&res);
      srv->Release();
      if(not res)
        continue;
      D3D11_RESOURCE_DIMENSION dim{};
      res->GetType(&dim);
      ID3D11Texture2D * t{};
      if(dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t));
      res->Release();
      if(not t)
        continue;
      t->GetDesc(&d);
      if(d.Width == s.patch_surface_width and d.Height == s.patch_surface_height)
        {
        tex = t;
        break;
        }
      t->Release();
      }
    if(not tex)
      return;
    if(not create(device, ctx))
      {
      tex->Release();
      return;
      }
    patched_frame = frame;

    // the superpower as the panel writes it; the emblem goes beside its row (patch_force overrides the word)
    read_text(ctx, tex, d, g.destination);
    bool const read_ok{reading.done and reading.result.superpower != superpower_e::none};
    allegiance_e const allegiance{
      s.patch_force >= 1 and s.patch_force <= 4 ? static_cast<allegiance_e>(s.patch_force)
      : read_ok                                 ? superpower_allegiance(reading.result.superpower)
                                                : allegiance_e::unknown
    };
    int emblem{-1};
    switch(allegiance)
      {
      case allegiance_e::federation:  emblem = 0; break;
      case allegiance_e::empire:      emblem = 1; break;
      case allegiance_e::alliance:    emblem = 2; break;
      case allegiance_e::independent: emblem = 3; break;
      default:                        break;
      }
    float const centre_y{reading.done and reading.result.label_found ? emblem_centre_y(reading.result) : s.patch_y};
    patch_centre_y = centre_y;
    // the list only for the destination it was made for, and only when it has something to say
    faction_list_t const list{s.list != 0 ? destination_factions() : faction_list_t{}};
    bool const listing{list.system == g.destination and list.source != list_source_e::none and list.count != 0};
    if(not test and emblem < 0 and not listing)
      {
      tex->Release();
      return;
      }
    ID3D11RenderTargetView * const rtv{rtv_for(tex)};
    ID3D11ShaderResourceView * const mask{rtv ? mask_for(d) : nullptr};
    if(not rtv or not mask)
      {
      tex->Release();
      return;
      }
    // what the game drew under the patch this frame: where it drew nothing (the panel hidden while the ship
    // still aligns, though Status.json still says charging), the patch's alpha goes to zero
    // (the box's alpha also gates what is drawn beside the panel: the shader reads it at the patch's centre)
    {
    float const left{s.patch_x - s.patch_width / 2.f}, top{centre_y - s.patch_height / 2.f};
    UINT const x0{left > 0.f ? static_cast<UINT>(left) : 0u}, y0{top > 0.f ? static_cast<UINT>(top) : 0u};
    UINT const x1{static_cast<UINT>(s.patch_x + s.patch_width / 2.f) + 1u}, y1{static_cast<UINT>(centre_y + s.patch_height / 2.f) + 1u};
    D3D11_BOX const area{x0, y0, 0, x1 < d.Width ? x1 : d.Width, y1 < d.Height ? y1 : d.Height, 1};
    if(area.left < area.right and area.top < area.bottom)
      ctx->CopySubresourceRegion(r.mask, 0, area.left, area.top, 0, tex, 0, &area);
    D3D11_MAPPED_SUBRESOURCE m{};
    if(FAILED(ctx->Map(r.gate, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
      {
      tex->Release();
      return;
      }
    gate_t const gate{
      {static_cast<std::int32_t>(area.left), static_cast<std::int32_t>(area.top), static_cast<std::int32_t>(area.right),
       static_cast<std::int32_t>(area.bottom)},
      {static_cast<std::int32_t>(s.patch_x), static_cast<std::int32_t>(centre_y), 0, 0}
    };
    std::memcpy(m.pData, &gate, sizeof gate);
    ctx->Unmap(r.gate, 0);
    }
    tex->Release();

    float const sw{static_cast<float>(d.Width)}, sh{static_cast<float>(d.Height)};
    ImGui::SetCurrentContext(r.imgui);
    ImGuiIO & io{ImGui::GetIO()};
    io.DisplaySize = ImVec2{sw, sh};
    io.DeltaTime = 1.f / 60.f;  // nothing of ours animates; ImGui only needs it above zero
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ImDrawList * const dl{ImGui::GetBackgroundDrawList()};
    if(test)
      {
      box(dl, s.patch_x, centre_y, s.patch_width, s.patch_height, im_colour(0xff00ffu));  // magenta: where the patch goes
      box(dl, s.patch_x, centre_y, s.patch_width - 8.f, s.patch_height - 8.f, im_colour(s.patch_ground));
      }
    else if(emblem >= 0)
      box(dl, s.patch_x, centre_y, s.patch_width, s.patch_height, im_colour(s.patch_ground));
    if(emblem >= 0)
      {
      float const eh{s.patch_emblem_height};
      float const ew{eh * static_cast<float>(r.emblem_width[emblem]) / static_cast<float>(r.emblem_height[emblem])};
      dl->AddImage(
        reinterpret_cast<ImTextureID>(r.emblem[emblem]),
        ImVec2{s.patch_x - ew / 2.f, centre_y - eh / 2.f},
        ImVec2{s.patch_x + ew / 2.f, centre_y + eh / 2.f},
        ImVec2{0.f, 0.f},
        ImVec2{1.f, 1.f},
        im_colour(colour_of(allegiance))
      );
      }
    ImGui::Render();

    // the backend puts back what it binds itself; the targets and the surface's own slots are ours to put back
    backup_t b;
    save(ctx, b);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->PSSetShaderResources(1, 1, &mask);
    ctx->PSSetConstantBuffers(0, 1, &r.gate);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    restore(ctx, b);

    // the list into its own texture, for the quad drawn after the game's draw of the panel (panel_list_after)
    if(s.list != 0 and (listing or test) and r.font and make_list_objects())
      {
      float const line{std::round(r.font->FontSize * 1.15f)};
      auto const w{static_cast<std::uint32_t>(std::clamp(s.list_width, 64.f, 4096.f))};
      // room for the most faction rows and the source's row
      auto const h{static_cast<std::uint32_t>(
        list_height(list_lines_t{std::max(s.list_rows, 1u), 1u}, line, std::round(r.font->FontSize * 0.3f), std::round(r.font->FontSize * 0.55f)) + 1.f
      )};
      if(list_target(w, h))
        {
        io.DisplaySize = ImVec2{static_cast<float>(w), static_cast<float>(h)};
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        float const used{draw_list(ImGui::GetBackgroundDrawList(), s, list, test, static_cast<float>(w) / 2.f, 0.f)};
        ImGui::Render();
        backup_t lb;
        save(ctx, lb);
        float const clear[4]{0.f, 0.f, 0.f, 0.f};
        ctx->ClearRenderTargetView(r.list_rtv, clear);
        ctx->OMSetRenderTargets(1, &r.list_rtv, nullptr);
        ctx->PSSetShaderResources(1, 1, &r.white);
        ctx->PSSetConstantBuffers(0, 1, &r.white_gate);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        restore(ctx, lb);
        ctx->GenerateMips(r.list_srv);
        list_used = std::min(used, static_cast<float>(h));
        list_frame = used > 0.f ? frame : ~0ull;
        }
      }
    if(listing and list.system != told_list_for)
      {
      told_list_for = list.system;
      log_line("list: %u faction(s) of %llu drawn, from %s", std::min(list.count, std::max(s.list_rows, 1u)),
               static_cast<unsigned long long>(list.system), list.source == list_source_e::data_source ? "the data source" : "EDSM");
      }
    if(not told_drawing)
      {
      told_drawing = true;
      log_line("patch: first drawn (%s, %s%s, surface %ux%u, list %s %u)", test ? "test" : "emblem", allegiance_name(allegiance),
               s.patch_force ? " forced" : "", d.Width, d.Height,
               list.source == list_source_e::data_source ? "from the data source" : list.source == list_source_e::edsm ? "from EDSM" : "none",
               list.count);
      }
    }

  auto panel_list_after(ID3D11DeviceContext * ctx, std::uint64_t frame, std::uint32_t index_count, std::uint32_t start_index,
                        std::int32_t base_vertex, std::uint32_t start_instance) noexcept -> void
    {
    settings_t const & s{settings()};
    if(frame != list_frame or not r.list_vs or index_count < 3 or index_count > 64)
      return;
    // only the panel surface's draws: PS t2 (the panel family), t1 (near a war settlement)
    ID3D11ShaderResourceView * surface{};
    for(UINT const slot: {2u, 1u})
      {
      ID3D11ShaderResourceView * srv{};
      ctx->PSGetShaderResources(slot, 1, &srv);
      if(not srv)
        continue;
      ID3D11Resource * res{};
      srv->GetResource(&res);
      ID3D11Texture2D * t{};
      if(res)
        {
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t));
        res->Release();
        }
      D3D11_TEXTURE2D_DESC d{};
      if(t)
        {
        t->GetDesc(&d);
        t->Release();
        }
      if(t and d.Width == s.patch_surface_width and d.Height == s.patch_surface_height)
        {
        surface = srv;
        break;
        }
      srv->Release();
      }
    if(not surface)
      return;

    ID3D11Buffer * ib{};
    DXGI_FORMAT ib_format{};
    UINT ib_offset{};
    ctx->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
    ID3D11Buffer * vb{};
    UINT vb_stride{}, vb_offset{};
    ctx->IAGetVertexBuffers(1, 1, &vb, &vb_stride, &vb_offset);
    panel_draw_t * entry{};
    if(ib and vb and vb_stride == vertex_stride)
      {
      for(panel_draw_t & e: panel_draws)
        if(e.state != panel_draw_t::state_e::empty and e.ib == ib and e.ib_offset == ib_offset and e.start_index == start_index
           and e.vb == vb and e.vb_offset == vb_offset and e.base_vertex == base_vertex)
          {
          entry = &e;
          break;
          }
      if(not entry and base_vertex >= 0)
        {
        // a draw not seen yet: its first triangle's indices and the vertices after its base, copied for judge()
        panel_draw_t & e{panel_draws[panel_draw_next++ % max_panel_draws]};
        drop_staged(e);
        std::uint32_t const size{ib_format == DXGI_FORMAT_R16_UINT ? 2u : 4u};
        e = panel_draw_t{ib, ib_offset, start_index, vb, vb_offset, base_vertex, panel_draw_t::state_e::pending, size};
        e.staged_ib = staging_copy(ctx, ib, ib_offset + std::uint64_t{start_index} * size, 3u * size);
        e.staged_vb = staging_copy(ctx, vb, vb_offset + static_cast<std::uint64_t>(base_vertex) * vertex_stride,
                                   staged_vertices * vertex_stride);
        e.queued = qpc();
        if(not e.staged_ib or not e.staged_vb)
          {
          drop_staged(e);
          e.state = panel_draw_t::state_e::other;
          }
        }
      else if(entry and entry->state == panel_draw_t::state_e::pending)
        judge(ctx, *entry, s);
      }
    if(ib)
      ib->Release();
    if(vb)
      vb->Release();
    if(not entry or entry->state != panel_draw_t::state_e::jump_panel)
      {
      surface->Release();
      return;
      }

    // the list's box, in the surface's pixels past the panel's edge, back to the panel's local plane
    float const left{s.list_x - s.list_width / 2.f}, right{left + s.list_width};
    float const top{s.list_top}, bottom{top + list_used};
    float const pxs[4][2]{{left, top}, {right, top}, {right, bottom}, {left, bottom}};
    list_cb_t cb{};
    for(int i{}; i != 4; ++i)
      {
      auto const local{local_of(entry->map, pxs[i][0], pxs[i][1])};
      if(not local)
        {
        surface->Release();
        return;
        }
      cb.corner[i][0] = local->x;
      cb.corner[i][1] = local->y;
      cb.corner[i][2] = local->z;
      }
    cb.uv_extent[0] = 1.f;
    cb.uv_extent[1] = list_used / static_cast<float>(r.list_height);
    cb.gate[0] = static_cast<std::int32_t>(s.patch_x);
    cb.gate[1] = static_cast<std::int32_t>(patch_centre_y);
    cb.gain[0] = s.list_gain;
    D3D11_MAPPED_SUBRESOURCE m{};
    if(FAILED(ctx->Map(r.list_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
      {
      surface->Release();
      return;
      }
    std::memcpy(m.pData, &cb, sizeof cb);
    ctx->Unmap(r.list_cb, 0);

    list_backup_t b;
    ctx->VSGetShader(&b.vs, b.vs_instances, &b.vs_instance_count);
    ctx->PSGetShader(&b.ps, b.ps_instances, &b.ps_instance_count);
    ctx->IAGetInputLayout(&b.layout);
    ctx->IAGetPrimitiveTopology(&b.topology);
    ctx->VSGetConstantBuffers(list_cb_slot, 1, &b.vs_cb);
    ctx->PSGetConstantBuffers(list_cb_slot, 1, &b.ps_cb);
    ctx->PSGetShaderResources(0, 2, b.ps_srv);
    ctx->PSGetSamplers(0, 1, &b.ps_sampler);
    ctx->OMGetBlendState(&b.blend, b.blend_factor, &b.sample_mask);
    ctx->OMGetDepthStencilState(&b.depth, &b.stencil_ref);
    ctx->RSGetState(&b.raster);

    if(not told_list_quad)
      {
      // what the quad is drawn into, and how the game blends its panel there (the list's ground looked grey)
      ID3D11RenderTargetView * rtv{};
      ID3D11DepthStencilView * dsv{};
      ctx->OMGetRenderTargets(1, &rtv, &dsv);
      D3D11_RENDER_TARGET_VIEW_DESC rd{};
      if(rtv)
        rtv->GetDesc(&rd);
      D3D11_BLEND_DESC bd{};
      if(b.blend)
        b.blend->GetDesc(&bd);
      D3D11_RENDER_TARGET_BLEND_DESC const & t{bd.RenderTarget[0]};
      log_line("list: drawn into format %u%s; the game's blend %s: src %u dest %u op %u, alpha src %u dest %u op %u, mask %x",
               static_cast<unsigned>(rd.Format), dsv ? " (with depth)" : "", b.blend ? (t.BlendEnable ? "on" : "off") : "default",
               t.SrcBlend, t.DestBlend, t.BlendOp, t.SrcBlendAlpha, t.DestBlendAlpha, t.BlendOpAlpha, t.RenderTargetWriteMask);
      if(rtv)
        rtv->Release();
      if(dsv)
        dsv->Release();
      }
    ID3D11ShaderResourceView * const srvs[2]{r.list_srv, surface};
    ctx->VSSetShader(r.list_vs, nullptr, 0);
    ctx->PSSetShader(r.list_ps, nullptr, 0);
    ctx->IASetInputLayout(r.list_layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetConstantBuffers(list_cb_slot, 1, &r.list_cb);
    ctx->PSSetConstantBuffers(list_cb_slot, 1, &r.list_cb);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &r.list_sampler);
    float const factor[4]{};
    ctx->OMSetBlendState(r.list_blend, factor, 0xffffffffu);
    ctx->OMSetDepthStencilState(r.list_depth, 0);
    ctx->RSSetState(r.list_raster);
    ctx->DrawInstanced(6, 1, 0, start_instance);

    ctx->VSSetShader(b.vs, b.vs_instances, b.vs_instance_count);
    ctx->PSSetShader(b.ps, b.ps_instances, b.ps_instance_count);
    ctx->IASetInputLayout(b.layout);
    ctx->IASetPrimitiveTopology(b.topology);
    ctx->VSSetConstantBuffers(list_cb_slot, 1, &b.vs_cb);
    ctx->PSSetConstantBuffers(list_cb_slot, 1, &b.ps_cb);
    ctx->PSSetShaderResources(0, 2, b.ps_srv);
    ctx->PSSetSamplers(0, 1, &b.ps_sampler);
    ctx->OMSetBlendState(b.blend, b.blend_factor, b.sample_mask);
    ctx->OMSetDepthStencilState(b.depth, b.stencil_ref);
    ctx->RSSetState(b.raster);
    for(IUnknown * u: std::initializer_list<IUnknown *>{b.vs, b.ps, b.layout, b.vs_cb, b.ps_cb, b.ps_srv[0], b.ps_srv[1], b.ps_sampler,
                                                       b.blend, b.depth, b.raster})
      if(u)
        u->Release();
    for(UINT i{}; i != b.vs_instance_count; ++i)
      b.vs_instances[i]->Release();
    for(UINT i{}; i != b.ps_instance_count; ++i)
      b.ps_instances[i]->Release();
    surface->Release();
    if(not told_list_quad)
      {
      told_list_quad = true;
      log_line("list: first drawn under the jump panel as a quad of its own (%.0f px of %u)", static_cast<double>(list_used), r.list_height);
      }
    }

  // ---- the compass (PoC): the dot read off the HUD's surface, the angles written beside the disc ----
  // Each frame the disc's square is copied before anything of edworld's is drawn; the copy is read a frame or more
  // later (Map without waiting), so the angles lag the dot by a few frames. The text goes onto the same surface, so it
  // rides the cockpit with the compass; it shows while the disc's rim shows (the surface's alpha at one rim pixel).
  namespace
    {
    struct compass_state_t
      {
      ID3D11Device * device{};
      ID3D11Texture2D * copy{};
      std::uint32_t copy_width{}, copy_height{};
      DXGI_FORMAT copy_format{};
      bool copied{};
      std::uint64_t copied_frame{};
      ID3D11Texture2D * mask{};
      ID3D11ShaderResourceView * mask_srv{};
      std::uint32_t mask_width{}, mask_height{};
      compass_reading_t reading{};  ///< the last one read, plausible or not
      bool have_reading{};
      compass_reading_t shown{};  ///< the last plausible one: what the angles come from
      bool have_shown{};
      double shown_at{};        ///< when it was read (seconds of the performance counter); it holds compass_stale_s
      float dot_hue{-1.f};      ///< its hue (-1: white or none): a disc turned that hue is a flash (read_compass)
      std::uint64_t flashes{};  ///< readings skipped as a flash of the compass
      std::uint64_t gated{};    ///< readings further from the one shown than the ship can turn (compass_max_rate_dps)
      bool was_fresh{};         ///< the reading shown held at the last frame: its going stale is logged once
      std::uint64_t held{};      ///< readings in a row not plausible (the last plausible one held meanwhile)
      std::uint64_t rejected{};  ///< all readings not plausible
      std::uint64_t lag{};  ///< frames between the copy and its reading
      std::uint64_t drawn_frame{~0ull};
      std::uint64_t reads{};
      LONGLONG last_log{};
      bool told{};
      // the spheres: their textures (premultiplied, drawn by ImGui, with mips), and which of the frame's 2048x1280 panel
      // draws comes next
      ID3D11Texture2D * sphere_tex[2]{};
      ID3D11RenderTargetView * sphere_rtv[2]{};
      ID3D11ShaderResourceView * sphere_srv[2]{};
      bool spheres_drawn{};
      std::uint64_t quad_frame{~0ull};
      std::uint32_t quad_index{};
      bool told_quads{};
      bool told_hide{};
      ///\brief the approach view shows (approach_view)
      bool approach{};
      ///\brief the dot's place averaged over the plausible readings while the approach view shows (else the last one), and
      /// when it was last moved (seconds of the performance counter)
      compass_reading_t smooth{};
      bool have_smooth{};
      double smooth_at{};
      };

    compass_state_t cs;

    ///\brief seconds of the performance counter
    auto seconds_now() noexcept -> double
      {
      LARGE_INTEGER t{}, fq{};
      QueryPerformanceCounter(&t);
      QueryPerformanceFrequency(&fq);
      return static_cast<double>(t.QuadPart) / static_cast<double>(fq.QuadPart);
      }

    ///\brief the reading shown still holds (compass_fresh): one came less than compass_stale_s ago
    auto shown_fresh(settings_t const & s) noexcept -> bool
      {
      return compass_fresh(cs.have_shown, seconds_now() - cs.shown_at, s.compass_stale_s);
      }

    auto compass_limits(settings_t const & s) noexcept -> compass_limits_t
      {
      return compass_limits_t{s.compass_filled_min, s.compass_filled_max, s.compass_hollow_min, s.compass_hollow_max};
      }

    auto compass_reset(ID3D11Device * device) -> void
      {
      for(IUnknown * u: std::initializer_list<IUnknown *>{cs.copy, cs.mask_srv, cs.mask, cs.sphere_srv[0], cs.sphere_srv[1], cs.sphere_rtv[0],
                                                         cs.sphere_rtv[1], cs.sphere_tex[0], cs.sphere_tex[1]})
        if(u)
          u->Release();
      cs = compass_state_t{};
      cs.device = device;
      }

    ///\brief the surface of the given size this draw samples (PS t2, else t1), AddRef'd; none when it is another
    auto surface_sampled(ID3D11DeviceContext * ctx, std::uint32_t width, std::uint32_t height, D3D11_TEXTURE2D_DESC & d)
      -> ID3D11Texture2D *
      {
      for(UINT const slot: {2u, 1u})
        {
        ID3D11ShaderResourceView * srv{};
        ctx->PSGetShaderResources(slot, 1, &srv);
        if(not srv)
          continue;
        ID3D11Resource * res{};
        srv->GetResource(&res);
        srv->Release();
        if(not res)
          continue;
        D3D11_RESOURCE_DIMENSION dim{};
        res->GetType(&dim);
        ID3D11Texture2D * t{};
        if(dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t));
        res->Release();
        if(not t)
          continue;
        t->GetDesc(&d);
        if(d.Width == width and d.Height == height)
          return t;
        t->Release();
        }
      return nullptr;
      }

    auto compass_mask_for(D3D11_TEXTURE2D_DESC const & surface) -> ID3D11ShaderResourceView *
      {
      if(cs.mask and cs.mask_width == surface.Width and cs.mask_height == surface.Height)
        return cs.mask_srv;
      release(cs.mask_srv);
      release(cs.mask);
      D3D11_TEXTURE2D_DESC d{};
      d.Width = surface.Width;
      d.Height = surface.Height;
      d.MipLevels = 1;
      d.ArraySize = 1;
      d.Format = surface.Format;
      d.SampleDesc.Count = 1;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if(FAILED(r.device->CreateTexture2D(&d, nullptr, &cs.mask)) or not cs.mask)
        return nullptr;
      D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
      vd.Format = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : d.Format;
      vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      vd.Texture2D.MipLevels = 1;
      if(FAILED(r.device->CreateShaderResourceView(cs.mask, &vd, &cs.mask_srv)))
        {
        release(cs.mask);
        return nullptr;
        }
      cs.mask_width = surface.Width;
      cs.mask_height = surface.Height;
      return cs.mask_srv;
      }

    ///\brief text on an opaque dark ground, top left at (x, y); returns the height used
    auto ground_text(ImDrawList * dl, float x, float y, float size, ImU32 colour, char const * text) -> float
      {
      ImVec2 const extent{r.font->CalcTextSizeA(size, FLT_MAX, 0.f, text)};
      dl->AddRectFilled(ImVec2{x - 4.f, y - 2.f}, ImVec2{x + extent.x + 4.f, y + extent.y + 2.f}, IM_COL32(2, 3, 4, 255));
      dl->AddText(r.font, size, ImVec2{x, y}, colour, text);
      return extent.y + 4.f;
      }

    constexpr char degree[]{"\xC2\xB0"};

    ///\brief a sphere's texture, large enough for both layouts: the lines under the sphere (its left 512 x 672), or a wide band
    /// of lines over it (896 x 640, the sphere's square in x 192..704, y 128..640)
    constexpr std::uint32_t sphere_w{896}, sphere_h{672};
    constexpr float below_w{512.f}, below_h{672.f}, above_w{896.f}, above_h{640.f}, above_band{128.f};

    auto sphere_targets() -> bool
      {
      if(cs.sphere_tex[0] and cs.sphere_tex[1])
        return true;
      for(int k{}; k != 2; ++k)
        {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = sphere_w;
        d.Height = sphere_h;
        d.MipLevels = 0;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if(FAILED(r.device->CreateTexture2D(&d, nullptr, &cs.sphere_tex[k])) or not cs.sphere_tex[k]
           or FAILED(r.device->CreateRenderTargetView(cs.sphere_tex[k], nullptr, &cs.sphere_rtv[k]))
           or FAILED(r.device->CreateShaderResourceView(cs.sphere_tex[k], nullptr, &cs.sphere_srv[k])))
          return false;
        }
      return true;
      }

    ///\brief 0xRRGGBB (a sphere colour from the settings) with an alpha, for ImGui
    auto col(std::uint32_t rgb, int alpha = 255) -> ImU32
      {
      return IM_COL32((rgb >> 16) & 0xffu, (rgb >> 8) & 0xffu, rgb & 0xffu, static_cast<unsigned>(alpha));
      }

    ///\brief the spheres' lines' font, else the list's
    auto sphere_font() -> ImFont * { return r.sphere_font ? r.sphere_font : r.font; }

    ///\brief the disc behind a sphere: dark, a little lighter towards its middle, translucent
    auto sphere_ground(ImDrawList * dl, ImVec2 c, float radius, int alpha) -> void
      {
      settings_t const & s{settings()};
      std::uint32_t const middle{mix_rgb(s.compass_colour_disc, s.compass_colour_rim, 0.2f)};
      for(int i{24}; i >= 1; --i)
        {
        float const f{static_cast<float>(i) / 24.f};
        dl->AddCircleFilled(c, radius * f, col(mix_rgb(s.compass_colour_disc, middle, 1.f - f), alpha), 96);
        }
      }

    ///\brief the pad's plane as the right sphere shows it this frame (PoC): on, the model's axes and the tilt's colour
    struct pad_plane_t
      {
      bool on;
      std::array<compass_direction_t, 3> axes;  ///< the pads' normal first, then the two axes of their plane
      std::uint32_t rgb;
      bool upside_down;  ///< the ship's top towards the pads (tilt over 90 degrees, the pads' up known)
      bool has_nose;     ///< the ship's nose on the pad is known (T1 table): the pad's mark is drawn
      compass_direction_t nose;
      };

    ///\brief whether the pad's plane shows: from the tower's grant at a port whose hologram shows its pads, held on the pad
    /// while the hologram keeps the same model; one log line each time the tilt crosses a limit. Once a frame, from the
    /// right sphere that is drawn; to_target and have are that sphere's direction to the target, for the pads' up
    pad_plane_t last_pad_plane{};  ///< the right sphere's plane of the last frame, for the left sphere's mark

    // edworld_eht's notes under the right sphere (Artur 2026-10-22: seen at once, reported while still there): the T1
    // table's watch at Docked for a minute, a new model's sample for ten seconds; text only, no fill
    struct sphere_note_t
      {
      char text[48];
      int colour;  ///< 0 the ok colour, 1 the wrong colour, 2 grey
      ULONGLONG until;
      };

    constexpr ULONGLONG t1_note_ms{60000};
    constexpr ULONGLONG model_note_ms{10000};
    sphere_note_t t1_note{};
    sphere_note_t model_note{};
    std::uint32_t model_note_pad{};  // the pad PAD <n> NEW was shown for (once a grant)

    ///\brief edworld_eht: the last pick of a pad's plane, for the log at Docked (the pads to learn)
    struct planes_look_t
      {
      std::uint32_t pad;  ///< 0 none
      std::uint32_t model;
      float angle_deg;
      bool known;
      };

    planes_look_t planes_look{};

    auto pad_plane_now(hologram_view_t const & h, compass_direction_t const & to_target, bool have) -> pad_plane_t
      {
      static std::uint32_t pad_model{};
      static std::uint32_t candidate{};        // another model the hologram shows, taken only once it holds a second
      static ULONGLONG candidate_since{};
      static std::int64_t pad_grant{};
      static pad_up_vote_t vote{};  // which end of X is the pads' up: voted afresh for each grant and model
      static int logged_band{-1};
      static bool logged_upside_down{};
      static bool logged_table{};
      settings_t const & s{settings()};
      flight_t const f{flight()};
      constexpr ULONGLONG model_hold_ms{1000};
      bool const granted{pad_granted(f.docking_granted_ms, unix_now() * 1000)};
      bool const down{(f.flags & 3u) != 0u};  // Status.json Flags bit 0 docked, bit 1 landed
#if defined(EDWORLD_EHT)
      // the T1 table's watch (Artur 2026-10-22: every docking checks the table, no flight only for it): at Docked, the
      // last look on the table's pad within t1_check_ms before it - the pads' plane to the ship's and the ship's nose to
      // the table's - agrees or deviates; a deviation is logged for Artur, the table is never changed by it
      static t1_look_t t1_look{};
      static bool was_docked{};
      bool const docked{(f.flags & 1u) != 0u};
      if(docked and not was_docked and planes_look.pad)
        log_line("hologram: pads' planes at Docked: pad %u of model %u indices, the last plane %+.0f degrees (%s)", planes_look.pad,
                 planes_look.model, static_cast<double>(planes_look.angle_deg), planes_look.known ? "its number's" : "nearest - to learn");
      if(docked != was_docked)
        planes_look = planes_look_t{};
      if(docked and not was_docked)
        {
        ULONGLONG const now{GetTickCount64()};
        switch(t1_verdict(t1_look, now))
          {
          case t1_verdict_e::match:
          case t1_verdict_e::deviates:
            {
            bool const ok{t1_verdict(t1_look, now) == t1_verdict_e::match};
            log_line("hologram: T1 check at Docked: pad %u of model %u indices: plane %.1f degrees to the ship's, nose %.1f "
                     "degrees off the table's - %s", t1_look.pad, t1_look.model, static_cast<double>(t1_look.tilt),
                     static_cast<double>(t1_look.nose_deg), ok ? "agrees" : "DEVIATES from the table");
            t1_note = sphere_note_t{{}, ok ? 0 : 1, now + t1_note_ms};
            std::snprintf(t1_note.text, sizeof t1_note.text, "T1 %s %.0f%s / %.0f%s", ok ? "MATCH" : "DEVIATES",
                          static_cast<double>(t1_look.tilt), degree, static_cast<double>(t1_look.nose_deg), degree);
            break;
            }
          case t1_verdict_e::no_look:
            log_line("hologram: T1 check at Docked: pad %u of model %u indices: no look within %llu ms before Docked", t1_look.pad,
                     t1_look.model, t1_check_ms);
            t1_note = sphere_note_t{"T1 NO LOOK", 2, now + t1_note_ms};
            break;
          case t1_verdict_e::none:
            break;
          }
        }
      if(docked != was_docked)
        t1_look = t1_look_t{};
      was_docked = docked;
#endif
      if(not h.have)
        return pad_plane_t{};
      // the own ship's hologram on the target's surface (near a carrier's pad) is no pads' model (2026-10-22: its X
      // gave a plane 90 degrees off at pad 5 and pad 15)
      static std::uint32_t logged_ship{};
      if(h.ship)
        {
        if(h.count != logged_ship)
          log_line("hologram: the target's hologram shows a ship (%u indices): no pad's plane from it", h.count);
        logged_ship = h.count;
        return pad_plane_t{};
        }
      logged_ship = 0u;
      auto const start = [&](std::uint32_t model)
        {
        pad_model = model;
        candidate = 0u;
        vote = pad_up_vote_t{};
        logged_band = -1;
        logged_upside_down = false;
        logged_table = false;
        };
      if(granted)
        {
        if(f.docking_granted_ms != pad_grant)
          {
          pad_grant = f.docking_granted_ms;
          start(h.count);
          }
        else if(h.count == pad_model)
          candidate = 0u;
        else if(pad_model == 0u)
          start(h.count);
        else
          {
          // a model of a moment (another hologram drawn smaller for a frame or two) leaves the vote as it is
          ULONGLONG const now{GetTickCount64()};
          if(candidate != h.count)
            {
            candidate = h.count;
            candidate_since = now;
            }
          else if(now - candidate_since >= model_hold_ms)
            start(h.count);
          }
        }
      else if(not down and pad_model != 0u)
        start(0u);
      if(pad_model == 0u or h.count != pad_model)
        return pad_plane_t{};
      // the kind of place the model shows (D, 2026-10-24), not the journal's StationType: edworld alone has none.
      // edworld_eht says once a grant and model whether the data source's StationType names the same kind
      t1_pad_t const * const t1{t1_pad(h.table, f.docking_pad)};
      static std::int64_t logged_place_grant{};
      static std::uint32_t logged_place_model{};
      if(f.docking_granted_ms != logged_place_grant or h.count != logged_place_model)
        {
        logged_place_grant = f.docking_granted_ms;
        logged_place_model = h.count;
        char const * const no_table{h.place == holo_place_e::outpost_t1 and not t1 ? ", no T1 table row for the pad" : ""};
#if defined(EDWORLD_EHT)
        holo_place_e const theirs{holo_place_of_station_type(f.station_type)};
        log_line("place: pad %u, model %u indices is %s%s; the data source's %.31s - %s", f.docking_pad, h.count,
                 holo_place_name(h.place), no_table, f.station_type[0] != '\0' ? f.station_type : "none",
                 h.place == theirs ? "agrees" : "DEVIATES");
#else
        log_line("place: pad %u, model %u indices is %s%s", f.docking_pad, h.count, holo_place_name(h.place), no_table);
#endif
        }
      // a place without pads on its hologram, a model not known, a T1 part without the pad's table row: no plane (a vote
      // on X goes wrong at an outpost in space)
      if(not holo_place_has_plane(h.place) or (h.place == holo_place_e::outpost_t1 and not t1))
        return pad_plane_t{};
      // a place on a planet (E, 2026-10-24): always flat - the pads' up is the model's thinnest axis, its end the vote's
      // (the ship always over the pads); a pad confirmed in the form gives its plane's angle and the ship's nose, since
      // the ship stands on the pad (no table in the code: a new model has its plane once confirmed). A model without a
      // fingerprint, or not a slab, has none
      bool const planet{h.place == holo_place_e::planet};
      static std::int64_t logged_flat_grant{};
      static std::uint32_t logged_flat_model{};
      bool const log_flat{planet and (f.docking_granted_ms != logged_flat_grant or h.count != logged_flat_model)};
      if(log_flat)
        {
        logged_flat_grant = f.docking_granted_ms;
        logged_flat_model = h.count;
        }
      if(planet and (h.flat.axis < 0 or h.flat.ratio > holo_flat_ratio_max))
        {
        if(log_flat)
          log_line("hologram: pad %u of model %u indices on a planet: %s - no plane", f.docking_pad, h.count,
                   h.flat.axis < 0 ? "its thinnest axis not known (no fingerprint, or its turn not sure)" : "not flat enough");
        return pad_plane_t{};
        }
      // a carrier without its row of planes (a fleet carrier, 73389 indices, 2026-10-24: its deck is on Y, so X and the vote
      // drew a plane 90 degrees off): none until its planes are in the table
      if(h.place == holo_place_e::carrier and not pad_planes_of(h.table))
        {
        static std::int64_t logged_carrier_grant{};
        static std::uint32_t logged_carrier_model{};
        if(f.docking_granted_ms != logged_carrier_grant or h.count != logged_carrier_model)
          {
          logged_carrier_grant = f.docking_granted_ms;
          logged_carrier_model = h.count;
          log_line("hologram: pad %u of carrier model %u indices: no row of its planes - no plane", f.docking_pad, h.count);
          }
        return pad_plane_t{};
        }
      // an outpost in space: its pad from the T1 table, no vote; a carrier its planes; on a planet the thinnest axis and
      // the vote
      pad_planes_t const * const carrier{t1 or planet ? nullptr : pad_planes_of(h.table)};
      int const n{t1 ? t1->up_axis : planet ? h.flat.axis : 0};
      std::array<compass_direction_t, 3> axes{h.axes[n], h.axes[(n + 1) % 3], h.axes[(n + 2) % 3]};
      char const axis_name{"XYZ"[n]};
      static float logged_carrier_angle{1000.f};
      pad_learned_t const * learned{};
      if(planet)
        {
        learned = pad_learned_of(h.table, f.destination_name, f.docking_pad);
        if(learned)
          {
          // the confirmed plane's angle from the thinnest axis, turning about the next and leaning to the third (13083's
          // pads were learned so: up Y, about Z, towards X)
          pad_planes_t const slab{h.table, static_cast<std::int8_t>(n), static_cast<std::int8_t>((n + 1) % 3),
                                  static_cast<std::int8_t>((n + 2) % 3), 1u, {0.f}, false};
          axes = pick_pad_plane(slab, h.axes, 1000.f, &learned->angle_deg).axes;
          }
        if(log_flat)
          log_line("hologram: pad %u of model %u indices on a planet: up the model's thinnest axis %c (ratio %.2f)%s",
                   f.docking_pad, h.count, axis_name, static_cast<double>(h.flat.ratio),
                   learned ? ", the pad's plane and the ship's nose confirmed in the form" : ", no direction");
        }
      else if(carrier)
        {
        // the pad's plane as confirmed in holo_db's form, else by its number, else of the model's planes the one nearest
        // the ship's up; up towards the ship, no vote
        learned = pad_learned_of(h.table, f.destination_name, f.docking_pad);
        static site_kind_e logged_kind{site_kind_e::unknown};
        if(site_kind_e const kind{site_kind_of(f.destination_name)}; kind != logged_kind)
          {
          logged_kind = kind;
          log_line("hologram: the destination is %s, by its name", site_kind_name(kind));
          }
        float const * const known{learned ? &learned->angle_deg : pad_plane_of_pad(h.table, f.docking_pad)};
        picked_plane_t const cp{pick_pad_plane(*carrier, h.axes, logged_carrier_angle, known)};
        axes = cp.axes;
        if(cp.angle_deg != logged_carrier_angle)
          {
          logged_carrier_angle = cp.angle_deg;
          log_line("hologram: pad %u of model %u indices: the plane at %+.0f degrees (%s)", f.docking_pad, h.count,
                   static_cast<double>(cp.angle_deg),
                   learned ? (learned->station[0] != '\0' ? "the station's pad, confirmed in the form"
                              : learned->kind != site_kind_e::unknown ? "the pad of its kind of place, confirmed in the form"
                                                                       : "the model's pad, confirmed in the form")
                   : known ? "the pad's, by its number"
                           : "nearest the ship's up");
          }
#if defined(EDWORLD_EHT)
        planes_look = planes_look_t{f.docking_pad, h.table, cp.angle_deg, known != nullptr};
        if(not known and carrier->planes > 1u and model_note_pad != f.docking_pad)
          {
          model_note_pad = f.docking_pad;  // a pad to learn: its plane at Docked goes to the log
          model_note = sphere_note_t{{}, 2, GetTickCount64() + model_note_ms};
          std::snprintf(model_note.text, sizeof model_note.text, "PAD %u NEW", f.docking_pad);
          }
#endif
        }
      else
        logged_carrier_angle = 1000.f;
      if(t1 and not logged_table)
        {
        logged_table = true;
        log_line("hologram: pad %u of model %u indices from the T1 table: up %c%c", f.docking_pad, h.count, t1->up_sign > 0 ? '+' : '-',
                 axis_name);
        }
      if(have and not t1 and not carrier)
        switch(pad_up_vote(vote, axes[0], to_target))
          {
          case pad_up_event_e::decided:
            log_line("hologram: pad %u up = %c%c of model %u indices, %d of %d votes (weights +%.2f -%.2f)", f.docking_pad,
                     vote.decided > 0 ? '+' : '-', axis_name, h.count, vote.decided > 0 ? vote.plus : vote.minus,
                     vote.plus + vote.minus, vote.plus_weight, vote.minus_weight);
            break;
          case pad_up_event_e::turned:
            log_line("hologram: pad %u up TURNED to %c%c of model %u indices: three strong votes against the decision",
                     f.docking_pad, vote.decided > 0 ? '+' : '-', axis_name, h.count);
            break;
          case pad_up_event_e::none:
            break;
          }
      int const up_sign{t1 ? static_cast<int>(t1->up_sign) : carrier ? 1 : vote.decided};
      float const tilt{pad_tilt_deg(axes[0], up_sign)};
      int const band{tilt <= s.pad_tilt_green_deg ? 0 : tilt < s.pad_tilt_red_deg ? 1 : 2};
      if(band != logged_band)
        {
        logged_band = band;
        log_line("hologram: pad %u plane %.1f degrees to the ship's (%s), model %u indices", f.docking_pad, tilt,
                 band == 0 ? "green" : band == 1 ? "towards red" : "red", h.count);
        }
      bool const upside_down{pad_upside_down(tilt, up_sign)};
      if(upside_down != logged_upside_down)
        {
        logged_upside_down = upside_down;
        log_line("hologram: pad %u %s (%.1f degrees)", f.docking_pad, upside_down ? "upside down" : "right way up again", tilt);
        }
      compass_direction_t nose{};
      if(t1)
        {
        compass_direction_t const & v{h.axes[static_cast<std::size_t>(t1->nose_axis)]};
        float const k{static_cast<float>(t1->nose_sign)};
        nose = compass_direction_t{k * v.x, k * v.y, k * v.z};
#if defined(EDWORLD_EHT)
        t1_look = t1_look_t{GetTickCount64(), f.docking_pad, h.table, tilt, nose_off_deg(nose)};
#endif
        }
      else if(learned and learned->nose != std::array<float, 3>{})
        for(std::size_t i{}; i != 3u; ++i)  // the confirmed nose in the model's axes, into the hologram camera's frame
          {
          nose.x += learned->nose[i] * h.axes[i].x;
          nose.y += learned->nose[i] * h.axes[i].y;
          nose.z += learned->nose[i] * h.axes[i].z;
          }
      else if(carrier and carrier->nose_along)
        nose = axes[1];  // along the carrier's length, towards its plus end, on any of its planes
      bool const has_nose{t1 != nullptr or (learned != nullptr and learned->nose != std::array<float, 3>{})
                          or (carrier != nullptr and carrier->nose_along)};
      return pad_plane_t{true, axes, pad_tilt_colour(tilt, s.pad_tilt_green_deg, s.pad_tilt_red_deg), upside_down, has_nose,
                         nose};
      }

    ///\brief the pad's plane as a translucent disc on the two axes of the pads' plane (the model's Y and Z, or the two the T1
    /// table leaves) through the sphere's centre, its edge fuller; upside down (Artur, 2026-10-21) a red wire grid instead of the
    /// fill, and UPSIDE DOWN over the sphere (centre c, radius rad)
    template<typename at_type>
    auto draw_pad_plane(ImDrawList * dl, at_type const & at, pad_plane_t const & plane, ImVec2 c, float rad) -> void
      {
      compass_direction_t const & py{plane.axes[1]};
      compass_direction_t const & pz{plane.axes[2]};
      constexpr int points{48};
      ImVec2 rim_points[points];
      for(int k{}; k != points; ++k)
        {
        float const t{static_cast<float>(k) * 6.2831853f / static_cast<float>(points)};
        float const co{std::cos(t)}, si{std::sin(t)};
        rim_points[k] = at(compass_direction_t{co * py.x + si * pz.x, co * py.y + si * pz.y, co * py.z + si * pz.z});
        }
      // the pad's mark `< |||` in its plane, the head to the ship's nose on the pad: the angle it is seen at tells when to
      // turn onto the pad's axis (Artur 2026-10-21)
      auto const draw_mark = [&](ImU32 colour)
        {
        if(not plane.has_nose)
          return;
        compass_direction_t const & n{plane.nose};
        compass_direction_t const & u{plane.axes[0]};
        compass_direction_t const side{n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x};
        auto const in_plane = [&](float a, float b) -> ImVec2
          {
          constexpr float scale{0.8f};  // in proportion to the dome (Artur 2026-10-21)
          a *= scale;
          b *= scale;
          return at(compass_direction_t{a * n.x + b * side.x, a * n.y + b * side.y, a * n.z + b * side.z});
          };
        for(pad_mark_segment_t const & m: pad_mark)
          dl->AddLine(in_plane(m.a, m.b), in_plane(m.c, m.d), colour, 9.f);
        };
      // half the disc's brightness, its hue kept: the mark stands out of the fill (Artur 2026-10-21)
      ImU32 const mark_colour{col(mix_rgb(plane.rgb, 0x000000u, 0.5f), 255)};
      if(not plane.upside_down)
        {
        dl->AddConvexPolyFilled(rim_points, points, col(plane.rgb, 72));
        dl->AddPolyline(rim_points, points, col(plane.rgb, 200), ImDrawFlags_Closed, 3.f);
        draw_mark(mark_colour);
        return;
        }
      // the grid: chords along Y and across it every quarter radius, as the dome's base
      auto const on = [&](float a, float b) -> ImVec2
        { return at(compass_direction_t{a * py.x + b * pz.x, a * py.y + b * pz.y, a * py.z + b * pz.z}); };
      ImU32 const wire{col(plane.rgb, 210)};
      for(float const u: {-0.75f, -0.5f, -0.25f, 0.f, 0.25f, 0.5f, 0.75f})
        {
        float const h{std::sqrt(1.f - u * u)};
        dl->AddLine(on(u, -h), on(u, h), wire, 2.f);
        dl->AddLine(on(-h, u), on(h, u), wire, 2.f);
        }
      dl->AddPolyline(rim_points, points, col(plane.rgb, 255), ImDrawFlags_Closed, 4.f);
      draw_mark(mark_colour);
      char const * const warning{"UPSIDE DOWN"};
      ImVec2 const size{sphere_font()->CalcTextSizeA(30.f, FLT_MAX, 0.f, warning)};
      dl->AddText(sphere_font(), 30.f, ImVec2{c.x - size.x / 2.f, c.y - rad - size.y - 4.f}, col(plane.rgb, 255), warning);
      }

    ///\brief the pad's mark on the left sphere (Artur 2026-10-21): the right sphere's mark of the last frame, half the disc's
    /// size around the centre, in this sphere's own projection (at), half opaque so that the dots show through it
    template<typename at_type>
    auto draw_left_mark(ImDrawList * dl, at_type const & at) -> void
      {
      pad_plane_t const & plane{last_pad_plane};
      if(not plane.on or not plane.has_nose)
        return;
      compass_direction_t const & n{plane.nose};
      compass_direction_t const & u{plane.axes[0]};
      compass_direction_t const side{n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x};
      auto const in_plane = [&](float a, float b) -> ImVec2
        {
        constexpr float scale{0.5f};
        a *= scale;
        b *= scale;
        return at(compass_direction_t{a * n.x + b * side.x, a * n.y + b * side.y, a * n.z + b * side.z});
        };
      ImU32 const colour{col(mix_rgb(plane.rgb, 0x000000u, 0.5f), 128)};
      for(pad_mark_segment_t const & m: pad_mark)
        dl->AddLine(in_plane(m.a, m.b), in_plane(m.c, m.d), colour, 9.f);
      }

    ///\brief the sphere seen from the front: the compass itself, larger; rings at 30 and 60 degrees off the nose
    auto draw_sphere_front(ImDrawList * dl, ImVec2 c, compass_direction_t const & d, bool have) -> void
      {
      float const radius{200.f};
      settings_t const & s{settings()};
      sphere_ground(dl, c, radius, 215);
      ImU32 const dim{col(s.compass_colour_grid)};
      for(float const a: {30.f, 60.f})
        dl->AddCircle(c, radius * std::sin(a / compass_degrees), dim, 96, 2.f);
      dl->AddLine(ImVec2{c.x - radius, c.y}, ImVec2{c.x + radius, c.y}, dim, 2.f);
      dl->AddLine(ImVec2{c.x, c.y - radius}, ImVec2{c.x, c.y + radius}, dim, 2.f);
      dl->AddCircle(c, radius, col(s.compass_colour_rim), 96, 5.f);
      draw_left_mark(dl, [&](compass_direction_t const & v) { return ImVec2{c.x + v.x * radius, c.y - v.y * radius}; });
      if(have)
        {
        ImVec2 const p{c.x + d.x * radius, c.y - d.y * radius};
        if(d.z >= 0.f)
          dl->AddCircleFilled(p, 15.f, col(s.compass_colour_target), 32);
        else
          dl->AddCircle(p, 13.f, col(s.compass_colour_behind), 32, 5.f);
        }
      }

    ///\brief the sphere seen from behind, the left and above, as the game's radar: the wings' plane, the nose, the target on a
    /// stalk to that plane, and where it should be
    auto draw_sphere_oblique(ImDrawList * dl, ImVec2 c, compass_direction_t const & d, bool have, bool show_should,
                             compass_direction_t const & should) -> void
      {
      constexpr float yaw{35.f}, pitch{20.f};
      float const radius{200.f};
      auto const at = [&](compass_direction_t const & v) -> ImVec2
        {
        sphere_point_t const p{sphere_view(v, yaw, pitch)};
        return ImVec2{c.x + p.x * radius, c.y - p.y * radius};
        };
      auto const facing = [&](compass_direction_t const & v) -> bool { return not sphere_view(v, yaw, pitch).on_far_side; };
      settings_t const & s{settings()};
      std::uint32_t const disc{s.compass_colour_disc}, grid{s.compass_colour_grid}, rim{s.compass_colour_rim};
      sphere_ground(dl, c, radius, 200);
      dl->AddCircle(c, radius, col(mix_rgb(disc, grid, 0.7f)), 96, 2.f);
      constexpr float pi2{6.2831853f};
      auto const curve = [&](auto && point, ImU32 bright, ImU32 faint, float width)
        {
        constexpr int steps{96};
        compass_direction_t a{point(0.f)};
        for(int i{1}; i <= steps; ++i)
          {
          compass_direction_t const b{point(pi2 * static_cast<float>(i) / steps)};
          dl->AddLine(at(a), at(b), facing(a) ? bright : faint, width);
          a = b;
          }
        };
      ImU32 const faint{col(mix_rgb(disc, grid, 0.5f))};
      for(float const a: {-30.f, -60.f})
        {
        float const sa{std::sin(a / compass_degrees)}, ca{std::cos(a / compass_degrees)};
        curve([&](float t) { return compass_direction_t{ca * std::cos(t), sa, ca * std::sin(t)}; }, col(mix_rgb(disc, grid, 0.85f)), faint, 1.5f);
        }
      ImU32 const meridian{col(mix_rgb(grid, rim, 0.33f))};
      curve([](float t) { return compass_direction_t{0.f, std::sin(t), std::cos(t)}; }, meridian, faint, 2.f);
      curve([](float t) { return compass_direction_t{std::cos(t), std::sin(t), 0.f}; }, meridian, faint, 2.f);
      curve([](float t) { return compass_direction_t{std::cos(t), 0.f, std::sin(t)}; }, col(rim), faint, 5.f);
      // the nose: an arrow forward in the wings' plane
      ImU32 const green{col(s.compass_colour_nose)};
      ImVec2 const o{at({0.f, 0.f, 0.f})}, n{at({0.f, 0.f, 1.f})};
      dl->AddLine(o, n, green, 5.f);
      float const dx{n.x - o.x}, dy{n.y - o.y}, len{std::max(1.f, std::sqrt(dx * dx + dy * dy))}, ux{dx / len}, uy{dy / len};
      dl->AddTriangleFilled(ImVec2{n.x + ux * 22.f, n.y + uy * 22.f}, ImVec2{n.x - uy * 13.f, n.y + ux * 13.f}, ImVec2{n.x + uy * 13.f, n.y - ux * 13.f},
                            green);
      dl->AddText(sphere_font(), 30.f, ImVec2{n.x + 14.f, n.y - 40.f}, green, "NOSE");
      last_pad_plane = pad_plane_t{};
      // the target's axes (PoC, station_axes): its hologram's model X, Y, Z, Y the strongest; fainter on the far side
      if(s.station_axes)
        if(hologram_view_t const h{hologram_view()}; h.have)
          {
          // the pad's plane (PoC): a disc on the model's Y and Z, its colour from the tilt to the ship's
          if(pad_plane_t const plane{last_pad_plane = pad_plane_now(h, d, have)}; plane.on)
            draw_pad_plane(dl, at, plane, c, radius);
          char const * const names[3]{"X", "Y", "Z"};
          for(int i{}; i != 3; ++i)
            {
            compass_direction_t const & v{h.axes[static_cast<std::size_t>(i)]};
            std::uint32_t const rgb{i == 1 ? s.compass_colour_target : s.compass_colour_rim};
            ImU32 const line{facing(v) ? col(rgb) : col(mix_rgb(disc, rgb, 0.45f))};
            ImVec2 const tip{at(v)};
            dl->AddLine(o, tip, line, i == 1 ? 6.f : 3.f);
            dl->AddCircleFilled(tip, i == 1 ? 9.f : 6.f, line, 16);
            dl->AddText(sphere_font(), 28.f, ImVec2{tip.x + 10.f, tip.y - 14.f}, line, names[i]);
            }
          }
      auto const mark = [&](compass_direction_t const & v, ImU32 colour, bool ring)
        {
        ImVec2 const p{at(v)}, foot{at({v.x, 0.f, v.z})};
        dl->AddLine(o, foot, (colour & 0x00ffffffu) | 0x80000000u, 2.f);
        dl->AddLine(foot, p, colour, 5.f);
        dl->AddCircle(foot, 6.f, colour, 16, 3.f);
        if(ring)
          dl->AddCircle(p, 20.f, colour, 32, 5.f);
        else
          dl->AddCircleFilled(p, 15.f, colour, 32);
        };
      if(show_should)
        mark(should, col(s.compass_colour_behind), true);
      if(have)
        mark(d, col(s.compass_colour_target), false);
      }


    ///\brief the left sphere in the approach view: the lower half seen from above, straight down in the middle, the nose up
    /// the picture; rings at 15, 30 and 60 degrees off the vertical, the rim the wings' plane. The target twice: a light
    /// blue ring seen from above, and the white dot where the view ahead (draw_sphere_front) has it
    auto draw_sphere_top(ImDrawList * dl, ImVec2 c, compass_direction_t const & d, bool have) -> void
      {
      settings_t const & s{settings()};
      float const radius{200.f};
      sphere_ground(dl, c, radius, 215);
      ImU32 const dim{col(s.compass_colour_grid)};
      for(float const a: {15.f, 30.f, 60.f})
        dl->AddCircle(c, radius * std::sin(a / compass_degrees), dim, 96, 2.f);
      dl->AddLine(ImVec2{c.x - radius, c.y}, ImVec2{c.x + radius, c.y}, dim, 2.f);
      dl->AddLine(ImVec2{c.x, c.y - radius}, ImVec2{c.x, c.y + radius}, dim, 2.f);
      dl->AddCircle(c, radius, col(s.compass_colour_rim), 96, 5.f);
      // the nose: a short arrow up the picture from the middle
      ImU32 const nose{col(s.compass_colour_nose)};
      dl->AddLine(ImVec2{c.x, c.y}, ImVec2{c.x, c.y - 56.f}, nose, 4.f);
      dl->AddTriangleFilled(ImVec2{c.x, c.y - 74.f}, ImVec2{c.x - 11.f, c.y - 54.f}, ImVec2{c.x + 11.f, c.y - 54.f}, nose);
      draw_left_mark(dl, [&](compass_direction_t const & v) { return ImVec2{c.x + v.x * radius, c.y - v.z * radius}; });
      if(have)
        {
        // the target seen from above: a light blue ring (orange under the wings' plane's other side, the target above)
        ImVec2 const p{c.x + d.x * radius, c.y - d.z * radius};
        dl->AddCircle(p, 18.f, col(d.y <= 0.f ? s.compass_colour_top : s.compass_colour_behind), 32, 5.f);
        // the white dot where the view ahead has it (Artur 2026-10-21: it stays the orientation on the target)
        ImVec2 const q{c.x + d.x * radius, c.y - d.y * radius};
        if(d.z >= 0.f)
          dl->AddCircleFilled(q, 15.f, col(s.compass_colour_target), 32);
        else
          dl->AddCircle(q, 13.f, col(s.compass_colour_behind), 32, 5.f);
        }
      }

    ///\brief the right sphere as a dome standing on the pad (Artur, 2026-10-08: as the game's own landing view, the pad below
    /// and the ship over it): its centre the target, its base the floor through it (parallel to the ship's wings), the ship
    /// a point on the dome on a stalk from the base (the direction from the target to the ship, -d); straight over the target
    /// is the top. Seen from behind, the left and above. full (in space, Artur 2026-10-08): the half under the base too,
    /// fainter, and the base as a tinted plane with a grid; near a planet only the dome (the ground is flat there)
    auto draw_sphere_dome(ImDrawList * dl, ImVec2 c, compass_direction_t const & d, bool have, bool full) -> void
      {
      constexpr float yaw{35.f}, pitch{30.f};
      float const radius{200.f};
      settings_t const & s{settings()};
      std::uint32_t const disc{s.compass_colour_disc}, grid{s.compass_colour_grid}, rim{s.compass_colour_rim};
      auto const at = [&](compass_direction_t const & v) -> ImVec2
        {
        sphere_point_t const p{sphere_view(v, yaw, pitch)};
        return ImVec2{c.x + p.x * radius, c.y - p.y * radius};
        };
      auto const facing = [&](compass_direction_t const & v) -> bool { return not sphere_view(v, yaw, pitch).on_far_side; };
      sphere_ground(dl, c, radius, 200);
      constexpr float pi{3.14159265f};
      constexpr int steps{96};
      ImU32 const faint{col(mix_rgb(disc, grid, 0.5f))};
      auto const curve = [&](auto && point, float from, float to, ImU32 bright, float width)
        {
        compass_direction_t a{point(from)};
        for(int i{1}; i <= steps; ++i)
          {
          compass_direction_t const b{point(from + (to - from) * static_cast<float>(i) / steps)};
          dl->AddLine(at(a), at(b), facing(a) ? bright : faint, width);
          a = b;
          }
        };
      if(full)
        {
        // the half under the base, faint, drawn first so that the base tints it
        ImU32 const under{col(mix_rgb(disc, grid, 0.6f))};
        for(float const a: {-30.f, -60.f})
          {
          float const sa{std::sin(a / compass_degrees)}, ca{std::cos(a / compass_degrees)};
          curve([&](float t) { return compass_direction_t{ca * std::cos(t), sa, ca * std::sin(t)}; }, 0.f, 2.f * pi, under, 1.5f);
          }
        curve([](float t) { return compass_direction_t{0.f, std::sin(t), std::cos(t)}; }, pi, 2.f * pi, under, 1.5f);
        curve([](float t) { return compass_direction_t{std::cos(t), std::sin(t), 0.f}; }, pi, 2.f * pi, under, 1.5f);
        }
      // the base: the floor through the target, translucent, its edge the wings' plane
      ImVec2 base[steps];
      for(int i{}; i != steps; ++i)
        {
        float const t{2.f * pi * static_cast<float>(i) / steps};
        base[i] = at({std::cos(t), 0.f, std::sin(t)});
        }
      // the base tinted by the side the ship is on (Artur, 2026-10-08): green over it, red under it. The side is the target
      // under or over the wings, so a nose put down far from the pad turns it red; near a planet the pad is on the ground
      // and the ship can not be under it, so it stays green there (Artur, 2026-10-08)
      // while the pad's plane shows (PoC) the base is light blue instead (Artur, 2026-10-21): green and red then mean
      // the tilt to the pads alone
      pad_plane_t const plane{last_pad_plane = s.station_axes ? pad_plane_now(hologram_view(), d, have) : pad_plane_t{}};
      std::uint32_t const floor_colour{mix_rgb(disc, grid, 0.5f)};
      std::uint32_t const side{plane.on                  ? s.compass_colour_pad_base
                               : not have                ? floor_colour
                               : not full or d.y <= 0.f ? s.compass_colour_ok
                                                         : s.compass_colour_wrong};
      dl->AddConvexPolyFilled(base, steps,
                              col(have or plane.on ? mix_rgb(floor_colour, side, 0.35f) : floor_colour, full ? 190 : 150));
      if(full)
        {
        // the plane's grid: chords along and across the nose every quarter radius
        ImU32 const chord{col(mix_rgb(grid, rim, 0.15f), 170)};
        for(float const u: {-0.75f, -0.5f, -0.25f, 0.f, 0.25f, 0.5f, 0.75f})
          {
          float const h{std::sqrt(1.f - u * u)};
          dl->AddLine(at({u, 0.f, -h}), at({u, 0.f, h}), chord, 1.5f);
          dl->AddLine(at({-h, 0.f, u}), at({h, 0.f, u}), chord, 1.5f);
          }
        }
      // the target in the middle of the base, as a small pad
      ImU32 const target{col(s.compass_colour_target)};
      ImVec2 const o{at({0.f, 0.f, 0.f})};
      // drawn again over the pad's plane and its mark: the pad to land on stays on top (Artur 2026-10-21)
      auto const draw_target_pad = [&](bool outlined)
        {
        constexpr float k{0.16f};
        ImVec2 const pad[4]{at({-k, 0.f, -k}), at({k, 0.f, -k}), at({k, 0.f, k}), at({-k, 0.f, k})};
        if(outlined)
          dl->AddPolyline(pad, 4, col(0x000000u, 200), ImDrawFlags_Closed, 7.f);
        dl->AddPolyline(pad, 4, target, ImDrawFlags_Closed, 3.f);
        };
      draw_target_pad(false);
      // the dome: rings at 30 and 60 degrees over the base, two half meridians, the top marked
      for(float const a: {30.f, 60.f})
        {
        float const sa{std::sin(a / compass_degrees)}, ca{std::cos(a / compass_degrees)};
        curve([&](float t) { return compass_direction_t{ca * std::cos(t), sa, ca * std::sin(t)}; }, 0.f, 2.f * pi,
              col(mix_rgb(disc, grid, 0.85f)), 1.5f);
        }
      ImU32 const meridian{col(mix_rgb(grid, rim, 0.33f))};
      curve([](float t) { return compass_direction_t{0.f, std::sin(t), std::cos(t)}; }, 0.f, pi, meridian, 2.f);
      curve([](float t) { return compass_direction_t{std::cos(t), std::sin(t), 0.f}; }, 0.f, pi, meridian, 2.f);
      ImVec2 const top{at({0.f, 1.f, 0.f})};
      dl->AddLine(ImVec2{top.x - 10.f, top.y}, ImVec2{top.x + 10.f, top.y}, meridian, 2.f);
      dl->AddLine(ImVec2{top.x, top.y - 10.f}, ImVec2{top.x, top.y + 10.f}, meridian, 2.f);
      dl->AddPolyline(base, steps, col(rim), ImDrawFlags_Closed, 5.f);
      // the pad's plane over the base: level with the pads, the two coincide
      if(plane.on)
        {
        draw_pad_plane(dl, at, plane, c, radius);
        draw_target_pad(true);
        }
      // the nose: an arrow forward on the base
      ImU32 const green{col(s.compass_colour_nose)};
      ImVec2 const n{at({0.f, 0.f, 1.f})};
      dl->AddLine(o, n, green, 5.f);
      float const dx{n.x - o.x}, dy{n.y - o.y}, len{std::max(1.f, std::sqrt(dx * dx + dy * dy))}, ux{dx / len}, uy{dy / len};
      dl->AddTriangleFilled(ImVec2{n.x + ux * 22.f, n.y + uy * 22.f}, ImVec2{n.x - uy * 13.f, n.y + ux * 13.f}, ImVec2{n.x + uy * 13.f, n.y - ux * 13.f},
                            green);
      if(have)
        {
        compass_direction_t const ship{-d.x, -d.y, -d.z};
        ImVec2 const p{at(ship)}, foot{at({ship.x, 0.f, ship.z})};
        dl->AddLine(o, foot, (target & 0x00ffffffu) | 0x80000000u, 2.f);
        dl->AddLine(foot, p, target, 5.f);
        dl->AddCircle(foot, 6.f, target, 16, 3.f);
        dl->AddCircleFilled(p, 15.f, target, 32);
        }
      }

    ///\brief a line of text under a sphere, centred, as large as asked but never wider than the texture; returns its height
    ///\brief one line of up to three parts in their own colours, centred in [x0, x0 + width), as large as asked but never wider;
    /// returns its height
    auto sphere_line(ImDrawList * dl, float x0, float width, float y, float size, ImU32 colour, char const * text, ImU32 colour2 = 0,
                     char const * text2 = nullptr, ImU32 colour3 = 0, char const * text3 = nullptr) -> float
      {
      ImFont * const font{sphere_font()};
      constexpr char gap[]{"  "};
      auto const measure = [&](float sz) -> float
        {
        float w{font->CalcTextSizeA(sz, FLT_MAX, 0.f, text).x};
        for(char const * t : {text2, text3})
          if(t and t[0])
            w += font->CalcTextSizeA(sz, FLT_MAX, 0.f, gap).x + font->CalcTextSizeA(sz, FLT_MAX, 0.f, t).x;
        return w;
        };
      float const widest{width - 16.f};
      float total{measure(size)};
      if(total > widest)
        {
        size *= widest / total;
        total = measure(size);
        }
      float x{x0 + (width - total) / 2.f};
      dl->AddText(font, size, ImVec2{x, y}, colour, text);
      char const * last{text};
      for(auto const & [t, c] : {std::pair{text2, colour2}, std::pair{text3, colour3}})
        if(t and t[0])
          {
          x += font->CalcTextSizeA(size, FLT_MAX, 0.f, last).x + font->CalcTextSizeA(size, FLT_MAX, 0.f, gap).x;
          dl->AddText(font, size, ImVec2{x, y}, c, t);
          last = t;
          }
      return font->CalcTextSizeA(size, FLT_MAX, 0.f, text).y;
      }

    ///\brief both spheres' textures from the reading shown; once a frame, after the compass was read
    auto render_spheres(ID3D11DeviceContext * ctx, settings_t const & s, flight_t const & f) -> void
      {
      if(not make_list_objects() or not sphere_targets())
        return;
      compass_reading_t const & c{cs.shown};
      bool const have{shown_fresh(s)};
      compass_direction_t const d{compass_direction(c.x, c.y, s.compass_radius, c.filled)};
      compass_angles_t const a{compass_angles(c.x, c.y, s.compass_radius, c.filled)};
      compass_direction_t const should{direction_of(-(90.f - s.compass_should_dive), 0.f)};
      // normal flight: not supercruise (Flags bit 4), docked (0) or landed (1), not gliding (Flags2 bit 12); there the
      // right sphere is the dome on the target near a planet (it is flat from there), and both turn to the approach view with the
      // target low under the wings
      bool const normal_flight{(f.flags & 0x13u) == 0 and (f.flags2 & (1ull << 12)) == 0};
      bool const was_approach{cs.approach};
      // a pad given by the tower is near too, with no destination chosen (Artur, 2026-10-21)
      bool const close{s.compass_approach_km <= 0.f
                      or near_target(f.has_position, f.has_place, f.latitude, f.longitude, f.altitude, f.place_latitude,
                                     f.place_longitude, f.planet_radius, f.destination_body, f.flags, s.compass_approach_km)
                      or pad_granted(f.docking_granted_ms, unix_now() * 1000)};
      cs.approach = s.compass_approach_below > 0.f
                    and approach_view(cs.approach, normal_flight, have, close, f.has_place, a.up, s.compass_approach_below);
      if(cs.approach != was_approach)
        log_line("compass: the approach view %s (the target %.1f degrees below the wings, %s, %s)", cs.approach ? "on" : "off",
                 static_cast<double>(-a.up), close ? "near" : "not near", have ? "read" : "no fresh reading");
      // the right sphere's dome with the left sphere's view from above, never alone (Artur 2026-10-20: both alike)
      bool const cut{cs.approach};
      compass_reading_t const & sm{cs.approach and cs.have_smooth ? cs.smooth : c};
      compass_direction_t const dp{compass_direction(sm.x, sm.y, s.compass_approach_radius, sm.filled)};
      pad_offsets_t const po{pad_offsets(dp)};
      // under A: the angles, large; under C: the approach against where it should be (near a planet), else how far off
      ImU32 const blue{col(s.compass_colour_text_left)}, orange{col(s.compass_colour_behind)}, yellow{col(s.compass_colour_text_right)};
      ImU32 const green_line{col(s.compass_colour_ok)};
      ImU32 const name_colour{col(mix_rgb(s.compass_colour_grid, s.compass_colour_text_left, 0.5f), 200)};
      bool const behind{have and not c.filled and not cs.approach};
      // docked (Flags bit 0) or landed (1): nothing to fly to, the right sphere without lines
      bool const grounded{(f.flags & 0x3u) != 0};
      // c3 the altitude and the vertical speed in one line; alt and climb the same apart, and to_go, for the line under the angles at a planet
      // to_go the distance to a place on the surface, for the left sphere's band at a planet
      char a1[48], a2[48], c1[48], c2[48], c3[64], alt[32], climb[32], to_go[32];
      a2[0] = c2[0] = c3[0] = alt[0] = climb[0] = to_go[0] = '\0';
      if(have and cs.approach)
        {
        std::snprintf(a1, sizeof a1, "%s %.1f%s", po.forward < 0.f ? "AFT" : "FWD", static_cast<double>(std::fabs(po.forward)), degree);
        std::snprintf(a2, sizeof a2, "%s %.1f%s", po.right < 0.f ? "LT" : "RT", static_cast<double>(std::fabs(po.right)), degree);
        }
      else if(have)
        {
        std::snprintf(a1, sizeof a1, "%s %.1f%s", a.up < 0.f ? "DN" : "UP", static_cast<double>(std::fabs(a.up)), degree);
        std::snprintf(a2, sizeof a2, "%s %.1f%s", a.right < 0.f ? "LT" : "RT", static_cast<double>(std::fabs(a.right)), degree);
        }
      else
        std::snprintf(a1, sizeof a1, "NO DOT");
      ImU32 c2_colour{yellow};
      if(not have)
        std::snprintf(c1, sizeof c1, "NO DOT");
      else if(cut)
        {
        float const below{-std::asin(std::clamp(dp.y, -1.f, 1.f)) * compass_degrees};
        std::snprintf(c1, sizeof c1, "%s %.0f%s", below >= 0.f ? "BELOW" : "ABOVE", static_cast<double>(std::fabs(below)), degree);
        std::snprintf(c2, sizeof c2, "OFF VERTICAL %.0f%s", static_cast<double>(po.off_vertical), degree);
        if(f.has_position)
          {
          std::snprintf(alt, sizeof alt, "ALT %.2f km", f.altitude / 1000.0);
          if(not std::isnan(f.vertical_speed))
            std::snprintf(climb, sizeof climb, "%+.0f m/s", f.vertical_speed);
          }
        }
      else if(f.has_position and f.has_place)
        {
        // a place on the surface as the target (where it stands from the data source): the straight line to it
        // against the line flown; flown steeper, the ship comes down short of it, shallower, it flies over it
        glide_t const g{glide_to(f.latitude, f.longitude, f.altitude, f.place_latitude, f.place_longitude, f.planet_radius)};
        std::snprintf(c1, sizeof c1, "TARGET %.0f%s %s", std::fabs(g.below), degree, g.below >= 0.0 ? "DOWN" : "UP");
        if(std::isnan(f.path_angle))
          std::snprintf(c2, sizeof c2, "PATH ?");
        else
          {
          double const off{-f.path_angle - g.below};
          if(std::fabs(off) < 2.0)
            {
            std::snprintf(c2, sizeof c2, "ON PATH");
            c2_colour = green_line;
            }
          else
            {
            std::snprintf(c2, sizeof c2, "%s %.0f%s", off > 0.0 ? "SHALLOWER" : "STEEPER", std::fabs(off), degree);
            c2_colour = orange;
            }
          }
        std::snprintf(alt, sizeof alt, "ALT %.1f km", f.altitude / 1000.0);
        std::snprintf(climb, sizeof climb, "%.1f km TO GO", g.distance / 1000.0);
        std::snprintf(to_go, sizeof to_go, "%s", climb);
        }
      else if(f.has_position)
        {
        // with the planet as the target: the nose below the horizon is 90 minus the target off the nose
        float const nose{90.f - a.off_nose};
        float const off_path{s.compass_should_dive - nose};
        std::snprintf(c1, sizeof c1, "NOSE %.0f%s %s", static_cast<double>(std::fabs(nose)), degree, nose >= 0.f ? "DOWN" : "UP");
        if(std::fabs(off_path) < 2.f)
          {
          std::snprintf(c2, sizeof c2, "ON PATH");
          c2_colour = green_line;
          }
        else
          {
          std::snprintf(c2, sizeof c2, "%s %.0f%s", off_path > 0.f ? "PUSH DOWN" : "PULL UP", static_cast<double>(std::fabs(off_path)), degree);
          c2_colour = orange;
          }
        std::snprintf(alt, sizeof alt, "ALT %.1f km", f.altitude / 1000.0);
        if(not std::isnan(f.vertical_speed))
          std::snprintf(climb, sizeof climb, "%+.2f km/s", f.vertical_speed / 1000.0);
        }
      else
        std::snprintf(c1, sizeof c1, "OFF %.0f%s", static_cast<double>(a.off_nose), degree);
      bool const planet_lines{have and f.has_position and not grounded};
      if(alt[0])
        std::snprintf(c3, sizeof c3, "%s%s%s", alt, climb[0] ? "  " : "", climb);
      // edworld_eht's notes under the right sphere's lines (t1_note, model_note), the T1 one first
      auto const notes = [&](ImDrawList * dl, float width, float y) -> void
        {
        ULONGLONG const now{GetTickCount64()};
        for(sphere_note_t const * n: {&t1_note, &model_note})
          if(n->text[0] and now < n->until)
            {
            ImU32 const c{n->colour == 0 ? green_line : n->colour == 1 ? col(s.compass_colour_wrong) : col(0x9aa0a6u)};
            y += sphere_line(dl, 0.f, width, y, 44.f, c, n->text);
            }
        };
      ImGui::SetCurrentContext(r.imgui);
      ImGuiIO & io{ImGui::GetIO()};
      io.DisplaySize = ImVec2{static_cast<float>(sphere_w), static_cast<float>(sphere_h)};
      for(int k{}; k != 2; ++k)
        {
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        ImDrawList * const dl{ImGui::GetBackgroundDrawList()};
        bool const above{s.compass_sphere_text_above != 0};
        ImVec2 const centre{above ? ImVec2{above_w / 2.f, above_band + 256.f} : ImVec2{256.f, 236.f}};
        ImFont * const name_font{sphere_font()};
        if(above)
          {
          // a band over the sphere, dark under the lines (they may stand over a bright sky or a planet)
          dl->AddRectFilled(ImVec2{8.f, 0.f}, ImVec2{above_w - 8.f, above_band - 6.f}, col(mix_rgb(s.compass_colour_disc, 0u, 0.5f), 170), 14.f);
          if(k == 0)
            {
            if(cs.approach)
              draw_sphere_top(dl, centre, dp, have);
            else
              draw_sphere_front(dl, centre, d, have);
            char wide[96];
            std::snprintf(wide, sizeof wide, "%s%s%s", a1, a2[0] ? "  " : "", a2);
            if(planet_lines)
              {
              // at a planet the right sphere's band is the game's (its latitude, longitude and gravity in the middle, its
              // altitude ladder by the left edge; Artur's shot 2026-10-11: the lines there unreadable), so they come here,
              // under the angles, BEHIND beside them
              float const used{sphere_line(dl, 0.f, above_w, 10.f, 64.f, behind ? orange : blue, wide, orange, behind ? "BEHIND" : nullptr)};
              sphere_line(dl, 0.f, above_w, 10.f + used + 4.f, 44.f, yellow, c1, c2_colour, c2, yellow, to_go[0] ? to_go : alt);
              }
            else
              {
              float const used{sphere_line(dl, 0.f, above_w, 10.f, 76.f, behind ? orange : blue, wide)};
              if(behind)
                sphere_line(dl, 0.f, above_w, 10.f + used + 2.f, 30.f, orange, "BEHIND");
              }
            ImVec2 const name{name_font->CalcTextSizeA(26.f, FLT_MAX, 0.f, "edworld")};
            dl->AddText(name_font, 26.f, ImVec2{above_w / 2.f + 256.f - name.x - 10.f, above_h - name.y - 6.f}, name_colour,
                        "edworld");
            }
          else
            {
            if(cut)
              draw_sphere_dome(dl, centre, dp, have, not f.has_position);
            else
              draw_sphere_oblique(dl, centre, d, have, f.has_position and not f.has_place, should);
            float y_band{8.f};
            if(not f.has_position and not grounded)
              {
              y_band += sphere_line(dl, 0.f, above_w, y_band, 66.f, yellow, c1, c2_colour, c2) + 4.f;
              if(c3[0])
                y_band += sphere_line(dl, 0.f, above_w, y_band, 38.f, yellow, c3) + 4.f;
              }
            if constexpr(with_eht)
              notes(dl, above_w, y_band);
            }
          }
        else
          {
          float y{448.f};
          if(k == 0)
            {
            if(cs.approach)
              draw_sphere_top(dl, centre, dp, have);
            else
              draw_sphere_front(dl, centre, d, have);
            y += sphere_line(dl, 0.f, below_w, y, 84.f, behind ? orange : blue, a1);
            if(a2[0])
              y += sphere_line(dl, 0.f, below_w, y, 84.f, behind ? orange : blue, a2);
            if(behind)
              sphere_line(dl, 0.f, below_w, y, 44.f, orange, "BEHIND");
            // the plugin's name, small, in the corner under the angles
            ImVec2 const name{name_font->CalcTextSizeA(26.f, FLT_MAX, 0.f, "edworld")};
            dl->AddText(name_font, 26.f, ImVec2{below_w - name.x - 10.f, below_h - name.y - 6.f}, name_colour, "edworld");
            }
          else
            {
            if(cut)
              draw_sphere_dome(dl, centre, dp, have, not f.has_position);
            else
              draw_sphere_oblique(dl, centre, d, have, f.has_position and not f.has_place, should);
            if(not grounded)
              {
              y += sphere_line(dl, 0.f, below_w, y, 80.f, yellow, c1);
              if(c2[0])
                y += sphere_line(dl, 0.f, below_w, y, 72.f, c2_colour, c2);
              if(c3[0])
                y += sphere_line(dl, 0.f, below_w, y + 4.f, 40.f, yellow, c3) + 4.f;
              }
            if constexpr(with_eht)
              notes(dl, below_w, y);
            }
          }
        ImGui::Render();
        backup_t b;
        save(ctx, b);
        float const clear[4]{0.f, 0.f, 0.f, 0.f};
        ctx->ClearRenderTargetView(cs.sphere_rtv[k], clear);
        ctx->OMSetRenderTargets(1, &cs.sphere_rtv[k], nullptr);
        ctx->PSSetShaderResources(1, 1, &r.white);
        ctx->PSSetConstantBuffers(0, 1, &r.white_gate);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        restore(ctx, b);
        ctx->GenerateMips(cs.sphere_srv[k]);
        }
      // a file edworld_sphere_dump beside the log: both textures (their largest level) written once to edworld_dumps, at the
      // first plausible reading after it appeared (checking)
      std::wstring const trigger{settings().dir + L"\\edworld_sphere_dump"};
      if(have and GetFileAttributesW(trigger.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
        DeleteFileW(trigger.c_str());
        CreateDirectoryW((settings().dir + L"\\edworld_dumps").c_str(), nullptr);
        for(int k{}; k != 2; ++k)
          {
          D3D11_TEXTURE2D_DESC sd{};
          sd.Width = sphere_w;
          sd.Height = sphere_h;
          sd.MipLevels = sd.ArraySize = 1;
          sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          sd.SampleDesc.Count = 1;
          sd.Usage = D3D11_USAGE_STAGING;
          sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          ID3D11Texture2D * staging{};
          if(FAILED(r.device->CreateTexture2D(&sd, nullptr, &staging)) or not staging)
            continue;
          ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, cs.sphere_tex[k], 0, nullptr);
          D3D11_MAPPED_SUBRESOURCE m{};
          if(SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)) and m.pData)
            {
            wchar_t name[MAX_PATH];
            std::swprintf(name, MAX_PATH, L"%ls\\edworld_dumps\\sphere%d_%ux%u_f28_pitch%u.raw", settings().dir.c_str(), k, sphere_w,
                          sphere_h, m.RowPitch);
            if(std::FILE * out{_wfopen(name, L"wb")})
              {
              std::fwrite(m.pData, 1, static_cast<std::size_t>(m.RowPitch) * sphere_h, out);
              std::fclose(out);
              }
            ctx->Unmap(staging, 0);
            }
          staging->Release();
          }
        log_line("compass: spheres written to edworld_dumps");
        }
      if(not cs.spheres_drawn)
        log_line("compass: spheres rendered (%ux%u px each)", sphere_w, sphere_h);
      cs.spheres_drawn = true;
      }
    }  // namespace

  auto panel_compass(ID3D11DeviceContext * ctx, ID3D11Device * device, std::uint64_t frame) noexcept -> void
    {
    settings_t const & s{settings()};
    if(s.compass == 0 or frame == cs.drawn_frame)
      return;
    D3D11_TEXTURE2D_DESC d{};
    ID3D11Texture2D * const tex{surface_sampled(ctx, s.compass_surface_width, s.compass_surface_height, d)};
    if(not tex)
      return;
    if(not create(device, ctx) or not r.font)
      {
      tex->Release();
      return;
      }
    if(cs.device != device)
      compass_reset(device);
    cs.drawn_frame = frame;

    // the disc's square: read the copy made earlier, then copy this frame's (before edworld draws anything)
    auto const half{static_cast<std::int32_t>(std::ceil(s.compass_radius * 1.4f))};
    std::int32_t const left{std::max(0, static_cast<std::int32_t>(s.compass_x) - half)};
    std::int32_t const top{std::max(0, static_cast<std::int32_t>(s.compass_y) - half)};
    std::int32_t const right{std::min(static_cast<std::int32_t>(d.Width), static_cast<std::int32_t>(s.compass_x) + half)};
    std::int32_t const bottom{std::min(static_cast<std::int32_t>(d.Height), static_cast<std::int32_t>(s.compass_y) + half)};
    if(right <= left or bottom <= top)
      {
      tex->Release();
      return;
      }
    auto const cw{static_cast<std::uint32_t>(right - left)}, ch{static_cast<std::uint32_t>(bottom - top)};
    if(not cs.copy or cs.copy_width != cw or cs.copy_height != ch or cs.copy_format != d.Format)
      {
      release(cs.copy);
      cs.copied = false;
      D3D11_TEXTURE2D_DESC c{};
      c.Width = cw;
      c.Height = ch;
      c.MipLevels = c.ArraySize = 1;
      c.Format = d.Format;
      c.SampleDesc.Count = 1;
      c.Usage = D3D11_USAGE_STAGING;
      c.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if(FAILED(r.device->CreateTexture2D(&c, nullptr, &cs.copy)) or not cs.copy)
        {
        tex->Release();
        return;
        }
      cs.copy_width = cw;
      cs.copy_height = ch;
      cs.copy_format = d.Format;
      }
    if(cs.copied)
      {
      D3D11_MAPPED_SUBRESOURCE m{};
      HRESULT const hr{ctx->Map(cs.copy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)};
      if(hr != DXGI_ERROR_WAS_STILL_DRAWING)
        {
        cs.copied = false;
        if(SUCCEEDED(hr) and m.pData)
          {
          double const now_s{seconds_now()};
          bool const fresh{compass_fresh(cs.have_shown, now_s - cs.shown_at, s.compass_stale_s)};
          // a flash is told by the hue of the dot shown, while it holds: a stale one (another theme since) tells nothing
          cs.reading = read_compass(static_cast<std::uint8_t const *>(m.pData), m.RowPitch, static_cast<std::int32_t>(cw),
                                    static_cast<std::int32_t>(ch), s.compass_x - static_cast<float>(left),
                                    s.compass_y - static_cast<float>(top), s.compass_radius * 1.25f, fresh ? cs.dot_hue : -1.f);
          cs.have_reading = true;
          // the gate: no further from the reading shown than the ship can turn since; open once that one went stale
          bool gated{};
          if(s.compass_max_rate_dps > 0.f and fresh and compass_plausible(cs.reading, compass_limits(s)))
            {
            float const turned{angle_between(compass_direction(cs.reading.x, cs.reading.y, s.compass_radius, cs.reading.filled),
                                             compass_direction(cs.shown.x, cs.shown.y, s.compass_radius, cs.shown.filled))};
            gated = turned > s.compass_max_rate_dps * static_cast<float>(now_s - cs.shown_at) + 5.f;
            }
          if(cs.reading.flash)
            {
            ++cs.held;
            ++cs.flashes;
            }
          else if(gated)
            {
            ++cs.held;
            ++cs.gated;
            }
          else if(compass_plausible(cs.reading, compass_limits(s)))
            {
            cs.shown = cs.reading;
            cs.have_shown = true;
            cs.shown_at = now_s;
            cs.dot_hue = cs.reading.dot_hue;
            cs.held = 0;
            if(cs.approach and cs.have_smooth)
              {
              float const k{smoothing_weight(static_cast<float>(now_s - cs.smooth_at), s.compass_approach_smooth_s)};
              cs.smooth.x += k * (cs.reading.x - cs.smooth.x);
              cs.smooth.y += k * (cs.reading.y - cs.smooth.y);
              cs.smooth.filled = cs.reading.filled;
              }
            else
              cs.smooth = cs.reading;
            cs.have_smooth = true;
            cs.smooth_at = now_s;
            }
          else
            {
            ++cs.held;
            ++cs.rejected;
            }
          cs.lag = frame - cs.copied_frame;
          ++cs.reads;
          ctx->Unmap(cs.copy, 0);
          }
        }
      }
    if(not cs.copied)
      {
      D3D11_BOX const area{static_cast<UINT>(left), static_cast<UINT>(top), 0, static_cast<UINT>(right), static_cast<UINT>(bottom), 1};
      ctx->CopySubresourceRegion(cs.copy, 0, 0, 0, 0, tex, 0, &area);
      cs.copied = true;
      cs.copied_frame = frame;
      }

    // the gate: one pixel of the rim (right of the centre); no box, so everything drawn follows that pixel's alpha
    ID3D11RenderTargetView * const rtv{rtv_for(tex)};
    ID3D11ShaderResourceView * const mask{rtv ? compass_mask_for(d) : nullptr};
    if(not rtv or not mask)
      {
      tex->Release();
      return;
      }
    auto const gx{static_cast<UINT>(s.compass_x + s.compass_radius)}, gy{static_cast<UINT>(s.compass_y)};
    if(gx + 1u < d.Width and gy + 1u < d.Height)
      {
      D3D11_BOX const g{gx, gy, 0, gx + 1u, gy + 1u, 1};
      ctx->CopySubresourceRegion(cs.mask, 0, gx, gy, 0, tex, 0, &g);
      }
    // the game's compass off the surface once read and the gate copied: its square made transparent (D3D11.1 ClearView)
    if(s.compass_hide_game)
      {
      ID3D11DeviceContext1 * ctx1{};
      if(SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&ctx1))) and ctx1)
        {
        float const clear[4]{0.f, 0.f, 0.f, 0.f};
        D3D11_RECT const square{left, top, right, bottom};
        ctx1->ClearView(rtv, clear, &square, 1);
        ctx1->Release();
        }
      else if(not cs.told_hide)
        {
        cs.told_hide = true;
        log_line("compass: no D3D11.1 context; the game's compass stays");
        }
      }
    tex->Release();
    D3D11_MAPPED_SUBRESOURCE gm{};
    if(FAILED(ctx->Map(r.gate, 0, D3D11_MAP_WRITE_DISCARD, 0, &gm)))
      return;
    gate_t const gate{{0, 0, 0, 0}, {static_cast<std::int32_t>(gx), static_cast<std::int32_t>(gy), 0, 0}};
    std::memcpy(gm.pData, &gate, sizeof gate);
    ctx->Unmap(r.gate, 0);

    flight_t const f{flight()};
    compass_reading_t const & c{cs.shown};
    compass_angles_t const a{compass_angles(c.x, c.y, s.compass_radius, c.filled)};

    float const sw{static_cast<float>(d.Width)}, sh{static_cast<float>(d.Height)};
    ImGui::SetCurrentContext(r.imgui);
    ImGuiIO & io{ImGui::GetIO()};
    io.DisplaySize = ImVec2{sw, sh};
    io.DeltaTime = 1.f / 60.f;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ImDrawList * const dl{ImGui::GetBackgroundDrawList()};
    char line[256];
    if(s.compass >= 3)
      {
      // the surface's pixels: magenta lines every 100, their coordinates every 200 (magenta is never read as the dot)
      ImU32 const grid{IM_COL32(255, 0, 255, 255)};
      for(float x{100.f}; x < sw; x += 100.f)
        dl->AddLine(ImVec2{x, 0.f}, ImVec2{x, sh}, grid, std::fmod(x, 500.f) == 0.f ? 4.f : 2.f);
      for(float y{100.f}; y < sh; y += 100.f)
        dl->AddLine(ImVec2{0.f, y}, ImVec2{sw, y}, grid, std::fmod(y, 500.f) == 0.f ? 4.f : 2.f);
      for(float y{0.f}; y < sh; y += 200.f)
        for(float x{0.f}; x < sw; x += 200.f)
          {
          std::snprintf(line, sizeof line, "%.0f,%.0f", static_cast<double>(x), static_cast<double>(y));
          dl->AddText(r.font, 22.f, ImVec2{x + 4.f, y + 4.f}, IM_COL32(255, 255, 0, 255), line);
          }
      // where the reading looks: the disc's rim and the square copied
      dl->AddCircle(ImVec2{s.compass_x, s.compass_y}, s.compass_radius, IM_COL32(255, 128, 0, 255), 64, 1.f);
      dl->AddRect(ImVec2{static_cast<float>(left), static_cast<float>(top)}, ImVec2{static_cast<float>(right), static_cast<float>(bottom)},
                  IM_COL32(255, 128, 0, 255));
      }
    // the angles: DN/UP below or above the wings' plane, LT/RT left or right of the nose; BEHIND under them, orange
    float y{s.compass_text_y};
    bool const fresh{shown_fresh(s)};
    if(fresh != cs.was_fresh)
      {
      cs.was_fresh = fresh;
      log_line("compass: %s (flashes %llu, gated %llu, rejected %llu)", fresh ? "reading" : "no fresh reading, NO DOT",
               static_cast<unsigned long long>(cs.flashes), static_cast<unsigned long long>(cs.gated), static_cast<unsigned long long>(cs.rejected));
      }
    bool const behind{fresh and not c.filled};
    if(not fresh)
      std::snprintf(line, sizeof line, "NO DOT");
    else
      std::snprintf(line, sizeof line, "%s %.1f%s  %s %.1f%s", a.up < 0.f ? "DN" : "UP", static_cast<double>(std::fabs(a.up)), degree,
                    a.right < 0.f ? "LT" : "RT", static_cast<double>(std::fabs(a.right)), degree);
    ImU32 const main_colour{behind ? IM_COL32(255, 150, 40, 255) : IM_COL32(150, 230, 255, 255)};
    if(s.compass_text)
      {
      y += ground_text(dl, s.compass_text_x, y, s.compass_text_size, main_colour, line);
      if(behind)
        ground_text(dl, s.compass_text_x, y, std::round(s.compass_text_size * 0.6f), main_colour, "BEHIND");
      }
    if(s.compass >= 2)
      {
      ImU32 const info{IM_COL32(255, 230, 120, 255)};
      float const x0{s.compass_info_x}, size{s.compass_info_size};
      float yi{s.compass_info_y};
      std::snprintf(line, sizeof line, "off %.1f%s r %.1f/%.0f dot %+.1f %+.1f", static_cast<double>(a.off_nose), degree,
                    static_cast<double>(std::sqrt(c.x * c.x + c.y * c.y)), static_cast<double>(s.compass_radius), static_cast<double>(c.x),
                    static_cast<double>(c.y));
      yi += ground_text(dl, x0, yi, size, info, line);
      std::snprintf(line, sizeof line, "px %u %s  held %llu  rejected %llu", c.pixels, c.filled ? "filled" : "hollow",
                    static_cast<unsigned long long>(cs.held), static_cast<unsigned long long>(cs.rejected));
      yi += ground_text(dl, x0, yi, size, info, line);
      if(f.has_position)
        {
        std::snprintf(line, sizeof line, "planet: nose %.1f%s path %.1f%s below", static_cast<double>(90.f - a.off_nose), degree,
                      -f.path_angle, degree);
        yi += ground_text(dl, x0, yi, size, info, line);
        std::snprintf(line, sizeof line, "alt %.2f km hdg %.0f", f.altitude / 1000.0, f.heading);
        yi += ground_text(dl, x0, yi, size, info, line);
        }
      else
        yi += ground_text(dl, x0, yi, size, info, "no position in Status.json");
      std::snprintf(line, sizeof line, "dest %u %.30s", f.destination_body, f.destination_name);
      yi += ground_text(dl, x0, yi, size, info, line);
      std::snprintf(line, sizeof line, "lag %llu fr %s", static_cast<unsigned long long>(cs.lag), EDWORLD_VERSION);
      yi += ground_text(dl, x0, yi, size, info, line);
      }
    ImGui::Render();

    backup_t b;
    save(ctx, b);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->PSSetShaderResources(1, 1, &mask);
    ctx->PSSetConstantBuffers(0, 1, &r.gate);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    restore(ctx, b);
    if(s.compass_spheres)
      render_spheres(ctx, s, f);

    if(not cs.told)
      {
      cs.told = true;
      log_line("compass: first drawn (mode %u, surface %ux%u, disc %.0f,%.0f r %.0f, text at %.0f,%.0f)", s.compass, d.Width, d.Height,
               static_cast<double>(s.compass_x), static_cast<double>(s.compass_y), static_cast<double>(s.compass_radius),
               static_cast<double>(s.compass_text_x), static_cast<double>(s.compass_text_y));
      }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    if(s.compass_log_ms and (now.QuadPart - cs.last_log) * 1000 / freq.QuadPart >= s.compass_log_ms)
      {
      cs.last_log = now.QuadPart;
      // the raw reading, plausible or not, and the angles shown (from the last plausible one)
      compass_reading_t const & raw{cs.reading};
      log_line("compass: %s %s dot %+.2f %+.2f px %u %s hue disc %.0f dot %.0f off %.2f up %+.2f right %+.2f lag %llu | alt %.0f lat %.6f lon %.6f hdg %.0f path %+.2f "
               "flags %llx dest %u %.40s",
               raw.found ? "dot" : "none", raw.filled ? "filled" : "hollow", static_cast<double>(raw.x), static_cast<double>(raw.y), raw.pixels,
               raw.flash ? "flash" : compass_plausible(raw, compass_limits(s)) ? "ok" : "rejected", static_cast<double>(raw.disc_hue),
               static_cast<double>(raw.dot_hue), static_cast<double>(a.off_nose), static_cast<double>(a.up),
               static_cast<double>(a.right), static_cast<unsigned long long>(cs.lag), f.has_position ? f.altitude : -1.0, f.latitude, f.longitude, f.heading, f.path_angle,
               static_cast<unsigned long long>(f.flags), f.destination_body, f.destination_name);
      }
    }

  namespace
    {
    ///\brief sphere k's quad right after the game's draw whose records are bound (its instance from start_instance), corners
    /// in that draw's local plane, the texture's used part to its edges
    auto draw_sphere_quad(ID3D11DeviceContext * ctx, std::uint32_t k, float const (&corners)[4][3], std::uint32_t start_instance) -> bool
      {
      settings_t const & s{settings()};
      bool const above{s.compass_sphere_text_above != 0};
      float const used_w{above ? above_w : below_w}, used_h{above ? above_h : below_h};
      list_cb_t cb{};
      for(int i{}; i != 4; ++i)
        for(int j{}; j != 3; ++j)
          cb.corner[i][j] = corners[i][j];
      cb.uv_extent[0] = used_w / static_cast<float>(sphere_w);
      cb.uv_extent[1] = used_h / static_cast<float>(sphere_h);
      cb.gain[0] = s.compass_sphere_gain;
      cb.colour[0] = s.compass_sphere_gamma;
      D3D11_MAPPED_SUBRESOURCE m{};
      if(FAILED(ctx->Map(r.list_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return false;
      std::memcpy(m.pData, &cb, sizeof cb);
      ctx->Unmap(r.list_cb, 0);

    list_backup_t b;
      ctx->VSGetShader(&b.vs, b.vs_instances, &b.vs_instance_count);
      ctx->PSGetShader(&b.ps, b.ps_instances, &b.ps_instance_count);
      ctx->IAGetInputLayout(&b.layout);
      ctx->IAGetPrimitiveTopology(&b.topology);
      ctx->VSGetConstantBuffers(list_cb_slot, 1, &b.vs_cb);
      ctx->PSGetConstantBuffers(list_cb_slot, 1, &b.ps_cb);
      ctx->PSGetShaderResources(0, 2, b.ps_srv);
      ctx->PSGetSamplers(0, 1, &b.ps_sampler);
      ctx->OMGetBlendState(&b.blend, b.blend_factor, &b.sample_mask);
      ctx->OMGetDepthStencilState(&b.depth, &b.stencil_ref);
      ctx->RSGetState(&b.raster);
      // the sphere's texture, and an opaque pixel as the gate (the spheres show whenever the panel is drawn)
      ID3D11ShaderResourceView * const srvs[2]{cs.sphere_srv[k], r.white};
      ctx->VSSetShader(r.list_vs, nullptr, 0);
      ctx->PSSetShader(r.list_ps, nullptr, 0);
      ctx->IASetInputLayout(r.list_layout);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->VSSetConstantBuffers(list_cb_slot, 1, &r.list_cb);
      ctx->PSSetConstantBuffers(list_cb_slot, 1, &r.list_cb);
      ctx->PSSetShaderResources(0, 2, srvs);
      ctx->PSSetSamplers(0, 1, &r.list_sampler);
      float const factor[4]{};
      ctx->OMSetBlendState(r.list_blend, factor, 0xffffffffu);
      ctx->OMSetDepthStencilState(r.list_depth, 0);
      ctx->RSSetState(r.list_raster);
      ctx->DrawInstanced(6, 1, 0, start_instance);

      ctx->VSSetShader(b.vs, b.vs_instances, b.vs_instance_count);
      ctx->PSSetShader(b.ps, b.ps_instances, b.ps_instance_count);
      ctx->IASetInputLayout(b.layout);
      ctx->IASetPrimitiveTopology(b.topology);
      ctx->VSSetConstantBuffers(list_cb_slot, 1, &b.vs_cb);
      ctx->PSSetConstantBuffers(list_cb_slot, 1, &b.ps_cb);
      ctx->PSSetShaderResources(0, 2, b.ps_srv);
      ctx->PSSetSamplers(0, 1, &b.ps_sampler);
      ctx->OMSetBlendState(b.blend, b.blend_factor, b.sample_mask);
      ctx->OMSetDepthStencilState(b.depth, b.stencil_ref);
      ctx->RSSetState(b.raster);
      for(IUnknown * u: std::initializer_list<IUnknown *>{b.vs, b.ps, b.layout, b.vs_cb, b.ps_cb, b.ps_srv[0], b.ps_srv[1], b.ps_sampler,
                                                         b.blend, b.depth, b.raster})
        if(u)
          u->Release();
      for(UINT i{}; i != b.vs_instance_count; ++i)
        b.vs_instances[i]->Release();
      for(UINT i{}; i != b.ps_instance_count; ++i)
        b.ps_instances[i]->Release();
      return true;
      }

    // ---- anchored: the draws of the HUD surface, each read once for the triangles holding the anchors ----
    struct anchor_draw_t
      {
      enum struct state_e : std::uint8_t
        {
        empty,
        pending,
        judged
        };
      void * ib;
      UINT ib_offset;
      std::uint32_t start_index;
      std::uint32_t index_count;
      void * vb;
      UINT vb_offset;
      std::int32_t base_vertex;
      state_e state;
      std::uint32_t index_bytes;
      std::uint32_t vertices;  ///< copied after the base vertex
      ID3D11Buffer * staged_ib;
      ID3D11Buffer * staged_vb;
      bool holds[2];
      panel_map_t map[2];
      };

    constexpr std::uint32_t max_anchor_draws{24};
    constexpr std::uint32_t max_anchor_indices{2048};
    constexpr std::uint32_t max_anchor_vertices{4096};
    anchor_draw_t anchor_draws[max_anchor_draws]{};
    std::uint32_t anchor_draw_next{};
    std::uint64_t anchor_drawn_frame[2]{~0ull, ~0ull};
    float anchor_diameter_local{};  ///< the compass's diameter in the HUD mesh's local units
    float anchor_right[2]{1.f, 0.f}, anchor_up[2]{0.f, 1.f};  ///< the surface's right and up in the mesh's local plane
    bool told_anchor[2]{};
    float told_place[2][3]{};  ///< the spheres' centres and size last logged, in the HUD mesh's local units

    auto anchor_points(settings_t const & s, float (&pt)[2][2]) -> void
      {
      pt[0][0] = s.compass_x;
      pt[0][1] = s.compass_y;
      pt[1][0] = s.compass_anchor_c_px;
      pt[1][1] = s.compass_anchor_c_py;
      }

    auto drop_anchor_staged(anchor_draw_t & e) -> void
      {
      if(e.staged_ib)
        e.staged_ib->Release();
      if(e.staged_vb)
        e.staged_vb->Release();
      e.staged_ib = nullptr;
      e.staged_vb = nullptr;
      }

    ///\brief the copied indices and vertices read: for each anchor, the triangle whose surface pixels hold it and its map
    auto judge_anchor(ID3D11DeviceContext * ctx, anchor_draw_t & e, settings_t const & s) -> void
      {
      D3D11_MAPPED_SUBRESOURCE mi{}, mv{};
      HRESULT const hi{ctx->Map(e.staged_ib, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mi)};
      if(hi == DXGI_ERROR_WAS_STILL_DRAWING)
        return;
      HRESULT const hv{SUCCEEDED(hi) ? ctx->Map(e.staged_vb, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mv) : hi};
      if(hv == DXGI_ERROR_WAS_STILL_DRAWING)
        {
        ctx->Unmap(e.staged_ib, 0);
        return;
        }
      e.state = anchor_draw_t::state_e::judged;
      float pt[2][2];
      anchor_points(s, pt);
      if(SUCCEEDED(hi) and SUCCEEDED(hv) and mi.pData and mv.pData)
        {
        auto const index = [&](std::uint32_t i) -> std::uint32_t
          {
          if(e.index_bytes == 2u)
            {
            std::uint16_t v{};
            std::memcpy(&v, static_cast<std::uint8_t const *>(mi.pData) + i * 2u, 2);
            return v;
            }
          std::uint32_t v{};
          std::memcpy(&v, static_cast<std::uint8_t const *>(mi.pData) + i * 4u, 4);
          return v;
          };
        for(std::uint32_t t{}; t + 2u < e.index_count; t += 3u)
          {
          std::uint32_t const idx[3]{index(t), index(t + 1u), index(t + 2u)};
          if(idx[0] >= e.vertices or idx[1] >= e.vertices or idx[2] >= e.vertices)
            continue;
          vec3_t p[3];
          float px[3][2];
          for(std::uint32_t i{}; i != 3; ++i)
            {
            std::uint32_t w[5];
            std::memcpy(w, static_cast<std::uint8_t const *>(mv.pData) + static_cast<std::size_t>(idx[i]) * vertex_stride, sizeof w);
            p[i] = decode_local(w[0], w[1], w[2]);
            float u, v;
            decode_uv(w[4], u, v);
            px[i][0] = u * static_cast<float>(s.compass_surface_width);
            px[i][1] = v * static_cast<float>(s.compass_surface_height);
            }
          for(int k{}; k != 2; ++k)
            {
            if(e.holds[k])
              continue;
            // the anchor inside the triangle's pixels (barycentric, either winding)
            float const d{(px[1][1] - px[2][1]) * (px[0][0] - px[2][0]) + (px[2][0] - px[1][0]) * (px[0][1] - px[2][1])};
            if(std::fabs(d) < 1e-6f)
              continue;
            float const l0{((px[1][1] - px[2][1]) * (pt[k][0] - px[2][0]) + (px[2][0] - px[1][0]) * (pt[k][1] - px[2][1])) / d};
            float const l1{((px[2][1] - px[0][1]) * (pt[k][0] - px[2][0]) + (px[0][0] - px[2][0]) * (pt[k][1] - px[2][1])) / d};
            if(l0 < 0.f or l1 < 0.f or l0 + l1 > 1.f)
              continue;
            if(auto const m{map_from(p, px)}; m)
              {
              e.holds[k] = true;
              e.map[k] = *m;
              log_line("compass: the %s sphere's anchor (%.0f, %.0f) is in a draw of %u indices (base vertex %d), triangle %u",
                       k == 0 ? "left" : "right", static_cast<double>(pt[k][0]), static_cast<double>(pt[k][1]), e.index_count, e.base_vertex,
                       t / 3u);
              }
            }
          }
        }
      if(SUCCEEDED(hi))
        ctx->Unmap(e.staged_ib, 0);
      if(SUCCEEDED(hv))
        ctx->Unmap(e.staged_vb, 0);
      drop_anchor_staged(e);
      }

    ///\brief anchored: after a draw of the HUD surface, the spheres whose anchors it holds
    auto anchored_after(ID3D11DeviceContext * ctx, std::uint64_t frame, std::uint32_t index_count, std::uint32_t start_index,
                        std::int32_t base_vertex, std::uint32_t start_instance) -> void
      {
      settings_t const & s{settings()};
      D3D11_TEXTURE2D_DESC d{};
      ID3D11Texture2D * const tex{surface_sampled(ctx, s.compass_surface_width, s.compass_surface_height, d)};
      if(not tex)
        return;
      tex->Release();
      ID3D11Buffer * ib{};
      DXGI_FORMAT ib_format{};
      UINT ib_offset{};
      ctx->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
      ID3D11Buffer * vb{};
      UINT vb_stride{}, vb_offset{};
      ctx->IAGetVertexBuffers(1, 1, &vb, &vb_stride, &vb_offset);
      anchor_draw_t * entry{};
      if(ib and vb and vb_stride == vertex_stride and index_count <= max_anchor_indices and base_vertex >= 0)
        {
        for(anchor_draw_t & e: anchor_draws)
          if(e.state != anchor_draw_t::state_e::empty and e.ib == ib and e.ib_offset == ib_offset and e.start_index == start_index
             and e.index_count == index_count and e.vb == vb and e.vb_offset == vb_offset and e.base_vertex == base_vertex)
            {
            entry = &e;
            break;
            }
        if(not entry)
          {
          anchor_draw_t & e{anchor_draws[anchor_draw_next++ % max_anchor_draws]};
          drop_anchor_staged(e);
          std::uint32_t const size{ib_format == DXGI_FORMAT_R16_UINT ? 2u : 4u};
          e = anchor_draw_t{ib, ib_offset, start_index, index_count, vb, vb_offset, base_vertex, anchor_draw_t::state_e::pending, size};
          D3D11_BUFFER_DESC vd{};
          vb->GetDesc(&vd);
          std::uint64_t const from{vb_offset + static_cast<std::uint64_t>(base_vertex) * vertex_stride};
          std::uint64_t const room{from < vd.ByteWidth ? (vd.ByteWidth - from) / vertex_stride : 0u};
          e.vertices = static_cast<std::uint32_t>(std::min<std::uint64_t>(room, max_anchor_vertices));
          e.staged_ib = staging_copy(ctx, ib, ib_offset + std::uint64_t{start_index} * size, index_count * size);
          e.staged_vb = e.vertices ? staging_copy(ctx, vb, from, e.vertices * vertex_stride) : nullptr;
          if(not e.staged_ib or not e.staged_vb)
            {
            drop_anchor_staged(e);
            e.state = anchor_draw_t::state_e::judged;
            }
          entry = nullptr;  // judged in a later frame
          }
        else if(entry->state == anchor_draw_t::state_e::pending)
          {
          judge_anchor(ctx, *entry, s);
          entry = nullptr;
          }
        }
      if(ib)
        ib->Release();
      if(vb)
        vb->Release();
      if(not entry or entry->state != anchor_draw_t::state_e::judged)
        return;
      float pt[2][2];
      anchor_points(s, pt);
      // sizes and offsets in the mesh's own units, from the compass's piece (the HUD's pieces are scaled differently on the
      // surface: the speed readout's pixels are not the compass's); kept for a draw that holds only the right anchor
      if(entry->holds[0])
        {
        // the compass's piece gives the mesh's directions too: right and up on the surface (y down there), in local units
        auto const o{local_of(entry->map[0], pt[0][0], pt[0][1])};
        auto const rx{local_of(entry->map[0], pt[0][0] + s.compass_radius, pt[0][1])};
        auto const uy{local_of(entry->map[0], pt[0][0], pt[0][1] - s.compass_radius)};
        if(o and rx and uy)
          {
          float const ax{rx->x - o->x}, ay{rx->y - o->y}, bx{uy->x - o->x}, by{uy->y - o->y};
          float const la{std::sqrt(ax * ax + ay * ay)}, lb{std::sqrt(bx * bx + by * by)};
          if(la > 0.f and lb > 0.f)
            {
            anchor_diameter_local = la + la;
            anchor_right[0] = ax / la;
            anchor_right[1] = ay / la;
            anchor_up[0] = bx / lb;
            anchor_up[1] = by / lb;
            }
          }
        }
      if(not(anchor_diameter_local > 0.f))
        return;
      float const dl{anchor_diameter_local};
      float const side{s.compass_anchor_scale * dl * 512.f / 400.f};  // the sphere is 400 of the square's 512
      bool const above{s.compass_sphere_text_above != 0};
      float const qw{side * (above ? above_w : below_w) / 512.f}, qh{side * (above ? above_h : below_h) / 512.f};
      // a point dx to the right and dy up of (x, y), in the mesh's local plane
      auto const step = [&](float x, float y, float dx, float dy, float z, float (&out)[3])
        {
        out[0] = x + dx * anchor_right[0] + dy * anchor_up[0];
        out[1] = y + dx * anchor_right[1] + dy * anchor_up[1];
        out[2] = z;
        };
      for(std::uint32_t k{}; k != 2u; ++k)
        {
        if(not entry->holds[k] or anchor_drawn_frame[k] == frame)
          continue;
        float const dx{k == 0u ? s.compass_anchor_a_dx : s.compass_anchor_c_dx}, dy{k == 0u ? s.compass_anchor_a_dy : s.compass_anchor_c_dy};
        auto const at{local_of(entry->map[k], pt[k][0], pt[k][1])};
        if(not at)
          continue;
        // the sphere's centre; the quad's top and bottom relative to it (the band over the square, or under it)
        float const top{above ? -side / 2.f + qh : side / 2.f}, bottom{top - qh};
        float const cx{dx * dl}, cy{dy * dl};
        float corners[4][3];
        step(at->x, at->y, cx - qw / 2.f, cy + top, at->z, corners[0]);
        step(at->x, at->y, cx + qw / 2.f, cy + top, at->z, corners[1]);
        step(at->x, at->y, cx + qw / 2.f, cy + bottom, at->z, corners[2]);
        step(at->x, at->y, cx - qw / 2.f, cy + bottom, at->z, corners[3]);
        if(draw_sphere_quad(ctx, k, corners, start_instance))
          {
          anchor_drawn_frame[k] = frame;
          if(not told_anchor[k])
            {
            told_anchor[k] = true;
            log_line("compass: the %s sphere first drawn by its anchor", k == 0u ? "left" : "right");
            }
          // the sphere's place, logged whenever it moves by more than 1% of its diameter (another ship, another cockpit),
          // so a tester's log tells where the spheres were and how large
          float const place[3]{at->x + cx * anchor_right[0] + cy * anchor_up[0], at->y + cx * anchor_right[1] + cy * anchor_up[1], dl};
          float const tol{0.01f * dl};
          if(std::fabs(place[0] - told_place[k][0]) > tol or std::fabs(place[1] - told_place[k][1]) > tol
             or std::fabs(place[2] - told_place[k][2]) > tol)
            {
            std::memcpy(told_place[k], place, sizeof place);
            log_line("compass: the %s sphere's centre at local (%.4f, %.4f, %.4f), compass diameter %.4f, right (%.3f, %.3f) up (%.3f, %.3f), "
                     "draw of %u indices",
                     k == 0u ? "left" : "right", static_cast<double>(place[0]), static_cast<double>(place[1]), static_cast<double>(at->z),
                     static_cast<double>(dl), static_cast<double>(anchor_right[0]), static_cast<double>(anchor_right[1]),
                     static_cast<double>(anchor_up[0]), static_cast<double>(anchor_up[1]), entry->index_count);
            }
          }
        }
      }
    }  // namespace

  auto panel_compass_after(ID3D11DeviceContext * ctx, std::uint64_t frame, std::uint32_t index_count, std::uint32_t start_index,
                           std::int32_t base_vertex, std::uint32_t start_instance) noexcept -> void
    {
    settings_t const & s{settings()};
    if(s.compass == 0 or not s.compass_spheres or not cs.spheres_drawn or not r.list_vs)
      return;
    if(s.compass_anchor)
      {
      anchored_after(ctx, frame, index_count, start_index, base_vertex, start_instance);
      return;
      }
    if(index_count != 12u)
      return;
    D3D11_TEXTURE2D_DESC d{};
    ID3D11Texture2D * const tex{surface_sampled(ctx, 2048u, 1280u, d)};
    if(not tex)
      return;
    tex->Release();
    if(frame != cs.quad_frame)
      {
      cs.quad_frame = frame;
      cs.quad_index = 0;
      }
    std::uint32_t const k{cs.quad_index++};
    if(k > 1u)
      return;
    // the sphere's square where the settings put it in the panel's plane; the lines' band under it or over it
    float const h{s.compass_sphere_height}, w{h * s.compass_sphere_aspect};
    float const cx{k == 0u ? s.compass_sphere_a_x : s.compass_sphere_c_x}, cy{k == 0u ? s.compass_sphere_a_y : s.compass_sphere_c_y};
    bool const above{s.compass_sphere_text_above != 0};
    float const used_w{above ? above_w : below_w}, used_h{above ? above_h : below_h};
    float const qw{w * used_w / 512.f}, qh{h * used_h / 512.f};
    float const top{above ? cy - h / 2.f + qh : cy + h / 2.f}, bottom{top - qh};
    float const corners[4][3]{{cx - qw / 2.f, top, 0.f}, {cx + qw / 2.f, top, 0.f}, {cx + qw / 2.f, bottom, 0.f}, {cx - qw / 2.f, bottom, 0.f}};
    if(not draw_sphere_quad(ctx, k, corners, start_instance))
      return;
    if(not cs.told_quads and k == 1u)
      {
      cs.told_quads = true;
      log_line("compass: spheres first drawn as quads on the two 2048x1280 panels (instances from %u)", start_instance);
      }
    }

  // ---- the pad's text (PoC): samples of the tower's box for learning its digits ----
  namespace
    {
    struct pad_text_state_t
      {
      ID3D11Device * device{};
      ID3D11Texture2D * copy{};
      DXGI_FORMAT copy_format{};
      bool copied{};
      std::uint64_t copied_frame{};
      bool shows{};  ///< the box showed at the last reading
      std::uint32_t samples{};
      std::vector<std::uint8_t> kept;  ///< the alpha of the last sample written (pad_box_width x pad_box_height)
      ULONGLONG written_ms{};          ///< when it was written: one sample a second at most (the text fades in)
      ULONGLONG burst_until_ms{};      ///< a sample a second until then: a countdown's last digit runs through 0..9
      pad_text_vote_t vote;            ///< the notice's pad read twice alike (pad_text_read.h)
      ULONGLONG shown_ms{};            ///< when the box showed this time
      bool taken_here{};               ///< a pad was taken from the notice since the box showed
      bool unsure_told{};              ///< an unsure notice told (and kept) since the box showed
      std::uint32_t given_pad{};       ///< the pad last taken from the notice and its grant (unix ms)
      std::int64_t given_ms{};
      ULONGLONG compare_from_ms{};     ///< edworld_eht: since then the taken pad waits for the data source's, 0 = none
      };

    pad_text_state_t pts;
    pad_text_bitmap_t pad_text_bits;  ///< the box's text pixels (render thread only)
    constexpr std::uint64_t pad_text_every{30};  ///< frames between copies: about two a second
    constexpr ULONGLONG pad_text_compare_ms{15000};
    constexpr ULONGLONG pad_unsure_after_ms{3000};  ///< a notice still unsure this long after the box showed is told  ///< edworld_eht: how long the taken pad waits for the data source's

    ///\brief the notice read from the box's copy: a pad read twice alike is given (edworld alone: as the data source's;
    /// edworld_eht: compared with the data source's, one log line a grant). A new grant: pad_text_new_grant
    auto read_pad_text(std::uint8_t const * pixels, std::uint32_t pitch, bool shows) -> pad_text_read_t
      {
      ULONGLONG const now{GetTickCount64()};
      if constexpr(with_eht)
        if(pts.compare_from_ms != 0u)
          {
          std::uint32_t const source{flight().docking_pad};
          if(source != 0u)
            {
            log_line("pad text: read pad %u, the data source's %u - %s", pts.given_pad, source,
                     source == pts.given_pad ? "agrees" : "DEVIATES");
            pts.compare_from_ms = 0u;
            }
          else if(now - pts.compare_from_ms >= pad_text_compare_ms)
            {
            log_line("pad text: read pad %u, the data source gave none in %llu s", pts.given_pad,
                     static_cast<unsigned long long>(pad_text_compare_ms / 1000u));
            pts.compare_from_ms = 0u;
            }
          }
      if(not shows)
        {
        pts.vote = pad_text_vote_t{};
        return {};
        }
      pad_text_bitmap(pixels, pitch, pad_text_bits);
      pad_text_read_t const notice{pad_text_read(pad_text_bits)};
      std::uint32_t const taken{pts.vote.add(notice)};
      if(taken == 0u)
        return notice;
      pts.taken_here = true;
      std::int64_t const granted_ms{(unix_now() - (600 - static_cast<std::int64_t>(notice.seconds_left))) * 1000};
      if(not pad_text_new_grant(taken, granted_ms, pts.given_pad, pts.given_ms))
        return notice;
      pts.given_pad = taken;
      pts.given_ms = granted_ms;
      log_line("pad text: pad %u read from the notice, %u:%02u left", taken, notice.seconds_left / 60u, notice.seconds_left % 60u);
      pad_text_grant(taken, granted_ms);
      if constexpr(with_eht)
        pts.compare_from_ms = now;
      return notice;
      }

    ///\brief the box's area as read, into edworld_padtext (a sample of the digits), and its alpha kept for pad_box_changed
    auto write_pad_sample(D3D11_MAPPED_SUBRESOURCE const & m, std::uint32_t source_pad, bool burst) -> void
      {
      pts.kept.resize(static_cast<std::size_t>(pad_box_width) * pad_box_height);
      for(std::int32_t y{}; y != pad_box_height; ++y)
        for(std::int32_t x{}; x != pad_box_width; ++x)
          pts.kept[static_cast<std::size_t>(y) * pad_box_width + x]
            = static_cast<std::uint8_t const *>(m.pData)[static_cast<std::size_t>(y) * m.RowPitch + static_cast<std::size_t>(x) * 4u + 3u];
      pts.written_ms = GetTickCount64();
      if(not burst)
        pts.burst_until_ms = pts.written_ms + pad_burst_ms;
      CreateDirectoryW((settings().dir + L"\\edworld_padtext").c_str(), nullptr);
      wchar_t name[MAX_PATH];
      std::swprintf(name, MAX_PATH, L"%ls\\edworld_padtext\\pad_%lld_src%u_%dx%d_pitch%u.raw", settings().dir.c_str(),
                    static_cast<long long>(unix_now()), source_pad, pad_box_width, pad_box_height, m.RowPitch);
      if(std::FILE * f{_wfopen(name, L"wb")})
        {
        std::fwrite(m.pData, 1, static_cast<std::size_t>(m.RowPitch) * pad_box_height, f);
        std::fclose(f);
        ++pts.samples;
        log_line("pad text: %s; sample %u written to %S (the data source's pad %u)", burst ? "a second on" : "a text in the box",
                 pts.samples, name, source_pad);
        }
      else
        log_line("pad text: a text in the box; its sample could not be written to %S", name);
      }
    }  // namespace

  auto panel_pad_text(ID3D11DeviceContext * ctx, ID3D11Device * device, std::uint64_t frame) noexcept -> void
    {
    if(settings().pad_text_samples == 0 and settings().pad_text_read == 0)
      return;
    if(frame == pts.copied_frame or (not pts.copied and frame % pad_text_every != 0))
      return;
    D3D11_TEXTURE2D_DESC d{};
    ID3D11Texture2D * const tex{surface_sampled(ctx, 2200u, 1800u, d)};
    if(not tex)
      return;
    if(pts.device != device or (pts.copy and pts.copy_format != d.Format))
      {
      release(pts.copy);
      pts = pad_text_state_t{};
      pts.device = device;
      }
    if(not pts.copy)
      {
      D3D11_TEXTURE2D_DESC c{};
      c.Width = static_cast<UINT>(pad_box_width);
      c.Height = static_cast<UINT>(pad_box_height);
      c.MipLevels = c.ArraySize = 1;
      c.Format = d.Format;
      c.SampleDesc.Count = 1;
      c.Usage = D3D11_USAGE_STAGING;
      c.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if(FAILED(device->CreateTexture2D(&c, nullptr, &pts.copy)) or not pts.copy)
        {
        tex->Release();
        return;
        }
      pts.copy_format = d.Format;
      }
    if(pts.copied)
      {
      D3D11_MAPPED_SUBRESOURCE m{};
      HRESULT const hr{ctx->Map(pts.copy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)};
      if(hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
        tex->Release();
        return;
        }
      pts.copied = false;
      if(SUCCEEDED(hr) and m.pData)
        {
        auto const * const pixels{static_cast<std::uint8_t const *>(m.pData)};
        bool const shows{pad_box_present(pixels, m.RowPitch, pad_box_width, pad_box_height)};
        ULONGLONG const now{GetTickCount64()};
        if(shows and not pts.shows)
          {
          pts.shown_ms = now;
          pts.taken_here = pts.unsure_told = false;
          }
        pad_text_read_t const notice{settings().pad_text_read ? read_pad_text(pixels, m.RowPitch, shows) : pad_text_read_t{}};
        // a tower's notice whose number stays unsure while it fades in and after (a case the templates have not met):
        // no pad from it; told once a box, and its picture kept for tools/pad_text_digits.py
        if(shows and notice.notice and not notice.sure and not pts.taken_here and not pts.unsure_told
           and now - pts.shown_ms >= pad_unsure_after_ms)
          {
          pts.unsure_told = true;
          log_line("pad text: a notice of the tower's, its number unsure (read %u, the least margin %u %%): no pad from it",
                   notice.pad, notice.margin);
          if(settings().pad_text_unsure_sample and not settings().pad_text_samples)
            write_pad_sample(m, flight().docking_pad, false);
          }
        bool const changed{shows and pts.shows and now - pts.written_ms >= 1000u
                           and pad_box_changed(pixels, m.RowPitch, pad_box_width, pad_box_height, pts.kept.empty() ? nullptr : pts.kept.data())};
        if(settings().pad_text_samples)
          {
          if(shows and (not pts.shows or changed))
            write_pad_sample(m, flight().docking_pad, false);
          else if(shows and pad_burst_due(now, pts.written_ms, pts.burst_until_ms))
            write_pad_sample(m, flight().docking_pad, true);
          }
        if(not shows and pts.shows)
          log_line("pad text: the box is gone");
        pts.shows = shows;
        ctx->Unmap(pts.copy, 0);
        }
      tex->Release();
      return;
      }
    D3D11_BOX const area{static_cast<UINT>(pad_box_x), static_cast<UINT>(pad_box_y), 0, static_cast<UINT>(pad_box_x + pad_box_width),
                         static_cast<UINT>(pad_box_y + pad_box_height), 1};
    ctx->CopySubresourceRegion(pts.copy, 0, 0, 0, 0, tex, 0, &area);
    pts.copied = true;
    pts.copied_frame = frame;
    tex->Release();
    }

  auto sphere_note_new_model(std::uint32_t count) noexcept -> void
    {
    model_note = sphere_note_t{{}, 2, GetTickCount64() + model_note_ms};
    std::snprintf(model_note.text, sizeof model_note.text, "NEW MODEL %u", count);
    }
  }  // namespace edworld
