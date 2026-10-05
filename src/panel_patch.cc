// edworld — the jump panel patch. While a hyperspace jump charges, just before the game composites the
// interface surface that carries the charge panel into the cockpit, the panel's wrong superpower emblem is
// painted over on that surface with the panel's own black and the right emblem. Drawn onto the surface, the
// patch then rides the panel through the cockpit like the rest of its text: camera lag, head look, the
// hologram's own effects. The patch shows only where the game itself drew on the surface this frame (its
// alpha under each pixel, copied just before): when the game hides the panel while the ship still aligns, Status.json
// still says charging, and the patch must go with the panel.
//
// Under the panel, on the same surface, the destination's factions are listed (from EHT's target in edworld_eht when
// it has them, else from EDSM, said so in the list's last row), so they too ride the panel. The game draws nothing
// there, so the list shows while the game draws the panel at one point of it (the gate, the patch's centre).
//
// The only place edworld changes what the game draws. The patch is a Dear ImGui draw list rendered by ImGui's
// D3D11 backend (its own context, no input, no files). Everything the game had bound is read back first and put
// back after, and our own draws go through our own objects.
#include "panel_patch.h"

#include "emblems.h"
#include "game_state.h"
#include "list_font.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
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
      ID3D11ShaderResourceView * emblem[3]{};
      std::uint32_t emblem_width[3]{};
      std::uint32_t emblem_height[3]{};
      // a copy of the panel's surface, only the patch's box written: what the game drew there this frame
      ID3D11Texture2D * mask{};
      ID3D11ShaderResourceView * mask_srv{};
      std::uint32_t mask_width{}, mask_height{};
      ///\brief the pixel shader's b0: the patch's box (alpha from under the pixel) and the gate (alpha elsewhere)
      ID3D11Buffer * gate{};
      ///\brief the list's font; none when it could not be made (no list then, the emblem still)
      ImFont * font{};
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
      }
      bool ok{ImGui_ImplDX11_Init(device, ctx)};
      ok = ok and ImGui_ImplDX11_CreateDeviceObjects();
      ok = ok and make_emblem(emblems::federation_mask, emblems::federation_width, emblems::federation_height, 0);
      ok = ok and make_emblem(emblems::empire_mask, emblems::empire_width, emblems::empire_height, 1);
      ok = ok and make_emblem(emblems::alliance_mask, emblems::alliance_width, emblems::alliance_height, 2);
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
        case allegiance_e::federation: return 0xd9534fu;
        case allegiance_e::empire:     return 0x4a90d9u;
        case allegiance_e::alliance:   return 0x3cb371u;
        default:                       return 0xffffffu;
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
    auto draw_list(ImDrawList * dl, settings_t const & s, faction_list_t const & list, bool test) -> void
      {
      if(not r.font)
        return;
      float const size{r.font->FontSize};
      float const line{std::round(size * 1.15f)};
      float const pad{4.f};
      float const left{s.list_x - s.list_width / 2.f}, right{s.list_x + s.list_width / 2.f};
      bool const placeholder{test and list.count == 0};
      std::uint32_t const source_rows{list.source == list_source_e::edsm or placeholder ? 1u : 0u};
      std::uint32_t const rows{std::max(s.list_rows, source_rows + 1u)};
      std::uint32_t const shown{placeholder ? rows - source_rows : std::min(list.count, rows - source_rows)};
      if(shown == 0)
        return;
      float const top{s.list_top};
      float const bottom{top + static_cast<float>(shown + source_rows) * line + 2.f * pad};
      dl->AddRectFilled(ImVec2{left, top}, ImVec2{right, bottom}, im_colour(s.patch_ground));
      if(test)
        dl->AddRect(ImVec2{left, top}, ImVec2{right, bottom}, im_colour(0xff00ffu), 0.f, 0, 2.f);

      float const cw{r.font->CalcTextSizeA(size, FLT_MAX, 0.f, "0").x};
      float const x_star{left + pad + cw};
      float const x_name{left + pad + 2.f * cw};
      float const x_influence{x_name + 39.f * cw};  // right edge: a name of 32, a space, "100.0%"
      float const x_states{x_influence + 2.f * cw};
      ImU32 const grey{im_colour(0x9aa0a6u)};
      char buf[128];
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
          dl->AddRectFilled(ImVec2{cx - half, mid - 1.f}, ImVec2{cx + half, mid + 1.f}, grey);
        if(row.controlling)
          dl->AddText(r.font, size, ImVec2{x_star, y}, im_colour(0xe0e0e0u), "*");
        std::uint32_t const tint{row.allegiance >= 1u and row.allegiance <= 3u ? colour_of(static_cast<allegiance_e>(row.allegiance)) : 0xc8c8c8u};
        ImVec4 const name_clip{x_name, y, x_influence - 7.f * cw, y + line};
        dl->AddText(r.font, size, ImVec2{x_name, y}, im_colour(tint), row.name, nullptr, 0.f, &name_clip);
        std::snprintf(buf, sizeof buf, "%.1f%%", static_cast<double>(row.influence) * 100.0);
        float const w{r.font->CalcTextSizeA(size, FLT_MAX, 0.f, buf).x};
        dl->AddText(r.font, size, ImVec2{x_influence - w, y}, im_colour(0xe0e0e0u), buf);
        if(row.states[0] != '\0')
          {
          ImVec4 const states_clip{x_states, y, right - pad, y + line};
          dl->AddText(r.font, size, ImVec2{x_states, y}, grey, row.states, nullptr, 0.f, &states_clip);
          }
        }
      if(source_rows != 0)
        {
        float const y{top + pad + static_cast<float>(shown) * line};
        if(placeholder)
          std::snprintf(buf, sizeof buf, "list test: %u rows at most, text %.0f px", rows, static_cast<double>(size));
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
        dl->AddText(r.font, size, ImVec2{x_name, y}, grey, buf);
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
    if(not g.charging)
      return;
    // the list only for the destination it was made for, and only when it has something to say
    faction_list_t const list{s.list != 0 ? destination_factions() : faction_list_t{}};
    bool const listing{list.system == g.destination and list.source != list_source_e::none and list.count != 0};
    if(not test and emblem < 0 and not listing)
      return;

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
    float const left{s.patch_x - s.patch_width / 2.f}, top{s.patch_y - s.patch_height / 2.f};
    UINT const x0{left > 0.f ? static_cast<UINT>(left) : 0u}, y0{top > 0.f ? static_cast<UINT>(top) : 0u};
    UINT const x1{static_cast<UINT>(s.patch_x + s.patch_width / 2.f) + 1u}, y1{static_cast<UINT>(s.patch_y + s.patch_height / 2.f) + 1u};
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
      {static_cast<std::int32_t>(s.patch_x), static_cast<std::int32_t>(s.patch_y), 0, 0}
    };
    std::memcpy(m.pData, &gate, sizeof gate);
    ctx->Unmap(r.gate, 0);
    }
    tex->Release();
    patched_frame = frame;

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
      box(dl, s.patch_x, s.patch_y, s.patch_width, s.patch_height, im_colour(0xff00ffu));  // magenta: where the patch goes
      box(dl, s.patch_x, s.patch_y, s.patch_width - 8.f, s.patch_height - 8.f, im_colour(s.patch_ground));
      }
    else if(emblem >= 0)
      box(dl, s.patch_x, s.patch_y, s.patch_width, s.patch_height, im_colour(s.patch_ground));
    if(emblem >= 0)
      {
      float const eh{s.patch_emblem_height};
      float const ew{eh * static_cast<float>(r.emblem_width[emblem]) / static_cast<float>(r.emblem_height[emblem])};
      dl->AddImage(
        reinterpret_cast<ImTextureID>(r.emblem[emblem]),
        ImVec2{s.patch_x - ew / 2.f, s.patch_y - eh / 2.f},
        ImVec2{s.patch_x + ew / 2.f, s.patch_y + eh / 2.f},
        ImVec2{0.f, 0.f},
        ImVec2{1.f, 1.f},
        im_colour(colour_of(allegiance))
      );
      }
    if(s.list != 0 and (listing or test))
      draw_list(dl, s, list, test);
    ImGui::Render();

    // the backend puts back what it binds itself; the targets and the surface's own slots are ours to put back
    backup_t b;
    save(ctx, b);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->PSSetShaderResources(1, 1, &mask);
    ctx->PSSetConstantBuffers(0, 1, &r.gate);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    restore(ctx, b);
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
  }  // namespace edworld
