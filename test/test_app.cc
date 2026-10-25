// edworld smoke test without the game: a D3D11 program that draws one "panel" (a watched VS with a known
// cb0 rows 4..7) and one other draw, through edworld's d3d11.dll, then checks what edworld published.
// Exit code 0 = pass. Run from a directory holding test_app.exe and edworld's d3d11.dll (native override).
// Built once per variant: EDWORLD_EHT = the edworld_eht build (EHT's target read, panels published).
#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/compass_math.h"
#include "../src/edworld_share.h"
#include "../src/faction_list.h"
#include "../src/gate_math.h"
#include "../src/hologram_math.h"
#include "../src/holo_models.h"
#include "../src/pad_text_read.h"
#include "../src/panel_math.h"
#include "../src/sample_context.h"
#include "../src/shader_dxbc.h"
#include "catalog_cs.h"
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

  // the hologram models' registry (holo_models.h): an asymmetric cloud of points, its fingerprint, turns and remaps
  auto holo_cloud() -> std::vector<edworld::vec3_t>
    {
    std::vector<edworld::vec3_t> out;
    std::uint32_t seed{12345u};
    auto const next = [&seed]() -> float
      {
      seed = seed * 1664525u + 1013904223u;
      return static_cast<float>(seed >> 8) / 16777216.f;
      };
    for(int i{}; i != 400; ++i)  // a slab 2 x 1.2 x 0.6, off centre
      out.push_back({next() * 2.f - 0.7f, next() * 1.2f, next() * 0.6f});
    for(int i{}; i != 60; ++i)  // a tower on one corner
      out.push_back({1.1f + next() * 0.2f, next() * 0.2f, 0.6f + next() * 1.4f});
    return out;
    }

  // a hologram model's sample in its context (sample_context.h): Status.json and the journal's place, a cut file null
  auto sample_context_tests() -> void
    {
    using namespace edworld;
    std::string_view const tail{"ent\":\"Music\" }\r\n{ \"timestamp\":\"2026-10-23T15:16:42Z\", \"event\":\"DockingGranted\" }\r\n{ \"ti"};
    check(last_complete_line(tail) == R"({ "timestamp":"2026-10-23T15:16:42Z", "event":"DockingGranted" })",
          "context: the journal's last complete line");
    check(last_complete_line("{ \"cut").empty() and last_complete_line("").empty(), "context: no complete line");
    std::string const whole{sample_context_text("103481344-01dd62ef6d758059_20261023T151723_499Z_1170",
                                                "{ \"Flags\":16842765, \"Destination\":{ \"Name\":\"Bischoff \\\"L\\\"\" } }\r\n",
                                                "Journal.2026-10-23T170745.01.log", 123456u, tail)};
    check(whole == "{\"sample\": \"103481344-01dd62ef6d758059_20261023T151723_499Z_1170\",\n"
                   " \"status\": { \"Flags\":16842765, \"Destination\":{ \"Name\":\"Bischoff \\\"L\\\"\" } },\n"
                   " \"journal\": {\"file\": \"Journal.2026-10-23T170745.01.log\", \"size\": 123456, \"last_line\": "
                   "{ \"timestamp\":\"2026-10-23T15:16:42Z\", \"event\":\"DockingGranted\" }}}\n",
          "context: Status.json and the journal's place");
    check(sample_context_text("s\"x", "{ \"Flags\":1", "", 0u, "") == "{\"sample\": \"s\\\"x\",\n \"status\": null,\n \"journal\": null}\n",
          "context: Status.json cut, no journal");
    }

  // the shaders' catalog (shader_dxbc.h): the shape of bytecode fxc made here, and bytes that are no DXBC
  auto shader_shape_tests() -> void
    {
    using namespace edworld;
    auto const has = [](std::string const & words, char const * word) -> bool
      { return (" " + words + " ").find(std::string{" "} + word + " ") != std::string::npos; };
    dxbc_shape_t const vs{dxbc_shape(g_panel_vs, sizeof g_panel_vs)};
    check(vs.ok and vs.stage == shader_stage_e::vertex and vs.major == 5u and vs.minor == 0u and
            vs.inputs == "INSTANCE0 POSITION0 TEXCOORD0" and vs.outputs == "SV_Position0 TEXCOORD0" and
            vs.decls.rfind("cb0[", 0) == 0 and vs.instructions > 0u,
          "shader shape: the panel's VS - stage, version, signatures, its cbuffer first");
    dxbc_shape_t const cs{dxbc_shape(g_catalog_cs, sizeof g_catalog_cs)};
    check(cs.ok and cs.stage == shader_stage_e::compute and has(cs.decls, "cb1[3]") and has(cs.decls, "t2") and
            has(cs.decls, "u0") and has(cs.decls, "tg8x4x1") and cs.inputs.empty() and cs.outputs.empty(),
          "shader shape: a compute shader - its cbuffer's rows, structured input, UAV and thread group");
    dxbc_shape_t const ps{dxbc_shape(g_panel_ps, sizeof g_panel_ps)};
    check(ps.ok and ps.stage == shader_stage_e::pixel and std::string{stage_name(ps.stage)} == "ps", "shader shape: the panel's PS is a pixel shader");
    std::vector<std::uint8_t> cut(g_catalog_cs, g_catalog_cs + sizeof g_catalog_cs / 2);
    std::uint8_t const junk[64]{'D', 'X', 'B', 'C'};
    check(not dxbc_shape(cut.data(), cut.size()).ok and not dxbc_shape(junk, sizeof junk).ok and not dxbc_shape(nullptr, 0).ok,
          "shader shape: half a shader, a bare header or nothing is no shape");
    // the game's own, when given (EDWORLD_SHADER_SAMPLE=<file.dxbc>[;<file.dxbc>...]): game data is not kept here
    if(char const * const env{std::getenv("EDWORLD_SHADER_SAMPLE")})
      for(std::string list{env}; not list.empty();)
        {
        std::size_t const end{list.find(';')};
        std::string const path{list.substr(0, end)};
        list = end == std::string::npos ? std::string{} : list.substr(end + 1);
        std::vector<std::uint8_t> bytes;
        if(FILE * const f{std::fopen(path.c_str(), "rb")})
          {
          std::uint8_t buf[4096];
          for(std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) != 0;)
            bytes.insert(bytes.end(), buf, buf + n);
          std::fclose(f);
          }
        dxbc_shape_t const g{dxbc_shape(bytes.data(), bytes.size())};
        std::printf("sample %s: %s %u.%u | %s | %s | %s | %u | %s\n", path.c_str(), stage_name(g.stage), g.major, g.minor, g.inputs.c_str(),
                    g.outputs.c_str(), g.decls.c_str(), g.instructions, g.chunks.c_str());
        check(g.ok, "shader shape: a shader of the game reads");
        }
    }

  auto holo_models_tests() -> void
    {
    using namespace edworld;
    std::vector<vec3_t> const cloud{holo_cloud()};
    holo_fingerprint_t const f{holo_fingerprint(cloud.data(), cloud.size(), 300u)};
    std::vector<vec3_t> shuffled{cloud};
    std::reverse(shuffled.begin(), shuffled.end());
    std::rotate(shuffled.begin(), shuffled.begin() + 77, shuffled.end());
    holo_fingerprint_t const g{holo_fingerprint(shuffled.data(), shuffled.size(), 300u)};
    check(std::memcmp(&f, &g, sizeof f) == 0, "holo models: the fingerprint does not depend on the order of the vertices");
    check(near_eq(holo_likeness(f, f), 1.f) and f.vertices == 460u and near_eq(std::max({f.extent[0], f.extent[1], f.extent[2]}), 1.f) and f.extent[1] < 1.f,
          "holo models: a fingerprint is like itself, the box's largest half-side 1");

    bool unique{true}, proper{true};
    for(std::size_t a{}; a != holo_turn_list.size(); ++a)
      {
      holo_turn_t const & t{holo_turn_list[a]};
      int const parity{(t.perm[0] == 0 and t.perm[1] == 1) or (t.perm[0] == 1 and t.perm[1] == 2) or (t.perm[0] == 2 and t.perm[1] == 0) ? 1 : -1};
      proper = proper and parity * t.sign[0] * t.sign[1] * t.sign[2] == 1;
      for(std::size_t b{}; b != a; ++b)
        unique = unique and std::memcmp(&holo_turn_list[a], &holo_turn_list[b], sizeof(holo_turn_t)) != 0;
      }
    check(unique and proper and holo_turn_list[0].perm == std::array<std::int8_t, 3>{0, 1, 2} and
            holo_turn_list[0].sign == std::array<std::int8_t, 3>{1, 1, 1},
          "holo models: 24 different turns without a mirror, the identity first");

    float worst{1.f}, best_other{0.f};
    for(std::size_t a{}; a != holo_turn_list.size(); ++a)
      {
      std::vector<vec3_t> moved;
      for(vec3_t const & v: cloud)
        moved.push_back(turn(holo_turn_list[a], v));
      holo_fingerprint_t const m{holo_fingerprint(moved.data(), moved.size(), 300u)};
      worst = std::min(worst, holo_likeness(m, turned(f, holo_turn_list[a])));
      if(a)
        best_other = std::max(best_other, holo_likeness(m, f));
      }
    check(worst > 0.95f and best_other < 0.9f, "holo models: a turned model's fingerprint is the turned fingerprint, not the unturned one");

    // the registry: the cloud as model 0 (count 100, build "a"), a slab without the tower as model 1
    std::vector<vec3_t> slab(cloud.begin(), cloud.begin() + 400);
    holo_fingerprint_t const fs{holo_fingerprint(slab.data(), slab.size(), 260u)};
    holo_model_t const models[]{{"cloud", holo_role_e::station, 100u, holo_status_e::confirmed, "a", true, f},
                                {"slab", holo_role_e::station, 200u, holo_status_e::confirmed, "a", true, fs},
                                {"no print", holo_role_e::ship, 300u, holo_status_e::measured, "", false, {}}};
    auto const r_known{holo_resolve(models, 3u, 100u, "a", nullptr)};
    auto const r_old{holo_resolve(models, 3u, 300u, "b", nullptr)};
    auto const r_none{holo_resolve(models, 3u, 999u, "b", nullptr)};
    check(r_known.how == holo_resolve_e::known and r_known.model == 0 and r_old.how == holo_resolve_e::known and r_old.model == 2 and
            r_none.how == holo_resolve_e::unknown and r_none.model == -1,
          "holo models: the count on its build, a count from before builds were logged, none without a fingerprint");
    auto const r_stable{holo_resolve(models, 3u, 100u, "b", &f)};
    check(r_stable.how == holo_resolve_e::stable and r_stable.model == 0, "holo models: the same count and shape on a new build is stable");

    holo_turn_t const t{holo_turn_list[7]};
    std::vector<vec3_t> moved;
    for(vec3_t const & v: cloud)
      moved.push_back(turn(t, v));
    holo_fingerprint_t const fm{holo_fingerprint(moved.data(), moved.size(), 300u)};
    auto const r_new{holo_resolve(models, 3u, 4242u, "b", &fm)};
    bool same_turn{r_new.turn == 7};
    check(r_new.how == holo_resolve_e::remapped and r_new.model == 0 and same_turn and r_new.score > 0.95f and r_new.second < 0.9f,
          "holo models: a new count with the cloud's shape, turned, is the cloud with that turn");
    auto const r_slab{holo_resolve(models, 3u, 4243u, "b", &fs)};
    check(r_slab.how == holo_resolve_e::remapped and r_slab.model == 1 and r_slab.turn == 0, "holo models: the slab is not taken for the cloud");
    std::vector<vec3_t> other;
    for(int i{}; i != 300; ++i)
      other.push_back({std::cos(i * 0.3f) * 0.2f, static_cast<float>(i) * 0.01f, std::sin(i * 0.3f) * 0.2f});
    holo_fingerprint_t const fo{holo_fingerprint(other.data(), other.size(), 600u)};
    check(holo_resolve(models, 3u, 4244u, "b", &fo).how == holo_resolve_e::unknown, "holo models: another shape stays unknown");

    // a model drawn turned: its axes (as the T1 table names them) come back from the drawn ones
    std::array<compass_direction_t, 3> const model_axes{{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}}};
    std::array<compass_direction_t, 3> drawn{};
    for(std::size_t k{}; k != 3; ++k)  // the drawn axis k is the model's axis perm[k], sign[k] its end
      {
      compass_direction_t const & a{model_axes[static_cast<std::size_t>(t.perm[k])]};
      drawn[k] = compass_direction_t{t.sign[k] * a.x, t.sign[k] * a.y, t.sign[k] * a.z};
      }
    auto const back{holo_model_axes(drawn, t)};
    bool same{true};
    for(std::size_t k{}; k != 3; ++k)
      same = same and near_eq(back[k].x, model_axes[k].x) and near_eq(back[k].y, model_axes[k].y) and near_eq(back[k].z, model_axes[k].z);
    check(same, "holo models: the model's axes from a turned model's drawn axes");

    bool counts_unique{true};
    for(std::size_t a{}; a != holo_models.size(); ++a)
      for(std::size_t b{}; b != a; ++b)
        counts_unique = counts_unique and (holo_models[a].count == 0u or holo_models[a].count != holo_models[b].count
                                           or std::strcmp(holo_models[a].build, holo_models[b].build) != 0);
    check(counts_unique, "holo models: a count is one model's on one build (the generated table's empty row aside)");
    {
      // a model confirmed in the form on the next build beside its measured entry (2026-10-24): its count known on that
      // build, and on a third its fingerprint finds it - its own copy no rival
      char const next[]{"103481344-01dd62ef6d758059"}, third[]{"103400000-01dd70000000000"};
      holo_model_t const two[]{
        {"Sidewinder", holo_role_e::ship, 3924u, holo_status_e::measured, holo_build_2026_10_13, true, holo_fp_sidewinder},
        {"Coriolis", holo_role_e::station, 948u, holo_status_e::measured, holo_build_2026_10_13, true, holo_fp_coriolis},
        {"Sidewinder (own)", holo_role_e::ship, 3924u, holo_status_e::confirmed, next, true, holo_fp_sidewinder},
      };
      auto const on_next{holo_resolve(two, std::size(two), 3924u, next, nullptr)};
      auto const on_third{holo_resolve(two, std::size(two), 4000u, third, &holo_fp_sidewinder)};
      check(on_next.how == holo_resolve_e::known and on_next.model == 2 and on_third.how == holo_resolve_e::remapped
              and on_third.model == 0 and holo_table_count(on_third, two) == 3924u,
            "holo models: a model confirmed on the next build is known there, and its copy no rival on a third");
    }
    {
      // near PRNH's pad 15 (2026-10-22): the Sidewinder (3924) on both surfaces; at an outpost the station's on the target's
      std::uint32_t const counts[]{3924u, 3924u}, outpost[]{27885u, 5238u};
      bool const own[]{true, false}, own2[]{true, false};
      auto const model = [](std::uint32_t count) -> holo_model_t const *
        {
        holo_resolved_t const r{holo_resolve(holo_models.data(), holo_models.size(), count, holo_build_2026_10_13, nullptr)};
        return r.model >= 0 ? &holo_models[r.model] : nullptr;
        };
      check(holo_shows_ship(3924u, counts, own, 2u, model(3924u)) and not holo_shows_ship(5238u, outpost, own2, 2u, model(5238u)) and
              holo_shows_ship(106830u, outpost, own2, 2u, model(106830u)) and not holo_shows_ship(4242u, outpost, own2, 2u, model(4242u)),
            "holo models: the own ship on the target's surface, or a registered ship, is a ship; a station or unknown is not");
      // at Ahn Hub after a ship swap (2026-10-25): the port (2760) on the own surface, the Kestrel (27885) on the
      // target's - the port is the target and no ship
      std::uint32_t const swapped[]{2760u, 27885u};
      auto const model_on = [](std::uint32_t count) -> holo_model_t const *
        {
        holo_resolved_t const r{holo_resolve(holo_models.data(), holo_models.size(), count, "103481344-01dd62ef6d758059", nullptr)};
        return r.model >= 0 ? &holo_models[r.model] : nullptr;
        };
      check(not holo_shows_ship(2760u, swapped, own, 2u, model_on(2760u)) and holo_shows_ship(27885u, swapped, own, 2u, model_on(27885u)),
            "holo models: a place on the own surface is no ship; the ship beside it is");
    }
    {
      // the registry's build (2026-10-13) and the next one: a count is a model's on its build; on another only by its
      // fingerprint - the Sidewinder kept or renumbered, a T1 station without a fingerprint unknown, its count free for
      // the tables nowhere
      char const next[]{"103481344-01dd44e0c0000000"};
      auto const known{holo_resolve(holo_models.data(), holo_models.size(), 5238u, holo_build_2026_10_13, nullptr)};
      auto const t1_next{holo_resolve(holo_models.data(), holo_models.size(), 5238u, next, nullptr)};
      auto const kept{holo_resolve(holo_models.data(), holo_models.size(), 3924u, next, &holo_fp_sidewinder)};
      auto const moved{holo_resolve(holo_models.data(), holo_models.size(), 3925u, next, &holo_fp_sidewinder)};
      auto const taken{holo_resolve(holo_models.data(), holo_models.size(), 3924u, next, &holo_fp_coriolis)};
      check(holo_table_count(known, holo_models.data()) == 5238u and t1_next.how == holo_resolve_e::unknown and
              holo_table_count(t1_next, holo_models.data()) == 0u,
            "holo models: a count on the registry's build is the tables'; on the next build, without a fingerprint, nobody's");
      check(kept.how == holo_resolve_e::stable and holo_table_count(kept, holo_models.data()) == 3924u and
              moved.how == holo_resolve_e::remapped and holo_table_count(moved, holo_models.data()) == 3924u and moved.turn == 0 and
              taken.how == holo_resolve_e::remapped and holo_table_count(taken, holo_models.data()) == 948u,
            "holo models: on the next build the Sidewinder's fingerprint keeps or carries its count; the Coriolis' under the "
            "Sidewinder's count is the Coriolis");
      bool clear{true};
      for(holo_model_t const & m: holo_models)
        if(m.has_fingerprint)
          {
          auto const self{holo_resolve(holo_models.data(), holo_models.size(), 1u, next, &m.fingerprint)};
          clear = clear and self.how == holo_resolve_e::remapped and holo_models[self.model].count == m.count and self.turn == 0;
          }
      check(clear, "holo models: each fingerprint read in the game finds its own model on another build, unturned, clear of the rest");
    }

    check(t1_agrees(3.f, 10.f) and not t1_agrees(20.f, 10.f) and not t1_agrees(3.f, 45.f) and
            std::fabs(nose_off_deg({0.f, 0.f, 1.f})) < 0.01f and std::fabs(nose_off_deg({1.f, 0.f, 0.f}) - 90.f) < 0.01f,
          "T1 watch: level and nose ahead agree; a tilted plane or a nose aside deviates");
    check(t1_verdict(t1_look_t{}, 5000u) == t1_verdict_e::none and
            t1_verdict(t1_look_t{4000u, 2u, 5238u, 3.f, 5.f}, 5000u) == t1_verdict_e::match and
            t1_verdict(t1_look_t{4000u, 2u, 5238u, 25.f, 5.f}, 5000u) == t1_verdict_e::deviates and
            t1_verdict(t1_look_t{1000u, 2u, 5238u, 3.f, 5.f}, 5000u) == t1_verdict_e::no_look,
          "T1 watch: nothing without a look on a T1 pad, MATCH, DEVIATES, NO LOOK for a look older than 3 s");
    {
      // the squadron carrier seen level (model axes = camera's): the beam's plane; leaning 24 degrees to +X, the wing's
      pad_planes_t const * const sc{pad_planes_of(84969u)};
      std::array<compass_direction_t, 3> const level{{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}}};
      picked_plane_t const flat{pick_pad_plane(*sc, level)};
      // the model's axes in the camera turned 24 degrees about Z: the wing leaning to +X is level with the ship
      float const r{24.f / 57.29578f};
      std::array<compass_direction_t, 3> const rolled{{{std::cos(r), std::sin(r), 0.f}, {-std::sin(r), std::cos(r), 0.f}, {0.f, 0.f, 1.f}}};
      picked_plane_t const wing{pick_pad_plane(*sc, rolled)};
      check(sc and not pad_planes_of(5238u) and flat.angle_deg == 0.f and std::fabs(flat.axes[0].y - 1.f) < 1e-4f and
              wing.angle_deg == 24.f and std::fabs(wing.axes[0].y - 1.f) < 1e-3f and std::fabs(wing.axes[1].z - 1.f) < 1e-4f,
            "carrier: the plane nearest the ship's up, the beam's level, the wing's when leaning 24 degrees");
      // half way between the beam's and the wing's (12 degrees, a little nearer the wing): the plane held stays
      float const h12{13.f / 57.29578f};
      std::array<compass_direction_t, 3> const between{{{std::cos(h12), std::sin(h12), 0.f}, {-std::sin(h12), std::cos(h12), 0.f}, {0.f, 0.f, 1.f}}};
      check(pick_pad_plane(*sc, between).angle_deg == 24.f and pick_pad_plane(*sc, between, 0.f).angle_deg == 0.f and
              pick_pad_plane(*sc, rolled, 0.f).angle_deg == 24.f and wing.axes[1].z > 0.99f,
            "carrier: the plane held stays until another is 4 degrees nearer; the nose along the length on every plane");
      float const * const pad1{pad_plane_of_pad(84969u, 1u)};
      pad_planes_t const * const site{pad_planes_of(13083u)};
      float const * const pad18{pad_plane_of_pad(84969u, 18u)};
      float const * const pad13{pad_plane_of_pad(84969u, 13u)};
      float const * const pad29{pad_plane_of_pad(84969u, 29u)};
      check(pad18 and *pad18 == -24.f and pad13 and *pad13 == 0.f and pad29 and *pad29 == -46.f and
              not pad_plane_of_pad(84969u, 33u) and not pad_plane_of_pad(13083u, 1u),
            "PRNH's pads by number: 18 the far wing's, 13 the beam's, 29 the far edge's; none past 32 nor of another model");
      pad_learned_t const learned_test[]{
        {13083u, "", 2u, 0.f, {0.f, 0.f, 1.f}}, {13083u, "Planetary Construction Site: Bischoff Legacy", 2u, 0.f, {0.f, 0.f, -1.f}},
        {84969u, "PRNH", 18u, -24.f, {0.f, 0.f, 1.f}}, {0u, "", 0u, 0.f, {0.f, 0.f, 0.f}}};
      pad_learned_t const * const own{pad_learned_in(learned_test, 13083u, "Planetary Construction Site: Bischoff Legacy", 2u)};
      pad_learned_t const * const other{pad_learned_in(learned_test, 13083u, "Planetary Construction Site: Biggs Base", 2u)};
      pad_learned_t const * const carrier18{pad_learned_in(learned_test, 84969u, "WACLAW SIERPINSKI | PRNH", 18u)};
      check(own and own->nose[2] == -1.f and other and other->nose[2] == 1.f and carrier18 and carrier18->angle_deg == -24.f and
              not pad_learned_in(learned_test, 84969u, "SOMEONE | XPRNH", 18u) and not pad_learned_in(learned_test, 84969u, "PRNH-2", 18u) and
              not pad_learned_in(learned_test, 13083u, "Planetary Construction Site: Biggs Base", 3u) and
              not pad_learned_of(84969u, "WACLAW SIERPINSKI | PRNH", 0u),
            "learned pads: the station's own before the model's, a carrier by its callsign after ' | ', none for another pad");
      check(site_kind_of("Planetary Construction Site: Eames Genetics Lab") == site_kind_e::construction_site and
              site_kind_of("Orbital Construction Site: AlexanderIII's Folly") == site_kind_e::construction_site and
              site_kind_of("Cardenas Plantations") == site_kind_e::other and site_kind_of("WACLAW SIERPINSKI | PRNH") == site_kind_e::other and
              site_kind_of("$POIScenario_Watson_Damaged_Sidewinder_01_Assassination_Hard; $USS_ThreatLevel:#threatLevel=3;") ==
                site_kind_e::unknown and site_kind_of("") == site_kind_e::unknown,
            "site kinds: a construction site by the ': ' in its name (Status.json 2026-10), a key of the game no place");
      pad_learned_t const kinds_test[]{
        {13083u, "", 1u, 0.f, {0.f, 0.f, 1.f}}, {13083u, "", 1u, 0.f, {0.f, 0.f, -1.f}, site_kind_e::construction_site},
        {13083u, "Planetary Construction Site: Bischoff Legacy", 1u, 0.f, {-1.f, 0.f, 0.f}},
        {13083u, "", 3u, 0.f, {1.f, 0.f, 0.f}, site_kind_e::other}, {0u, "", 0u, 0.f, {0.f, 0.f, 0.f}}};
      pad_learned_t const * const site_own{pad_learned_in(kinds_test, 13083u, "Planetary Construction Site: Bischoff Legacy", 1u)};
      pad_learned_t const * const site_any{pad_learned_in(kinds_test, 13083u, "Planetary Construction Site: Bauer Command", 1u)};
      pad_learned_t const * const settlement1{pad_learned_in(kinds_test, 13083u, "Cardenas Plantations", 1u)};
      pad_learned_t const * const settlement3{pad_learned_in(kinds_test, 13083u, "Painter Hydroponics", 3u)};
      check(site_own and site_own->nose[0] == -1.f and site_any and site_any->nose[2] == -1.f and settlement1 and
              settlement1->nose[2] == 1.f and settlement3 and settlement3->nose[0] == 1.f and
              not pad_learned_in(kinds_test, 13083u, "Planetary Construction Site: Bauer Command", 3u) and
              not pad_learned_in(kinds_test, 13083u, "$USS_HighGradeEmissions;", 3u),
            "learned pads: the station's own, then its kind's, then the model's; another kind's row never");
      check(pad1 and *pad1 == 24.f and pick_pad_plane(*sc, level, 0.f, pad1).angle_deg == 24.f and
              site and site->planes == 1u and not site->nose_along and std::fabs(pick_pad_plane(*site, rolled).axes[0].y - std::cos(r)) < 1e-4f,
            "pads' planes: a pad known by its number keeps its plane over another (from the stern over a wing); the site's ground");
    }

    // a sample from the game, when given (EDWORLD_HOLO_SAMPLE=<ib_u16.bin>;<vb_s40.bin>;<count>): game data is not kept here
    if(char const * const env{std::getenv("EDWORLD_HOLO_SAMPLE")})
      {
      std::string const spec{env};
      auto const a{spec.find(';')}, b{spec.rfind(';')};
      auto const read = [](std::string const & path) -> std::vector<std::uint8_t>
        {
        std::vector<std::uint8_t> bytes;
        if(std::FILE * f{std::fopen(path.c_str(), "rb")})
          {
          std::uint8_t buf[65536];
          for(std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) != 0;)
            bytes.insert(bytes.end(), buf, buf + n);
          std::fclose(f);
          }
        return bytes;
        };
      std::vector<std::uint8_t> const ib{read(spec.substr(0, a))}, vb{read(spec.substr(a + 1, b - a - 1))};
      auto const count{static_cast<std::uint32_t>(std::strtoul(spec.c_str() + b + 1, nullptr, 10))};
      std::vector<vec3_t> used;
      bool const ok{ib.size() >= count * 2u and
                    holo_used_vertices(ib.data(), 2u, count, vb.data(), static_cast<std::uint32_t>(vb.size()), used)};
      holo_fingerprint_t const real{ok ? holo_fingerprint(used.data(), used.size(), count / 3u) : holo_fingerprint_t{}};
      std::vector<std::uint8_t> perm_ib(ib.begin(), ib.begin() + std::min<std::size_t>(ib.size(), count * 2u));
      for(std::size_t tri{}; tri + 6u <= perm_ib.size(); tri += 6u)  // each triangle's indices turned: the same mesh
        std::rotate(perm_ib.begin() + static_cast<std::ptrdiff_t>(tri), perm_ib.begin() + static_cast<std::ptrdiff_t>(tri + 2u),
                    perm_ib.begin() + static_cast<std::ptrdiff_t>(tri + 6u));
      std::vector<vec3_t> used2;
      bool const ok2{ok and holo_used_vertices(perm_ib.data(), 2u, count, vb.data(), static_cast<std::uint32_t>(vb.size()), used2)};
      holo_fingerprint_t const again{ok2 ? holo_fingerprint(used2.data(), used2.size(), count / 3u) : holo_fingerprint_t{}};
      std::vector<vec3_t> moved_real;
      for(vec3_t const & v: used)
        moved_real.push_back(turn(holo_turn_list[11], v));
      holo_fingerprint_t const real_turned{holo_fingerprint(moved_real.data(), moved_real.size(), count / 3u)};
      holo_model_t const reg[]{{"sample", holo_role_e::station, count, holo_status_e::confirmed, "a", true, real}};
      auto const r{holo_resolve(reg, 1u, count + 1u, "b", &real_turned)};
      std::printf("      sample: %u indices, %u vertices, likeness turned %.3f, turn %d\n", count, real.vertices, static_cast<double>(r.score), r.turn);
      check(ok and ok2 and std::memcmp(&real, &again, sizeof real) == 0 and
              (r.how == holo_resolve_e::remapped or r.how == holo_resolve_e::ambiguous) and r.model == 0,
            "holo models: a game's sample: the order of indices does not count, turned it is found again");
      }
    }
  // the pad's text (pad_text_read.h): the game's own glyphs of the notices' digits (the samples of 2026-10-21..25), row
  // y's bit x lit; the first of each digit the one drawn by default. The box is 12, 13 or 14 px wide by where the number
  // stands: the lone 8 (13 px) and the 8 of 18 (12 px), the 3 of 3, 13 and 30, the 0 of 10 and 30
  struct pad_glyph_t
    {
    char digit;
    int width;
    std::array<std::uint16_t, 18> rows;
    };

  inline constexpr pad_glyph_t pad_glyphs[]{
      {'1', 8, {0x00f0, 0x00f8, 0x00f8, 0x00dc, 0x00ce, 0x00c7, 0x00c2, 0x00c0, 0x00c0, 0x00c0, 0x00c0, 0x00c0, 0x00c0, 0x00c0, 0x00c0, 0x00c0, 0x00c0, 0x00c0}},  // pad 1
      {'2', 13, {0x03f8, 0x0ffe, 0x0c06, 0x1806, 0x1806, 0x1803, 0x1800, 0x1800, 0x0c00, 0x0f80, 0x07fc, 0x003e, 0x0006, 0x0007, 0x0007, 0x0003, 0x1fff, 0x1fff}},  // pad 2
      {'3', 13, {0x03f8, 0x0ffe, 0x0c06, 0x0c02, 0x1c03, 0x0803, 0x0c00, 0x0c00, 0x07e0, 0x07e0, 0x0c00, 0x1800, 0x1803, 0x1803, 0x1803, 0x0c07, 0x0ffe, 0x03f8}},  // pad 3
      {'4', 15, {0x0e00, 0x0f00, 0x0f80, 0x0dc0, 0x0cc0, 0x0c60, 0x0c70, 0x0c30, 0x0c18, 0x0c0c, 0x0c0e, 0x0c06, 0x7fff, 0x7fff, 0x0c00, 0x0c00, 0x0c00, 0x0c00}},  // pad 4
      {'5', 13, {0x0ffe, 0x0ffe, 0x0006, 0x0006, 0x0006, 0x01e6, 0x07fe, 0x0ffe, 0x1c06, 0x1800, 0x1800, 0x1800, 0x1800, 0x1803, 0x1806, 0x0c06, 0x0ffe, 0x03f8}},  // pad 5
      {'6', 13, {0x03f8, 0x0ffe, 0x0c0e, 0x1806, 0x1807, 0x0007, 0x0007, 0x07f7, 0x0fff, 0x1c07, 0x1807, 0x1803, 0x1803, 0x1803, 0x1806, 0x1c06, 0x0ffc, 0x07f8}},  // pad 6
      {'7', 13, {0x1fff, 0x1fff, 0x1800, 0x0c00, 0x0c00, 0x0600, 0x0700, 0x0300, 0x0380, 0x0180, 0x01c0, 0x00c0, 0x0060, 0x0070, 0x0030, 0x0038, 0x0018, 0x001c}},  // pad 7
      {'8', 13, {0x03f8, 0x0ffe, 0x1c06, 0x1806, 0x1803, 0x1802, 0x1806, 0x0c06, 0x07fc, 0x0ffe, 0x1c06, 0x1803, 0x1803, 0x1803, 0x1803, 0x1c06, 0x0ffe, 0x07f8}},  // pad 8
      {'9', 13, {0x03f8, 0x0ffe, 0x0c06, 0x1803, 0x1803, 0x1803, 0x1803, 0x1803, 0x1c06, 0x1ffe, 0x1bf8, 0x1800, 0x1800, 0x1803, 0x1803, 0x1c07, 0x0ffe, 0x03f8}},  // pad 9
      {'0', 14, {0x07f8, 0x1ffc, 0x180e, 0x3806, 0x3003, 0x3003, 0x3003, 0x3003, 0x3003, 0x3003, 0x3003, 0x3003, 0x3003, 0x3003, 0x1806, 0x1806, 0x1ffc, 0x07f8}},  // pad 10, 2nd
      {'3', 13, {0x03f8, 0x0ffc, 0x1c0e, 0x1806, 0x1806, 0x1806, 0x1800, 0x0c00, 0x0fe0, 0x0fe0, 0x1c00, 0x1800, 0x1803, 0x1803, 0x1806, 0x1c06, 0x0ffe, 0x07f8}},  // pad 13, 2nd
      {'8', 12, {0x03fc, 0x07fe, 0x0e07, 0x0c03, 0x0c03, 0x0c03, 0x0c03, 0x0c07, 0x07fe, 0x07fe, 0x0c03, 0x0c03, 0x0c03, 0x0c03, 0x0c03, 0x0c03, 0x0fff, 0x03fc}},  // pad 18, 2nd
      {'3', 12, {0x03fc, 0x07fe, 0x0e07, 0x0c03, 0x0c03, 0x0c03, 0x0c00, 0x0e00, 0x07e0, 0x07e0, 0x0c00, 0x0c00, 0x0c03, 0x0c03, 0x0c03, 0x0c03, 0x0ffe, 0x03fc}},  // pad 30, 1st
      {'0', 14, {0x07f8, 0x0ffe, 0x1c06, 0x1803, 0x1803, 0x3003, 0x3003, 0x3003, 0x3003, 0x3003, 0x3003, 0x3803, 0x3803, 0x3803, 0x1803, 0x1c06, 0x0ffe, 0x07f8}},  // pad 30, 2nd
  };

  struct pad_box_t
    {
    std::vector<std::uint8_t> px = std::vector<std::uint8_t>(static_cast<std::size_t>(edworld::pad_text_width) * edworld::pad_text_height * 4u);

    pad_box_t()
      {
      for(std::size_t i{}; i != px.size(); i += 4u)
        px[i] = px[i + 1u] = px[i + 2u] = 12u, px[i + 3u] = 128u;
      }

    auto set(int x, int y) -> void
      {
      std::size_t const at{(static_cast<std::size_t>(y) * edworld::pad_text_width + static_cast<std::size_t>(x)) * 4u};
      px[at] = px[at + 1u] = px[at + 2u] = 240u;
      px[at + 3u] = 255u;
      }

    auto block(int x, int y, int w, int h) -> int  ///< a glyph like a letter, its right edge returned
      {
      for(int j{}; j != h; ++j)
        for(int i{}; i != w; ++i)
          set(x + i, y + j);
      return x + w;
      }

    auto glyph(int x, int y, pad_glyph_t const & g) -> int
      {
      for(int j{}; j != 18; ++j)
        for(int i{}; i != g.width; ++i)
          if(g.rows[static_cast<std::size_t>(j)] >> i & 1u)
            set(x + i, y + j);
      return x + g.width;
      }

    ///\brief the digit's first glyph, the n-th of that digit with `nth`
    auto digit(int x, int y, char d, int nth = 0) -> int
      {
      for(pad_glyph_t const & g: pad_glyphs)
        if(g.digit == d and nth-- == 0)
          return glyph(x, y, g);
      return x;
      }

    ///\brief "AAA AA <number> -" over "m:ss", the game's sizes: lines 18 px high, letters 2 px apart, digits 7, words 12;
    /// each digit of the number its `nth` glyph (digits of nth, '0' = the first)
    auto notice(char const * number, char const * clock, char const * nth = nullptr) -> pad_box_t &
      {
      int x{40};
      for(int w: {3, 2})
        {
        for(int i{}; i != w; ++i)
          x = block(x, 19, 12, 18) + 2;
        x += 10;
        }
      for(char const * d{number}; *d; ++d)
        x = digit(x, 19, *d, nth ? nth[d - number] - '0' : 0) + 7;
      block(x + 5, 27, 7, 3);
      x = 170;
      x = digit(x, 47, clock[0]) + 2;
      block(x, 50, 3, 3);
      block(x, 60, 3, 3);
      x += 5;
      x = digit(x, 47, clock[2]) + 2;
      digit(x, 47, clock[3]);
      return *this;
      }

    auto read() const -> edworld::pad_text_read_t
      {
      static edworld::pad_text_bitmap_t b;
      edworld::pad_text_bitmap(px.data(), static_cast<std::uint32_t>(edworld::pad_text_width) * 4u, b);
      return edworld::pad_text_read(b);
      }
    };

  auto pad_text_tests() -> void
    {
    using namespace edworld;
    check(std::size(pad_digits) == 10u, "pad text: a template for each digit");
    pad_text_read_t const two{pad_box_t{}.notice("21", "9:58").read()}, one{pad_box_t{}.notice("7", "0:05").read()};
    check(two.notice and two.sure and two.pad == 21u and two.seconds_left == 9u * 60u + 58u and one.notice and one.pad == 7u
            and one.seconds_left == 5u,
          "pad text: the tower's notice read: the pad's number of one or two digits and the countdown");
    bool every{true};
    for(char const * n: {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "38", "83"})
      {
      pad_text_read_t const r{pad_box_t{}.notice(n, "4:31").read()};
      every = every and r.notice and r.sure and r.pad == static_cast<std::uint32_t>(std::atoi(n)) and r.seconds_left == 271u;
      }
    check(every, "pad text: each digit of the game's own glyphs read sure");
    {
      // the number by where it stands (2026-10-25): the lone 8 and the 8 of 18, the 3 of 13 and of 30, the 0 of 30
      pad_text_read_t const eight{pad_box_t{}.notice("8", "9:59").read()}, eighteen{pad_box_t{}.notice("18", "9:59", "01").read()},
        thirteen{pad_box_t{}.notice("13", "9:59", "01").read()}, thirty{pad_box_t{}.notice("30", "9:59", "21").read()};
      check(eight.sure and eight.pad == 8u and eighteen.sure and eighteen.pad == 18u and thirteen.sure and thirteen.pad == 13u
              and thirty.sure and thirty.pad == 30u,
            "pad text: each glyph of a digit read sure, its box 12, 13 or 14 px wide (8, 18, 13, 30)");
    }
    {
      // a block for a digit is no digit: unsure, no pad from it
      pad_box_t blocked{};
      blocked.notice("4", "9:59");
      blocked.block(130, 19, 15, 18);  // over the 4 (x 130, 15 px wide)
      pad_text_read_t const r{blocked.read()};
      check(r.notice and not r.sure, "pad text: a glyph like no digit read unsure");
    }
    {
      // the zones in per mille, integers: a lit box all 1000; a box 8 px wide lit on its left half: the two left zones
      static pad_text_bitmap_t b{};
      for(int y{}; y != 18; ++y)
        for(int x{}; x != 4; ++x)
          b.on[static_cast<std::size_t>(y + 2) * pad_text_width + static_cast<std::size_t>(x + 3)] = 1u;
      auto const half{detail::pad_text_zones(b, pad_text_glyph_t{3, 11, 2, 20})};
      auto const lit{detail::pad_text_zones(b, pad_text_glyph_t{3, 7, 2, 20})};
      bool ok{true};
      for(std::size_t k{}; k != half.size(); ++k)
        ok = ok and half[k] == (k % 4u < 2u ? 1000u : 0u) and lit[k] == 1000u;
      check(ok, "pad text: the zones' shares of lit pixels in per mille");
    }
    pad_box_t bare{}, one_line{}, no_dash{};
    one_line.block(40, 30, 12, 18);
    one_line.digit(60, 30, '4');
    no_dash.notice("21", "9:58");
    for(int y{27}; y != 30; ++y)  // the dash wiped
      for(int x{0}; x != edworld::pad_text_width; ++x)
        {
        std::size_t const at{(static_cast<std::size_t>(y) * edworld::pad_text_width + static_cast<std::size_t>(x)) * 4u};
        if(x >= 175 and no_dash.px[at] == 240u)
          no_dash.px[at] = no_dash.px[at + 1u] = no_dash.px[at + 2u] = 12u;
        }
    check(not bare.read().notice and not one_line.read().notice and not no_dash.read().notice,
          "pad text: an empty box, one line (LEAVE STATION - m:ss, GRAVITY WELL) or no dash is no notice");
    pad_text_vote_t v{};
    pad_text_read_t const unsure{true, false, 21u, 500u};
    check(v.add(two) == 0u and v.add(two) == 21u and v.add(unsure) == 0u and v.add(two) == 0u and v.add(two) == 21u,
          "pad text: a pad taken when read sure twice in a row");
    // Marth's Inheritance, 2026-10-24: pad 5 at 9:13 left, then misread 9:18 and read 9:12 half a second on - the same
    // grant; asked anew from 9:59 a minute on, or another pad, a new one
    std::int64_t const g{1'000'000'000'000};
    check(not pad_text_new_grant(5u, g + 5000, 5u, g) and not pad_text_new_grant(5u, g - 1000, 5u, g)
            and pad_text_new_grant(5u, g + 46000, 5u, g) and pad_text_new_grant(6u, g, 5u, g) and pad_text_new_grant(5u, g, 0u, 0),
          "pad text: the same pad is a new grant only with its countdown well up; another pad always");
    // the samples of the game, when given (EDWORLD_PADTEXT_DIR: an edworld_padtext directory): game data is not kept here
    if(char const * const dir{std::getenv("EDWORLD_PADTEXT_DIR")})
      {
      std::uint32_t right{}, wrong{}, unsure{}, notices{}, samples{};
      WIN32_FIND_DATAA fd{};
      HANDLE const h{FindFirstFileA((std::string{dir} + "\\pad_*_src*_410x81_pitch*.raw").c_str(), &fd)};
      static pad_text_bitmap_t b;
      for(bool more{h != INVALID_HANDLE_VALUE}; more; more = FindNextFileA(h, &fd) != 0)
        {
        std::string const name{fd.cFileName};
        auto const label{static_cast<std::uint32_t>(std::strtoul(name.c_str() + name.find("_src") + 4u, nullptr, 10))};
        auto const pitch{static_cast<std::uint32_t>(std::strtoul(name.c_str() + name.find("pitch") + 5u, nullptr, 10))};
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(pitch) * pad_text_height);
        std::FILE * f{std::fopen((std::string{dir} + "\\" + name).c_str(), "rb")};
        if(not f)
          continue;
        bool const whole{std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size()};
        std::fclose(f);
        if(not whole)
          continue;
        ++samples;
        pad_text_bitmap(bytes.data(), pitch, b);
        pad_text_read_t const r{pad_text_read(b)};
        notices += r.notice ? 1u : 0u;
        if(r.notice and label != 0u)
          ++(not r.sure ? unsure : r.pad == label ? right : wrong);
        }
      if(h != INVALID_HANDLE_VALUE)
        FindClose(h);
      std::printf("      samples %u, notices %u, the data source's pad read right %u, unsure %u, wrong %u\n", samples, notices, right,
                  unsure, wrong);
      check(samples != 0u and right != 0u and wrong == 0u and unsure == 0u,
            "pad text: the game's samples read sure, as the data source named the pad");
      }
    }
  }  // namespace

int main()
  {
  sample_context_tests();
  pad_text_tests();
  shader_shape_tests();
  holo_models_tests();
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
  dot(cx - 40.f, cy + 10.f, 7.f, 4.4f);  // a ring as the game's since 2026-10-20 (~93 lit pixels; it had ~111)
  auto const hollow{edworld::read_compass(area.data(), w * 4u, w, h, cx, cy, radius * 1.25f)};
  check(hollow.found and not hollow.filled and std::fabs(hollow.x + 40.f) < 0.3f and std::fabs(hollow.y - 10.f) < 0.3f,
        "compass: a hollow dot found and told from a filled one");
  check(edworld::compass_plausible(filled) and edworld::compass_plausible(hollow), "compass: both dots plausible");
  check(not edworld::compass_plausible(edworld::compass_reading_t{true, true, 1.f, 1.f, 4u}) and
          not edworld::compass_plausible(edworld::compass_reading_t{true, false, 1.f, 1.f, 1234u}),
        "compass: a few lit pixels or the whole disc lit is no dot");
  clear();
  check(not edworld::read_compass(area.data(), w * 4u, w, h, cx, cy, radius * 1.25f).found, "compass: no dot on a bare disc");
  check(filled.dot_hue < 0.f and filled.disc_hue > 200.f and filled.disc_hue < 240.f, "compass: a white dot has no hue, the disc its blue");
  // the game's own HUD since its update of 2026-10-20: an orange disc, a cyan dot (130, 255, 255), read off Sjona's
  // surface dumps; the same reading, no colour of its own
  auto const paint = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b, float x, float y, float outer, float inner)
    {
    for(std::int32_t py{}; py != h; ++py)
      for(std::int32_t px{}; px != w; ++px)
        {
        float const d{std::hypot(static_cast<float>(px) - x, static_cast<float>(py) - y)};
        if(d <= outer and d >= inner)
          {
          std::uint8_t * const p{&area[(static_cast<std::size_t>(py) * w + px) * 4u]};
          p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
          }
        }
    };
  auto const orange_disc = [&]
    {
    std::fill(area.begin(), area.end(), std::uint8_t{0});
    paint(60, 25, 5, cx, cy, radius, 0.f);           // the dark grid's orange
    paint(230, 110, 20, cx, cy, radius + 2.f, radius - 3.f);  // the rim
    };
  orange_disc();
  paint(130, 255, 255, cx + 1.f, cy + 21.f, 8.f, 0.f);
  auto const cyan{edworld::read_compass(area.data(), w * 4u, w, h, cx, cy, radius * 1.25f)};
  check(cyan.found and cyan.filled and std::fabs(cyan.x - 1.f) < 0.3f and std::fabs(cyan.y - 21.f) < 0.3f and
          edworld::compass_plausible(cyan),
        "compass: a cyan dot on the game's orange disc, read and plausible");
  check(std::fabs(cyan.dot_hue - 180.f) < 1.f and std::fabs(cyan.disc_hue - 25.f) < 0.1f, "compass: the dot's and the disc's hues");
  orange_disc();
  paint(130, 255, 255, cx + 4.f, cy + 53.f, 8.f, 0.f);  // on the rim: 90 degrees below the nose
  auto const on_rim{edworld::read_compass(area.data(), w * 4u, w, h, cx, cy, radius * 1.25f)};
  check(on_rim.found and on_rim.filled and std::fabs(on_rim.y - 53.f) < 0.5f, "compass: a dot over the rim, told by its hue");
  orange_disc();
  paint(130, 255, 255, cx + 3.f, cy + 23.f, 7.f, 4.4f);
  auto const ring{edworld::read_compass(area.data(), w * 4u, w, h, cx, cy, radius * 1.25f)};
  check(ring.found and not ring.filled and edworld::compass_plausible(ring), "compass: a cyan ring on the orange disc, hollow and plausible");
  std::fill(area.begin(), area.end(), std::uint8_t{0});
  paint(130, 255, 255, cx, cy, radius + 2.f, radius - 3.f);  // the compass flashing in the dot's colour
  paint(130, 255, 255, cx + 1.f, cy + 21.f, 8.f, 0.f);
  auto const flash{edworld::read_compass(area.data(), w * 4u, w, h, cx, cy, radius * 1.25f, 180.f)};
  check(flash.flash and not flash.found and not edworld::compass_plausible(flash), "compass: a disc in the dot's hue is a flash, not read");
  auto const first{edworld::read_compass(area.data(), w * 4u, w, h, cx, cy, radius * 1.25f)};
  check(not first.flash, "compass: no flash told without a dot shown before");
  check(edworld::compass_fresh(true, 0.5, 1.f) and not edworld::compass_fresh(true, 1.5, 1.f) and edworld::compass_fresh(true, 100.0, 0.f)
          and not edworld::compass_fresh(false, 0.0, 1.f),
        "compass: a reading holds its second, for ever with 0, never without one");
  check(std::fabs(edworld::angle_between(edworld::direction_of(0.f, 0.f), edworld::direction_of(30.f, 0.f)) - 30.f) < 0.01f,
        "compass: the angle between two directions");
  {
  // the target's hologram (2026-10-20, Brescia Platform): the record's orientation as unorm16, docked and in flight
  auto const record = [](std::uint16_t a, std::uint16_t b, std::uint16_t c, std::uint16_t d)
    {
    std::uint8_t r[16]{};
    std::uint16_t const u[4]{a, b, c, d};
    std::memcpy(r + 8, u, sizeof u);
    return edworld::hologram_orientation(r);
    };
  auto const length = [](edworld::quat_t const & q) { return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); };
  edworld::quat_t const docked{record(32733, 56926, 32804, 54904)}, flying{record(25262, 20617, 30955, 62202)};
  check(std::fabs(length(docked) - 1.f) < 1e-3f and std::fabs(length(flying) - 1.f) < 1e-3f, "hologram: the decoded orientations are unit");
  check(std::fabs(flying.x + 0.2290f) < 1e-3f and std::fabs(flying.y + 0.3708f) < 1e-3f and std::fabs(flying.w - 0.8983f) < 1e-3f,
        "hologram: the orientation as the shader decodes it (u / 32767 - 1)");
  edworld::vec3_t const x{edworld::rotate(flying, {1.f, 0.f, 0.f})};
  check(std::fabs(x.x - 0.7189f) < 2e-3f and std::fabs(x.y - 0.0705f) < 2e-3f and std::fabs(x.z - 0.6915f) < 2e-3f,
        "hologram: q v q* turns X to the rotation's first column");
  float const ortho[4][4]{{2.f, 0.f, 0.f, 0.f}, {0.f, 3.f, 0.f, 0.f}, {0.f, 0.f, 0.5f, 0.f}, {7.f, 8.f, 0.1f, 1.f}};
  auto const cam{edworld::camera_axes(ortho)};
  check(cam and std::fabs(cam->right.x - 1.f) < 1e-5f and std::fabs(cam->up.y - 1.f) < 1e-5f and std::fabs(cam->forward.z - 1.f) < 1e-5f,
        "hologram: an orthographic camera's axes from its columns");
  // a perspective camera turned 90 degrees to the right: it looks along world +X, its right is world -Z
  float const turned[4][4]{{0.f, 0.f, 0.5f, 1.f}, {0.f, 1.f, 0.f, 0.f}, {-1.f, 0.f, 0.f, 0.f}, {0.f, 0.f, -0.1f, 0.f}};
  auto const persp{edworld::camera_axes(turned)};
  check(persp and std::fabs(persp->forward.x - 1.f) < 1e-5f and std::fabs(persp->right.z + 1.f) < 1e-5f and std::fabs(persp->up.y - 1.f) < 1e-5f,
        "hologram: a perspective camera's forward from its w column");
  auto const axes{edworld::hologram_axes(edworld::quat_t{0.f, 0.f, 0.f, 1.f}, *persp)};
  check(std::fabs(axes[0].z - 1.f) < 1e-5f and std::fabs(axes[2].x + 1.f) < 1e-5f and std::fabs(axes[1].y - 1.f) < 1e-5f,
        "hologram: the model's X ahead and Z to the left for that camera");
  float const empty[4][4]{};
  check(not edworld::camera_axes(empty), "hologram: no camera from empty rows");
  // the pad's plane (2026-10-21): X from the logs on the pad at Brewer Site (up) and over it at Brescia Platform (down)
  check(edworld::pad_tilt_deg({0.f, 1.f, 0.f}) < 0.01f and edworld::pad_tilt_deg({0.f, -1.f, 0.f}) < 0.01f,
        "pad plane: level whichever way X points");
  check(std::fabs(edworld::pad_tilt_deg({0.01f, 0.99f, -0.14f}) - 8.1f) < 0.2f, "pad plane: Brewer's approach, about 8 degrees");
  check(std::fabs(edworld::pad_tilt_deg({1.f, 0.f, 0.f}) - 90.f) < 0.01f and edworld::pad_tilt_deg({0.f, 0.f, 0.f}) == 90.f,
        "pad plane: square to the ship, and no normal");
  check(edworld::pad_up_sign({0.f, 1.f, 0.f}, {0.f, -1.f, 0.f}) == 1 and edworld::pad_up_sign({0.f, -1.f, 0.f}, {0.f, -1.f, 0.f}) == -1,
        "pad plane: the pads' up towards the ship, the target below (Brewer +X, Brescia -X)");
  check(edworld::pad_up_sign({0.f, 1.f, 0.f}, {0.f, -0.1f, 1.f}) == 0 and edworld::pad_up_sign({0.f, 1.f, 0.f}, {0.f, 0.f, 0.f}) == 0,
        "pad plane: no sign with the target near the pads' plane, nor without a direction");
  check(std::fabs(edworld::pad_tilt_deg({0.f, -1.f, 0.f}, 1) - 180.f) < 0.01f and edworld::pad_tilt_deg({0.f, -1.f, 0.f}, -1) < 0.01f,
        "pad plane: upside down over the pads is 180 degrees once the up is known");
  check(std::fabs(edworld::pad_tilt_deg({0.f, -0.5f, 0.866f}, 1) - 120.f) < 0.1f and
          std::fabs(edworld::pad_tilt_deg({0.f, -0.5f, 0.866f}) - 60.f) < 0.1f,
        "pad plane: 120 degrees stays 120 with the up known, folds to 60 without");
  {
    // Dunkan at a construction site (2026-10-22): the Panther's own hologram first (smaller scale), the site's second
    edworld::holo_part_t const site[]{{0.0024f, 106830u, false}, {0.009f, 13083u, true}};
    check(edworld::holo_target_part(site, 2u) == 1, "hologram: the target is drawn to the surface laid over second, not the smaller scale");
    // Sjona at an outpost (2026-10-21): the Kestrel, then the station's parts; the smallest scale of them, most indices
    edworld::holo_part_t const outpost[]{{0.0055f, 27885u, false}, {0.0004f, 780u, true}, {0.0004f, 3534u, true}, {0.0009f, 5238u, true}};
    check(edworld::holo_target_part(outpost, 4u) == 2, "hologram: of the target's parts the smallest scale, the most indices among equal ones");
    edworld::holo_part_t const own[]{{0.0024f, 106830u, false}};
    check(edworld::holo_target_part(own, 1u) == -1 and edworld::holo_target_part(own, 0u) == -1,
          "hologram: no target without its surface");
    edworld::holo_part_t const unread[]{{0.f, 13083u, true}};
    check(edworld::holo_target_part(unread, 1u) == -1, "hologram: a target's draw without a scale read is no target");
    // the target by what it shows (Artur 2026-10-25): Dunkan at Ahn Hub after a ship swap, the Kestrel laid over second
    // and the port (2760, a place) first
    edworld::holo_part_t const swapped[]{{0.0055f, 27885u, true, false, false}, {0.0004f, 2760u, false, true, true}};
    check(edworld::holo_target_part(swapped, 2u) == 1, "hologram: a place on the own surface is the target when the target's shows a ship");
    edworld::holo_part_t const both[]{{0.0004f, 2760u, false, true, true}, {0.0009f, 13083u, true, false, true}};
    check(edworld::holo_target_part(both, 2u) == 1, "hologram: a place on the target's surface before one on the own");
    edworld::holo_part_t const ships[]{{0.0055f, 27885u, false, true, false}, {0.0024f, 106830u, true, false, false},
                                       {0.0030f, 62688u, true, false, false}};
    check(edworld::holo_target_part(ships, 3u) == 1, "hologram: two ships: the target's surface, the smallest scale");
    edworld::holo_part_t const unknown[]{{0.0004f, 9999u, false, true, false}, {0.0055f, 27885u, true, false, false}};
    check(edworld::holo_target_part(unknown, 2u) == 1, "hologram: an unknown model on the own surface is no place: the target's surface");
    edworld::holo_part_t const aside[]{{0.0004f, 2760u, false, false, true}};
    check(edworld::holo_target_part(aside, 1u) == -1, "hologram: a place on a surface not laid over is no target");
    edworld::holo_part_t const own_place[]{{0.0004f, 84969u, false, true, true}};
    check(edworld::holo_target_part(own_place, 1u) == 0, "hologram: a carrier drawn to the own surface at its pad is the target");
  }
  check(edworld::pad_tilt_deg({-0.001f, 1.f, -0.003f}) < 0.5f and std::fabs(edworld::pad_tilt_deg({0.996f, 0.f, -0.087f}) - 90.f) < 0.1f,
        "pad plane: hovering level over Carrel Agricultural, level on Y and square on X");
  check(edworld::t1_pad(5478u, 5u) and edworld::t1_pad(5478u, 5u)->up_axis == 1 and edworld::t1_pad(5478u, 5u)->up_sign == -1 and
          edworld::t1_pad(1953u, 4u)->up_axis == 0 and edworld::t1_pad(1953u, 4u)->up_sign == -1 and
          not edworld::t1_pad(5478u, 6u) and not edworld::t1_pad(27885u, 1u),
        "pad plane: the T1 table's pads (Civilian 5, Commercial 4), none for a pad or model not in it");
  {
    float head{-1.f};
    bool head_first{true};
    for(edworld::pad_mark_segment_t const & m: edworld::pad_mark)
      head = std::max(head, std::max(m.a, m.c));
    head_first = edworld::pad_mark[0].a == head and edworld::pad_mark[1].a == head and std::size(edworld::pad_mark) == 5u;
    check(head_first, "pad plane: the mark < ||| has its head furthest towards the nose, then three bars");
  }
  {
    bool rule{true};  // the ship's frame on each pad: up, nose and right on three different axes, so no entry repeats an axis
    for(edworld::t1_pad_t const & e: edworld::t1_pads)
      rule = rule and e.up_axis != e.nose_axis and (e.up_sign == 1 or e.up_sign == -1) and (e.nose_sign == 1 or e.nose_sign == -1);
    check(rule, "pad plane: every T1 entry's up and nose on two different axes");
  }
  {
    // the pad's text: the box's fill (alpha 136, dark) as in the dumps, white text over a fifth, and an empty surface
    std::vector<std::uint8_t> box(static_cast<std::size_t>(edworld::pad_box_width) * edworld::pad_box_height * 4u);
    for(std::size_t i{}; i < box.size(); i += 4u)
      {
      box[i] = 8u;
      box[i + 1u] = 12u;
      box[i + 2u] = 16u;
      box[i + 3u] = 136u;
      }
    auto const pitch{static_cast<std::uint32_t>(edworld::pad_box_width) * 4u};
    check(edworld::pad_box_present(box.data(), pitch, edworld::pad_box_width, edworld::pad_box_height), "pad text: the box's fill shows");
    for(std::size_t i{}; i < box.size() / 5u; i += 4u)
      box[i] = box[i + 1u] = box[i + 2u] = box[i + 3u] = 255u;
    check(edworld::pad_box_present(box.data(), pitch, edworld::pad_box_width, edworld::pad_box_height), "pad text: with its text over a fifth");
    std::vector<std::uint8_t> const empty(box.size());
    check(not edworld::pad_box_present(empty.data(), pitch, edworld::pad_box_width, edworld::pad_box_height) and
            not edworld::pad_box_present(nullptr, pitch, 1, 1),
          "pad text: no box on an empty surface, nor without pixels");
    std::vector<std::uint8_t> kept(static_cast<std::size_t>(edworld::pad_box_width) * edworld::pad_box_height);
    for(std::size_t i{}; i != kept.size(); ++i)
      kept[i] = box[i * 4u + 3u];
    check(not edworld::pad_box_changed(box.data(), pitch, edworld::pad_box_width, edworld::pad_box_height, kept.data()),
          "pad text: the same text is no change");
    for(std::size_t i{box.size() / 2u}; i < box.size() * 3u / 5u; i += 4u)
      box[i + 3u] = 255u;
    check(edworld::pad_box_changed(box.data(), pitch, edworld::pad_box_width, edworld::pad_box_height, kept.data()) and
            edworld::pad_box_changed(box.data(), pitch, edworld::pad_box_width, edworld::pad_box_height, nullptr),
          "pad text: another text over a tenth is a change, and so is the first");
    check(edworld::pad_burst_due(2000u, 1000u, 11500u) and not edworld::pad_burst_due(1500u, 1000u, 11500u) and
            not edworld::pad_burst_due(11600u, 10600u, 11500u),
          "pad text: a burst's sample a second, none after its end");
  }
  {
  // the vote on the pads' up (v7): Brewer 09:37 - the pads' normal X mostly the ship's up, one look from a climb away
  // with the target behind, then looks ahead from the way down
  using edworld::pad_up_event_e;
  edworld::compass_direction_t const x{0.f, 1.f, 0.f};
  edworld::pad_up_vote_t v{};
  check(pad_up_vote(v, x, {0.f, 0.3f, -1.f}) == pad_up_event_e::none and v.plus + v.minus == 0,
        "pad up vote: no vote with the target behind (a hollow dot)");
  check(pad_up_vote(v, x, {0.f, 0.05f, 1.f}) == pad_up_event_e::none and v.plus + v.minus == 0,
        "pad up vote: no vote with the target near the pads' plane");
  check(pad_up_vote(v, x, {0.f, -0.5f, 1.f}) == pad_up_event_e::none and v.plus == 1, "pad up vote: one vote from ahead");
  check(pad_up_vote(v, x, {0.f, -0.51f, 1.f}) == pad_up_event_e::none and v.plus == 1,
        "pad up vote: no second vote from nearly the same direction (under 3 degrees)");
  check(pad_up_vote(v, x, {0.f, -0.8f, 1.f}) == pad_up_event_e::none and v.plus == 2, "pad up vote: a second from another angle");
  check(pad_up_vote(v, x, {0.f, -1.2f, 1.f}) == pad_up_event_e::decided and v.decided == 1 and v.plus == 3,
        "pad up vote: three agreeing decide +X");
  edworld::pad_up_vote_t split{};
  pad_up_vote(split, x, {0.f, -0.5f, 1.f});
  pad_up_vote(split, x, {0.f, 0.5f, 1.f});
  pad_up_vote(split, x, {0.2f, -0.8f, 1.f});
  pad_up_vote(split, x, {0.2f, 0.8f, 1.f});
  check(split.decided == 0 and split.plus == 2 and split.minus == 2, "pad up vote: two to two decide nothing");
  check(pad_up_vote(split, x, {-0.3f, -1.f, 1.f}) == pad_up_event_e::decided and split.decided == 1,
        "pad up vote: a fifth settles two to two");
  edworld::pad_up_vote_t wrong{};
  pad_up_vote(wrong, x, {0.f, 0.5f, 1.f});
  pad_up_vote(wrong, x, {0.f, 0.8f, 1.f});
  pad_up_vote(wrong, x, {0.f, 1.2f, 1.f});
  check(wrong.decided == -1, "pad up vote: a wrong start decides -X");
  check(pad_up_vote(wrong, x, {0.f, -0.3f, 1.f}) == pad_up_event_e::none and wrong.against == 0,
        "pad up vote: a weak vote against does not count towards turning");
  pad_up_vote(wrong, x, {0.f, -1.f, 1.f});
  pad_up_vote(wrong, x, {0.f, -1.5f, 1.f});
  check(pad_up_vote(wrong, x, {0.f, -2.5f, 1.f}) == pad_up_event_e::turned and wrong.decided == 1,
        "pad up vote: three strong votes in a row against turn it to +X");
  }
  check(edworld::pad_granted(1000, 1000 + 9 * 60 * 1000) and not edworld::pad_granted(1000, 1000 + 10 * 60 * 1000)
          and not edworld::pad_granted(0, 5000),
        "pad granted: held ten minutes at most, none without a grant");
  check(edworld::pad_upside_down(120.f, 1) and not edworld::pad_upside_down(90.f, 1) and not edworld::pad_upside_down(60.f, -1)
          and not edworld::pad_upside_down(120.f, 0),
        "pad plane: upside down past 90 degrees, only with the pads' up known");
  check(edworld::pad_tilt_colour(4.f, 10.f, 20.f) == 0x40c060u and edworld::pad_tilt_colour(10.f, 10.f, 20.f) == 0x40c060u,
        "pad plane: green up to the first limit");
  check(edworld::pad_tilt_colour(15.f, 10.f, 20.f) == 0xd8c040u and edworld::pad_tilt_colour(25.f, 10.f, 20.f) == 0xd04030u,
        "pad plane: yellow half way, red past the second limit");
  std::uint32_t const early{edworld::pad_tilt_colour(12.5f, 10.f, 20.f)};
  check(early != 0x40c060u and early != 0xd8c040u and ((early >> 16) & 0xffu) > 0x40u and ((early >> 16) & 0xffu) < 0xd8u,
        "pad plane: a hue between green and yellow, not a step");
  check(edworld::pad_tilt_colour(12.f, 10.f, 10.f) == 0xd04030u, "pad plane: equal limits make a plain step");
  using edworld::holo_place_e;
  check(edworld::holo_place_has_plane(holo_place_e::outpost_t1) and edworld::holo_place_has_plane(holo_place_e::planet) and
          edworld::holo_place_has_plane(holo_place_e::carrier),
        "pad plane: orbital outposts, places on a planet and carriers show their pads");
  check(not edworld::holo_place_has_plane(holo_place_e::big_station) and not edworld::holo_place_has_plane(holo_place_e::none) and
          not edworld::holo_place_has_plane(holo_place_e::construction_orbital),
        "pad plane: not the big stations, no model known, nor the orbital construction site yet");
  check(edworld::pad_planes_of(84969u) and edworld::pad_planes_of(73389u) and not edworld::pad_planes_of(5238u),
        "pad plane: the squadron carrier and a fleet carrier have their rows of planes, a T1 outpost none");
  // a fleet carrier's pads by number, against the ship's up measured on them (Maria Sklodowska, 2026-10-24)
  {
  edworld::pad_planes_t const & fc{*edworld::pad_planes_of(73389u)};
  std::array<edworld::compass_direction_t, 3> const model{{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}}};
  auto const off = [](edworld::compass_direction_t const & a, float x, float y, float z) -> float
    {
    float const d{(a.x * x + a.y * y + a.z * z) / std::sqrt(x * x + y * y + z * z)};
    return std::acos(std::clamp(d, -1.f, 1.f)) * 57.29578f;
    };
  auto const plane_of = [&](std::uint32_t pad) { return edworld::pick_pad_plane(fc, model, 1000.f, edworld::pad_plane_of_pad(73389u, pad)); };
  auto const p2{plane_of(2u)}, p7{plane_of(7u)}, p13{plane_of(13u)}, p11{plane_of(11u)};
  check(off(p2.axes[0], 0.015f, 0.984f, 0.176f) < 1.f and off(p7.axes[0], 0.015f, 0.984f, 0.176f) < 1.f and
          off(p13.axes[0], -0.383f, 0.924f, 0.003f) < 1.f and off(p11.axes[0], 0.383f, 0.924f, 0.003f) < 1.f,
        "fleet carrier: the planes of pads 2, 7, 13 and 11 are the ship's up measured on them, within a degree");
  check(off(p2.axes[1], -0.004f, -0.176f, 0.984f) < 1.f and off(p13.axes[1], 0.f, 0.f, 1.f) < 1.f and fc.nose_along,
        "fleet carrier: the nose along its length, on the deck leaning with it");
  }
  check(edworld::holo_place_of_station_type("Outpost") == holo_place_e::outpost_t1 and
          edworld::holo_place_of_station_type("CraterOutpost") == holo_place_e::planet and
          edworld::holo_place_of_station_type("OnFootSettlement") == holo_place_e::planet and
          edworld::holo_place_of_station_type("PlanetaryConstructionDepot") == holo_place_e::planet and
          edworld::holo_place_of_station_type("FleetCarrier") == holo_place_e::carrier and
          edworld::holo_place_of_station_type("Coriolis") == holo_place_e::big_station and
          edworld::holo_place_of_station_type("Bernal") == holo_place_e::big_station and
          edworld::holo_place_of_station_type("SpaceConstructionDepot") == holo_place_e::construction_orbital and
          edworld::holo_place_of_station_type("MegaShip") == holo_place_e::none and
          edworld::holo_place_of_station_type("") == holo_place_e::none,
        "place: the journal's StationType as the kind of place edworld_eht checks the model's against");
  // the registry's places: the T1 parts with the pads' table, the construction site, the carrier, the Coriolis
  auto const place_of = [](std::uint32_t count) -> holo_place_e
    {
    edworld::holo_resolved_t const r{
      edworld::holo_resolve(edworld::holo_models.data(), edworld::holo_models.size(), count, edworld::holo_build_2026_10_13, nullptr)};
    return edworld::holo_place_of(r, edworld::holo_models.data());
    };
  check(place_of(5238u) == holo_place_e::outpost_t1 and place_of(7023u) == holo_place_e::outpost_t1 and
          place_of(13083u) == holo_place_e::planet and place_of(4596u) == holo_place_e::planet and
          place_of(84969u) == holo_place_e::carrier and place_of(948u) == holo_place_e::big_station and
          place_of(3576u) == holo_place_e::none and place_of(27885u) == holo_place_e::none and place_of(1u) == holo_place_e::none,
        "place: the measured models' kinds of place, none for a ship, the T1 station part and a count not known");
  // E: a place on a planet is flat, its up the model's thinnest axis (the three known models, 2026-10-24)
  {
  edworld::holo_flat_t const site{edworld::holo_flat_of(std::array<float, 3>{1.00f, 0.33f, 0.97f})};
  edworld::holo_flat_t const crater{edworld::holo_flat_of(std::array<float, 3>{0.01f, 0.79f, 1.00f})};
  edworld::holo_flat_t const park{edworld::holo_flat_of(std::array<float, 3>{0.01f, 1.00f, 0.34f})};
  edworld::holo_flat_t const block{edworld::holo_flat_of(std::array<float, 3>{1.00f, 0.80f, 0.90f})};
  check(site.axis == 1 and crater.axis == 0 and park.axis == 0 and site.ratio < 0.35f and crater.ratio < 0.02f and
          park.ratio < 0.04f and block.axis == 1 and block.ratio > edworld::holo_flat_ratio_max,
        "planet: the thinnest axis of the construction site (Y), the crater outpost and Park Site (X); a block is no slab");
  edworld::holo_resolved_t const none{};
  check(edworld::holo_flat_of(none, edworld::holo_models.data()).axis == -1,
        "planet: no thinnest axis for a draw of no model");
  // a confirmed pad's plane from the thinnest axis is the one 13083's table gave: up Y, about Z, towards X
  edworld::pad_planes_t const * const table{edworld::pad_planes_of(13083u)};
  edworld::pad_planes_t const slab{13083u, 1, 2, 0, 1u, {0.f}, false};
  float const tilted{7.f};
  std::array<edworld::compass_direction_t, 3> const model{{{0.f, 0.f, 1.f}, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}}};
  auto const a{edworld::pick_pad_plane(*table, model, 1000.f, &tilted)};
  auto const b{edworld::pick_pad_plane(slab, model, 1000.f, &tilted)};
  check(table and std::fabs(a.axes[0].x - b.axes[0].x) < 1e-6f and std::fabs(a.axes[0].y - b.axes[0].y) < 1e-6f and
          std::fabs(a.axes[0].z - b.axes[0].z) < 1e-6f and std::fabs(a.axes[1].z - b.axes[1].z) < 1e-6f,
        "planet: a confirmed pad's plane from the thinnest axis is the table's of before");
  }
  }
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
  auto const over{edworld::glide_to(10.0, 20.0, 5000.0, 10.0, 20.0, 1.0e6)};
  check(std::fabs(over.below - 90.0) < 1e-9 and over.distance == 0.0, "glide: the place under the ship is straight down");
  // 0.01 rad over a sphere of 1000 km, 1 km up: the ground falls away by 50 m on the way
  auto const ten{edworld::glide_to(0.0, 0.0, 1000.0, 0.0, 0.57295779513, 1.0e6)};
  check(std::fabs(ten.distance - 10000.0) < 0.1 and std::fabs(ten.below - 5.994) < 0.01,
        "glide: 10 km away and 1 km up is 6 degrees down, the curve included");
  auto const level{edworld::glide_to(0.0, 0.0, 0.0, 0.0, 0.57295779513, 1.0e6)};
  check(level.below > 0.28 and level.below < 0.29, "glide: from the ground a place 10 km off lies under the horizon");
  auto const ahead{edworld::compass_direction(0.f, 0.f, radius, true)};
  check(std::fabs(ahead.z - 1.f) < 1e-6f and std::fabs(ahead.x) < 1e-6f, "spheres: the dot in the middle is the nose");
  auto const d{edworld::compass_direction(0.f, 27.f, radius, true)};
  auto const back{edworld::direction_of(-30.f, 0.f)};
  check(std::fabs(d.y - back.y) < 1e-5f and std::fabs(d.z - back.z) < 1e-5f, "spheres: the dot and the angles give one direction");
  // the gates down (PoC), every angle from the planet's visible edge: the dive point at the ship's height above Berdo
  // Forge (R 1957 km) sees the place 17 degrees below the edge
  {
  double const R{1957252.5};
  auto const start{edworld::descent_start(-32.877357, 62.797558, 432586.0, -26.187031, 47.741756, R, 17.0)};
  check(start.found and std::fabs(edworld::below_limb(start.point, edworld::planet_point(-26.187031, 47.741756, R), R) - 17.0) < 1e-6
          and std::fabs(edworld::length(start.point) - R - 432586.0) < 1e-3,
        "gates: from the start, at the ship's height, the place stands 17 degrees below the edge");
  check(not edworld::descent_start(0.0, 0.0, 2.5 * R, 1.0, 1.0, R, 17.0).found and edworld::descent_start(0.0, 0.0, 2.3 * R, 1.0, 1.0, R, 17.0).found,
        "gates: from above R / sin(17) = 3.42 R no point sees the place 17 below the edge");
  // the recorded dives (Status.json 2026-10-11): Moller's Fortress (R 504 km) at 16:21:45, the nose put down, the place
  // 18 below the edge; Kisekka Tourist Lodge (R 5391 km) at 15:19:58, still flat, 11.7 below
  double const RM{504163.4375}, RK{5391284.0};
  edworld::dvec3_t const moller{edworld::planet_point(0.248625, 148.506454, RM)}, kisekka{edworld::planet_point(-11.595967, -135.406815, RK)};
  check(std::fabs(edworld::below_limb(edworld::planet_point(10.273772, 157.013962, RM + 221041.0), moller, RM) - 18.0) < 0.05,
        "gates: Moller's dive started with the place 18 degrees below the edge");
  auto const flat{edworld::approach_path(-7.162842, -154.094452, 2834866.0, -11.595967, -135.406815, RK, 17.0, 0.3, 30.0, 90.0)};
  bool flat_ok{flat.phase == edworld::approach_phase_e::setup and flat.count == 4u and flat.dive_from == 2u
               and std::fabs(flat.below_limb - 11.70) < 0.05};
  if(flat_ok)
    {
    // E at 0.3 R sees the place 17 below the edge; the line Q-E has the nose on the edge seen from E; the turn at Q is 30
    edworld::dvec3_t const e{flat.point[2]}, qp{flat.point[1]};
    edworld::dvec3_t const f{edworld::unit(e - qp)};
    double const nose_below{std::asin(-edworld::dot(f, edworld::unit(e))) / edworld::gate_rad};
    double const turn{std::acos(edworld::dot(edworld::unit(qp - flat.point[0]), f)) / edworld::gate_rad};
    flat_ok = std::fabs(edworld::length(e) - 1.3 * RK) < 1e-3 and std::fabs(edworld::below_limb(e, kisekka, RK) - 17.0) < 1e-6
              and std::fabs(nose_below - edworld::limb_dip(1.3 * RK, RK) / edworld::gate_rad) < 1e-6 and std::fabs(turn - 30.0) < 1e-6;
    }
  check(flat_ok, "gates: Kisekka still flat: to Q with a 30 degree turn, along the line with the nose on the edge, E at 0.3 R");
  auto const moller_dive{edworld::approach_path(10.273772, 157.013962, 221041.0, 0.248625, 148.506454, RM, 17.0, 0.3, 30.0, 73.0)};
  check(moller_dive.phase == edworld::approach_phase_e::dive and moller_dive.count == 2u,
        "gates: Moller at 18 below the edge (64 below the horizontal): the dive, within the steepest 73");
  // head on from 14 R: no Q gives a turn of 30 or less, straight to E (a turn of 64 there); from 2 R straight over the
  // place 19.5 below the edge: the dive, straight down, unless the steepest dive is limited
  auto const head_on{edworld::approach_path(-11.595967, -135.406815, 14.0 * RK, -11.595967, -135.406815, RK, 17.0, 0.3, 30.0, 90.0)};
  check(head_on.phase == edworld::approach_phase_e::setup and head_on.count == 3u and head_on.dive_from == 1u and head_on.turn > 60.0 and head_on.turn < 66.0,
        "gates: head on from far, straight to E and a sharp turn there");
  auto const over{edworld::approach_path(-11.595967, -135.406815, 2.0 * RK, -11.595967, -135.406815, RK, 17.0, 0.3, 30.0, 90.0)};
  auto const over_limited{edworld::approach_path(-11.595967, -135.406815, 2.0 * RK, -11.595967, -135.406815, RK, 17.0, 0.3, 30.0, 73.0)};
  check(over.phase == edworld::approach_phase_e::dive and over_limited.phase == edworld::approach_phase_e::setup,
        "gates: straight over the place from 2 R the dive is vertical, set up instead with the steepest at 73");
  auto const pg{edworld::path_gates(flat, 12u, 0.05, 50.0)};
  bool pg_ok{pg.count == 12u and edworld::length(pg.gate[0].gate.centre - flat.point[0]) < 1e-6 and not pg.gate[0].dive and pg.gate[11].dive};
  for(std::size_t i{1u}; i != pg.count; ++i)
    pg_ok = pg_ok and (pg.gate[i].dive or not pg.gate[i - 1u].dive)
            and edworld::length(pg.gate[i].gate.corner[0] - pg.gate[i].gate.centre) < edworld::length(pg.gate[i - 1u].gate.corner[0] - pg.gate[i - 1u].gate.centre);
  check(pg_ok, "gates: along the whole path from the ship, the setup ones first then the dive ones, narrowing");
  }
  {
  double const R{1957252.5};
  auto const start{edworld::descent_start(-32.877357, 62.797558, 432586.0, -26.187031, 47.741756, R, 17.0)};
  edworld::dvec3_t const place{edworld::planet_point(-26.187031, 47.741756, R)};
  auto const gates{edworld::descent_gates(start.point, place, 10u, 0.05, 50.0)};
  edworld::dvec3_t const along{edworld::unit(place - start.point)};
  bool on_line{true}, square{true}, narrowing{true};
  for(std::size_t i{}; i != gates.count; ++i)
    {
    auto const & g{gates.gate[i]};
    edworld::dvec3_t const off{g.centre - start.point};
    on_line = on_line and edworld::length(off - along * edworld::dot(off, along)) < 1e-3;
    for(auto const & c : g.corner)
      square = square and std::fabs(edworld::dot(c - g.centre, along)) < 1e-3;
    if(i != 0u)
      narrowing = narrowing and edworld::length(g.corner[0] - g.centre) < edworld::length(gates.gate[i - 1u].corner[0] - gates.gate[i - 1u].centre);
    }
  check(gates.count == 10u and edworld::length(gates.gate[0].centre - start.point) < 1e-6 and on_line and square and narrowing,
        "gates: along the line from the start, square to it, narrowing towards the place");
  // the attitude back from the place's direction as the compass would show it, for a few headings, pitches and rolls
  bool attitude_ok{true};
  edworld::local_frame_t const at{edworld::local_frame(-30.0, 55.0)};
  edworld::dvec3_t const toward{edworld::unit(at.east * 0.3 + at.north * 0.5 - at.up * 0.8)};
  for(double heading : {0.0, 45.0, 299.0})
    for(double pitch : {-50.0, -10.0, 20.0})
      for(double roll : {0.0, 30.0, -100.0})
        {
        double const h{heading * edworld::gate_rad}, p{pitch * edworld::gate_rad}, f{roll * edworld::gate_rad};
        edworld::dvec3_t const level{at.east * std::sin(h) + at.north * std::cos(h)};
        edworld::dvec3_t const forward{level * std::cos(p) + at.up * std::sin(p)};
        edworld::dvec3_t const right0{at.east * std::cos(h) - at.north * std::sin(h)};
        edworld::dvec3_t const up0{edworld::cross(right0, forward)};
        edworld::dvec3_t const right{right0 * std::cos(f) - up0 * std::sin(f)}, up{right0 * std::sin(f) + up0 * std::cos(f)};
        edworld::compass_direction_t const seen{static_cast<float>(edworld::dot(toward, right)), static_cast<float>(edworld::dot(toward, up)),
                                                static_cast<float>(edworld::dot(toward, forward))};
        auto const a{edworld::attitude_from(at, toward, seen, heading, pitch + 3.0)};
        attitude_ok = attitude_ok and a.found and std::fabs(a.pitch - pitch) < 0.01 and std::fabs(std::remainder(a.roll - roll, 360.0)) < 0.01;
        }
  check(attitude_ok, "gates: the attitude from the compass and the heading comes back for 27 attitudes");
  edworld::attitude_t const axes{true, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, 0.0, 0.0};
  auto const middle{edworld::project({0.0, 0.0, 100.0}, {}, axes, 60.0, 1920.0, 1080.0)};
  auto const top{edworld::project({0.0, 100.0 * std::tan(30.0 * edworld::gate_rad), 100.0}, {}, axes, 60.0, 1920.0, 1080.0)};
  check(middle.in_front and std::fabs(middle.x - 960.0) < 1e-9 and std::fabs(middle.y - 540.0) < 1e-9 and std::fabs(top.y) < 1e-6
          and not edworld::project({0.0, 0.0, -1.0}, {}, axes, 60.0, 1920.0, 1080.0).in_front,
        "gates: the projection puts the nose in the middle, half the field at the edge, nothing behind");
  }
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
  check(edworld::approach_view(false, true, true, true, true, -25.f, 20.f) and not edworld::approach_view(false, true, true, true, true, -15.f, 20.f),
        "approach: on at 20 degrees below the wings");
  check(edworld::approach_view(true, true, true, true, true, -15.f, 20.f) and not edworld::approach_view(true, true, true, true, true, -5.f, 20.f),
        "approach: kept down to 10 degrees, off above");
  check(not edworld::approach_view(false, true, true, false, true, -60.f, 20.f) and not edworld::approach_view(true, true, true, false, false, -60.f, 20.f),
        "approach: never with the target far (after undocking it may stand low in another system)");
  check(edworld::approach_view(false, true, true, true, false, 10.f, 20.f),
        "approach: by a station or a carrier, close is enough - its pad stands ahead until the last metres");
  // near: a place 1 km off at 500 m, 20 km off; a station in this system while mass locked, the system itself
  constexpr double r_planet{1.0e6}, km_deg{1000.0 / r_planet * 57.29577951308232};
  check(edworld::near_target(true, true, 10.0, 20.0, 500.0, 10.0 + km_deg, 20.0, r_planet, 9u, 0u, 5.f)
          and not edworld::near_target(true, true, 10.0, 20.0, 500.0, 10.0 + 20.0 * km_deg, 20.0, r_planet, 9u, 0u, 5.f),
        "approach: a place within 5 km is near, 20 km is not");
  check(edworld::near_target(false, false, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 9u, 1ull << 16, 5.f)
          and not edworld::near_target(false, false, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 9u, 0u, 5.f)
          and not edworld::near_target(false, false, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0u, 1ull << 16, 5.f),
        "approach: a station in this system is near while mass locked, a system never");
  check(std::fabs(edworld::smoothing_weight(0.4f, 0.4f) - 0.63212f) < 1e-4f and edworld::smoothing_weight(0.f, 0.4f) == 0.f
          and edworld::smoothing_weight(0.1f, 0.f) == 1.f,
        "approach: the smoothing's weight, none after no time, all without a time constant");
  check(not edworld::approach_view(true, false, true, true, true, -80.f, 20.f) and not edworld::approach_view(true, true, false, true, true, -80.f, 20.f),
        "approach: off outside normal flight and once the reading went stale");
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
  // and of the whole frame: every draw with its constants
  if(std::FILE * all{_wfopen((output_dir + L"\\edworld_dump_all").c_str(), L"wb")})
    std::fclose(all);

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
  // the compass's dot, white, 10 px right of and 10 px above its disc's centre (100, 64); as large as the game's since
  // 2026-10-20 (~197 px)
  for(int y{46}; y != 63; ++y)
    for(int x{102}; x != 119; ++x)
      if((x - 110) * (x - 110) + (y - 54) * (y - 54) <= 64)
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
  {
  std::string full_log;
  if(std::FILE * lf{_wfopen(log_path.c_str(), L"rb")})
    {
    char buf[4096];
    for(std::size_t got; (got = std::fread(buf, 1, sizeof buf, lf)) != 0;)
      full_log.append(buf, got);
    std::fclose(lf);
    }
  check(full_log.find("dump: whole frame armed") != std::string::npos and full_log.find("draw(s) recorded") != std::string::npos
          and full_log.find("dump: whole frame's files queued") != std::string::npos,
        "whole frame: armed, closed, its files queued");
  // the files come from the writer thread, a moment later
  std::string draws_text;
  for(int tries{}; tries != 30 and draws_text.empty(); ++tries)
    {
    WIN32_FIND_DATAW draws_found{};
    HANDLE const draws{FindFirstFileW((output_dir + L"\\edworld_dumps\\*_draws.txt").c_str(), &draws_found)};
    if(draws != INVALID_HANDLE_VALUE)
      {
      FindClose(draws);
      if(std::FILE * df{_wfopen((output_dir + L"\\edworld_dumps\\" + draws_found.cFileName).c_str(), L"rb")})
        {
        char buf[4096];
        for(std::size_t got; (got = std::fread(buf, 1, sizeof buf, df)) != 0;)
          draws_text.append(buf, got);
        std::fclose(df);
        }
      }
    if(draws_text.empty())
      Sleep(100);
    }
  check(draws_text.rfind("a0 ", 0) == 0, "whole frame: its draws listed in <stamp>_draws.txt");
  WIN32_FIND_DATAW cb_found{};
  HANDLE const cbs{FindFirstFileW((output_dir + L"\\edworld_dumps\\*_a0_cb*.bin").c_str(), &cb_found)};
  check(cbs != INVALID_HANDLE_VALUE, "whole frame: a draw's constants written");
  if(cbs != INVALID_HANDLE_VALUE)
    FindClose(cbs);
  check(GetFileAttributesW((output_dir + L"\\edworld_dump_all").c_str()) == INVALID_FILE_ATTRIBUTES, "whole frame trigger removed");
  }
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
  if constexpr(with_eht)
    if(GetEnvironmentVariableW(L"EDWORLD_TEST_CATALOG", nullptr, 0))
      {
      // the shaders' catalog writes once no shader was created for 5 s: the panel's VS and PS, each a line and a file
      Sleep(7500);
      WIN32_FIND_DATAW found{};
      std::string text;
      std::wstring dir;
      if(HANDLE const h{FindFirstFileW(L"edworld_shaders\\*", &found)}; h != INVALID_HANDLE_VALUE)
        {
        do
          if(found.cFileName[0] != L'.' and (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            dir = std::wstring{L"edworld_shaders\\"} + found.cFileName;
        while(FindNextFileW(h, &found));
        FindClose(h);
        }
      if(FILE * const f{dir.empty() ? nullptr : _wfopen((dir + L"\\catalog.tsv").c_str(), L"rb")})
        {
        char buf[4096];
        for(std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) != 0;)
          text.append(buf, n);
        std::fclose(f);
        }
      char vs_line[64], vs_file[96];
      std::snprintf(vs_line, sizeof vs_line, "%016llX\tvs\t", static_cast<unsigned long long>(edworld::fnv1a64(g_panel_vs, sizeof g_panel_vs)));
      std::snprintf(vs_file, sizeof vs_file, "\\vs_%016llX.dxbc", static_cast<unsigned long long>(edworld::fnv1a64(g_panel_vs, sizeof g_panel_vs)));
      bool const file{not dir.empty() and GetFileAttributesW((dir + std::wstring(vs_file, vs_file + std::strlen(vs_file))).c_str()) != INVALID_FILE_ATTRIBUTES};
      check(text.find(vs_line) != std::string::npos and text.find("\tps\t") != std::string::npos and
              text.find("SV_Position0 TEXCOORD0") != std::string::npos and file,
            "shader catalog: the panel's VS and PS written for the build, each with its shape and bytecode");
      }
  std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
  }
