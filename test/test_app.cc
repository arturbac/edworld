// edworld smoke test without the game: a D3D11 program that draws one "panel" (a watched VS with a known
// cb0 rows 4..7) and one other draw, through edworld's d3d11.dll, then checks what edworld published.
// Exit code 0 = pass. Run from a directory holding test_app.exe and edworld's d3d11.dll (native override).
// Built once per variant: EDWORLD_EHT = the edworld_eht build (EHT's target read, panels published).
#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/edworld_share.h"
#include "../src/panel_math.h"
#include "found_vs.h"
#include "other_vs.h"
#include "panel_ps.h"
#include "panel_vs.h"

namespace
  {
  int failures{};

  auto check(bool ok, char const * what) -> void
    {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if(not ok)
      ++failures;
    }

  auto near_eq(float a, float b) -> bool { return std::fabs(a - b) < 1e-5f; }

#if defined(EDWORLD_EHT)
  constexpr bool with_eht{true};
  constexpr wchar_t plugin_name[]{L"edworld_eht"};
#else
  constexpr bool with_eht{false};
  constexpr wchar_t plugin_name[]{L"edworld"};
#endif
  }  // namespace

int main()
  {
  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir{exe};
  dir.resize(dir.find_last_of(L"\\/"));
  std::wstring const shm_dir{dir + L"\\shm"};
  std::wstring const share_path{shm_dir + L"\\panels"};
  // edworld's own files: beside it, or under EDLOADER_DIR when the chain test runs it through edloader
  std::wstring config_dir{dir};
  std::wstring output_dir{dir};
  if(wchar_t root[MAX_PATH]{}; GetEnvironmentVariableW(L"EDLOADER_DIR", root, MAX_PATH))
    {
    config_dir = std::wstring{root} + L"\\config";
    output_dir = std::wstring{root} + L"\\logs";
    CreateDirectoryW(root, nullptr);
    CreateDirectoryW(config_dir.c_str(), nullptr);
    CreateDirectoryW(output_dir.c_str(), nullptr);
    }

  // The settings must be in place before the first d3d11 export call.
  std::uint64_t const watched{edworld::fnv1a64(g_panel_vs, sizeof g_panel_vs)};
  {
  std::FILE * ini{_wfopen((config_dir + L"\\" + plugin_name + L".ini").c_str(), L"wb")};
  wchar_t next[MAX_PATH]{};
  GetEnvironmentVariableW(L"EDWORLD_TEST_NEXT", next, MAX_PATH);
  if(next[0])
    std::fprintf(ini, "next = %ls\n", next);
  std::fprintf(ini, "patch = 2\npatch_surface = 512x128\npatch_x = 256\npatch_y = 64\npatch_width = 100\npatch_height = 50\npatch_emblem_height = 40\nedsm = %d\n", GetEnvironmentVariableW(L"EDWORLD_TEST_EDSM", nullptr, 0) ? 1 : 0);
  std::fprintf(ini, "watch_vs = %016llX, 1989E6D3B405FDE0\nlog_interval_ms = 1\nlog_all_vs = 1\n",
               static_cast<unsigned long long>(watched));
  if constexpr(with_eht)
    std::fprintf(ini, "shm_dir = %ls\n", shm_dir.c_str());
  else
    std::fprintf(ini, "patch_force = 2\n");  // no data source and no EDSM in the test: the Empire emblem by hand
  std::fclose(ini);
  }
  DeleteFileW(share_path.c_str());
  std::wstring const log_path{output_dir + L"\\" + plugin_name + L".log"};
  // the data source's target, as EHT writes it: the destination is known, an Empire system
  if constexpr(with_eht)
  {
  CreateDirectoryW(shm_dir.c_str(), nullptr);
  edworld::target_t t{};
  t.magic = edworld::target_magic;
  t.version = edworld::target_version;
  t.size = sizeof t;
  t.sequence = 2;
  t.system_address = 3932277478106ull;
  t.known = 1;
  t.allegiance = 2;
  std::strcpy(t.name, "Shinrarta Dezhra");
  std::FILE * tf{_wfopen((shm_dir + L"\\target").c_str(), L"wb")};
  std::fwrite(&t, sizeof t, 1, tf);
  std::fclose(tf);
  }
  std::wstring status_file;
  {
  wchar_t profile[MAX_PATH]{};
  GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  std::wstring status_dir{std::wstring{profile} + L"\\Saved Games"};
  CreateDirectoryW(status_dir.c_str(), nullptr);
  status_dir += L"\\Frontier Developments";
  CreateDirectoryW(status_dir.c_str(), nullptr);
  status_dir += L"\\Elite Dangerous";
  CreateDirectoryW(status_dir.c_str(), nullptr);
  status_file = status_dir + L"\\Status.json";
  std::FILE * status{_wfopen(status_file.c_str(), L"wb")};
  std::fprintf(status, "{ \"timestamp\":\"2026-10-04T12:00:00Z\", \"event\":\"Status\", \"Flags\":16842760, \"Flags2\":524288, "
                       "\"Destination\":{ \"System\":3932277478106, \"Body\":0, \"Name\":\"Shinrarta Dezhra\" } }\r\n");  // the game's own line end
  std::fclose(status);
  }
  Sleep(300);  // the state thread polls every 100 ms

  ID3D11Device * dev{};
  ID3D11DeviceContext * ctx{};
  D3D_FEATURE_LEVEL level{};
  HRESULT hr{D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx)};
  check(SUCCEEDED(hr), "D3D11CreateDevice through edworld");
  if(FAILED(hr))
    return 2;

  ID3D11VertexShader * panel_vs{};
  ID3D11VertexShader * other_vs{};
  ID3D11PixelShader * ps{};
  dev->CreateVertexShader(g_panel_vs, sizeof g_panel_vs, nullptr, &panel_vs);
  dev->CreateVertexShader(g_other_vs, sizeof g_other_vs, nullptr, &other_vs);
  dev->CreatePixelShader(g_panel_ps, sizeof g_panel_ps, nullptr, &ps);
  // as the game's instanced path: VB0 = instance entries (8 bytes, first u32 = record index), VB1 = vertices
  D3D11_INPUT_ELEMENT_DESC const layout_desc[]{
    {"INSTANCE", 0, DXGI_FORMAT_R32G32_UINT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  ID3D11InputLayout * layout{};
  hr = dev->CreateInputLayout(layout_desc, 3, g_panel_vs, sizeof g_panel_vs, &layout);
  check(SUCCEEDED(hr), "input layout");

  // instance entries: the panel draws with start_instance 3, whose entry names record 2
  std::uint32_t const entries[]{0, 0, 1, 0, 9, 0, 2, 0, 7, 0};
  D3D11_BUFFER_DESC ebd{sizeof entries, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA einit{entries, 0, 0};
  ID3D11Buffer * instances{};
  dev->CreateBuffer(&ebd, &einit, &instances);

  // t33: 4 records of 336 bytes; record 2 = scale 1.5, position (1, 2, 3)
  std::uint8_t records[4 * 336]{};
  float const scale{1.5f}, position[3]{1.f, 2.f, 3.f};
  std::uint32_t const quat_xy{0x8000u | (0x8000u << 16)}, quat_zw{0x8000u | (0xFFFFu << 16)};
  std::memcpy(records + 2 * 336 + 4, &scale, 4);
  std::memcpy(records + 2 * 336 + 8, &quat_xy, 4);
  std::memcpy(records + 2 * 336 + 12, &quat_zw, 4);
  std::memcpy(records + 2 * 336 + 16, position, 12);
  D3D11_BUFFER_DESC pbd{sizeof records, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 336};
  D3D11_SUBRESOURCE_DATA pinit{records, 0, 0};
  ID3D11Buffer * pool{};
  dev->CreateBuffer(&pbd, &pinit, &pool);
  ID3D11ShaderResourceView * pool_srv{};
  dev->CreateShaderResourceView(pool, nullptr, &pool_srv);

  // cb1: 276 rows, row 275 = the world-rebase origin (0.5, 0.5, 0.5)
  float cb1_rows[276][4]{};
  cb1_rows[275][0] = cb1_rows[275][1] = cb1_rows[275][2] = 0.5f;
  D3D11_BUFFER_DESC c1d{sizeof cb1_rows, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA c1init{cb1_rows, 0, 0};
  ID3D11Buffer * cb1{};
  dev->CreateBuffer(&c1d, &c1init, &cb1);

  // a dump of the panels' surfaces asked for before the first frame
  std::FILE * trigger{_wfopen((output_dir + L"\\edworld_dump").c_str(), L"wb")};
  if(trigger)
    std::fclose(trigger);

  float const quad[]{-1, -1, 0, 0, 1, 1, -1, 0, 1, 1, -1, 1, 0, 0, 0, 1, 1, 0, 1, 0};
  D3D11_BUFFER_DESC vbd{sizeof quad, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA vinit{quad, 0, 0};
  ID3D11Buffer * vb{};
  dev->CreateBuffer(&vbd, &vinit, &vb);
  unsigned short const indices[]{0, 1, 2, 2, 1, 3};
  D3D11_BUFFER_DESC ibd{sizeof indices, D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA iinit{indices, 0, 0};
  ID3D11Buffer * ib{};
  dev->CreateBuffer(&ibd, &iinit, &ib);

  // cb0 like the game's: dynamic, rewritten before each draw.
  D3D11_BUFFER_DESC cbd{192, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  ID3D11Buffer * cb{};
  dev->CreateBuffer(&cbd, nullptr, &cb);

  // The panel's interface surface: 512x128, bound at PS t2.
  D3D11_TEXTURE2D_DESC td{};
  td.Width = 512;
  td.Height = 128;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D * surface{};
  dev->CreateTexture2D(&td, nullptr, &surface);
  D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
  svd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  svd.Texture2D.MipLevels = 1;
  ID3D11ShaderResourceView * srv{};
  dev->CreateShaderResourceView(surface, &svd, &srv);
  // the game's panel as the patch sees it: opaque (alpha 255) around the patch's box, nothing elsewhere
  std::vector<std::uint32_t> drawn(512 * 128, 0u);
  for(int y{34}; y != 95; ++y)
    for(int x{200}; x != 313; ++x)
      drawn[static_cast<std::size_t>(y) * 512 + x] = 0xff000000u;
  ctx->UpdateSubresource(surface, 0, nullptr, drawn.data(), 512 * 4, 0);

  D3D11_TEXTURE2D_DESC rd{};
  rd.Width = 256;
  rd.Height = 256;
  rd.MipLevels = 1;
  rd.ArraySize = 1;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rd.SampleDesc.Count = 1;
  rd.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D * target{};
  dev->CreateTexture2D(&rd, nullptr, &target);
  ID3D11RenderTargetView * rtv{};
  dev->CreateRenderTargetView(target, nullptr, &rtv);
  D3D11_VIEWPORT const vp{0, 0, 256, 256, 0, 1};

  // rows 4..7: some 3x3, and the w column the anchor test checks: clip (0.5, -0.25, 0.3, 2) -> ndc (0.25, -0.125)
  float rows[12][4]{};
  float const r4[4]{0.8f, 0.0f, 0.1f, 0.5f}, r5[4]{0.0f, 0.9f, 0.0f, -0.25f}, r6[4]{0.0f, 0.0f, 0.0f, 0.3f},
    r7[4]{0.0f, 0.0f, 0.2f, 2.0f};
  std::memcpy(rows[4], r4, 16);
  std::memcpy(rows[5], r5, 16);
  std::memcpy(rows[6], r6, 16);
  std::memcpy(rows[7], r7, 16);

  UINT const stride{20}, offset{0};
  for(int f{}; f != 30; ++f)
    {
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(layout);
    UINT const entry_stride{8};
    ctx->IASetVertexBuffers(0, 1, &instances, &entry_stride, &offset);
    ctx->IASetVertexBuffers(1, 1, &vb, &stride, &offset);
    ctx->VSSetShaderResources(33, 1, &pool_srv);
    ctx->VSSetConstantBuffers(1, 1, &cb1);
    ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(2, 1, &srv);

    D3D11_MAPPED_SUBRESOURCE m{};
    ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    std::memcpy(m.pData, rows, sizeof rows);
    ctx->Unmap(cb, 0);
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->VSSetShader(panel_vs, nullptr, 0);
    ctx->DrawIndexedInstanced(6, 1, 0, 0, 3);

    // The same buffer rewritten for an unwatched draw: the panel's copy must still hold the panel's rows.
    float junk[12][4];
    for(auto & r: junk)
      for(float & v: r)
        v = 99.0f;
    ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    std::memcpy(m.pData, junk, sizeof junk);
    ctx->Unmap(cb, 0);
    ctx->VSSetShader(other_vs, nullptr, 0);
    ctx->DrawIndexed(6, 0, 0);
    ctx->Flush();
    Sleep(15);  // longer than frame_gap_us: the next panel draw starts a new frame
    }

  {
  ID3D11RenderTargetView * bound_rtv{};
  ctx->OMGetRenderTargets(1, &bound_rtv, nullptr);
  ID3D11ShaderResourceView * bound_srv{};
  ctx->PSGetShaderResources(2, 1, &bound_srv);
  ID3D11VertexShader * bound_vs{};
  ctx->VSGetShader(&bound_vs, nullptr, nullptr);
  check(bound_rtv == rtv and bound_srv == srv and bound_vs == other_vs, "the game's targets, surface and shader are back after the patch");
  if(bound_rtv) bound_rtv->Release();
  if(bound_srv) bound_srv->Release();
  if(bound_vs) bound_vs->Release();

  D3D11_TEXTURE2D_DESC sd{td};
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D * readback{};
  dev->CreateTexture2D(&sd, nullptr, &readback);
  ctx->CopyResource(readback, surface);
  D3D11_MAPPED_SUBRESOURCE m{};
  ctx->Map(readback, 0, D3D11_MAP_READ, 0, &m);
  auto const pixel{[&](int x, int y) -> std::uint32_t
    {
    std::uint8_t const * p{static_cast<std::uint8_t const *>(m.pData) + y * m.RowPitch + x * 4};
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
    }};
  std::printf("surface pixels: centre %06x, frame %06x, outside %06x\n", pixel(256, 64), pixel(208, 64), pixel(10, 10));
  std::uint32_t emblem_pixels{};
  std::uint32_t upper_half{};
  for(int y{44}; y != 84; ++y)
    for(int x{226}; x != 286; ++x)
      if(std::uint32_t const c{pixel(x, y)}; (c & 0xffu) > 0x80u and ((c >> 16) & 0xffu) < 0x80u)
        {
        ++emblem_pixels;
        if(y < 64)
          ++upper_half;
        }
  std::printf("empire-blue pixels in the emblem box: %u (upper half %u)\n", emblem_pixels, upper_half);
  check(emblem_pixels > 150, with_eht ? "Empire emblem from the data source's target, in its colour" : "Empire emblem (patch_force), in its colour");
  // the Empire's V is wide at the top and comes to a point at the bottom
  check(upper_half * 2 > emblem_pixels, "emblem upright, not flipped");
  check(pixel(256 - 45, 64) == 0x020304u, "patch ground drawn beside the emblem");
  check(pixel(208, 64) == 0xff00ffu, "test frame drawn at the patch's edge");
  check(pixel(10, 10) == 0u, "surface untouched outside the patch");
  ctx->Unmap(readback, 0);

  // StartJump: Flags bit 30 (FSD jump) set while Flags2 bit 19 still is; the panel is gone, nothing may be drawn.
  {
  std::FILE * status{_wfopen(status_file.c_str(), L"wb")};
  std::fprintf(status, "{ \"timestamp\":\"2026-10-04T12:00:10Z\", \"event\":\"Status\", \"Flags\":1090584584, \"Flags2\":524288, "
                       "\"Destination\":{ \"System\":3932277478106, \"Body\":0, \"Name\":\"Shinrarta Dezhra\" } }\r\n");
  std::fclose(status);
  }
  Sleep(300);
  std::vector<std::uint32_t> const zeros(512 * 128, 0u);
  ctx->UpdateSubresource(surface, 0, nullptr, zeros.data(), 512 * 4, 0);
  ctx->VSSetShader(panel_vs, nullptr, 0);
  ctx->DrawIndexedInstanced(6, 1, 0, 0, 3);
  ctx->Flush();
  ctx->CopyResource(readback, surface);
  ctx->Map(readback, 0, D3D11_MAP_READ, 0, &m);
  std::printf("in the jump: surface pixel beside the emblem %06x\n", pixel(256 - 45, 64));
  check(pixel(256 - 45, 64) == 0u and pixel(208, 64) == 0u, "no patch once the jump has started (Flags bit 30)");
  ctx->Unmap(readback, 0);

  // A charge with no panel draw for over a second: the probe logs the draws that sample a panel-sized surface.
  {
  std::FILE * status{_wfopen(status_file.c_str(), L"wb")};
  std::fprintf(status, "{ \"timestamp\":\"2026-10-04T12:00:20Z\", \"event\":\"Status\", \"Flags\":16842760, \"Flags2\":524288, "
                       "\"Destination\":{ \"System\":3932277478106, \"Body\":0, \"Name\":\"Shinrarta Dezhra\" } }\r\n");
  std::fclose(status);
  }
  Sleep(300);
  // The panel hidden while the ship still aligns (Status.json still says charging): nothing of the game's under
  // the patch, so nothing of the patch may show.
  ctx->UpdateSubresource(surface, 0, nullptr, zeros.data(), 512 * 4, 0);
  ctx->PSSetShaderResources(2, 1, &srv);
  ctx->VSSetShader(panel_vs, nullptr, 0);
  ctx->DrawIndexedInstanced(6, 1, 0, 0, 3);
  ctx->Flush();
  ctx->CopyResource(readback, surface);
  ctx->Map(readback, 0, D3D11_MAP_READ, 0, &m);
  check(pixel(256 - 45, 64) == 0u and pixel(208, 64) == 0u, "no patch where the game drew no panel (hidden while aligning)");
  ctx->Unmap(readback, 0);
  D3D11_TEXTURE2D_DESC bd{td};
  bd.Width = 3072;
  bd.Height = 660;
  ID3D11Texture2D * big{};
  dev->CreateTexture2D(&bd, nullptr, &big);
  ID3D11ShaderResourceView * big_srv{};
  dev->CreateShaderResourceView(big, &svd, &big_srv);
  Sleep(1500);  // the probe thread arms after a second without a panel draw, polling every 100 ms
  ctx->PSSetShaderResources(2, 1, &big_srv);
  ctx->VSSetShader(other_vs, nullptr, 0);
  ctx->DrawIndexed(6, 0, 0);
  Sleep(300);  // past the probe's window: the next draw closes it
  ctx->DrawIndexed(6, 0, 0);
  ctx->Flush();
  Sleep(200);
  std::string log_text;
  if(std::FILE * lf{_wfopen(log_path.c_str(), L"rb")})
    {
    char buf[4096];
    for(std::size_t got; (got = std::fread(buf, 1, sizeof buf, lf)) != 0;)
      log_text.append(buf, got);
    std::fclose(lf);
    }
  check(log_text.find("probe: a jump charges") != std::string::npos, "probe armed by a charge without panel draws");
  check(log_text.find("PS t2 3072x660") != std::string::npos, "probe names the draw sampling a panel-sized surface");
  check(log_text.find("probe: done, 2 draw(s) seen, 2 with a panel-sized surface") != std::string::npos, "probe closes after its window");
  check(log_text.find("draws a panel surface (PS t2 3072x660) and is not in watch_vs: watched from now on (first draws)")
          != std::string::npos, "an unlisted shader drawing a panel surface is found by its first draws");

  // A shader judged no panel by its first draws that later draws one at PS t1: the probe of the next blind charge finds it.
  ID3D11VertexShader * found_vs{};
  dev->CreateVertexShader(g_found_vs, sizeof g_found_vs, nullptr, &found_vs);
  ctx->PSSetShaderResources(2, 1, &srv);
  ctx->VSSetShader(found_vs, nullptr, 0);
  for(int i{}; i != 70; ++i)
    ctx->DrawIndexed(6, 0, 0);
  ctx->Flush();
  for(char const * flags2: {"0", "524288"})
    {
    std::FILE * status{_wfopen(status_file.c_str(), L"wb")};
    std::fprintf(status, "{ \"timestamp\":\"2026-10-04T12:00:30Z\", \"event\":\"Status\", \"Flags\":16842760, \"Flags2\":%s, "
                         "\"Destination\":{ \"System\":3932277478106, \"Body\":0, \"Name\":\"Shinrarta Dezhra\" } }\r\n", flags2);
    std::fclose(status);
    Sleep(400);
    }
  Sleep(1300);
  ID3D11ShaderResourceView * none{};
  ctx->PSSetShaderResources(2, 1, &none);
  ctx->PSSetShaderResources(1, 1, &big_srv);
  ctx->DrawIndexed(6, 0, 0);
  ctx->Flush();
  Sleep(200);
  log_text.clear();
  if(std::FILE * lf{_wfopen(log_path.c_str(), L"rb")})
    {
    char buf[4096];
    for(std::size_t got; (got = std::fread(buf, 1, sizeof buf, lf)) != 0;)
      log_text.append(buf, got);
    std::fclose(lf);
    }
  check(log_text.find("draws a panel surface (PS t1 3072x660) and is not in watch_vs: watched from now on (probe)")
          != std::string::npos, "a shader judged no panel is found at PS t1 by the probe of a blind charge");
  found_vs->Release();
  big_srv->Release();
  big->Release();
  }

  WIN32_FIND_DATAW found{};
  HANDLE const dumps{FindFirstFileW((output_dir + L"\\edworld_dumps\\*_512x128_f27_*.raw").c_str(), &found)};
  check(dumps != INVALID_HANDLE_VALUE, "the panel's surface dumped (512x128)");
  if(dumps != INVALID_HANDLE_VALUE)
    FindClose(dumps);
  check(GetFileAttributesW((output_dir + L"\\edworld_dump").c_str()) == INVALID_FILE_ATTRIBUTES, "dump trigger removed");
  if(HMODULE const fake{GetModuleHandleW(L"fake_next.dll")})
    {
    using calls_fn = LONG (*)();
    auto const calls{reinterpret_cast<calls_fn>(GetProcAddress(fake, "fake_next_calls"))};
    check(calls and calls() == 1, "chained proxy called once, its by-name call routed to the system copy");
    }
  if constexpr(not with_eht)
    {
    std::string log_text;
    if(std::FILE * lf{_wfopen(log_path.c_str(), L"rb")})
      {
      char buf[4096];
      for(std::size_t got; (got = std::fread(buf, 1, sizeof buf, lf)) != 0;)
        log_text.append(buf, got);
      std::fclose(lf);
      }
    check(log_text.find("share:") == std::string::npos and log_text.find("data source") == std::string::npos,
          "edworld alone neither publishes nor reads a data source");
    check(log_text.find(": totals draws ") != std::string::npos, "frame summary in the log");
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
    }
  HANDLE const file{CreateFileW(share_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
  check(file != INVALID_HANDLE_VALUE, "share file exists");
  if(file == INVALID_HANDLE_VALUE)
    return 1;
  edworld::share_t s{};
  DWORD got{};
  ReadFile(file, &s, sizeof s, &got, nullptr);
  CloseHandle(file);
  check(got == sizeof s, "share file has the whole record");
  check(s.magic == edworld::share_magic and s.version == edworld::share_version, "magic and version");
  check(s.sequence % 2 == 0 and s.sequence >= 2, "seqlock even and written");
  check(s.panel_count == 1, "exactly one panel per frame (the unwatched draw is not recorded)");
  check(s.frame >= 20, "frames counted by the draw gap");
  edworld::panel_t const & p{s.panels[0]};
  check(p.surface_width == 512 and p.surface_height == 128, "surface size from PS t2");
  check(p.surface_format == DXGI_FORMAT_R8G8B8A8_TYPELESS, "surface format (typeless, as the game's)");
  check(p.index_count == 6 and p.instance_count == 1 and p.start_instance == 3, "draw arguments");
  check(p.flags == 1u and p.record_index == 2u, "instance entry -> record 2");
  check(near_eq(p.scale, 1.5f) and near_eq(p.position[0], 0.5f) and near_eq(p.position[1], 1.5f) and near_eq(p.position[2], 2.5f),
        "record position minus the rebase origin, scale");
  check(near_eq(s.rebase[0], 0.5f) and s.pool_bytes == 4 * 336, "rebase row and pool size");
  check(p.vs_index == 0, "watched list index");
  check(near_eq(p.cb0[4][0], 0.8f) and near_eq(p.cb0[7][2], 0.2f), "cb0 rows are the panel's, not the later draw's");
  // anchor = rows 4..7 . (0.5, 1.5, 2.5, 1): x 0.8*.5+0.1*2.5+0.5 = 1.15, y 0.9*1.5-0.25 = 1.1, w 0.2*2.5+2 = 2.5
  auto const ndc{edworld::clip_to_ndc(p.anchor_clip)};
  check(ndc and near_eq(ndc->x, 1.15f / 2.5f) and near_eq(ndc->y, 1.1f / 2.5f) and near_eq(ndc->w, 2.5f), "anchor from the record's position");
  std::printf("frame %llu source %llu panels %u anchor clip %.4f %.4f %.4f %.4f\n",
              static_cast<unsigned long long>(s.frame), static_cast<unsigned long long>(s.source_frame), s.panel_count,
              p.anchor_clip[0], p.anchor_clip[1], p.anchor_clip[2], p.anchor_clip[3]);
  if(GetEnvironmentVariableW(L"EDWORLD_TEST_EDSM", nullptr, 0))
    Sleep(3000);  // the state thread's question to EDSM
  std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
  }
