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

#include "../src/compass_math.h"
#include "../src/edworld_share.h"
#include "../src/faction_list.h"
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
  // the jump panel's four vertices as the game drew them on 2026-10-05 (ed-lab, geometry dump g10): local
  // x +-0.1196, y +-0.0588, shown at surface pixels x 1031..2045, y 2..493 of 3072x660
  {
  std::uint32_t const words[4][5]{
    {0xf2af0b26u, 0x3ffff9f0u, 0x01007e7fu, 0x007f7ffeu, 0x85fa82aeu},
    {0x0ccf0b26u, 0x3ffffa0fu, 0x01007e7fu, 0x007f7ffeu, 0x800582aeu},
    {0x0cd0f4d5u, 0x3ffffa0fu, 0x01007e7fu, 0x007f7ffeu, 0x80058552u},
    {0xf2b0f4d5u, 0x3ffff9f0u, 0x01007e7fu, 0x007f7ffeu, 0x85fa8552u}};
  edworld::vec3_t p[4];
  float px[4][2];
  for(int i{}; i != 4; ++i)
    {
    p[i] = edworld::decode_local(words[i][0], words[i][1], words[i][2]);
    float u, v;
    edworld::decode_uv(words[i][4], u, v);
    px[i][0] = u * 3072.f;
    px[i][1] = v * 660.f;
    }
  auto const near_px = [](float a, float b) { return std::fabs(a - b) < 1.f; };
  check(std::fabs(p[0].x + 0.1196f) < 1e-3f and std::fabs(p[0].y + 0.0588f) < 1e-3f and std::fabs(p[2].x - 0.1196f) < 1e-3f,
        "panel vertex: local position decoded");
  check(near_px(px[0][0], 1030.5f) and near_px(px[0][1], 493.4f) and near_px(px[2][0], 2044.6f) and near_px(px[2][1], 1.9f),
        "panel vertex: surface pixel decoded");
  auto const map{edworld::map_from({p[0], p[1], p[2]}, {{px[0][0], px[0][1]}, {px[1][0], px[1][1]}, {px[2][0], px[2][1]}})};
  check(map.has_value(), "panel map from three corners");
  if(map)
    {
    auto const back{edworld::local_of(*map, px[3][0], px[3][1])};
    check(back and std::fabs(back->x - p[3].x) < 1e-4f and std::fabs(back->y - p[3].y) < 1e-4f, "panel map: the fourth corner back");
    // below the panel the local y keeps falling: the list's bottom edge lands under the panel's
    auto const below{edworld::local_of(*map, 1540.f, 571.f)};
    check(below and below->y < p[0].y and below->x > p[0].x and below->x < p[2].x, "panel map: a pixel under the panel");
    }
  check(not edworld::map_from({p[0], p[0], p[2]}, {{0.f, 0.f}, {0.f, 0.f}, {1.f, 1.f}}).has_value(), "no map through a line");
  }
  // the SUPERPOWER row read from the panel's text: rows as the game draws them on 3072x660 (labels from x 1112;
  // SUPERPOWER 231 px wide, ALLIANCE 154), in an area copied from (1100, 100) as edworld copies it
  {
  constexpr std::int32_t w{edworld::superpower_value_x1 - edworld::superpower_label_x0};
  constexpr std::int32_t h{edworld::superpower_area_y1 - edworld::superpower_area_y0};
  std::vector<std::uint8_t> area(static_cast<std::size_t>(w) * h * 4);
  auto const fill = [&](std::int32_t sx0, std::int32_t sx1, std::int32_t sy0, std::int32_t sy1)
    {
    for(std::int32_t y{sy0 - 100}; y != sy1 - 100; ++y)
      for(std::int32_t x{sx0 - 1100}; x <= sx1 - 1100; ++x)
        {
        std::uint8_t * px{&area[(static_cast<std::size_t>(y) * w + x) * 4]};
        px[0] = px[1] = px[2] = 200;
        px[3] = 255;
        }
    };
  fill(1112, 1500, 175, 195);  // the region
  fill(1112, 1343, 212, 215);  // a separator line: no row
  fill(1112, 1343, 232, 253);  // the system, as wide as the label, its distance as wide as EMPIRE: never the superpower
  fill(1840, 1957, 232, 253);
  auto none{edworld::read_superpower(area.data(), w * 4u, w, h, 1100, 100)};
  check(not none.label_found, "superpower: the system's row is never taken for it");
  fill(1112, 1343, 272, 293);  // SUPERPOWER  ALLIANCE
  fill(1806, 1960, 272, 293);
  fill(1112, 1381, 312, 333);  // SECURITY LEVEL
  auto const found{edworld::read_superpower(area.data(), w * 4u, w, h, 1100, 100)};
  check(found.label_found and found.superpower == edworld::superpower_e::alliance and found.row_top == 272 and found.row_bottom == 292,
        "superpower: ALLIANCE told by its width, its row found");
  check(std::fabs(edworld::emblem_centre_y(found) - 322.f) < 0.6f, "superpower: the emblem 40 px under its row's centre");
  }
  // the list's rows: as many as the factions, up to seven, EDSM's source row on top of them, never instead of one
  {
  using edworld::list_lines;
  auto const one{list_lines(1u, false, false, 7u)}, seven{list_lines(7u, false, false, 7u)};
  auto const seven_edsm{list_lines(7u, true, false, 7u)}, many{list_lines(12u, false, false, 7u)};
  check(one.factions == 1u and one.source == 0u and seven.factions == 7u, "list: one row per faction, up to seven");
  check(seven_edsm.factions == 7u and seven_edsm.source == 1u, "list: EDSM's source row comes on top of seven factions");
  check(many.factions == 7u, "list: seven rows at most");
  check(std::fabs(edworld::list_height(one, 58.f, 15.f, 28.f) - 116.f) < 0.01f and
          std::fabs(edworld::list_height(seven_edsm, 58.f, 15.f, 28.f) - 522.f) < 0.01f,
        "list: the box as high as its rows and its footer");
  }
  // the compass: a filled dot and a hollow one drawn into a copy of the disc's square, read back; the angles from the
  // sphere's projection; the flight path angle from two fixes Status.json gave on 2026-10-06 (ed-lab, Kestrel)
  {
  constexpr std::int32_t w{152}, h{152};
  constexpr float cx{76.f}, cy{76.f}, radius{54.f};
  std::vector<std::uint8_t> area(static_cast<std::size_t>(w) * h * 4u);
  auto const clear = [&]
    {
    for(std::size_t i{}; i != area.size(); i += 4)
      {
      area[i] = 30; area[i + 1] = 80; area[i + 2] = 200; area[i + 3] = 255;  // the disc's blue: never the dot
      }
    };
  auto const dot = [&](float x, float y, float outer, float inner)
    {
    for(std::int32_t py{}; py != h; ++py)
      for(std::int32_t px{}; px != w; ++px)
        {
        float const d{std::hypot(static_cast<float>(px) - x, static_cast<float>(py) - y)};
        if(d <= outer and d >= inner)
          std::memset(&area[(static_cast<std::size_t>(py) * w + px) * 4u], 255, 4);
        }
    };
  clear();
  dot(cx + 20.f, cy - 30.f, 6.6f, 0.f);
  auto const filled{edworld::read_compass(area.data(), w * 4u, w, h, cx, cy, radius * 1.25f)};
  check(filled.found and filled.filled and std::fabs(filled.x - 20.f) < 0.2f and std::fabs(filled.y + 30.f) < 0.2f and
          filled.pixels > 100u,
        "compass: a filled dot found where it is drawn");
  clear();
  dot(cx - 40.f, cy + 10.f, 6.6f, 5.8f);  // a ring as thin as the game's (~25-30 lit pixels)
  auto const hollow{edworld::read_compass(area.data(), w * 4u, w, h, cx, cy, radius * 1.25f)};
  check(hollow.found and not hollow.filled and std::fabs(hollow.x + 40.f) < 0.3f and std::fabs(hollow.y - 10.f) < 0.3f,
        "compass: a hollow dot found and told from a filled one");
  check(edworld::compass_plausible(filled) and edworld::compass_plausible(hollow), "compass: both dots plausible");
  check(not edworld::compass_plausible(edworld::compass_reading_t{true, true, 1.f, 1.f, 4u}) and
          not edworld::compass_plausible(edworld::compass_reading_t{true, false, 1.f, 1.f, 1234u}),
        "compass: a few lit pixels or the whole disc lit is no dot");
  clear();
  check(not edworld::read_compass(area.data(), w * 4u, w, h, cx, cy, radius * 1.25f).found, "compass: no dot on a bare disc");
  auto const up30{edworld::compass_angles(0.f, -27.f, radius, true)};
  check(std::fabs(up30.up - 30.f) < 0.01f and std::fabs(up30.off_nose - 30.f) < 0.01f and std::fabs(up30.right) < 0.01f,
        "compass: half the radius straight up is 30 degrees up, not 45 (the sphere's projection)");
  auto const behind{edworld::compass_angles(27.f, 0.f, radius, false)};
  check(std::fabs(behind.off_nose - 150.f) < 0.01f and std::fabs(behind.right - 150.f) < 0.01f and std::fabs(behind.up) < 0.01f,
        "compass: hollow half the radius right is 150 degrees right, behind");
  auto const rim{edworld::compass_angles(0.f, 80.f, radius, true)};
  check(std::fabs(rim.up + 90.f) < 0.01f, "compass: past the rim counts as on it");
  double const g{edworld::flight_path_angle(-13.579245, 55.208015, 879953.0, -12.421914, 52.95866, 719708.0, 884209.9375)};
  check(std::fabs(g + 65.6) < 0.1, "compass: the flight path angle of two fixes (-65.6 degrees, the run of 2026-10-06)");
  check(std::isnan(edworld::flight_path_angle(1.0, 2.0, 3.0, 1.0, 2.0, 3.0, 1000.0)), "compass: no angle without a move");
  auto const ahead{edworld::compass_direction(0.f, 0.f, radius, true)};
  check(std::fabs(ahead.z - 1.f) < 1e-6f and std::fabs(ahead.x) < 1e-6f, "spheres: the dot in the middle is the nose");
  auto const d{edworld::compass_direction(0.f, 27.f, radius, true)};
  auto const back{edworld::direction_of(-30.f, 0.f)};
  check(std::fabs(d.y - back.y) < 1e-5f and std::fabs(d.z - back.z) < 1e-5f, "spheres: the dot and the angles give one direction");
  auto const nose{edworld::sphere_view({0.f, 0.f, 1.f}, 35.f, 20.f)};
  auto const tail{edworld::sphere_view({0.f, 0.f, -1.f}, 35.f, 20.f)};
  check(nose.on_far_side and not tail.on_far_side and nose.x < 0.f and nose.y > 0.f,
        "spheres: seen from behind, the left and above, the nose points away, up the picture and to its left");
  auto const below{edworld::sphere_view({0.f, -1.f, 0.f}, 35.f, 20.f)};
  check(below.y < -0.9f and std::fabs(below.x) < 1e-5f, "spheres: straight down is down the picture");
  check(edworld::mix_rgb(0x080e1eu, 0x5096ffu, 0.f) == 0x080e1eu and edworld::mix_rgb(0x080e1eu, 0x5096ffu, 1.f) == 0x5096ffu,
        "spheres: a mix's ends are its colours");
  check(edworld::mix_rgb(0x000000u, 0xff8040u, 0.5f) == 0x804020u, "spheres: a mix halfway, each channel on its own");
  auto const pad{edworld::pad_offsets(edworld::direction_of(-80.f, 0.f))};
  check(std::fabs(pad.forward - 10.f) < 0.01f and std::fabs(pad.right) < 0.01f and std::fabs(pad.off_vertical - 10.f) < 0.01f,
        "approach: 80 degrees below the nose is 10 degrees ahead of the vertical");
  auto const aft{edworld::pad_offsets(edworld::compass_direction(0.f, 54.f, 54.f, false))};
  check(std::fabs(aft.forward) < 0.01f and std::fabs(aft.off_vertical) < 0.01f, "approach: the dot on the rim is straight down");
  check(edworld::approach_view(false, true, true, -25.f, 20.f) and not edworld::approach_view(false, true, true, -15.f, 20.f),
        "approach: on at 20 degrees below the wings");
  check(edworld::approach_view(true, true, true, -15.f, 20.f) and not edworld::approach_view(true, true, true, -5.f, 20.f),
        "approach: kept down to 10 degrees, off above");
  check(not edworld::approach_view(true, false, true, -80.f, 20.f) and edworld::approach_view(true, true, false, 0.f, 20.f),
        "approach: off outside normal flight, kept without a reading");
  }
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

  // EDSM's answer read into the list (pure code, no d3d11): a saved answer for Shinrarta Dezhra
  {
  std::string body;
  if(std::FILE * f{_wfopen((dir + L"\\edsm_factions_shinrarta.json").c_str(), L"rb")})
    {
    char buf[4096];
    for(std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
      body.append(buf, n);
    std::fclose(f);
    }
  edworld::faction_list_t l{};
  check(edworld::list_from_edsm(body, 3932277478106ull, l) and l.source == edworld::list_source_e::edsm, "EDSM's answer read");
  check(l.count == 6, "six factions with influence (the one at none left out)");
  check(std::strcmp(l.rows[0].name, "The Dark Wheel") == 0 and l.rows[0].allegiance == 4u, "highest influence first");
  check(l.rows[1].controlling and not l.rows[0].controlling, "the controlling faction marked");
  check(l.rows[2].allegiance == 1u and std::strcmp(l.rows[5].states, "Boom") == 0, "allegiance and active states");
  check(l.updated_unix_s == 1791114047 and l.rows[0].trend == 0u, "EDSM's update time; no trend from EDSM");
  edworld::faction_list_t bad{};
  check(not edworld::list_from_edsm("{\"factions\":[", 1ull, bad) and not edworld::list_from_edsm("", 1ull, bad), "a broken answer is no list");
  check(edworld::list_from_edsm("[]", 1ull, bad) == false and edworld::list_from_edsm("{}", 1ull, bad) and bad.count == 0, "no factions, empty list");
  }

  // The settings must be in place before the first d3d11 export call.
  std::uint64_t const watched{edworld::fnv1a64(g_panel_vs, sizeof g_panel_vs)};
  {
  std::FILE * ini{_wfopen((config_dir + L"\\" + plugin_name + L".ini").c_str(), L"wb")};
  wchar_t next[MAX_PATH]{};
  GetEnvironmentVariableW(L"EDWORLD_TEST_NEXT", next, MAX_PATH);
  if(next[0])
    std::fprintf(ini, "next = %ls\n", next);
  std::fprintf(ini, "patch = 2\npatch_surface = 512x128\npatch_x = 256\npatch_y = 64\npatch_width = 100\npatch_height = 50\npatch_emblem_height = 40\nedsm = %d\n"
                    "list_x = 256\nlist_top = 96\nlist_width = 200\nlist_rows = 3\nlist_text = 10\n", GetEnvironmentVariableW(L"EDWORLD_TEST_EDSM", nullptr, 0) ? 1 : 0);
  std::fprintf(ini, "watch_vs = %016llX, 1989E6D3B405FDE0\nlog_interval_ms = 1\nlog_all_vs = 1\n",
               static_cast<unsigned long long>(watched));
  if constexpr(with_eht)
    std::fprintf(ini, "shm_dir = %ls\n", shm_dir.c_str());
  // the superpower is read from the panel's text, which this test's surface has none of: the Empire emblem by hand
  std::fprintf(ini, "patch_force = 2\n");
  // the compass on the same surface: its disc at (100, 64), the text under the patch's box (its checks untouched)
  std::fprintf(ini, "compass = 2\ncompass_surface = 512x128\ncompass_x = 100\ncompass_y = 64\ncompass_radius = 30\n"
                    "compass_text_x = 150\ncompass_text_y = 100\ncompass_text_size = 12\ncompass_log_ms = 1\ncompass_spheres = 1\ncompass_hide_game = 1\ncompass_sphere_text_above = 1\ncompass_anchor = 1\ncompass_anchor_c_px = 300\ncompass_anchor_c_py = 64\n");
  std::fclose(ini);
  }
  // the spheres' textures written once for a look (edworld_dumps/sphere0_*.raw, sphere1_*.raw)
  if(std::FILE * t{_wfopen((output_dir + L"\\edworld_sphere_dump").c_str(), L"wb")})
    std::fclose(t);
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
  t.factions_known = 1;
  t.faction_count = 2;
  std::strcpy(t.factions[0].name, "Imperial Faction");
  std::strcpy(t.factions[0].states, "Boom");
  t.factions[0].influence = 0.6f;
  t.factions[0].allegiance = 2;
  t.factions[0].trend = 1;
  t.factions[0].controlling = 1;
  std::strcpy(t.factions[1].name, "Second Faction");
  t.factions[1].influence = 0.4f;
  t.factions[1].allegiance = 4;
  t.factions[1].trend = 3;
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
  // the compass's dot, white, 10 px right of and 10 px above its disc's centre (100, 64)
  for(int y{48}; y != 61; ++y)
    for(int x{104}; x != 117; ++x)
      if((x - 110) * (x - 110) + (y - 54) * (y - 54) <= 36)
        drawn[static_cast<std::size_t>(y) * 512 + x] = 0xffffffffu;
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
  check(emblem_pixels > 150, "Empire emblem (patch_force), in its colour");
  // the Empire's V is wide at the top and comes to a point at the bottom
  check(upper_half * 2 > emblem_pixels, "emblem upright, not flipped");
  check(pixel(256 - 45, 64) == 0x020304u, "patch ground drawn beside the emblem");
  check(pixel(208, 64) == 0xff00ffu, "test frame drawn at the patch's edge");
  check(pixel(10, 10) == 0u, "surface untouched outside the patch");
  // the list is no longer drawn on the surface: there the rows under the panel belong to other panels; it is a quad of
  // its own drawn after the panel's draw, placed by the panel's vertices (stride 40, which this test's draw lacks)
  std::uint32_t lit_under{};
  for(int y{98}; y != 128; ++y)
    for(int x{160}; x != 352; ++x)
      if(pixel(x, y) != 0u)
        ++lit_under;
  check(lit_under == 0, "nothing drawn on the surface under the panel (the list is a quad of its own)");
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
  {
  std::uint32_t n{};
  for(int y{98}; y != 128; ++y)
    for(int x{160}; x != 352; ++x)
      n += pixel(x, y) != 0u ? 1u : 0u;
  check(n == 0u, "no list either while the game hides the panel (the gate)");
  }
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

  {
  std::string compass_log;
  if(std::FILE * lf{_wfopen(log_path.c_str(), L"rb")})
    {
    char buf[4096];
    for(std::size_t got; (got = std::fread(buf, 1, sizeof buf, lf)) != 0;)
      compass_log.append(buf, got);
    std::fclose(lf);
    }
  check(compass_log.find("compass: first drawn (mode 2, surface 512x128") != std::string::npos, "compass: drawn on its surface");
  check(compass_log.find("compass: dot filled dot +10.00 -10.00") != std::string::npos, "compass: the dot read back where it was put");
  check(compass_log.find("compass: spheres rendered (896x672 px each)") != std::string::npos, "spheres: both textures drawn");
  check(compass_log.find("compass: spheres written to edworld_dumps") != std::string::npos, "spheres: both textures written for a look");
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
