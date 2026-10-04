// edworld smoke test without the game: a D3D11 program that draws one "panel" (a watched VS with a known
// cb0 rows 4..7) and one other draw, through edworld's d3d11.dll, then checks what edworld published.
// Exit code 0 = pass. Run from a directory holding test_app.exe and edworld's d3d11.dll (native override).
#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/edworld_share.h"
#include "../src/panel_math.h"
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
  }  // namespace

int main()
  {
  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir{exe};
  dir.resize(dir.find_last_of(L"\\/"));
  std::wstring const share_path{dir + L"\\edworld_test.shm"};

  // The settings must be in place before the first d3d11 export call.
  std::uint64_t const watched{edworld::fnv1a64(g_panel_vs, sizeof g_panel_vs)};
  {
  std::FILE * ini{_wfopen((dir + L"\\edworld.ini").c_str(), L"wb")};
  wchar_t next[MAX_PATH]{};
  GetEnvironmentVariableW(L"EDWORLD_TEST_NEXT", next, MAX_PATH);
  if(next[0])
    std::fprintf(ini, "next = %ls\n", next);
  std::fprintf(ini, "watch_vs = %016llX, 1989E6D3B405FDE0\nshare = %ls\nlog_interval_ms = 1\nlog_all_vs = 1\n",
               static_cast<unsigned long long>(watched), share_path.c_str());
  std::fclose(ini);
  }
  DeleteFileW(share_path.c_str());

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
  std::FILE * trigger{_wfopen((dir + L"\\edworld_dump").c_str(), L"wb")};
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
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D * surface{};
  dev->CreateTexture2D(&td, nullptr, &surface);
  ID3D11ShaderResourceView * srv{};
  dev->CreateShaderResourceView(surface, nullptr, &srv);

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
  check(p.surface_format == DXGI_FORMAT_R8G8B8A8_UNORM, "surface format");
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
  WIN32_FIND_DATAW found{};
  HANDLE const dumps{FindFirstFileW((dir + L"\\edworld_dumps\\*_512x128_f28_*.raw").c_str(), &found)};
  check(dumps != INVALID_HANDLE_VALUE, "the panel's surface dumped (512x128)");
  if(dumps != INVALID_HANDLE_VALUE)
    FindClose(dumps);
  check(GetFileAttributesW((dir + L"\\edworld_dump").c_str()) == INVALID_FILE_ATTRIBUTES, "dump trigger removed");
  std::printf("frame %llu source %llu panels %u anchor clip %.4f %.4f %.4f %.4f\n",
              static_cast<unsigned long long>(s.frame), static_cast<unsigned long long>(s.source_frame), s.panel_count,
              p.anchor_clip[0], p.anchor_clip[1], p.anchor_clip[2], p.anchor_clip[3]);
  if(HMODULE const fake{GetModuleHandleW(L"fake_next.dll")})
    {
    using calls_fn = LONG (*)();
    auto const calls{reinterpret_cast<calls_fn>(GetProcAddress(fake, "fake_next_calls"))};
    check(calls and calls() == 1, "chained proxy called once, its by-name call routed to the system copy");
    }
  std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
  }
