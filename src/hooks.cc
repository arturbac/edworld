// edworld — the observer: which vertex shaders draw panels, and what each panel draw carries.
// edworld alone only finds the panels' draws (for the patch); edworld_eht also copies what each carries and
// publishes it to EHT (`panels`).
//
// Read-only by construction: every hook calls the game's call through unchanged, and what edworld adds is its own
// copies into its own staging buffers. Lessons taken from EDVR (MIT) and its Proton issue 65:
//  - per-draw work is a pointer compare; Get* calls only for the few matched draws;
//  - never read a mapped dynamic buffer on the CPU (write-combined under DXVK); copy on the GPU into staging
//    and map it frames later with DO_NOT_WAIT;
//  - GetType before GetDesc; a fault in edworld's code disables edworld, never the game call.
#include "atmosphere_planets.h"
#include "atmosphere_kind.h"
#include "aurora.h"
#include "blue_noise.h"
#include "compass_math.h"
#include "galmap.h"
#include "edworld_share.h"
#include "gpu_timing.h"
#include "game_state.h"
#include "hologram.h"
#include "holo_models.h"
#include "panel_math.h"
#include "cb_binding.h"
#include "clouds_values.h"
#include "panel_patch.h"
#include "d3d11_enum_names.h"
#include "own_draw.h"
#include "runtime.h"
#include "snow.h"
#include "shader_catalog.h"

#include <edplugin_common/dxgi/device_removed.h>

#include <d3d11_1.h>
#include <d3d11_4.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <string>
#include <vector>
#include "clouds3d_ps.h"
#include "clouds3d_vs.h"
#include "clouds_up_ps.h"
#include "clouds_up_vs.h"
#include "clouds_ps.h"
#include "clouds_vs.h"
#include <string_view>

#include <stralgo/stralgo.h>

namespace edworld
  {
  using namespace std::string_view_literals;

  namespace
    {
    // ---- vtable slots (the D3D11 ABI; IUnknown 0..2, ID3D11DeviceChild 3..6) ----
    constexpr int slot_create_vertex_shader{12};
    constexpr int slot_vs_set_shader{11};
    constexpr int slot_draw_indexed{12};
    constexpr int slot_draw{13};
    constexpr int slot_draw_indexed_instanced{20};
    constexpr int slot_draw_instanced{21};
    constexpr int slot_draw_indexed_instanced_indirect{39};
    constexpr int slot_draw_instanced_indirect{40};
    constexpr int slot_execute_command_list{58};
    // the catalog's (edworld_eht): the device's other creations of shaders and input layouts (d3d11.h)
    constexpr int slot_create_input_layout{11};
    constexpr int slot_create_geometry_shader{13};
    constexpr int slot_create_geometry_shader_so{14};
    constexpr int slot_create_pixel_shader{15};
    constexpr int slot_create_hull_shader{16};
    constexpr int slot_create_domain_shader{17};
    constexpr int slot_create_compute_shader{18};

    using create_vs_fn = HRESULT(STDMETHODCALLTYPE *)(
      ID3D11Device *, void const *, SIZE_T, ID3D11ClassLinkage *, ID3D11VertexShader **
    );
    using vs_set_shader_fn
      = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11ClassInstance * const *, UINT);
    using draw_indexed_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
    using draw_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
    using draw_indexed_instanced_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
    using draw_instanced_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
    using draw_indirect_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
    using execute_command_list_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);

    create_vs_fn orig_create_vs{};
    vs_set_shader_fn orig_vs_set_shader{};
    draw_indexed_fn orig_draw_indexed{};
    draw_fn orig_draw{};
    draw_indexed_instanced_fn orig_draw_indexed_instanced{};
    draw_instanced_fn orig_draw_instanced{};
    draw_indirect_fn orig_draw_indexed_instanced_indirect{};
    draw_indirect_fn orig_draw_instanced_indirect{};
    execute_command_list_fn orig_execute_command_list{};

    // ---- watched vertex shaders: written by CreateVertexShader (any thread), read on the render thread ----
    constexpr std::uint32_t max_watched{32};
    std::atomic<void *> watched_ptr[max_watched]{};
    std::uint32_t watched_index[max_watched]{};
    bool watched_found[max_watched]{};  // found in the game, not listed: patched, not published
    std::atomic<std::uint32_t> watched_count{};  // slots reserved; a slot counts once its pointer is set
    std::atomic<std::uint64_t> vs_created{};
    // every vertex shader's hash by pointer, for the probe only (append-only; a released and reused pointer
    // keeps its first hash: good enough for a diagnostic line)
    constexpr std::uint32_t max_vs_seen{8192};
    std::atomic<void *> seen_ptr[max_vs_seen]{};
    std::uint64_t seen_hash[max_vs_seen]{};
    std::atomic<std::uint32_t> seen_reserved{};
    // the bytecode of the shaders in start_fields().dump_vs, kept from creation for a whole-frame dump (a slot counts once
    // its hash is set)
    constexpr std::uint32_t max_kept_vs{8};
    struct kept_vs_t
      {
      std::atomic<std::uint64_t> hash;
      void * bytes;
      std::uint32_t length;
      };
    kept_vs_t kept_vs[max_kept_vs]{};
    std::atomic<std::uint32_t> kept_vs_reserved{};

    // ---- each shader's verdict: does it draw a panel's interface surface? (pointer -> seen slot, open addressing) ----
    enum struct verdict_e : std::uint8_t
      {
      pending,
      panel,
      not_panel
      };

    constexpr std::uint32_t verdict_draws{64};  // draws looked at before a shader is judged no panel
    constexpr std::uint32_t slot_table_size{16384};
    std::atomic<void *> table_key[slot_table_size]{};
    std::atomic<std::uint32_t> table_slot[slot_table_size]{};  ///< slot + 1; 0 while the key is claimed but no slot written
    std::atomic<verdict_e> verdict[max_vs_seen]{};
    std::uint8_t verdict_looked[max_vs_seen]{};  // render thread only
    std::atomic<std::uint8_t> slot_watched[max_vs_seen]{};  ///< the watched list's index + 1 of a seen slot's shader, 0 = not

    auto table_home(void const * p) noexcept -> std::uint32_t
      {
      return static_cast<std::uint32_t>((reinterpret_cast<std::uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 50) & (slot_table_size - 1);
      }

    auto table_put(void * p, std::uint32_t slot) noexcept -> void
      {
      for(std::uint32_t i{table_home(p)}, n{}; n != slot_table_size; ++n, i = (i + 1) & (slot_table_size - 1))
        {
        void * expected{};
        if(table_key[i].compare_exchange_strong(expected, p) or expected == p)
          {
          table_slot[i].store(slot + 1u, std::memory_order_release);  // a reused pointer takes its new shader's slot
          return;
          }
        }
      }

    ///\brief the seen slot of a shader, or max_vs_seen when unknown
    auto table_get(void const * p) noexcept -> std::uint32_t
      {
      for(std::uint32_t i{table_home(p)}, n{}; n != slot_table_size; ++n, i = (i + 1) & (slot_table_size - 1))
        {
        void * const k{table_key[i].load(std::memory_order_acquire)};
        if(k == p)
          {
          // another thread may have claimed the key and not written the slot yet: unknown for this call
          std::uint32_t const slot{table_slot[i].load(std::memory_order_acquire)};
          return slot == 0u ? max_vs_seen : slot - 1u;
          }
        if(not k)
          break;
        }
      return max_vs_seen;
      }

    // ---- the probe: a jump charges but no panel draw comes; log what is drawn instead ----
    std::atomic<std::uint64_t> last_watched_ms{};  // GetTickCount64 of the latest panel draw
    std::atomic<bool> probe_request{};             // set by the probe thread, cleared by the render thread
    std::atomic<std::uint32_t> probe_draws{};

    // ---- the draw census (diagnostics): which contexts the game's draws come through in the first minutes after the
    // first device, and whether the hooks still stand in the contexts' tables ----
    constexpr std::uint32_t census_contexts{4};
    std::atomic<ID3D11DeviceContext *> census_context[census_contexts]{};  // each device's own, in the order they came
    std::atomic<std::uint32_t> census_attached{};
    std::atomic<std::uint32_t> census_draws[census_contexts + 1]{};  // the last one: any other context
    std::atomic<std::uint32_t> census_indirect{};
    std::atomic<std::uint32_t> census_lists{};
    std::atomic<std::uint32_t> census_shell{};
    std::atomic<bool> census_on{};

    ///\brief +1 without a locked instruction: on every draw it has to cost next to nothing, and a count lost between two
    /// threads does not matter to a diagnostic line
    auto census_add(std::atomic<std::uint32_t> & counter) noexcept -> void
      {
      counter.store(counter.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
      }

    auto census_note(ID3D11DeviceContext * self) noexcept -> void
      {
      if(not census_on.load(std::memory_order_relaxed))
        return;
      std::uint32_t at{};
      while(at != census_contexts and census_context[at].load(std::memory_order_relaxed) != self)
        ++at;
      census_add(census_draws[at]);
      }

    ///\brief the census counts (with `counts`) and each device's draw hooks, one line each
    auto census_report(char const * when, bool counts) noexcept -> void;

    // ---- render-thread state ----
    std::atomic<ID3D11DeviceContext *> immediate{};
    ID3D11Device * device{};
    ID3D11DeviceContext1 * immediate1{};
    int current_watched{-1};
    // (PoC) the game's shell of the atmosphere over a planet that has none of its own clouds (atmosphere_shell_vs,
    // shader_ids.h); its draw is what the clouds are drawn after. It says where the planet is and how large, in its
    // own constants
    std::atomic<void *> shell_vs{};
    bool current_shell{};
    std::uint32_t current_pending{max_vs_seen};  // the bound shader's seen slot while its verdict is pending
    std::atomic<bool> disabled{false};
    ///\brief edworld's own work under a guard on this thread: its own draws and shader binds (ImGui's, the state put
    /// back) pass the hooks straight to the context, unobserved
    thread_local int own_depth{};
    ///\brief edworld's work in the hooks under edplugin_common's guard: each fault or C++ exception logged with its place
    /// (the first 16 of a session); the eighth, a stack overflow or a fault in another module turns every guarded path
    /// off for the session, said once
    constinit edplugin_common::guard_t hooks_guard{"hooks", 8};
    ///\brief the name and version in a corner of the picture under a guard of its own, off after its first fault: with
    /// edworld_eht off (chain_refusal) the corner's line is all that is drawn
    constinit edplugin_common::guard_t build_info_guard{"build_info", 1};

    ///\brief the faults of a session in the summaries: the hooks' and the corner line's
    [[nodiscard]]
    auto faults_seen() noexcept -> int
      {
      return hooks_guard.faults() + build_info_guard.faults();
      }

    ///\brief whether the hooks leave the game alone: edworld_eht refused (proxy.cc) or the guard off
    [[nodiscard]]
    auto hooks_off() noexcept -> bool
      {
      return disabled.load(std::memory_order_relaxed) or hooks_guard.broken();
      }

    ///\brief `work()` under hooks_guard; after a fault `handler()` sets the place's own state right (the library's locks
    /// and the thread's log batch are the guard's)
    template<typename work_t, typename handler_t>
    auto hook_work(work_t && work, handler_t && handler) noexcept -> void
      {
      (void)hooks_guard.run(work,
                            [&](edplugin_common::fault_t const &) noexcept
                            {
                              // a fault while edworld made its own shaders must not mark the game's as edworld's for good
                              creating_own = false;
                              handler();
                            });
      }

    auto no_handler() noexcept -> void {}

    constexpr std::uint32_t record_bytes{cb0_rows * 16u};
    constexpr std::uint32_t ring_slots{6};

    struct meta_t
      {
      std::uint64_t surface_id;
      std::uint32_t surface_width, surface_height, surface_format;
      std::uint32_t vs_index;
      std::uint32_t index_count, instance_count, start_index;
      std::int32_t base_vertex;
      std::uint32_t start_instance;
      };

    constexpr std::uint32_t entry_bytes{16u};  // one VB0 instance entry (8 used) per panel draw
    constexpr std::uint32_t cb1_first_row{268u};
    constexpr std::uint32_t cb1_rows{8u};  // 268..275; 275 = world-rebase origin
    constexpr std::uint32_t side_bytes{max_panels * entry_bytes + cb1_rows * 16u};
    constexpr std::uint32_t pool_limit{64u * 1024u * 1024u};

    struct slot_t
      {
      ID3D11Buffer * staging{};
      ID3D11Buffer * side{};  ///< VB0 entries at panel * 16, then cb1 rows 268..275
      ID3D11Buffer * pool{};  ///< the frame's t33 record pool, the window of it the panels' records lie in (panel_pool)
      std::uint32_t pool_capacity{};
      std::uint32_t pool_bytes{};  ///< the bytes copied, from pool_base of the game's pool
      std::uint32_t pool_base{};
      std::uint32_t pool_first{};
      std::uint32_t pool_stride{};
      bool have_pool{};
      bool have_cb1{};
      bool have_entry[max_panels]{};
      meta_t meta[max_panels]{};
      std::uint32_t count{};
      std::uint64_t frame{};
      LONGLONG qpc{};
      bool filled{};
      };

    slot_t ring[ring_slots];
    int recording{-1};
    std::uint64_t frame{};
    LONGLONG last_draw_qpc{};
    LONGLONG last_any_draw_qpc{};  ///< the end of the immediate context's last draw, for frames without panels
    LONGLONG qpc_frequency{1};

    // ---- statistics for the log ----
    std::uint64_t stat_draws{}, stat_published{}, stat_dropped{}, stat_full{};
    LONGLONG last_log_qpc{};
    LONGLONG last_removed_check_qpc{};  ///< the last look at the device's GetDeviceRemovedReason (once a second)

    // ---- the published file ----
    share_t * share{};

    struct surface_cache_t
      {
      void * resource;
      std::uint32_t width, height, format;
      };


    auto qpc_now() noexcept -> LONGLONG
      {
      LARGE_INTEGER v;
      QueryPerformanceCounter(&v);
      return v.QuadPart;
      }

    auto unix_ms_now() noexcept -> std::int64_t
      {
      FILETIME ft;
      GetSystemTimeAsFileTime(&ft);
      ULARGE_INTEGER u;
      u.LowPart = ft.dwLowDateTime;
      u.HighPart = ft.dwHighDateTime;
      return static_cast<std::int64_t>(u.QuadPart / 10000ull) - 11644473600000ll;
      }

    auto patch_slot(void * object, int slot, void * hook, void ** original) noexcept -> bool
      {
      void ** const table{*static_cast<void ***>(object)};
      if(table[slot] == hook)
        return true;  // a second device of the same class: already ours, keep the first original
      DWORD old{};
      if(not VirtualProtect(&table[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old))
        return false;
      *original = table[slot];
      table[slot] = hook;
      VirtualProtect(&table[slot], sizeof(void *), old, &old);
      FlushInstructionCache(GetCurrentProcess(), &table[slot], sizeof(void *));
      return true;
      }

    // ---- the game's context on a table of edworld's own (EDVR's LiveCopy, src/common/vtable_hook.h) ----
    // Windows' d3d11 rewrites the entries of its own context table when it switches between its variants of the
    // methods (the multithread protection; EDVR measured it 2026-08-18). A hook written into that table loses to it:
    // a player's log (2026-11-08, Windows, the system d3d11 alone) had every draw slot back to d3d11.dll 30 s after
    // the start and no draw seen. Here the context object points at a table of edworld's own instead: edworld's hooks
    // in their slots, and in every slot a stub that jumps through the runtime's entry as it is at the call, so nothing
    // is frozen and a rewrite of the runtime's table is followed. Only for a context whose methods are the system
    // d3d11's: a wrapper's object (3Dmigoto, ReShade) keeps its table, patched in place as before.
    constexpr std::size_t own_table_max{256};
    constexpr std::size_t own_table_header{2};  // before entry 0: the C++ ABIs' type information, copied as it is
    constexpr std::size_t own_stub_bytes{12};   // mov rax, &runtime[i]; jmp [rax]
    constexpr std::size_t own_sample{96};

    enum struct context_hook_e : std::uint8_t
      {
      in_place,
      own_table
      };

    struct own_table_t
      {
      void ** runtime{};  ///< the table the context came with
      void ** entries{};  ///< edworld's; the context points here
      std::size_t count{};
      };

    own_table_t own_table{};
    context_hook_e context_hook{context_hook_e::in_place};
    ///\brief the runtime's entries of edworld's context slots when the context moved to the own table
    struct own_watch_t
      {
      int slot;
      void * at_start;
      };

    own_watch_t own_watch[9]{};
    std::size_t own_watched{};
    std::atomic<ID3D11Multithread *> multithread{};
    bool multithread_at_start{};

    // whether a context table's entry is there and leads to code: the table's own rules, kept with the hooks that use them
    [[nodiscard]]
    auto table_readable(void const * p) noexcept -> bool
      {
      MEMORY_BASIC_INFORMATION m{};
      return VirtualQuery(p, &m, sizeof m) == sizeof m and m.State == MEM_COMMIT
             and not(m.Protect & (PAGE_GUARD | PAGE_NOACCESS));
      }

    [[nodiscard]]
    auto table_executable(void const * p) noexcept -> bool
      {
      MEMORY_BASIC_INFORMATION m{};
      return VirtualQuery(p, &m, sizeof m) == sizeof m and m.State == MEM_COMMIT
             and (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
      }

    ///\brief the table's entries up to the first that is not code
    [[nodiscard]]
    auto table_length(void ** table) noexcept -> std::size_t
      {
      std::size_t n{};
      while(n != own_table_max and table_readable(&table[n]) and table_executable(table[n]))
        ++n;
      return n;
      }

    [[nodiscard]]
    auto entries_in(void ** table, std::size_t count, HMODULE module) noexcept -> std::size_t
      {
      std::size_t in{};
      for(std::size_t i{}; i != count; ++i)
        {
        if(HMODULE const m{edplugin_common::module_of(table[i])}; m and m == module)
          ++in;
        }
      return in;
      }

    ///\brief a table of `count` stubs, each jumping through the runtime's entry of its slot at the call; null when the
    /// memory could not be had (nothing changed then)
    [[nodiscard]]
    auto make_own_table(void ** runtime, std::size_t count) noexcept -> void **
      {
      auto * const words{static_cast<void **>(
        VirtualAlloc(nullptr, (own_table_header + count) * sizeof(void *), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
      )};
      auto * const stubs{static_cast<std::uint8_t *>(
        VirtualAlloc(nullptr, count * own_stub_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
      )};
      if(not words or not stubs)
        {
        if(words)
          VirtualFree(words, 0, MEM_RELEASE);
        if(stubs)
          VirtualFree(stubs, 0, MEM_RELEASE);
        return nullptr;
        }
      for(std::size_t h{}; h != own_table_header; ++h)
        if(void ** const at{runtime - own_table_header + h}; table_readable(at))
          words[h] = *at;
      void ** const entries{words + own_table_header};
      for(std::size_t i{}; i != count; ++i)
        {
        std::uint8_t * const stub{stubs + i * own_stub_bytes};
        void ** const slot{runtime + i};
        stub[0] = 0x48;  // mov rax, imm64
        stub[1] = 0xB8;
        std::memcpy(stub + 2, &slot, sizeof slot);
        stub[10] = 0xFF;  // jmp qword ptr [rax]
        stub[11] = 0x20;
        entries[i] = stub;
        }
      DWORD old{};
      if(not VirtualProtect(stubs, count * own_stub_bytes, PAGE_EXECUTE_READ, &old))
        {
        VirtualFree(words, 0, MEM_RELEASE);
        VirtualFree(stubs, 0, MEM_RELEASE);
        return nullptr;
        }
      FlushInstructionCache(GetCurrentProcess(), stubs, count * own_stub_bytes);
      return entries;
      }

    ///\brief the way the context is watched, said in the log: start_fields().context_hook, else by whose code its methods
    /// are (EDVR's threshold: three quarters of the first 96 in the system d3d11 is the runtime's own object)
    [[nodiscard]]
    auto pick_context_hook(ID3D11DeviceContext * ctx) noexcept -> context_hook_e
      {
      void ** const table{*reinterpret_cast<void ***>(ctx)};
      if(own_table.entries and table == own_table.entries)
        return context_hook_e::own_table;
      std::size_t const count{table_length(table)};
      std::size_t const sample{std::min(count, own_sample)};
      std::size_t const in_system{system_d3d11() ? entries_in(table, sample, system_d3d11()) : 0u};
      std::uint32_t const asked{start_fields().context_hook};
      bool const runtime_own{sample != 0 and in_system * 4 >= sample * 3};
      bool const long_enough{count > static_cast<std::size_t>(slot_execute_command_list)};
      bool const own{long_enough and (asked == 2 or (asked == 0 and runtime_own))};
      char const * why{""};
      if(not long_enough)
        why = "; too short for a table of edworld's own";
      else if(asked == 1)
        why = "; context_hook = 1";
      else if(asked == 2)
        why = "; context_hook = 2";
      log_line(
        "context: {} ({} of its first {} methods are the system d3d11.dll's, {} in its table{})",
        own ? "a table of edworld's own, each call through the runtime's entry as it is then" : "its own table patched in place",
        in_system,
        sample,
        count,
        why);
      return own ? context_hook_e::own_table : context_hook_e::in_place;
      }

    ///\brief one slot of the context: on edworld's table, or in place
    [[nodiscard]]
    auto hook_context(ID3D11DeviceContext * ctx, int slot, void * hook, void ** original) noexcept -> bool
      {
      if(context_hook != context_hook_e::own_table)
        return patch_slot(ctx, slot, hook, original);
      if(own_table.entries[slot] == hook)
        return true;  // a second device on the same table: already ours, keep the first original
      *original = own_table.entries[slot];  // the stub: whatever the runtime's table holds at the call
      own_table.entries[slot] = hook;
      if(own_watched != std::size(own_watch))
        own_watch[own_watched++] = own_watch_t{slot, own_table.runtime[slot]};
      return true;
      }

    ///\brief before the context's slots are hooked: the table of edworld's own made (or kept) for this context's table;
    /// false = in place after all
    [[nodiscard]]
    auto prepare_own_table(ID3D11DeviceContext * ctx) noexcept -> bool
      {
      void ** const table{*reinterpret_cast<void ***>(ctx)};
      if(own_table.entries and (table == own_table.entries or table == own_table.runtime))
        return true;
      std::size_t const count{table_length(table)};
      void ** const entries{make_own_table(table, count)};
      if(not entries)
        {
        log_line("context: the memory for a table of edworld's own could not be had (error {}); patched in place",
                 GetLastError());
        return false;
        }
      if(own_table.entries)
        log_line("context: another context table ({} after {}): a new table of edworld's own, the latest device wins",
                 static_cast<void const *>(static_cast<void *>(table)), static_cast<void const *>(static_cast<void *>(own_table.runtime)));
      own_table = own_table_t{table, entries, count};
      own_watched = 0;
      return true;
      }

    ///\brief a diagnosis of hooks on Windows (2026-11-23), said once at the attach, nothing changed: where the context's
    /// table lies (its memory's type and protection, the module and PE section holding it) and which of the context's
    /// interfaces come at its own pointer, so a rule for a table of edworld's own can be told by where Windows keeps it
    auto log_context_table(ID3D11DeviceContext * ctx) noexcept -> void
      {
      void ** const table{*reinterpret_cast<void ***>(ctx)};
      MEMORY_BASIC_INFORMATION m{};
      bool const queried{VirtualQuery(table, &m, sizeof m) == sizeof m};
      char const * const type{not queried                  ? "?"
                              : m.State == MEM_FREE   ? "free"
                              : m.Type == MEM_IMAGE   ? "image"
                              : m.Type == MEM_PRIVATE ? "private"
                              : m.Type == MEM_MAPPED  ? "mapped"
                                                      : "other"};
      HMODULE const module{edplugin_common::module_of(table)};
      wchar_t path[MAX_PATH]{L"-"};
      char section[9]{"-"};
      if(module)
        {
        if(GetModuleFileNameW(module, path, MAX_PATH) == 0)
          std::wcscpy(path, L"?");
        edplugin_common::section_of(module, table, section);
        }
      log_line("context: its table at {} ({} memory, protection 0x{:X}, {} entries), in {} section {}{}",
               static_cast<void const *>(static_cast<void *>(table)), type, queried ? m.Protect : 0u, table_length(table), wide(path),
               section, module and module == system_d3d11() ? " (the system d3d11.dll)" : "");
      auto same = [&](REFIID id) -> char const *
        {
        void * other{};
        if(FAILED(ctx->QueryInterface(id, &other)) or not other)
          return "none";
        // pointers compared, not identities: an interface of the same object may come at another pointer
        char const * const answer{other == static_cast<void *>(ctx) ? "same pointer" : "other pointer"};
        static_cast<IUnknown *>(other)->Release();
        return answer;
        };
      log_line("context: its interfaces - ID3D11DeviceContext1 {}, 2 {}, 3 {}, 4 {}; ID3D11Multithread {}",
               same(__uuidof(ID3D11DeviceContext1)), same(__uuidof(ID3D11DeviceContext2)), same(__uuidof(ID3D11DeviceContext3)),
               same(__uuidof(ID3D11DeviceContext4)), same(__uuidof(ID3D11Multithread)));
      }

    ///\brief the multithread protection decides which of the runtime's variants of the context's methods run; asked
    /// at the start and in the census, never set
    auto note_multithread(ID3D11Device * dev, ID3D11DeviceContext * ctx) noexcept -> void
      {
      if(multithread.load())
        return;
      ID3D11Multithread * mt{};
      if(FAILED(ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void **>(&mt))))
        mt = nullptr;
      if(not mt and FAILED(dev->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void **>(&mt))))
        mt = nullptr;
      if(not mt)
        {
        log_line("context: the multithread protection cannot be asked (no ID3D11Multithread)");
        return;
        }
      multithread_at_start = mt->GetMultithreadProtected() != FALSE;
      multithread.store(mt);  // kept for the process lifetime
      log_line("context: the multithread protection is {} at the start", multithread_at_start ? "on" : "off");
      }

    // ---- the published record (edworld_eht) ----
#if defined(EDWORLD_EHT)
    auto open_share() noexcept -> void
      {
      std::wstring const & dir{start_fields().shm_dir};
      if(dir.empty())
        return;
      CreateDirectoryW(dir.c_str(), nullptr);
      std::wstring const path{stralgo::stl::merge(dir, L"\\panels"sv)};
      // edplugin_common's: opened or made, grown to the record's size, mapped; a failure logged with the tag
      share = static_cast<share_t *>(edplugin_common::map_file_rw(path, sizeof(share_t), "share"));
      if(not share)
        return;
      std::memset(share, 0, sizeof(share_t));
      share->magic = share_magic;
      share->version = share_version;
      share->size = sizeof(share_t);
      share->writer_pid = GetCurrentProcessId();
      log_line("share: publishing to {} ({} bytes)", wide(path), sizeof(share_t));
      }
#endif

    // the panels' records lie in a small part of the pool (D4, 2026-11-18: the whole pool, 0.7..3.4 MB, was copied each
    // frame): the records the last read frames used, with a margin, are copied; a record outside them, a new pool or
    // the first frame copy the whole pool once. Render thread only
    struct pool_window_t
      {
      std::uint64_t seen_lo{~0ull}, seen_hi{};  ///< record numbers (from the pool's start) read since the last copy
      std::uint64_t lo{}, hi{};                 ///< the window of the next copies, records
      bool known{};
      bool missed{true};
      };
    pool_window_t panel_pool{};
    constexpr std::uint64_t pool_margin_records{64};

    auto pool_note(std::uint64_t record) noexcept -> void
      {
      panel_pool.seen_lo = std::min(panel_pool.seen_lo, record);
      panel_pool.seen_hi = std::max(panel_pool.seen_hi, record);
      }

    struct mapped_t
      {
      std::uint8_t const * cb0{};   ///< the cb0 rows of every panel draw
      std::uint8_t const * side{};  ///< VB0 entries, then cb1 rows 268..275; null when not read
      std::uint8_t const * pool{};  ///< the t33 pool; null when not read
      };

    ///\brief the panel's record: VB0 entry -> record index -> 336-byte record in the pool
    auto read_record(slot_t const & slot, mapped_t const & m, std::uint32_t i, record_t & out, std::uint32_t & index)
      noexcept -> bool
      {
      if(not m.side or not m.pool or not slot.have_entry[i] or not slot.pool_stride)
        return false;
      std::memcpy(&index, m.side + i * entry_bytes, 4);
      std::uint64_t const in_pool{(static_cast<std::uint64_t>(slot.pool_first) + index) * slot.pool_stride};
      pool_note(in_pool / slot.pool_stride);
      if(in_pool < slot.pool_base or in_pool - slot.pool_base + 28u > slot.pool_bytes)
        {
        panel_pool.missed = true;  // outside the window copied: the whole pool next frame
        return false;
        }
      out = decode_record(m.pool + (in_pool - slot.pool_base));
      return true;
      }

    auto rebase_of(slot_t const & slot, mapped_t const & m, float (&rebase)[4]) noexcept -> bool
      {
      if(not m.side or not slot.have_cb1)
        return false;
      std::memcpy(rebase, m.side + max_panels * entry_bytes + 7u * 16u, 16);  // row 275 = 268 + 7
      return true;
      }

    ///\brief everything about panel i, from what was mapped
    auto describe(slot_t const & slot, mapped_t const & m, std::uint32_t i, panel_t & p) noexcept -> void
      {
      meta_t const & d{slot.meta[i]};
      p.surface_id = d.surface_id;
      p.surface_width = d.surface_width;
      p.surface_height = d.surface_height;
      p.surface_format = d.surface_format;
      p.vs_index = d.vs_index;
      p.index_count = d.index_count;
      p.instance_count = d.instance_count;
      p.start_index = d.start_index;
      p.base_vertex = d.base_vertex;
      p.start_instance = d.start_instance;
      p.ordinal = i;
      std::memcpy(p.cb0, m.cb0 + i * record_bytes, record_bytes);
      record_t r{};
      std::uint32_t index{};
      float rebase[4]{};
      if(read_record(slot, m, i, r, index) and rebase_of(slot, m, rebase))
        {
        p.position[0] = r.position[0] - rebase[0];
        p.position[1] = r.position[1] - rebase[1];
        p.position[2] = r.position[2] - rebase[2];
        p.scale = r.scale;
        std::memcpy(p.orientation, r.orientation, sizeof p.orientation);
        p.record_index = index;
        p.flags = 1u;
        project_local(p.cb0, p.position[0], p.position[1], p.position[2], p.anchor_clip);
        }
      else
        {
        std::memset(p.position, 0, sizeof p.position);
        p.scale = 0.f;
        std::memset(p.orientation, 0, sizeof p.orientation);
        p.record_index = 0;
        p.flags = 0;
        anchor_from_cb0(p.cb0, p.anchor_clip);
        }
      }

    auto publish(slot_t const & slot, mapped_t const & m) noexcept -> void
      {
      if(not share)
        return;
      std::atomic_ref<std::uint32_t> sequence{share->sequence};
      sequence.fetch_add(1u, std::memory_order_acq_rel);  // odd: inside
      share->frame = frame;
      share->source_frame = slot.frame;
      share->unix_ms = unix_ms_now();
      share->panel_count = slot.count;
      share->pool_bytes = m.pool ? slot.pool_bytes : 0u;
      if(not rebase_of(slot, m, share->rebase))
        std::memset(share->rebase, 0, sizeof share->rebase);
      for(std::uint32_t i{}; i != slot.count; ++i)
        describe(slot, m, i, share->panels[i]);
      sequence.fetch_add(1u, std::memory_order_acq_rel);  // even: done
      }

    auto log_summary(slot_t const & slot, mapped_t const & m) noexcept -> void
      {
      log_line(
        "frame {}: {} panel draw(s); totals draws {} published {} dropped {} full {} faults {} vs {} pool {}",
        static_cast<unsigned long long>(slot.frame),
        slot.count,
        static_cast<unsigned long long>(stat_draws),
        static_cast<unsigned long long>(stat_published),
        static_cast<unsigned long long>(stat_dropped),
        static_cast<unsigned long long>(stat_full),
        faults_seen(),
        static_cast<unsigned long long>(vs_created.load()),
        m.pool ? slot.pool_bytes : 0u
      );
      for(std::uint32_t i{}; i != slot.count; ++i)
        {
        panel_t p{};
        describe(slot, m, i, p);
        auto const ndc{clip_to_ndc(p.anchor_clip)};
        log_line(
          "  #{} vs{} surf {:08x} {}x{} n{} inst{} rec{}{} pos {:.3f} {:.3f} {:.3f} s{:.3f} anchor {} {:.4f} {:.4f} w {:.4f}",
          i,
          p.vs_index,
          static_cast<unsigned long long>(p.surface_id & 0xffffffffull),
          p.surface_width,
          p.surface_height,
          p.index_count,
          p.start_instance,
          p.flags ? "" : "-",
          p.record_index,
          p.position[0],
          p.position[1],
          p.position[2],
          p.scale,
          ndc ? "ndc" : "behind",
          ndc ? ndc->x : p.anchor_clip[0],
          ndc ? ndc->y : p.anchor_clip[1],
          p.anchor_clip[3]
        );
        }
      }

    // ---- readback: oldest first, without waiting; too old = dropped ----
    auto drain(LONGLONG now) noexcept -> void
      {
      for(;;)
        {
        slot_t * oldest{};
        for(slot_t & s: ring)
          if(s.filled and (not oldest or s.frame < oldest->frame))
            oldest = &s;
        if(not oldest)
          return;
        ID3D11DeviceContext * const ctx{immediate.load()};
        D3D11_MAPPED_SUBRESOURCE main{};
        HRESULT const hr{ctx->Map(oldest->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &main)};
        if(hr == DXGI_ERROR_WAS_STILL_DRAWING)
          {
          if((now - oldest->qpc) * 1000 / qpc_frequency > 100)
            {
            oldest->filled = false;
            ++stat_dropped;
            continue;
            }
          return;  // the GPU has not got there; newer slots are not ready either
          }
        if(FAILED(hr) or not main.pData)
          {
          oldest->filled = false;
          ++stat_dropped;
          continue;
          }
        // the side and pool copies were recorded before the frame's last cb0 copy: ready when it is
        mapped_t m{static_cast<std::uint8_t const *>(main.pData)};
        D3D11_MAPPED_SUBRESOURCE side{}, pool{};
        bool const side_mapped{SUCCEEDED(ctx->Map(oldest->side, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &side))
                               and side.pData};
        bool const pool_mapped{oldest->have_pool
                               and SUCCEEDED(ctx->Map(oldest->pool, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &pool))
                               and pool.pData};
        if(side_mapped)
          m.side = static_cast<std::uint8_t const *>(side.pData);
        if(pool_mapped)
          m.pool = static_cast<std::uint8_t const *>(pool.pData);
        bool const stale{(now - oldest->qpc) * 1000 / qpc_frequency > 100};
        if(not stale)
          {
          publish(*oldest, m);
          ++stat_published;
          if(start_fields().log_interval_ms
             and (now - last_log_qpc) * 1000 / qpc_frequency >= start_fields().log_interval_ms)
            {
            last_log_qpc = now;
            log_batch_begin();  // the summary's lines in one write
            log_summary(*oldest, m);
            log_batch_end();
            }
          }
        else
          ++stat_dropped;
        if(pool_mapped)
          ctx->Unmap(oldest->pool, 0);
        if(side_mapped)
          ctx->Unmap(oldest->side, 0);
        ctx->Unmap(oldest->staging, 0);
        oldest->filled = false;
        }
      }

    // ---- the dumps' files, written on a thread of their own: thousands of files under Wine took the render thread
    // 18-26 s (2026-10-20). The render thread copies the mapped bytes and queues them; the writer keeps their order,
    // so a file queued last (`dump_done`) is on the disk after all the others ----
    struct alignas(MEMORY_ALLOCATION_ALIGNMENT) write_item_t
      {
      SLIST_ENTRY entry;
      write_item_t * next;  ///< the writer's own order, oldest first
      wchar_t path[MAX_PATH];
      wchar_t rename_to[MAX_PATH];  ///< non-empty: written as `path`, then moved over this
      bool loud;                    ///< a log line once written
      bool context;                 ///< the bytes are a sample's name: its context is read and written (sample_context)
      std::uint32_t bytes;
      // the bytes follow
      };

    SLIST_HEADER write_queue;
    HANDLE write_wake{};
    INIT_ONCE writer_once = INIT_ONCE_STATIC_INIT;  ///< the queue made once, before any thread pushes to it
    std::atomic<bool> writer_running{};             ///< the thread runs; without it nothing is queued
    std::atomic<std::uint32_t> writes_queued{}, writes_done{};

    auto writer_body(void *) -> DWORD
      {
      for(;;)
        {
        WaitForSingleObject(write_wake, 500);
        auto * taken{reinterpret_cast<write_item_t *>(InterlockedFlushSList(&write_queue))};
        // the list comes newest first
        write_item_t * oldest{};
        while(taken)
          {
          write_item_t * const after{reinterpret_cast<write_item_t *>(taken->entry.Next)};
          taken->next = oldest;
          oldest = taken;
          taken = after;
          }
        while(oldest)
          {
          write_item_t * const item{oldest};
          oldest = item->next;
          std::string context;
          if(item->context)
            try
              {
              context = sample_context(std::string(reinterpret_cast<char const *>(item + 1), item->bytes).c_str());
              }
            catch(std::exception const & e)
              {
              log_line("dump: {} without its context ({})", wide(item->path), e.what());
              }
          bool written{};
          // with context and none made: logged above, nothing to write
          if(not(item->context and context.empty()))
            try
              {
              // Windows' own calls: not written whole, the file is removed and logged (write_whole_file), and a
              // `.part` never replaces the finished file. The path's copy may fail (out of memory): the item is still
              // freed below and the queue goes on
              std::wstring const path{item->path};
              written = item->context ? write_whole_file(path, context.data(), context.size())
                                      : write_whole_file(path, item + 1, item->bytes);
              }
            catch(...)
              {
              log_line("dump: {} not written (out of memory)", wide(item->path));
              }
          if(written)
            {
            if(item->rename_to[0] and not MoveFileExW(item->path, item->rename_to, MOVEFILE_REPLACE_EXISTING))
              log_line("dump: {} could not replace {} (error {})", wide(item->path), wide(item->rename_to), GetLastError());
            else if(item->loud)
              log_line("dump: wrote {}", wide(item->path));
            }
          _aligned_free(item);
          writes_done.fetch_add(1, std::memory_order_release);
          }
        }
      }

    ///\brief a file of these bytes, written later by the writer thread; false when it cannot be queued (logged)
    auto queue_write(wchar_t const * path, void const * data, std::uint32_t bytes, bool loud = false,
                     wchar_t const * rename_to = nullptr, bool context = false) noexcept -> bool
      {
      InitOnceExecuteOnce(
        &writer_once,
        [](PINIT_ONCE, PVOID, PVOID *) -> BOOL
          {
          InitializeSListHead(&write_queue);
          write_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
          auto const writer_thread{[](LPVOID) -> DWORD
                                   {
                                     DWORD const r{run_guarded_thread("writer", &writer_body, nullptr)};
                                     // stopped after its faults: nothing more is queued for no one to write
                                     writer_running.store(false);
                                     log_line("dump: the writer thread stopped; files are no longer written");
                                     return r;
                                   }};
          if(HANDLE const t{write_wake ? CreateThread(nullptr, 0, writer_thread, nullptr, 0, nullptr) : nullptr})
            {
            CloseHandle(t);
            writer_running = true;
            }
          else
            log_line("dump: the writer thread could not start (error {}); nothing is written", GetLastError());
          return TRUE;
          },
        nullptr,
        nullptr
      );
      if(not writer_running)
        return false;
      auto * item{static_cast<write_item_t *>(_aligned_malloc(sizeof(write_item_t) + bytes, MEMORY_ALLOCATION_ALIGNMENT))};
      if(not item)
        {
        log_line("dump: {} not written (no memory for {} bytes)", wide(path), bytes);
        return false;
        }
      std::memset(item, 0, sizeof(write_item_t));
      format_into(item->path, MAX_PATH, L"{}", path);
      if(rename_to)
        format_into(item->rename_to, MAX_PATH, L"{}", rename_to);
      item->loud = loud;
      item->context = context;
      item->bytes = bytes;
      std::memcpy(item + 1, data, bytes);
      writes_queued.fetch_add(1, std::memory_order_relaxed);
      InterlockedPushEntrySList(&write_queue, &item->entry);
      SetEvent(write_wake);
      return true;
      }

    // ---- surface dumps, on request (discovery): which panel shows what ----
    struct pending_dump_t
      {
      ID3D11Texture2D * staging;
      std::uint64_t id;
      std::uint32_t width, height, format;
      LONGLONG qpc;
      std::uint32_t checkpoint;  ///< 0 a panel's surface; else 1 + the draw of a whole-frame dump before which the
                                 ///< render target was copied (full_checkpoint)
      std::uint32_t ps_slot;     ///< with checkpoint: 0 the render target; else 1 + the pixel shader's slot it reads, or
                                 ///< rt_after_slot: the render target right after a dump_vs draw
      };
    constexpr std::uint32_t rt_after_slot{0xFFu};

    constexpr std::uint32_t max_dumps{16};
    constexpr std::uint32_t max_pending{72};  // the panels' surfaces and a whole frame's checkpoints
    pending_dump_t pending_dumps[max_pending]{};
    std::uint32_t pending_count{};
    std::uint64_t dumped_ids[max_dumps]{};
    std::uint32_t dumped_count{};
    std::uint32_t geometry_draws{};  ///< panel draws of the dump frame whose buffers were queued
    bool dump_armed{};
    std::uint64_t dump_frame{};
    LONGLONG last_trigger_check{};
    char dump_stamp[32]{};

    auto trigger_path() -> std::wstring { return stralgo::stl::merge(start_fields().dir, L"\\edworld_dump"sv); }

    auto check_trigger(LONGLONG now) -> void
      {
      if(dump_armed)
        {
        if(frame > dump_frame)  // one frame of panel draws asked; done
          {
          dump_armed = false;
          DeleteFileW(trigger_path().c_str());
          log_line("dump: {} surface(s) queued", dumped_count);
          }
        return;
        }
      if((now - last_trigger_check) * 1000 / qpc_frequency < 500)
        return;
      last_trigger_check = now;
      if(GetFileAttributesW(trigger_path().c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
      dump_armed = true;
      dump_frame = frame;
      dumped_count = 0;
      geometry_draws = 0;
      SYSTEMTIME t;
      GetSystemTime(&t);
      format_into(dump_stamp, "{:04}{:02}{:02}T{:02}{:02}{:02}_{:03}Z", t.wYear, t.wMonth, t.wDay, t.wHour,
                    t.wMinute, t.wSecond, t.wMilliseconds);
      CreateDirectoryW(stralgo::stl::merge(start_fields().dir, L"\\edworld_dumps"sv).c_str(), nullptr);
      log_line("dump: armed ({})", dump_stamp);
      }

    auto queue_dump(ID3D11DeviceContext * ctx) noexcept -> void
      {
      if(pending_count == max_pending or dumped_count == max_dumps)
        return;
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
      auto const id{reinterpret_cast<std::uint64_t>(static_cast<void *>(res))};
      for(std::uint32_t i{}; i != dumped_count; ++i)
        if(dumped_ids[i] == id)
          {
          res->Release();
          return;
          }
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
      dumped_ids[dumped_count++] = id;
      if(d.SampleDesc.Count != 1 or d.ArraySize != 1)
        {
        tex->Release();
        log_line("dump: surface {:08x} {}x{} skipped (samples {}, array {})", static_cast<unsigned long long>(id & 0xffffffffull),
                 d.Width, d.Height, d.SampleDesc.Count, d.ArraySize);
        return;
        }
      D3D11_TEXTURE2D_DESC sd{d};
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      ID3D11Texture2D * staging{};
      if(FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) or not staging)
        {
        tex->Release();
        return;
        }
      ctx->CopyResource(staging, tex);
      tex->Release();
      pending_dumps[pending_count++] = pending_dump_t{staging, id, d.Width, d.Height, static_cast<std::uint32_t>(d.Format), qpc_now(), 0, 0};
      }

    ///\brief writes the dumps whose copies the GPU has finished; a few frames after queueing
    auto write_dumps(LONGLONG now) noexcept -> void
      {
      std::uint32_t kept{};
      for(std::uint32_t i{}; i != pending_count; ++i)
        {
        pending_dump_t & p{pending_dumps[i]};
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT const hr{immediate.load()->Map(p.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)};
        if(hr == DXGI_ERROR_WAS_STILL_DRAWING and (now - p.qpc) * 1000 / qpc_frequency < 2000)
          {
          pending_dumps[kept++] = p;
          continue;
          }
        if(SUCCEEDED(hr) and m.pData)
          {
          wchar_t name[MAX_PATH];
          if(p.checkpoint and p.ps_slot == rt_after_slot)
            format_into(name, MAX_PATH, L"{}\\edworld_dumps\\{}_a{}_rt_after_{}x{}_f{}_pitch{}.raw", start_fields().dir,
                          utf8(dump_stamp), p.checkpoint - 1u, p.width, p.height, p.format, m.RowPitch);
          else if(p.checkpoint and p.ps_slot)
            format_into(name, MAX_PATH, L"{}\\edworld_dumps\\{}_a{}_ps{}_{}x{}_f{}_pitch{}.raw", start_fields().dir,
                          utf8(dump_stamp), p.checkpoint - 1u, p.ps_slot - 1u, p.width, p.height, p.format, m.RowPitch);
          else if(p.checkpoint)
            format_into(name, MAX_PATH, L"{}\\edworld_dumps\\{}_c{:04}_{}x{}_f{}_pitch{}.raw", start_fields().dir,
                          utf8(dump_stamp), p.checkpoint - 1u, p.width, p.height, p.format, m.RowPitch);
          else
            format_into(name, MAX_PATH, L"{}\\edworld_dumps\\{}_{:08x}_{}x{}_f{}_pitch{}.raw", start_fields().dir,
                          utf8(dump_stamp), static_cast<unsigned long long>(p.id & 0xffffffffull), p.width, p.height, p.format,
                          m.RowPitch);
          queue_write(name, m.pData, m.RowPitch * mapped_rows(p.format, p.height), true);
          immediate.load()->Unmap(p.staging, 0);
          }
        else
          log_line("dump: surface {:08x} lost (hr 0x{:08X})", static_cast<unsigned long long>(p.id & 0xffffffffull),
                   static_cast<unsigned long>(hr));
        p.staging->Release();
        }
      pending_count = kept;
      }

    // ---- geometry dumps, with the surface dumps (discovery): what a panel draw reads besides the surface ----
    // Each panel draw of the dump frame: its index range, the vertex buffers bound (the instance stream from its
    // start_instance, the others from its base vertex), the VS constant buffers 0..3 from their first constant and
    // the t33 record pool, each to edworld_dumps/<stamp>_g<draw>_<what>.bin; the draw's arguments go to the log.
    struct pending_buffer_t
      {
      ID3D11Buffer * staging;
      std::uint32_t bytes;
      char name[64];
      LONGLONG qpc;
      };

    constexpr std::uint32_t max_buffers{16384};  // a whole frame's constants too (full_draw)
    constexpr std::uint32_t buffer_window{256u * 1024u};
    constexpr std::uint32_t pool_window{16u * 1024u * 1024u};
    pending_buffer_t pending_buffers[max_buffers]{};
    std::uint32_t pending_buffer_count{};

    auto write_buffers(LONGLONG now) noexcept -> void
      {
      std::uint32_t kept{};
      for(std::uint32_t i{}; i != pending_buffer_count; ++i)
        {
        pending_buffer_t & p{pending_buffers[i]};
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT const hr{immediate.load()->Map(p.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)};
        if(hr == DXGI_ERROR_WAS_STILL_DRAWING and (now - p.qpc) * 1000 / qpc_frequency < 2000)
          {
          pending_buffers[kept++] = p;
          continue;
          }
        if(SUCCEEDED(hr) and m.pData)
          {
          wchar_t name[MAX_PATH];
          format_into(name, MAX_PATH, L"{}\\edworld_dumps\\{}_{}.bin", start_fields().dir, utf8(dump_stamp), utf8(p.name));
          queue_write(name, m.pData, p.bytes);
          immediate.load()->Unmap(p.staging, 0);
          }
        else
          log_line("dump: buffer {} lost (hr 0x{:08X})", p.name, static_cast<unsigned long>(hr));
        p.staging->Release();
        }
      pending_buffer_count = kept;
      }

    ///\brief a copy of bytes [from, from + bytes) of a buffer, written by write_buffers once the GPU has made it
    auto queue_buffer(ID3D11DeviceContext * ctx, ID3D11Buffer * source, std::uint64_t from, std::uint64_t bytes,
                      char const * name, bool loud = true) noexcept -> void
      {
      if(pending_buffer_count == max_buffers or not source)
        return;
      D3D11_BUFFER_DESC bd{};
      source->GetDesc(&bd);
      if(from >= bd.ByteWidth)
        {
        log_line("dump: {} starts past its buffer ({} of {})", name, static_cast<unsigned long long>(from), bd.ByteWidth);
        return;
        }
      bytes = std::min<std::uint64_t>(bytes, bd.ByteWidth - from);
      // a staging buffer's size is a multiple of 16 for some drivers
      std::uint32_t const width{static_cast<std::uint32_t>((bytes + 15u) & ~std::uint64_t{15u})};
      D3D11_BUFFER_DESC sd{};
      sd.ByteWidth = width;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Buffer * staging{};
      if(FAILED(device->CreateBuffer(&sd, nullptr, &staging)) or not staging)
        return;
      D3D11_BOX const box{static_cast<UINT>(from), 0, 0, static_cast<UINT>(from + bytes), 1, 1};
      ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);
      pending_buffer_t & p{pending_buffers[pending_buffer_count++]};
      p.staging = staging;
      p.bytes = static_cast<std::uint32_t>(bytes);
      format_into(p.name, "{}", name);
      p.qpc = qpc_now();
      if(loud)
        log_line("dump: {} = {} byte(s) from {} of a buffer of {} (stride {}, bind 0x{:x}, misc 0x{:x})", name, p.bytes,
               static_cast<unsigned long long>(from), bd.ByteWidth, bd.StructureByteStride, as_unsigned(bd.BindFlags), as_unsigned(bd.MiscFlags));
      }

    // ---- the atmosphere read off the game's shell of it (both builds since 2026-11-11): every clouds_kind_read_s
    // seconds while the shell is drawn into the picture, its PS t0 (a table of the light through the atmosphere, 256x512
    // R11G11B10) and its VS and PS constant buffers 0..3, copied on the GPU, read back a few frames later: the kind from
    // the table's hue, the day from the planet's turn, for the clouds; nothing written. edworld_eht also keeps them (the
    // fingerprint, PoC 2026-11-09, to put beside EHT's scans; the planet's radius, VS cb1 row 126 x, is the journal's
    // Radius) at most every atmo_record_s seconds, once a session per planet and octave of the camera's distance over the
    // radius, to edworld_atmo beside the log: <stamp>.atmo (the constants) and t0_<hash>_....raw (each table once)
    struct atmo_pending_t
      {
      ID3D11Texture2D * t0;
      ID3D11Buffer * cbs[8];  ///< VS b0..b3, PS b0..b3, each from its first constant
      std::uint32_t bytes[8];
      std::uint32_t width, height, format;
      LONGLONG qpc;
      char stamp[32];
      std::uint32_t shell_index;  ///< which of the frame's shell draws it was (0 = the first)
      };
    constexpr std::uint32_t atmo_max_cb_bytes{8192};
    constexpr std::uint32_t atmo_furthest_band{7};  ///< the camera further than 2^8 radii: the planet a few pixels
    atmo_pending_t atmo_pending{};
    bool atmo_busy{};
    LONGLONG atmo_last{};
    LONGLONG atmo_shell_qpc{};             ///< the last shell draw seen
    std::uint32_t atmo_shells_in_frame{};  ///< the shell's draws in this frame so far
    std::uint32_t atmo_skip{};             ///< the frame's shell draws to pass over before the next read
#if defined(EDWORLD_EHT)
    constexpr std::uint32_t atmo_max_seen{1024};
    LONGLONG atmo_recorded{};
    std::uint64_t atmo_seen[atmo_max_seen]{};  ///< the radius in metres << 8 | the band
    std::uint32_t atmo_seen_count{};
    alignas(16) std::byte atmo_record[64 + 8 * (4 + atmo_max_cb_bytes)];
#endif

    ///\brief how often the shell is read, seconds: for the clouds, and in edworld_eht for the fingerprint too; 0 = never
    [[nodiscard]]
    auto atmo_period_s(settings_t const & live) noexcept -> std::uint32_t
      {
      std::uint32_t const kind{live.clouds_kind_read_s};
#if defined(EDWORLD_EHT)
      std::uint32_t const record{start_fields().atmo_record_s};
      if(record != 0u and (kind == 0u or record < kind))
        return record;
#endif
      return kind;
      }
    constexpr std::size_t atmo_hue_capacity{256u * 256u};  ///< the upper half of a 256x512 table
    float atmo_hue_r[atmo_hue_capacity], atmo_hue_b[atmo_hue_capacity];
    // the planets read, each with its kind (atmosphere_planets.h); the nearest one's is published below for the clouds
    atmosphere_planets_t atmo_planets{};
    // the kind told to the clouds and the planet it is of (its radius in metres), since when (GetTickCount64): the
    // clouds take it in over atmo_kind_fade_ms. Written when the copies are read back, read at the clouds' draw
    std::atomic<float> atmo_kind_radius{};
    std::atomic<std::uint8_t> atmo_kind{};
    std::atomic<bool> atmo_kind_volcanic{};  ///< neon's second table: a neon atmosphere over volcanism
    std::atomic<bool> atmo_kind_cold{};      ///< ammonia's second table: a cold, oversaturated ammonia atmosphere
    std::atomic<ULONGLONG> atmo_kind_since{};
    // the star's light at the last read (VS b1 row 293): red and blue over green and its level, for the star's wind
    // on the CPU (the substorms) as the shader has it (the oval)
    std::atomic<float> atmo_light_rg{}, atmo_light_bg{}, atmo_light_g{};
    constexpr float atmo_kind_fade_ms{2000.f};

    auto atmo_kind_name(atmosphere_kind_e kind) noexcept -> std::string_view
      {
      for(atmosphere_print_t const & print: atmosphere_prints)
        if(print.kind == kind)
          return print.name;
      return "unknown";
      }

    ///\brief the kind just read for the planet of this radius, kept with that planet (atmosphere_planets.h): a new
    /// planet, or another kind three reads in a row, logged; then the nearest planet's kind told to the clouds
    auto atmo_note_kind(float radius, float closeness, atmosphere_hue_t const & hue) noexcept -> std::size_t
      {
      atmosphere_print_t const * found{atmosphere_kind_of(hue)};
      atmosphere_kind_e const kind{found ? found->kind : atmosphere_kind_e::unknown};
      bool const volcanic{found and kind == atmosphere_kind_e::neon and found->r_g > 1.f};
      bool const cold{found and kind == atmosphere_kind_e::ammonia and found->r_g > 0.7f};
      ULONGLONG const now{GetTickCount64()};
      atmosphere_note_t const note{atmosphere_planet_note(atmo_planets, radius, closeness, kind, volcanic, cold, now)};
      atmosphere_planet_t const & planet{atmo_planets.slot[note.index]};
      if(note.fresh or note.changed)
        log_line("clouds: the planet of radius {:.0f} m is {} (r/g {:.4f} b/g {:.4f})", double(radius),
                 found ? std::string_view{found->name} : "unknown"sv, double(hue.r_g), double(hue.b_g));
      else if(note.held and (planet.other_reads == 1u or note.first_unknown))
        {
        std::string_view const kept{atmo_kind_name(planet.kind)};
        log_line("clouds: the planet of radius {:.0f} m stays {} (a read gave {}, r/g {:.4f} b/g {:.4f})", double(radius),
                 std::string_view{kept}, found ? std::string_view{found->name} : "unknown"sv, double(hue.r_g), double(hue.b_g));
        }
      // the planet the clouds are for: the nearest of those read lately
      if(atmosphere_planet_t const * const closest{atmosphere_planet_nearest(atmo_planets, now)})
        {
        // another planet the nearest now: its clouds come in from now, as a new kind's (the second audit, 2026-11-18:
        // they stood whole at once, the planet's kind being older)
        bool const other_planet{closest->radius != atmo_kind_radius.load(std::memory_order_relaxed)};
        atmo_kind_since.store(other_planet ? std::max<ULONGLONG>(closest->kind_since_ms, now) : closest->kind_since_ms,
                              std::memory_order_relaxed);
        atmo_kind_volcanic.store(closest->volcanic, std::memory_order_relaxed);
        atmo_kind_cold.store(closest->cold, std::memory_order_relaxed);
        atmo_kind.store(static_cast<std::uint8_t>(closest->kind), std::memory_order_relaxed);
        atmo_kind_radius.store(closest->radius, std::memory_order_relaxed);
        }
      return note.index;
      }

#if defined(EDWORLD_EHT)
    // ---- the atmosphere last read, published to `atmosphere` in shm_dir for EHT: is its print known ----
    atmosphere_t * atmo_share{};
    bool atmo_share_failed{};

    auto atmo_share_open() -> atmosphere_t *
      {
      if(atmo_share or atmo_share_failed)
        return atmo_share;
      atmo_share_failed = true;
      std::wstring const & dir{start_fields().shm_dir};
      if(dir.empty())
        return nullptr;
      CreateDirectoryW(dir.c_str(), nullptr);
      std::wstring const path{stralgo::stl::merge(dir, L"\\atmosphere"sv)};
      // edplugin_common's: opened or made, grown to the record's size, mapped; a failure logged with the tag
      auto * const view{static_cast<atmosphere_t *>(edplugin_common::map_file_rw(path, sizeof(atmosphere_t), "atmo"))};
      if(not view)
        return nullptr;
      std::memset(view, 0, sizeof(atmosphere_t));
      view->magic = atmosphere_magic;
      view->version = atmosphere_version;
      view->size = sizeof(atmosphere_t);
      atmo_share = view;
      atmo_share_failed = false;
      log_line("atmo: publishing each read's print to {} ({} bytes)", wide(path), sizeof(atmosphere_t));
      return view;
      }

    ///\brief every read: the planet, its print and whether it is one edworld knows
    auto atmo_publish(float radius, float distance, atmosphere_hue_t const & hue) -> void
      {
      atmosphere_t * const share{atmo_share_open()};
      if(not share)
        return;
      atmosphere_print_t const * found{hue.ok ? atmosphere_kind_of(hue) : nullptr};
      std::atomic_ref<std::uint32_t> sequence{share->sequence};
      sequence.fetch_add(1u, std::memory_order_acq_rel);  // odd: inside
      share->unix_ms = unix_ms_now();
      share->radius = radius;
      share->distance = distance;
      share->r_g = hue.r_g;
      share->b_g = hue.b_g;
      share->ok = hue.ok ? 1u : 0u;
      share->known = found ? 1u : 0u;
      share->second = found and atmosphere_second_print(*found) ? 1u : 0u;
      std::memset(share->kind, 0, sizeof share->kind);
      if(found)
        std::memcpy(share->kind, found->name.data(), std::min(found->name.size(), sizeof share->kind - 1u));
      ++share->reads;
      sequence.fetch_add(1u, std::memory_order_acq_rel);  // even: done
      }
#endif

    // why the shell's reads do not get through (2026-11-12: none in a settlement after logging in, and no word why),
    // counted while none does and logged at most once a minute; the camera too far from the planet is not counted
    struct atmo_misses_t
      {
      std::uint32_t small_target, no_table, odd_table, radius_off;
      float radius, distance;
      };
    atmo_misses_t atmo_misses{};
    LONGLONG atmo_misses_since{};

    auto atmo_misses_log(LONGLONG now) noexcept -> void
      {
      if(atmo_misses_since == 0)
        atmo_misses_since = now;
      if((now - atmo_misses_since) * 1000 / qpc_frequency < 60000)
        return;
      atmo_misses_t const & m{atmo_misses};
      if(m.small_target or m.no_table or m.odd_table or m.radius_off)
        log_line("atmo: no read of the shell got through in a minute: a small target {}, no table {}, a table of more "
                 "samples or layers {}, the radius not the planet's {} (the last radius {:.0f} m, distance {:.0f} km)",
                 m.small_target, m.no_table, m.odd_table, m.radius_off, double(m.radius), double(m.distance) / 1000.0);
      atmo_misses = atmo_misses_t{};
      atmo_misses_since = now;
      }

    // the planet's day from its turn between two reads (the shell's object matrix, VS b0 rows 9..11, turns about the
    // model's y with the planet: 2026-11-10, the day of 20 planets as the scans give it to 0.1%). Known once two reads
    // in a row agree to 1%; a read across the game's change of its frame (a turn of 30 to 180 degrees) agrees with
    // nothing. spin: +1 the planet turns about +y, -1 about -y
    // per planet (the slots of atmo_planets), render thread only
    struct atmo_turn_t
      {
      float radius{};
      float rows[9]{};
      LONGLONG qpc{};
      double day{};    ///< the last read's, to be agreed by the next
      float day_s{};   ///< known: two reads in a row agreed
      float spin{};
      ULONGLONG known_ms{};  ///< when the day became known (GetTickCount64)
      };
    atmo_turn_t atmo_turns[atmosphere_planets_t::size]{};

    auto atmo_note_turn(std::size_t slot, float radius, float const (&rows)[9], LONGLONG qpc) noexcept -> void
      {
      atmo_turn_t & t{atmo_turns[slot]};
      float unit[9];
      for(std::uint32_t r{}; r != 3u; ++r)
        {
        float const n{std::sqrt(rows[r * 3] * rows[r * 3] + rows[r * 3 + 1] * rows[r * 3 + 1] + rows[r * 3 + 2] * rows[r * 3 + 2])};
        for(std::uint32_t c{}; c != 3u; ++c)
          unit[r * 3 + c] = n > 0.f ? rows[r * 3 + c] / n : 0.f;
        }
      bool const same{std::fabs(radius - t.radius) < 4.f};
      double const seconds{static_cast<double>(qpc - t.qpc) / static_cast<double>(qpc_frequency)};
      if(not same)
        t = atmo_turn_t{};  // the slot taken by another planet
      else if(seconds > 1. and seconds < 120.)
        {
        planet_turn_t const turn{planet_turn(t.rows, unit, seconds)};
        double const day{turn.day_s};
        if(turn.ok and t.day > 0. and std::fabs(day - t.day) < 0.01 * t.day)
          {
          bool const news{t.day_s == 0.f};
          if(news)
            t.known_ms = GetTickCount64();
          t.day_s = static_cast<float>(day);
          t.spin = turn.about_y > 0. ? 1.f : -1.f;
          if(news)
            log_line("clouds: the planet of radius {:.0f} m turns in {:.2f} h (about {}y)", double(radius), day / 3600.,
                     turn.about_y > 0. ? "+" : "-");
          }
        t.day = turn.ok ? day : 0.;
        }
      t.radius = radius;
      std::memcpy(t.rows, unit, sizeof unit);
      t.qpc = qpc;
      }

    ///\brief the turn known for the planet of this radius; none (day 0) while not known
    auto atmo_turn_of(float radius) noexcept -> atmo_turn_t const *
      {
      for(atmo_turn_t const & t: atmo_turns)
        if(t.radius > 0.f and std::fabs(radius - t.radius) < 4.f)
          return &t;
      return nullptr;
      }

    snow_walk_t snow_walk{};
    snow_drift_t snow_drift{};
    snow_gravity_t snow_g{};
    ///\brief snow_bench: when it was switched on (steady seconds) and the step logged last (-1 = none)
    double snow_bench_from{-1.0};
    int snow_bench_logged{-1};

    ///\brief the snow's values 112..123 (clouds3d.hlsl, src/snow.h): under a full cloud and in a clear sky (0 = none:
    /// snow off, no kind of snow, not on foot outdoors nor in a craft in the air), how fast the crystals fall, where the
    /// eye went east, north and up since a reference fix, then how fast it goes east, north and up, then the parts drawn
    /// (snow_bench)
    auto atmo_snow_values(settings_t const & live, float * v) noexcept -> void
      {
      std::fill(v, v + 12, 0.f);
      if(not live.snow)
        {
        snow_walk.have = false;
        return;
        }
      auto const kind{static_cast<atmosphere_kind_e>(atmo_kind.load(std::memory_order_relaxed))};
      snow_t const snow{snow_of(kind, atmo_kind_cold.load(std::memory_order_relaxed))};
      flight_t const f{flight()};
      double const gravity{snow_gravity(snow_g, f.has_ground, f.gravity, f.planet_radius)};
      if(snow.under_clouds <= 0.f or not snow_where(f.flags, f.flags2, f.has_ground, f.has_position))
        {
        snow_walk.have = false;
        return;
        }
      float const gain{std::max(live.snow_gain, 0.f)};
      v[0] = snow.under_clouds * snow_most * gain;
      v[1] = snow.clear * snow_most * gain;
      v[2] = snow_fall_speed(gravity, snow.air_density);
      // a clock finer than the tick's 16 ms: at 300 m/s a tick is 5 m of the eye's way
      double const now{std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()};
      v[9] = snow_parts_all;
      if(live.snow_bench)
        {
        if(snow_bench_from < 0.0)
          snow_bench_from = now;
        auto const seconds{static_cast<std::uint64_t>(now - snow_bench_from)};
        snow_bench_step_t const step{snow_bench_step(seconds)};
        v[9] = step.parts;
        int const index{static_cast<int>(seconds / 10u)};
        if(index != snow_bench_logged)
          {
          snow_bench_logged = index;
          log_line("snow bench: {} from now, for 10 s", step.name);
          }
        }
      else if(snow_bench_from >= 0.0)
        {
        snow_bench_from = -1.0;
        snow_bench_logged = -1;
        log_line("snow bench: off");
        }
      // the fix's own time on this clock: when the state thread read it, less the file's age then (C8)
      double const fix_at{now - static_cast<double>(GetTickCount64() - std::min<std::uint64_t>(GetTickCount64(), f.fix_tick_ms) + f.fix_age_ms) / 1000.0};
      // Flags bit 29: the altitude from the mean radius (1) or from the ground under the ship (0); a switch moves it by the
      // ground's height and is a new reference, no move (research 2026-10-10: pads' one-off fixes, C7)
      bool const from_mean{((f.flags >> 29) & 1u) != 0u};
      snow_eye_t const eye{snow_walked(snow_walk, f.latitude, f.longitude, f.altitude, f.has_ground, f.planet_radius, now, fix_at, from_mean)};
      // the fall and the wind's drift summed here in double, the whole wrapped for the shader (C5)
      std::array<double, 2> const drift{snow_drift_step(snow_drift, v[2], now)};
      v[3] = snow_wrapped(eye.place[2] + snow_drift.fall);
      v[4] = snow_wrapped(eye.place[0] + snow_drift.east);
      v[5] = snow_wrapped(eye.place[1] + snow_drift.north);
      v[6] = eye.speed[0];
      v[7] = eye.speed[1];
      v[8] = eye.speed[2];
      v[10] = static_cast<float>(drift[0]);
      v[11] = static_cast<float>(drift[1]);
      if(f.has_ground)
        v[9] += 4.f;  // on foot: the shader drops its 5 km limit over the mean radius (C21)
      }

    ///\brief the day read off the turn of the planet of this radius, 0 = not known
    ///\brief how far the storms and the aurora have come in: 0 until the planet's day is read, then up to 1 over
    /// atmo_day_fade_ms (the audit's C19: they stood on the kind's median day first and jumped once the day was read)
    constexpr float atmo_day_fade_ms{4000.f};

    auto atmo_day_fade(float radius) noexcept -> float
      {
      atmo_turn_t const * const t{atmo_turn_of(radius)};
      if(not t or t->day_s <= 0.f)
        return 0.f;
      return std::clamp(static_cast<float>(GetTickCount64() - t->known_ms) / atmo_day_fade_ms, 0.f, 1.f);
      }

    auto atmo_day_of(float radius) noexcept -> float
      {
      atmo_turn_t const * const t{atmo_turn_of(radius)};
      return t ? t->day_s : 0.f;
      }

    // the planet the clouds are for, as read: what clouds_values (clouds_values.h) makes the clouds' buffer of
    auto atmo_clouds_planet() noexcept -> clouds_planet_t
      {
      clouds_planet_t p{};
      p.kind = static_cast<atmosphere_kind_e>(atmo_kind.load(std::memory_order_relaxed));
      p.radius = atmo_kind_radius.load(std::memory_order_relaxed);
      float const since{static_cast<float>(GetTickCount64() - atmo_kind_since.load(std::memory_order_relaxed))};
      p.kind_weight = std::clamp(since / atmo_kind_fade_ms, 0.f, 1.f);
      p.day_s = atmo_day_of(p.radius);
      p.day_fade = atmo_day_fade(p.radius);
      p.spin = p.day_s > 0.f ? atmo_turn_of(p.radius)->spin : -1.f;
      p.volcanic = atmo_kind_volcanic.load(std::memory_order_relaxed);
      p.light_rg = atmo_light_rg.load(std::memory_order_relaxed);
      p.light_bg = atmo_light_bg.load(std::memory_order_relaxed);
      p.light_g = atmo_light_g.load(std::memory_order_relaxed);
      FILETIME ft{};
      GetSystemTimeAsFileTime(&ft);
      p.utc_s = static_cast<double>((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 1e7;
      return p;
      }

    float atmo_cyclones_radius{};
    std::uint32_t atmo_cyclones_count{~0u};
    bool atmo_cyclones_day{};
    float atmo_aurora_radius{};
    bool atmo_aurora_day{};
    bool atmo_aurora_field{};

    ///\brief the log lines of a change in what the clouds' buffer was made of: the storms, the aurora
    auto atmo_report_changes(settings_t const & live, clouds_planet_t const & p, clouds_report_t const & r) noexcept
      -> void
      {
      if(p.kind == atmosphere_kind_e::unknown or not live.clouds_by_kind)
        return;
      bool const day{p.day_s > 0.f};
      if(live.clouds_cyclones
         and (atmo_cyclones_radius != p.radius or atmo_cyclones_count != r.storms.count or atmo_cyclones_day != day))
        {
        atmo_cyclones_radius = p.radius;
        atmo_cyclones_count = r.storms.count;
        atmo_cyclones_day = day;
        if(day)
          log_line("clouds: {} storm(s) on the planet of radius {:.0f} m, R/L {:.2f} (day {:.2f} h)", r.storms.count, double(p.radius),
                   double(r.rossby), double(p.day_s) / 3600.);
        else
          log_line("clouds: the storms of the planet of radius {:.0f} m wait for its day", double(p.radius));
        }
      if(r.aurora_on and (atmo_aurora_radius != p.radius or atmo_aurora_day != day or atmo_aurora_field != r.aurora.field))
        {
        atmo_aurora_radius = p.radius;
        atmo_aurora_day = day;
        atmo_aurora_field = r.aurora.field;
        log_line("aurora: the planet of radius {:.0f} m {}, the dipole leaning {:.0f} degrees, substorms each {:.1f} h, K {:.1f} now, {:.0f}..{:.0f} km high{}",
                 double(p.radius), r.aurora.field ? "has a field of its own" : "has no field of its own (a faint glow only)",
                 double(r.aurora.lean_deg), double(r.aurora.substorm_s) / 3600., double(r.level), double(r.height.floor * p.radius) / 1000.,
                 double(r.height.ceiling * p.radius) / 1000., day ? "" : " (its day not read yet)");
        }
      }

    // the staging copies, made once and kept (D9, 2026-11-18: made and released at each read before, on the render
    // thread): eight buffers of atmo_max_cb_bytes, the table's texture made again only for another description.
    // atmo_pending_t points at them, holding nothing
    ID3D11Buffer * atmo_cb_staging[8]{};
    ID3D11Texture2D * atmo_t0_staging{};
    D3D11_TEXTURE2D_DESC atmo_t0_desc{};

    auto atmo_release(atmo_pending_t & p) noexcept -> void
      {
      p = atmo_pending_t{};
      atmo_busy = false;
      }

    ///\brief a staging copy of a bound constant buffer from its first constant; null when there is none
    auto atmo_copy_cb(ID3D11DeviceContext * ctx, ID3D11Buffer * source, UINT first, UINT count, std::uint32_t & bytes,
                      std::uint32_t slot) noexcept -> ID3D11Buffer *
      {
      bytes = 0;
      if(not source)
        return nullptr;
      D3D11_BUFFER_DESC bd{};
      source->GetDesc(&bd);
      std::uint32_t const from{first * 16u};
      if(from >= bd.ByteWidth)
        return nullptr;
      std::uint32_t const whole{count ? std::min(count * 16u, bd.ByteWidth - from) : bd.ByteWidth - from};
      bytes = std::min(whole, atmo_max_cb_bytes);
      ID3D11Buffer *& staging{atmo_cb_staging[slot]};
      if(not staging)
        {
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = atmo_max_cb_bytes;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if(FAILED(device->CreateBuffer(&sd, nullptr, &staging)))
          staging = nullptr;
        }
      if(not staging)
        {
        bytes = 0;
        return nullptr;
        }
      D3D11_BOX const box{from, 0, 0, from + bytes, 1, 1};
      ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);
      return staging;
      }

    auto atmo_drain(LONGLONG now) -> void;

    ///\brief at the shell's draw: the copies of its table and constants, when it is time for the next
    auto atmo_queue(ID3D11DeviceContext * ctx, settings_t const & live) -> void
      {
      std::uint32_t const period{atmo_period_s(live)};
      if(period == 0u)
        return;
      LONGLONG const now{qpc_now()};
      // the shell drawn into a small target too, for the reflections: not counted among the frame's shells, or a read
      // waiting for a shell past it waited for good (2026-11-18, the second audit)
      D3D11_VIEWPORT port{};
      UINT ports{1u};
      ctx->RSGetViewports(&ports, &port);
      if(port.Width < 1024.f)
        {
        ++atmo_misses.small_target;
        return;
        }
      // the shell's draws of one frame come within milliseconds: count them, so the reads take each planet in view in
      // turn (atmo_skip, set by the last try), back to the first when a frame has fewer
      if(atmo_shell_qpc == 0 or (now - atmo_shell_qpc) * 1000000 / qpc_frequency > 5000)
        {
        if(atmo_shells_in_frame <= atmo_skip)
          atmo_skip = 0;
        atmo_shells_in_frame = 0;
        }
      std::uint32_t const shell_index{atmo_shells_in_frame++};
      atmo_shell_qpc = now;
      // the copies read back here too, not only at a frame's start: that is found by the HUD's panels, which a
      // commander on foot does not have (2026-11-12: a read made in a settlement waited two minutes for the ship)
      if(atmo_busy)
        atmo_drain(now);
      if(atmo_busy)
        return;
      if((now - atmo_last) * 1000 / qpc_frequency < static_cast<LONGLONG>(period) * 1000)
        return;
      if(shell_index < atmo_skip)
        return;
      atmo_misses_log(now);
      atmo_last = now;
      atmo_skip = shell_index + 1u;  // a try that gives nothing goes on to the next shell; a read sets it the same

      ID3D11ShaderResourceView * srv{};
      ctx->PSGetShaderResources(0, 1, &srv);
      if(not srv)
        {
        ++atmo_misses.no_table;
        return;
        }
      ID3D11Resource * res{};
      srv->GetResource(&res);
      srv->Release();
      ID3D11Texture2D * tex{};
      if(res)
        {
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
        res->Release();
        }
      if(not tex)
        {
        ++atmo_misses.no_table;
        return;
        }
      D3D11_TEXTURE2D_DESC d{};
      tex->GetDesc(&d);
      if(d.SampleDesc.Count != 1 or d.ArraySize != 1)
        {
        ++atmo_misses.odd_table;
        tex->Release();
        return;
        }
      D3D11_TEXTURE2D_DESC sd{d};
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      atmo_pending_t & p{atmo_pending};
      if(atmo_t0_staging
         and (atmo_t0_desc.Width != sd.Width or atmo_t0_desc.Height != sd.Height or atmo_t0_desc.Format != sd.Format
              or atmo_t0_desc.MipLevels != sd.MipLevels))
        {
        atmo_t0_staging->Release();
        atmo_t0_staging = nullptr;
        }
      if(not atmo_t0_staging)
        {
        if(FAILED(device->CreateTexture2D(&sd, nullptr, &atmo_t0_staging)))
          atmo_t0_staging = nullptr;
        atmo_t0_desc = sd;
        }
      if(not atmo_t0_staging)
        {
        tex->Release();
        return;
        }
      p.t0 = atmo_t0_staging;
      ctx->CopyResource(p.t0, tex);
      tex->Release();
      p.width = d.Width;
      p.height = d.Height;
      p.format = static_cast<std::uint32_t>(d.Format);

      ID3D11Buffer * cbs[8]{};
      UINT firsts[8]{}, counts[8]{};
      if(immediate1)
        {
        immediate1->VSGetConstantBuffers1(0, 4, cbs, firsts, counts);
        immediate1->PSGetConstantBuffers1(0, 4, cbs + 4, firsts + 4, counts + 4);
        }
      else
        {
        ctx->VSGetConstantBuffers(0, 4, cbs);
        ctx->PSGetConstantBuffers(0, 4, cbs + 4);
        }
      for(std::uint32_t c{}; c != 8; ++c)
        {
        p.cbs[c] = atmo_copy_cb(ctx, cbs[c], firsts[c], counts[c], p.bytes[c], c);
        if(cbs[c])
          cbs[c]->Release();
        }
      p.qpc = now;
      p.shell_index = shell_index;
      SYSTEMTIME t;
      GetSystemTime(&t);
      format_into(p.stamp, "{:04}{:02}{:02}T{:02}{:02}{:02}_{:03}Z", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                    t.wSecond, t.wMilliseconds);
      atmo_busy = true;
      }

    auto atmo_float(atmo_pending_t const & p, void const * const * data, std::uint32_t slot, std::uint32_t row, std::uint32_t lane)
      noexcept -> float
      {
      std::uint32_t const at{row * 16u + lane * 4u};
      if(not data[slot] or at + 4u > p.bytes[slot])
        return 0.f;
      float v;
      std::memcpy(&v, static_cast<std::byte const *>(data[slot]) + at, sizeof v);
      return v;
      }

    ///\brief once a frame and at the shell's draw: the copies read back once the GPU has made them, kept when the planet
    /// and band are new
    // ---- the atmosphere's thread (D10, 2026-11-18): one job at a time. The render thread fills the job from the
    // read's copies and starts it (working); the thread does atmo_work and says done; the render thread takes the
    // result in at its next look (atmo_apply), and only then may the next read be made. The state and the job's number
    // share one word (atmo_job_word): a thread late with a job dropped meanwhile cannot mark the next one done
    enum struct atmo_job_e : std::uint32_t
      {
      idle,
      working,
      done
      };
    struct atmo_job_t
      {
      float radius, distance, over;
      LONGLONG qpc;
      std::uint32_t width, height, format, pitch, table_bytes, band;
      float turn_rows[9];
      bool record;
      char stamp[32];
      std::uint32_t cb_bytes[8];
      std::byte cbs[8][atmo_max_cb_bytes];
      atmosphere_hue_t hue;  ///< the thread's result
      ULONGLONG started;     ///< GetTickCount64 at the start
      };
    atmo_job_t atmo_job{};
    std::uint8_t atmo_table_copy[2u * 1024u * 1024u];  ///< the table as read (256x512 R11G11B10: 512 KB)
    std::atomic<std::uint32_t> atmo_job_word{};          ///< the job's number << 2 | its state
    HANDLE atmo_wake{};
    bool atmo_thread_tried{};
    std::atomic<bool> atmo_thread_up{};  ///< the thread runs: false when it could not start, stopped or was given up
    std::uint32_t atmo_dropped{};

    auto atmo_job_state() noexcept -> atmo_job_e
      {
      return static_cast<atmo_job_e>(atmo_job_word.load(std::memory_order_acquire) & 3u);
      }

    auto atmo_job_set(atmo_job_e state) noexcept -> void
      {
      std::uint32_t const w{atmo_job_word.load(std::memory_order_relaxed)};
      atmo_job_word.store((w & ~3u) | static_cast<std::uint32_t>(state), std::memory_order_release);
      }

    auto atmo_work() -> void;

    auto atmo_thread_body(void *) -> DWORD
      {
      for(;;)
        {
        WaitForSingleObject(atmo_wake, INFINITE);
        std::uint32_t const w{atmo_job_word.load(std::memory_order_acquire)};
        if(static_cast<atmo_job_e>(w & 3u) != atmo_job_e::working)
          continue;
        atmo_work();
        std::uint32_t expected{w};
        atmo_job_word.compare_exchange_strong(expected, (w & ~3u) | static_cast<std::uint32_t>(atmo_job_e::done),
                                              std::memory_order_acq_rel);
        }
      }

    ///\brief the job filled: to the thread, or done here when there is no thread (it could not start, stopped or was
    /// given up)
    auto atmo_start_job(LONGLONG) -> void
      {
      if(not atmo_thread_tried)
        {
        atmo_thread_tried = true;
        atmo_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        auto const thread{[](LPVOID) -> DWORD
                            {
                              DWORD const r{run_guarded_thread("atmo", &atmo_thread_body, nullptr)};
                              atmo_thread_up.store(false);  // stopped after its faults: the reads are worked out in place
                              return r;
                            }};
        atmo_thread_up.store(true);
        if(HANDLE const h{atmo_wake ? CreateThread(nullptr, 0, thread, nullptr, 0, nullptr) : nullptr})
          CloseHandle(h);
        else
          {
          atmo_thread_up.store(false);
          log_line("atmo: no thread of its own (error {}); the reads are worked out on the render thread", GetLastError());
          }
        }
      atmo_job.started = GetTickCount64();
      // a new number, working
      std::uint32_t const number{(atmo_job_word.load(std::memory_order_relaxed) >> 2) + 1u};
      if(not atmo_thread_up.load())
        {
        atmo_job_word.store(number << 2 | static_cast<std::uint32_t>(atmo_job_e::working), std::memory_order_release);
        atmo_work();
        atmo_job_set(atmo_job_e::done);
        return;
        }
      atmo_job_word.store(number << 2 | static_cast<std::uint32_t>(atmo_job_e::working), std::memory_order_release);
      SetEvent(atmo_wake);
      }

    ///\brief the job's result taken in, on the render thread: the planet's kind and day, the share (edworld_eht); then
    /// the next read may come. A job the thread has not finished in 10 s is dropped and the thread given up: the next
    /// reads are worked out in place (a thread late with the dropped one may still read the copies while they are
    /// filled again; its result no longer counts)
    auto atmo_apply() -> void
      {
      atmo_job_e const state{atmo_job_state()};
      if(state == atmo_job_e::working)
        {
        if(GetTickCount64() - atmo_job.started < 10000u)
          return;
        ++atmo_dropped;
        atmo_thread_up.store(false);
        log_line("atmo: a read not worked out in 10 s, dropped ({} so far); the reads are worked out on the render thread "
                 "from now on", atmo_dropped);
        }
      else if(state == atmo_job_e::done)
        {
        atmo_job_t const & j{atmo_job};
        std::size_t const slot{atmo_note_kind(j.radius, j.over, j.hue)};
        atmo_note_turn(slot, j.radius, j.turn_rows, j.qpc);
#if defined(EDWORLD_EHT)
        atmo_publish(j.radius, j.distance, j.hue);
#endif
        }
      else
        return;
      // idle under a new number: a late "done" for the old one no longer matches
      std::uint32_t const number{(atmo_job_word.load(std::memory_order_relaxed) >> 2) + 1u};
      atmo_job_word.store(number << 2 | static_cast<std::uint32_t>(atmo_job_e::idle), std::memory_order_release);
      atmo_busy = false;
      }

    auto atmo_drain(LONGLONG now) -> void
      {
      atmo_apply();
      if(not atmo_busy or atmo_job_state() != atmo_job_e::idle)
        return;
      atmo_pending_t & p{atmo_pending};
      // the copies are made in order: the last one done, all are
      ID3D11Resource * last{p.t0};
      for(ID3D11Buffer * cb: p.cbs)
        if(cb)
          last = cb;
      ID3D11DeviceContext * ctx{immediate.load()};
      D3D11_MAPPED_SUBRESOURCE probe{};
      HRESULT const hr{ctx->Map(last, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &probe)};
      if(hr == DXGI_ERROR_WAS_STILL_DRAWING and (now - p.qpc) * 1000 / qpc_frequency < 2000)
        return;
      if(FAILED(hr))
        {
        log_line("atmo: {} lost (hr 0x{:08X})", p.stamp, static_cast<unsigned long>(hr));
        atmo_release(p);
        return;
        }
      ctx->Unmap(last, 0);

      D3D11_MAPPED_SUBRESOURCE maps[8]{};
      void const * data[8]{};
      for(std::uint32_t c{}; c != 8; ++c)
        if(p.cbs[c] and SUCCEEDED(ctx->Map(p.cbs[c], 0, D3D11_MAP_READ, 0, &maps[c])))
          data[c] = maps[c].pData;
      auto const unmap_cbs{[&]() noexcept
                             {
                               for(std::uint32_t c{}; c != 8; ++c)
                                 if(data[c])
                                   ctx->Unmap(p.cbs[c], 0);
                             }};

      // VS b1 row 126 x the planet's radius, VS b0 rows 9..11 w its centre from the camera, both in metres
      float const radius{atmo_float(p, data, 1, 126, 0)};
      float const x{atmo_float(p, data, 0, 9, 3)}, y{atmo_float(p, data, 0, 10, 3)}, z{atmo_float(p, data, 0, 11, 3)};
      float const distance{std::sqrt(x * x + y * y + z * z)};
      float const over{radius > 1000.f ? distance / radius : 0.f};
      if(over < 0.9f or over >= float(2u << atmo_furthest_band))  // below 0.9: the radius not the planet's
        {
        if(over < 0.9f)
          {
          ++atmo_misses.radius_off;
          atmo_misses.radius = radius;
          atmo_misses.distance = distance;
          }
        atmo_skip = p.shell_index + 1u;
        unmap_cbs();
        atmo_release(p);
        return;
        }
      atmo_misses = atmo_misses_t{};
      atmo_misses_since = p.qpc;
      // the work on what was read goes to the atmosphere's thread (atmo_work); here only the copies out of the mapped
      // buffers. Its result comes back to this thread (atmo_apply), which alone keeps the planets, their days and the
      // kind the clouds take
      atmo_job_t & j{atmo_job};
      j.radius = radius;
      j.distance = distance;
      j.over = over;
      j.qpc = p.qpc;
      j.width = p.width;
      j.height = p.height;
      j.format = p.format;
      j.hue = atmosphere_hue_t{};
      j.table_bytes = 0u;
      j.pitch = 0u;
      {
      float const lr{atmo_float(p, data, 1, 293, 0)}, lg{atmo_float(p, data, 1, 293, 1)}, lb{atmo_float(p, data, 1, 293, 2)};
      if(lg > 0.f)
        {
        atmo_light_rg.store(lr / lg, std::memory_order_relaxed);
        atmo_light_bg.store(lb / lg, std::memory_order_relaxed);
        atmo_light_g.store(lg, std::memory_order_relaxed);
        }
      }
      // the next read tries the frame's next shell, so each planet in view is read in turn (atmo_queue)
      atmo_skip = p.shell_index + 1u;
      float const turn_rows[9]{atmo_float(p, data, 0, 9, 0),  atmo_float(p, data, 0, 9, 1),  atmo_float(p, data, 0, 9, 2),
                               atmo_float(p, data, 0, 10, 0), atmo_float(p, data, 0, 10, 1), atmo_float(p, data, 0, 10, 2),
                               atmo_float(p, data, 0, 11, 0), atmo_float(p, data, 0, 11, 1), atmo_float(p, data, 0, 11, 2)};
      std::memcpy(j.turn_rows, turn_rows, sizeof turn_rows);
      j.record = false;
#if defined(EDWORLD_EHT)
      // the fingerprint: at most every atmo_record_s seconds, once a session per planet and band
      j.band = over < 1.f ? 0u : static_cast<std::uint32_t>(std::floor(std::log2(over)));
      std::uint64_t const key{(static_cast<std::uint64_t>(std::llround(radius)) << 8) | j.band};
      bool record{start_fields().atmo_record_s != 0u and atmo_seen_count != atmo_max_seen
                  and (p.qpc - atmo_recorded) * 1000 / qpc_frequency >= static_cast<LONGLONG>(start_fields().atmo_record_s) * 1000};
      for(std::uint32_t i{}; record and i != atmo_seen_count; ++i)
        if(atmo_seen[i] == key)
          record = false;
      if(record)
        {
        atmo_seen[atmo_seen_count++] = key;
        atmo_recorded = p.qpc;
        j.record = true;
        std::memcpy(j.stamp, p.stamp, sizeof j.stamp);
        for(std::uint32_t c{}; c != 8; ++c)
          {
          j.cb_bytes[c] = data[c] ? p.bytes[c] : 0u;
          if(j.cb_bytes[c])
            std::memcpy(j.cbs[c], data[c], j.cb_bytes[c]);
          }
        }
#endif
      unmap_cbs();
      // the table: for the hue when it is R11G11B10, whole for the record
      D3D11_MAPPED_SUBRESOURCE tm{};
      if((p.format == dxgi_r11g11b10_float or j.record) and SUCCEEDED(ctx->Map(p.t0, 0, D3D11_MAP_READ, 0, &tm)) and tm.pData)
        {
        std::uint64_t const bytes{static_cast<std::uint64_t>(tm.RowPitch) * mapped_rows(p.format, p.height)};
        if(bytes <= sizeof atmo_table_copy)
          {
          std::memcpy(atmo_table_copy, tm.pData, static_cast<std::size_t>(bytes));
          j.table_bytes = static_cast<std::uint32_t>(bytes);
          j.pitch = tm.RowPitch;
          }
        ctx->Unmap(p.t0, 0);
        }
      p = atmo_pending_t{};  // the copies are free for the next read once the job is back (atmo_busy stays)
      atmo_start_job(now);
      }

    ///\brief the atmosphere thread's part (D10, 2026-11-18: on the render thread before, the hue of 32768 texels and two
    /// medians every read, in edworld_eht the table's hash over 512 KB and the record): the hue, and the fingerprint's
    /// files; nothing the render thread reads but the job's result
    auto atmo_work() -> void
      {
      atmo_job_t & j{atmo_job};
      if(j.format == dxgi_r11g11b10_float and j.table_bytes)
        j.hue = atmosphere_hue(atmo_table_copy, j.width, j.height, j.pitch, atmo_hue_r, atmo_hue_b, atmo_hue_capacity);
#if defined(EDWORLD_EHT)
      if(not j.record)
        return;
      std::wstring const folder{stralgo::stl::merge(start_fields().dir, L"\\edworld_atmo"sv)};
      CreateDirectoryW(folder.c_str(), nullptr);
      std::uint64_t hash{0xcbf29ce484222325ull};
      bool new_table{};
      if(j.table_bytes)
        {
        for(std::uint32_t i{}; i != j.table_bytes; ++i)
          hash = (hash ^ atmo_table_copy[i]) * 0x100000001b3ull;
        wchar_t name[MAX_PATH];
        format_into(name, MAX_PATH, L"{}\\t0_{:016x}_{}x{}_f{}_pitch{}.raw", folder,
                      static_cast<unsigned long long>(hash), j.width, j.height, j.format, j.pitch);
        if(GetFileAttributesW(name) == INVALID_FILE_ATTRIBUTES)
          new_table = queue_write(name, atmo_table_copy, j.table_bytes);
        }

      // EDWATMO1, then u32 width height format pitch, u64 the table's hash (0: not read), f32 radius distance, then
      // for each of VS b0..b3, PS b0..b3: u32 bytes and the bytes
      std::byte * at{atmo_record};
      auto const put{[&](void const * v, std::size_t n) noexcept
                       {
                         std::memcpy(at, v, n);
                         at += n;
                       }};
      put("EDWATMO1", 8);
      std::uint32_t const head[4]{j.width, j.height, j.format, j.pitch};
      put(head, sizeof head);
      std::uint64_t const table{j.table_bytes ? hash : 0ull};
      put(&table, sizeof table);
      float const where[2]{j.radius, j.distance};
      put(where, sizeof where);
      for(std::uint32_t c{}; c != 8; ++c)
        {
        put(&j.cb_bytes[c], sizeof j.cb_bytes[c]);
        if(j.cb_bytes[c])
          put(j.cbs[c], j.cb_bytes[c]);
        }
      wchar_t name[MAX_PATH];
      format_into(name, MAX_PATH, L"{}\\{}.atmo", folder, utf8(j.stamp));
      queue_write(name, atmo_record, static_cast<std::uint32_t>(at - atmo_record));
      atmosphere_print_t const * kind{atmosphere_kind_of(j.hue)};
      log_line("atmo: {} radius {:.0f} m, distance {:.0f} km (band {}), t0 {:016x} {}x{} f{}{}, kind {} (r/g {:.4f} b/g {:.4f})",
               j.stamp, double(j.radius), double(j.distance) / 1000.0, j.band, static_cast<unsigned long long>(table), j.width,
               j.height, j.format, new_table ? " (new table)" : "", kind ? std::string_view{kind->name} : "unknown"sv,
               double(j.hue.r_g), double(j.hue.b_g));
#endif
      }

    auto guarded_atmo(ID3D11DeviceContext * ctx, settings_t const & live) noexcept -> void
      {
      ++own_depth;
      hook_work(
        [&]
        {
          atmo_queue(ctx, live);
        },
        no_handler);
      --own_depth;
      }

    auto guarded_atmo_drain(LONGLONG now) noexcept -> void
      {
      ++own_depth;
      hook_work(
        [&]
        {
          atmo_drain(now);
        },
        no_handler);
      --own_depth;
      }

    // ---- the whole frame (discovery, full_dump_request): every draw of one frame on the immediate context, one log
    // line each (its kind, the vertex shader's hash, counts, viewport, render target) and the first full_cb_bytes of
    // its VS constant buffers 0..3 as edworld_dumps/<stamp>_a<draw>_cb<slot>.bin; the surfaces of that frame too.
    // Made to find what is not a panel - the target's hologram, the camera, the light - by what changes with it.
    // 12000: at a carrier (2026-10-22) the frame held more than 4000 draws before the holograms' (drawn near its end)
    constexpr std::uint32_t full_max_draws{12000};
    constexpr std::uint32_t full_cb_bytes{1024};
    std::atomic<bool> full_armed{};  // read by every context's draw hook (another one only counts itself)
    std::uint64_t full_frame{};
    std::uint32_t full_draws{};
    LONGLONG last_full_check{};
    // the draws' lines go to <stamp>_draws.txt, not to the log: a line flushed per draw cost the render thread too
    constexpr std::uint32_t full_text_bytes{full_max_draws * 192u};
    char full_text[full_text_bytes];
    std::uint32_t full_text_used{};
    bool full_kept_written[max_kept_vs]{};  ///< a dump_vs bytecode already written with this dump
    // checkpoints: the screen-sized render target copied before every full_checkpoint_every-th draw (and at the
    // frame's end), to see between which draws something appears on the picture
    constexpr std::uint32_t full_checkpoint_every{1000};
    constexpr std::uint32_t full_max_checkpoints{56};
    // the frame begins with the panels laid over the scene: there, before every full_early_every-th draw up to
    // full_early_draws; and each screen-sized target once, as the draws leave it (a pass done, e.g. the UI atlas)
    constexpr std::uint32_t full_early_draws{52};
    constexpr std::uint32_t full_early_every{4};
    constexpr std::uint32_t full_max_left{12};
    ID3D11Texture2D * full_prev_rt{};  ///< the screen-sized target of the draw before, held
    void * full_left[full_max_left]{};
    std::uint32_t full_left_count{};
    std::uint32_t full_next_checkpoint{};
    std::uint32_t full_checkpoints{};
    ID3D11Texture2D * full_last_rt{};  ///< the last screen-sized target drawn to, for the end's checkpoint
    std::uint32_t full_after_draw{};   ///< 1 + a dump_vs draw whose target is copied after the game's draw; 0 none

    ///\brief a copy of this render target as it is now (before draw `a`), written by write_dumps as `<stamp>_c<a>_...`
    auto full_checkpoint(ID3D11DeviceContext * ctx, ID3D11Texture2D * tex, D3D11_TEXTURE2D_DESC const & d, std::uint32_t a,
                         std::uint32_t ps_slot = 0) noexcept -> void
      {
      if(pending_count == max_pending or full_checkpoints == full_max_checkpoints)
        return;
      D3D11_TEXTURE2D_DESC sd{d};
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      sd.MipLevels = 1;
      ID3D11Texture2D * staging{};
      if(FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) or not staging)
        {
        log_line("dump: checkpoint before a{} not made (no staging {}x{} f{})", a, d.Width, d.Height, static_cast<unsigned>(d.Format));
        return;
        }
      ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
      ++full_checkpoints;
      pending_dumps[pending_count++] = pending_dump_t{staging, 0, d.Width, d.Height, static_cast<std::uint32_t>(d.Format), qpc_now(), a + 1u,
                                                      ps_slot};
      }
    // why a dump's frame ended (2026-10-22: with EDHM behind edloader each whole frame held 18 draws): the draws of
    // other contexts while armed (a deferred context's or a wrapper's - not recorded), how many such contexts, the gap
    // before the panel draw that began the next frame, and the time from arming
    std::atomic<std::uint32_t> full_other_draws{};
    constexpr std::uint32_t full_max_other{4};
    std::atomic<ID3D11DeviceContext *> full_other[full_max_other]{};
    std::atomic<std::uint32_t> full_other_more{};  ///< draws of contexts beyond full_max_other
    LONGLONG full_armed_qpc{};
    LONGLONG frame_gap_qpc{};  ///< the gap before the panel draw that began the current frame

    ///\brief a draw of another context than the immediate one while a whole frame is armed: counted, not recorded
    auto note_full_other(ID3D11DeviceContext * self) noexcept -> void
      {
      full_other_draws.fetch_add(1, std::memory_order_relaxed);
      for(std::atomic<ID3D11DeviceContext *> & slot: full_other)
        {
        ID3D11DeviceContext * seen{slot.load(std::memory_order_relaxed)};
        if(seen == self)
          return;
        if(not seen and slot.compare_exchange_strong(seen, self, std::memory_order_relaxed))
          return;
        if(seen == self)
          return;
        }
      full_other_more.fetch_add(1, std::memory_order_relaxed);
      }

    bool full_done_waiting{};               ///< the frame is over; dump_done once its copies are queued
    std::uint32_t full_done_draws{};

    template<typename... args_t>
    auto full_line(std::format_string<args_t...> fmt, args_t &&... args) noexcept -> void
      {
      if(full_text_bytes - full_text_used < 2u)
        return;
      // the room less one byte, kept for the line's end: format_into's NUL lands where the line's end goes
      std::size_t const n{format_into(full_text + full_text_used, full_text_bytes - full_text_used - 1u, fmt, std::forward<args_t>(args)...)};
      full_text_used = std::min<std::uint32_t>(full_text_used + static_cast<std::uint32_t>(n), full_text_bytes - 2u);
      full_text[full_text_used++] = '\n';
      }

    auto in_dump_vs(std::uint64_t hash) noexcept -> bool
      {
      for(std::uint64_t const h: start_fields().dump_vs)
        if(h == hash)
          return true;
      return false;
      }

    ///\brief once the whole frame's copies are all queued: the draws' lines, then (edworld_eht) `dump_done` in
    /// shm_dir with the stamp and the draw count, which the data source shows; queued last, it lands last
    auto finish_full() noexcept -> void
      {
      full_done_waiting = false;
      wchar_t name[MAX_PATH];
      format_into(name, MAX_PATH, L"{}\\edworld_dumps\\{}_draws.txt", start_fields().dir, utf8(dump_stamp));
      queue_write(name, full_text, full_text_used, true);
#if defined(EDWORLD_EHT)
      if(not start_fields().shm_dir.empty())
        {
        char text[64];
        std::size_t const n{format_into(text, "{} {}\n", dump_stamp, full_done_draws)};
        std::wstring const done{stralgo::stl::merge(start_fields().shm_dir, L"\\dump_done"sv)};
        queue_write(stralgo::stl::merge(done, L".part"sv).c_str(), text, static_cast<std::uint32_t>(n), false, done.c_str());
        }
#endif
      log_line("dump: whole frame's files queued ({} written of {} so far)", writes_done.load(), writes_queued.load());
      }

    auto full_trigger_path() -> std::wstring { return stralgo::stl::merge(start_fields().dir, L"\\edworld_dump_all"sv); }

    auto check_full(LONGLONG now, bool panels) -> void
      {
      if(full_armed.load(std::memory_order_relaxed))
        {
        if(frame > full_frame)
          {
          full_armed.store(false, std::memory_order_relaxed);
          full_done_waiting = true;
          full_done_draws = full_draws;
          if(full_last_rt)
            {
            D3D11_TEXTURE2D_DESC d{};
            full_last_rt->GetDesc(&d);
            full_checkpoint(immediate.load(), full_last_rt, d, full_draws);
            full_last_rt->Release();
            full_last_rt = nullptr;
            }
          if(full_prev_rt)
            {
            full_prev_rt->Release();
            full_prev_rt = nullptr;
            }
          DeleteFileW(full_trigger_path().c_str());
          log_line("dump: whole frame {}, {} draw(s) recorded", static_cast<unsigned long long>(full_frame), full_draws);
          std::uint32_t contexts{};
          for(std::atomic<ID3D11DeviceContext *> const & slot: full_other)
            contexts += slot.load(std::memory_order_relaxed) ? 1u : 0u;
          log_line("dump: the frame ended {:.2f} ms after arming, at a draw after a gap of {} us (frame_gap_us {}); "
                   "draws of other contexts meanwhile {} in {} context(s){} (not recorded)",
                   static_cast<double>(now - full_armed_qpc) * 1000.0 / static_cast<double>(qpc_frequency),
                   frame_gap_qpc * 1000000 / qpc_frequency, start_fields().frame_gap_us, full_other_draws.load(), contexts,
                   full_other_more.load() ? " and more" : "");
          }
        return;
        }
      bool asked{full_dump_request.exchange(false)};
      if(not asked and (now - last_full_check) * 1000 / qpc_frequency >= 500)
        {
        last_full_check = now;
        asked = GetFileAttributesW(full_trigger_path().c_str()) != INVALID_FILE_ATTRIBUTES;
        }
      if(not asked)
        return;
      // the surfaces of the same frame, as edworld_dump makes them (unless that dump is armed already; not in a frame
      // without panels, which has none)
      if(panels and not dump_armed)
        {
        dump_armed = true;
        dump_frame = frame;
        dumped_count = 0;
        geometry_draws = 0;
        }
      full_other_draws.store(0);
      full_other_more.store(0);
      for(std::atomic<ID3D11DeviceContext *> & slot: full_other)
        slot.store(nullptr);
      full_armed_qpc = now;
      full_armed.store(true, std::memory_order_relaxed);
      full_frame = frame;
      full_draws = 0;
      full_text_used = 0;
      for(bool & w: full_kept_written)
        w = false;
      full_next_checkpoint = full_checkpoint_every;
      full_checkpoints = 0;
      full_left_count = 0;
      SYSTEMTIME t;
      GetSystemTime(&t);
      format_into(dump_stamp, "{:04}{:02}{:02}T{:02}{:02}{:02}_{:03}Z", t.wYear, t.wMonth, t.wDay, t.wHour,
                    t.wMinute, t.wSecond, t.wMilliseconds);
      CreateDirectoryW(stralgo::stl::merge(start_fields().dir, L"\\edworld_dumps"sv).c_str(), nullptr);
      log_line("dump: whole frame armed ({})", dump_stamp);
      }

    auto holo_end_laying_over() noexcept -> void;

    ///\brief panels: a panel draw began the frame; without (frame_without_panels) a surface dump is not armed or closed
    /// there, it waits for a frame with panels
    auto begin_frame(LONGLONG now, bool panels = true) -> void
      {
      if(recording >= 0 and ring[recording].count)
        ring[recording].filled = true;
      recording = -1;
      ++frame;
      holo_end_laying_over();
      if constexpr(not with_eht)
        {
        if(start_fields().log_interval_ms and (now - last_log_qpc) * 1000 / qpc_frequency >= start_fields().log_interval_ms)
          {
          last_log_qpc = now;
          log_line("frame {}: totals draws {} faults {} vs {}", static_cast<unsigned long long>(frame),
                   static_cast<unsigned long long>(stat_draws), faults_seen(), static_cast<unsigned long long>(vs_created.load()));
          }
        }
      // a lost device logged once, flushed (edplugin_common): its reason read at most once a second
      if((now - last_removed_check_qpc) * 1000 / qpc_frequency >= 1000)
        {
        last_removed_check_qpc = now;
        edplugin_common::check_device_removed(device);
        }
      drain(now);
      if(pending_count)
        write_dumps(now);
      if(pending_buffer_count)
        write_buffers(now);
      guarded_atmo_drain(now);
      if(panels)
        check_trigger(now);
      check_full(now, panels);
      if(full_done_waiting and not full_armed.load(std::memory_order_relaxed) and pending_count == 0 and pending_buffer_count == 0)
        finish_full();
      if constexpr(not with_eht)
        return;
      for(int i{}; i != static_cast<int>(ring_slots); ++i)
        if(not ring[i].filled and ring[i].staging and ring[i].side)
          {
          recording = i;
          ring[i].count = 0;
          ring[i].have_pool = false;
          ring[i].have_cb1 = false;
          ring[i].frame = frame;
          ring[i].qpc = now;
          return;
          }
      ++stat_full;
      }

    auto surface_of(ID3D11DeviceContext * ctx, surface_cache_t & out) noexcept -> bool
      {
      ID3D11ShaderResourceView * srv{};
      ctx->PSGetShaderResources(2, 1, &srv);
      if(not srv)
        ctx->PSGetShaderResources(1, 1, &srv);
      if(not srv)
        return false;
      ID3D11Resource * res{};
      srv->GetResource(&res);
      srv->Release();
      if(not res)
        return false;
      // read every time (a few panel draws a frame): a texture made again at a freed one's address (a change of
      // resolution) must not keep the old one's size
      void * const key{res};
      D3D11_RESOURCE_DIMENSION dim{};
      res->GetType(&dim);
      out = {key, 0, 0, 0};
      if(dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        {
        ID3D11Texture2D * tex{};
        if(SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) and tex)
          {
          D3D11_TEXTURE2D_DESC d{};
          tex->GetDesc(&d);
          out.width = d.Width;
          out.height = d.Height;
          out.format = static_cast<std::uint32_t>(d.Format);
          tex->Release();
          }
        }
      res->Release();
      return true;
      }

    ///\brief the dump frame's panel draw: everything it reads besides the surface (see write_buffers)
    auto queue_geometry(ID3D11DeviceContext * ctx, std::uint32_t index_count, std::uint32_t instance_count,
                        std::uint32_t start_index, std::int32_t base_vertex, std::uint32_t start_instance) noexcept -> void
      {
      if(geometry_draws == max_dumps)
        return;
      std::uint32_t const g{geometry_draws++};
      surface_cache_t surface{};
      surface_of(ctx, surface);
      log_line("dump: g{} draw: indices {} from {}, base vertex {}, instances {} from {}, surface {:08x} {}x{}", g,
               index_count, start_index, base_vertex, instance_count, start_instance,
               static_cast<unsigned long long>(reinterpret_cast<std::uint64_t>(surface.resource) & 0xffffffffull),
               surface.width, surface.height);
      char name[64];

      ID3D11Buffer * ib{};
      DXGI_FORMAT ib_format{};
      UINT ib_offset{};
      ctx->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
      if(ib)
        {
        std::uint32_t const size{ib_format == DXGI_FORMAT_R16_UINT ? 2u : 4u};
        format_into(name, "g{}_ib_u{}", g, size * 8u);
        queue_buffer(ctx, ib, ib_offset + static_cast<std::uint64_t>(start_index) * size,
                     static_cast<std::uint64_t>(index_count) * size, name);
        ib->Release();
        }

      ID3D11Buffer * vbs[4]{};
      UINT strides[4]{}, offsets[4]{};
      ctx->IAGetVertexBuffers(0, 4, vbs, strides, offsets);
      for(std::uint32_t v{}; v != 4; ++v)
        {
        if(not vbs[v])
          continue;
        // slot 0 is the instance stream (stage_entry), the others are read from the base vertex
        std::uint64_t const first{v == 0 ? start_instance : static_cast<std::uint64_t>(std::max(base_vertex, 0))};
        format_into(name, "g{}_vb{}_s{}", g, v, strides[v]);
        queue_buffer(ctx, vbs[v], offsets[v] + first * strides[v],
                     v == 0 ? static_cast<std::uint64_t>(instance_count) * strides[v] : buffer_window, name);
        vbs[v]->Release();
        }

      ID3D11Buffer * cbs[4]{};
      UINT firsts[4]{}, counts[4]{};
      if(immediate1)
        immediate1->VSGetConstantBuffers1(0, 4, cbs, firsts, counts);
      else
        ctx->VSGetConstantBuffers(0, 4, cbs);
      for(std::uint32_t c{}; c != 4; ++c)
        {
        if(not cbs[c])
          continue;
        format_into(name, "g{}_cb{}", g, c);
        queue_buffer(ctx, cbs[c], firsts[c] * 16ull, counts[c] ? counts[c] * 16ull : 65536ull, name);
        cbs[c]->Release();
        }

      ID3D11ShaderResourceView * pool{};
      ctx->VSGetShaderResources(33, 1, &pool);
      if(pool)
        {
        ID3D11Resource * res{};
        pool->GetResource(&res);
        pool->Release();
        ID3D11Buffer * buffer{};
        if(res)
          {
          res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void **>(&buffer));
          res->Release();
          }
        if(buffer)
          {
          format_into(name, "g{}_t33", g);
          queue_buffer(ctx, buffer, 0, pool_window, name);
          buffer->Release();
          }
        }
      }

    ///\brief once a frame, at its first panel draw: the t33 record pool and cb1 rows 268..275
    auto stage_frame(ID3D11DeviceContext * ctx, slot_t & slot) noexcept -> void
      {
      ID3D11Buffer * cb1{};
      UINT first{}, count{};
      if(immediate1)
        immediate1->VSGetConstantBuffers1(1, 1, &cb1, &first, &count);
      else
        ctx->VSGetConstantBuffers(1, 1, &cb1);
      if(cb1)
        {
        D3D11_BUFFER_DESC bd{};
        cb1->GetDesc(&bd);
        std::uint32_t const offset{(first + cb1_first_row) * 16u};
        if(offset + cb1_rows * 16u <= bd.ByteWidth)
          {
          D3D11_BOX const box{offset, 0, 0, offset + cb1_rows * 16u, 1, 1};
          ctx->CopySubresourceRegion(slot.side, 0, max_panels * entry_bytes, 0, 0, cb1, 0, &box);
          slot.have_cb1 = true;
          }
        cb1->Release();
        }

      ID3D11ShaderResourceView * srv{};
      ctx->VSGetShaderResources(33, 1, &srv);
      if(not srv)
        return;
      D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
      srv->GetDesc(&vd);
      std::uint32_t first_element{};
      if(vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER)
        first_element = vd.Buffer.FirstElement;
      else if(vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX)
        first_element = vd.BufferEx.FirstElement;
      ID3D11Resource * res{};
      srv->GetResource(&res);
      srv->Release();
      if(not res)
        return;
      D3D11_RESOURCE_DIMENSION dim{};
      res->GetType(&dim);
      ID3D11Buffer * buf{};
      if(dim == D3D11_RESOURCE_DIMENSION_BUFFER)
        res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void **>(&buf));
      res->Release();
      if(not buf)
        return;
      D3D11_BUFFER_DESC bd{};
      buf->GetDesc(&bd);
      if(bd.ByteWidth == 0 or bd.ByteWidth > pool_limit)
        {
        buf->Release();
        return;
        }
      if(slot.pool_capacity < bd.ByteWidth)
        {
        if(slot.pool)
          slot.pool->Release();
        slot.pool = nullptr;
        slot.pool_capacity = 0;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bd.ByteWidth;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if(FAILED(device->CreateBuffer(&d, nullptr, &slot.pool)) or not slot.pool)
          {
          slot.pool = nullptr;
          buf->Release();
          return;
          }
        slot.pool_capacity = bd.ByteWidth;
        }
      std::uint32_t const stride{bd.StructureByteStride ? bd.StructureByteStride : 336u};
      pool_window_t & w{panel_pool};
      if(w.seen_lo <= w.seen_hi)
        {
        w.lo = w.seen_lo > pool_margin_records ? w.seen_lo - pool_margin_records : 0u;
        w.hi = w.seen_hi + pool_margin_records;
        w.known = true;
        w.seen_lo = ~0ull;
        w.seen_hi = 0u;
        }
      std::uint64_t from{0u}, to{bd.ByteWidth};
      if(w.known and not w.missed and stride == slot.pool_stride and first_element == slot.pool_first)
        {
        from = std::min<std::uint64_t>(w.lo * stride, bd.ByteWidth);
        to = std::min<std::uint64_t>((w.hi + 1u) * stride, bd.ByteWidth);
        }
      w.missed = false;
      if(to <= from)
        {
        from = 0u;
        to = bd.ByteWidth;
        }
      D3D11_BOX const box{static_cast<UINT>(from), 0, 0, static_cast<UINT>(to), 1, 1};
      ctx->CopySubresourceRegion(slot.pool, 0, 0, 0, 0, buf, 0, &box);
      buf->Release();
      slot.pool_base = static_cast<std::uint32_t>(from);
      slot.pool_bytes = static_cast<std::uint32_t>(to - from);
      slot.pool_first = first_element;
      slot.pool_stride = stride;
      slot.have_pool = true;
      }

    ///\brief the draw's own instance entry (VB0, at start_instance): its first u32 indexes the pool
    auto stage_entry(ID3D11DeviceContext * ctx, slot_t & slot, std::uint32_t panel, std::uint32_t start_instance)
      noexcept -> bool
      {
      ID3D11Buffer * vb0{};
      UINT stride{}, offset{};
      ctx->IAGetVertexBuffers(0, 1, &vb0, &stride, &offset);
      if(not vb0)
        return false;
      D3D11_BUFFER_DESC bd{};
      vb0->GetDesc(&bd);
      std::uint64_t const at{offset + static_cast<std::uint64_t>(start_instance) * (stride ? stride : 8u)};
      bool const fits{at + 8u <= bd.ByteWidth};
      if(fits)
        {
        D3D11_BOX const box{static_cast<UINT>(at), 0, 0, static_cast<UINT>(at + 8u), 1, 1};
        ctx->CopySubresourceRegion(slot.side, 0, panel * entry_bytes, 0, 0, vb0, 0, &box);
        }
      vb0->Release();
      return fits;
      }

    auto observe_panel(
      ID3D11DeviceContext * ctx,
      std::uint32_t index_count,
      std::uint32_t instance_count,
      std::uint32_t start_index,
      std::int32_t base_vertex,
      std::uint32_t start_instance
    ) -> void
      {
      LONGLONG const now{qpc_now()};
      if((now - last_draw_qpc) * 1000000 / qpc_frequency > start_fields().frame_gap_us)
        {
        frame_gap_qpc = now - last_draw_qpc;
        begin_frame(now);
        }
      // the gap to the next panel is measured from the end of this one: edworld's own work here (a dump's copies and
      // log lines, milliseconds) is no gap of the game's and must not end a frame early
      struct gap_from_end_t
        {
        ~gap_from_end_t() { last_draw_qpc = qpc_now(); }
        } const gap_from_end{};
      ++stat_draws;
      last_watched_ms.store(GetTickCount64(), std::memory_order_relaxed);
      if(start_fields().compass)
        panel_compass(ctx, device, frame);
      if(start_fields().pad_text_samples or start_fields().pad_text_read)
        panel_pad_text(ctx, device, frame);
      if(watched_found[current_watched])
        {
        // found in the game: its draws carry the panel surface, but whether its records decode like the family's
        // is not known, so it gets the patch and publishes nothing
        if(start_fields().patch)
          panel_patch(ctx, device, frame);
        return;
        }
      if constexpr(not with_eht)
        {
        // nothing is published: no copies, the patch alone
        if(dump_armed)
          {
          queue_dump(ctx);
          queue_geometry(ctx, index_count, instance_count, start_index, base_vertex, start_instance);
          }
        if(start_fields().patch)
          panel_patch(ctx, device, frame);
        return;
        }
      if(recording < 0)
        return;
      slot_t & slot{ring[recording]};
      if(slot.count == max_panels)
        return;

      ID3D11Buffer * cb{};
      UINT first{}, count{};
      if(immediate1)
        immediate1->VSGetConstantBuffers1(0, 1, &cb, &first, &count);
      else
        ctx->VSGetConstantBuffers(0, 1, &cb);
      if(not cb)
        return;
      D3D11_BUFFER_DESC bd{};
      cb->GetDesc(&bd);
      std::uint32_t const offset{first * 16u};
      if(offset + record_bytes > bd.ByteWidth)
        {
        cb->Release();
        return;
        }
      D3D11_BOX const box{offset, 0, 0, offset + record_bytes, 1, 1};
      ctx->CopySubresourceRegion(slot.staging, 0, slot.count * record_bytes, 0, 0, cb, 0, &box);
      cb->Release();

      if(dump_armed)
        {
        queue_dump(ctx);
        queue_geometry(ctx, index_count, instance_count, start_index, base_vertex, start_instance);
        }
      if(slot.count == 0)
        stage_frame(ctx, slot);
      slot.have_entry[slot.count] = stage_entry(ctx, slot, slot.count, start_instance);

      if(start_fields().patch)
        panel_patch(ctx, device, frame);

      surface_cache_t surface{};
      surface_of(ctx, surface);
      slot.meta[slot.count++] = meta_t{
        reinterpret_cast<std::uint64_t>(surface.resource),
        surface.width,
        surface.height,
        surface.format,
        watched_index[current_watched],
        index_count,
        instance_count,
        start_index,
        base_vertex,
        start_instance
      };
      }

    // SEH around edworld's own work only: a fault counts against it and, past a few, switches it off.
    auto guarded_observe(
      ID3D11DeviceContext * ctx,
      std::uint32_t index_count,
      std::uint32_t instance_count,
      std::uint32_t start_index,
      std::int32_t base_vertex,
      std::uint32_t start_instance
    ) noexcept -> void
      {
      ++own_depth;
      hook_work(
        [&]
        {
          observe_panel(ctx, index_count, instance_count, start_index, base_vertex, start_instance);
        },
        no_handler);
      --own_depth;
      }

    // without panels (the galaxy map, 2026-11-16: F10 there dumped nothing) no panel draw begins a frame: once none came
    // for panel_less_ms, while a whole-frame dump is asked or under way, a draw of the immediate context after a gap of
    // frame_gap_us since the end of the one before begins one, so the dump still comes (only then: a surface dump still
    // waits for a frame with panels)
    constexpr LONGLONG panel_less_ms{200};
    LONGLONG last_panel_less_check{};

    ///\brief a panel drawn within panel_less_ms: the frames have panels, and the draws between them need no time of
    /// their own (D7, 2026-11-18: two QueryPerformanceCounter calls on each of thousands of draws a frame before; the
    /// tick count is a read of the shared page)
    auto panels_recent() noexcept -> bool
      {
      return GetTickCount64() - last_watched_ms.load(std::memory_order_relaxed) < static_cast<std::uint64_t>(panel_less_ms);
      }

    auto full_dump_wanted(LONGLONG now) -> bool
      {
      if(full_armed.load(std::memory_order_relaxed) or full_done_waiting or pending_count or pending_buffer_count or full_dump_request.load(std::memory_order_relaxed))
        return true;
      if((now - last_panel_less_check) * 1000 / qpc_frequency < 500)
        return false;
      last_panel_less_check = now;
      return GetFileAttributesW(full_trigger_path().c_str()) != INVALID_FILE_ATTRIBUTES;
      }

    auto frame_without_panels(ID3D11DeviceContext * ctx) -> void
      {
      if(ctx != immediate.load(std::memory_order_relaxed) or panels_recent())
        return;
      LONGLONG const now{qpc_now()};
      if((now - last_draw_qpc) * 1000 / qpc_frequency < panel_less_ms or not full_dump_wanted(now))
        return;
      if((now - last_any_draw_qpc) * 1000000 / qpc_frequency > start_fields().frame_gap_us)
        {
        frame_gap_qpc = now - last_any_draw_qpc;
        begin_frame(now, false);
        }
      }

    // tools/test.sh: a fault on the first draw (EDWORLD_TEST_FAULT draw-seh or draw-cpp in a function called under the
    // guard, draw-inline in the guarded body itself)
    char const * const draw_test_fault{[]() noexcept
                                         {
                                           char value[16]{};
                                           GetEnvironmentVariableA("EDWORLD_TEST_FAULT", value, sizeof value);
                                           return std::strcmp(value, "draw-cpp") == 0 ? "draw-cpp" : "draw-seh";
                                         }()};
    std::atomic<bool> inline_test_fault{[]() noexcept
                                          {
                                            char value[16]{};
                                            GetEnvironmentVariableA("EDWORLD_TEST_FAULT", value, sizeof value);
                                            return std::strcmp(value, "draw-inline") == 0;
                                          }()};

    auto guarded_frame_without_panels(ID3D11DeviceContext * ctx) noexcept -> void
      {
      ++own_depth;
      hook_work(
        [&]
        {
          // a load first: no locked exchange on every draw outside the test
          if(inline_test_fault.load(std::memory_order_relaxed) and inline_test_fault.exchange(false, std::memory_order_relaxed))
            *static_cast<int volatile *>(nullptr) = 1;
          test_fault(draw_test_fault);
          frame_without_panels(ctx);
        },
        no_handler);
      --own_depth;
      }

    ///\brief the end of a draw of the immediate context (after edworld's own work in it, a dump's copies: no gap of the
    /// game's)
    auto note_draw_end(ID3D11DeviceContext * ctx) noexcept -> void
      {
      if(ctx == immediate.load(std::memory_order_relaxed) and not panels_recent())
        last_any_draw_qpc = qpc_now();
      }

    // the list under the jump panel, right after the game's draw of it; SEH as around the observation
    auto guarded_after(
      ID3D11DeviceContext * ctx, std::uint32_t index_count, std::uint32_t start_index, std::int32_t base_vertex, std::uint32_t start_instance
    ) noexcept -> void
      {
      bool const list{start_fields().patch and start_fields().list};
      bool const spheres{start_fields().compass and start_fields().compass_spheres};
      if(not list and not spheres)
        return;
      ++own_depth;
      hook_work(
        [&]
        {
          if(list)
            panel_list_after(ctx, frame, index_count, start_index, base_vertex, start_instance);
          if(spheres)
            panel_compass_after(ctx, frame, index_count, start_index, base_vertex, start_instance);
        },
        no_handler);
      --own_depth;
      }

    // edworld's name and version in a corner of the picture, right after a non-indexed draw of the game (its last
    // composite to the back buffer is one); SEH as around the observation
    auto guarded_build_info(ID3D11DeviceContext * ctx) noexcept -> void
      {
      bool const refused{chain_refusal()[0] != '\0'};
      if(start_fields().build_info == 0 or ctx != immediate.load(std::memory_order_relaxed) or (hooks_off() and not refused)
         or build_info_guard.broken())
        return;
      ++own_depth;
      (void)build_info_guard.run([&] { build_info_after(ctx, device); },
                                 [](edplugin_common::fault_t const &) noexcept { creating_own = false; });
      --own_depth;
      }

    auto watched_draw(ID3D11DeviceContext * ctx) noexcept -> bool
      {
      return ctx == immediate.load(std::memory_order_relaxed) and current_watched >= 0 and not hooks_off();
      }

    ///\brief a shader into the watched list; `found` = not listed in watch_vs (any thread)
    auto watch(void * shader, std::uint32_t index, bool found) noexcept -> bool
      {
      std::uint32_t const at{watched_count.fetch_add(1)};
      if(at >= max_watched)
        return false;
      static_cast<IUnknown *>(shader)->AddRef();  // pinned: a released pointer must never be reused for another shader that would be matched
      watched_index[at] = index;
      watched_found[at] = found;
      watched_ptr[at].store(shader, std::memory_order_release);
      // marked in its seen slot too: VSSetShader finds it with the one look into the table it makes anyway (D12)
      if(std::uint32_t const seen{table_get(shader)}; seen < max_vs_seen)
        slot_watched[seen].store(static_cast<std::uint8_t>(at + 1u), std::memory_order_release);
      return true;
      }

    ///\brief the panel-sized surface at PS t1 or t2, if any: its slot and size
    auto panel_surface_at(ID3D11DeviceContext * ctx, std::uint32_t & slot, std::uint32_t & w, std::uint32_t & h) noexcept -> bool;

    ///\brief a shader found drawing a panel's surface though not listed: watched from now on, and said so once
    auto adopt(void * vs, std::uint32_t seen, char const * how, std::uint32_t slot, std::uint32_t w, std::uint32_t h) noexcept -> void
      {
      if(seen < max_vs_seen)
        verdict[seen].store(verdict_e::panel, std::memory_order_relaxed);
      std::uint64_t const hash{seen < max_vs_seen ? seen_hash[seen] : 0};
      bool const ok{watch(vs, static_cast<std::uint32_t>(start_fields().watch_vs.size()), true)};
      log_line("vs {:016X} ({}) draws a panel surface (PS t{} {}x{}) and is not in watch_vs: {} ({})",
               static_cast<unsigned long long>(hash), static_cast<void const *>(vs), slot, w, h, ok ? "watched from now on" : "watched list full", how);
      }

    ///\brief a draw of a shader whose verdict is pending: a panel surface makes it watched; enough draws without, no panel
    auto classify(ID3D11DeviceContext * ctx, void * vs, std::uint32_t seen) noexcept -> void
      {
      std::uint32_t slot{}, w{}, h{};
      if(panel_surface_at(ctx, slot, w, h))
        {
        adopt(vs, seen, "first draws", slot, w, h);
        current_pending = max_vs_seen;
        return;
        }
      if(++verdict_looked[seen] >= verdict_draws)
        {
        verdict[seen].store(verdict_e::not_panel, std::memory_order_relaxed);
        current_pending = max_vs_seen;
        }
      }

    auto guarded_classify(ID3D11DeviceContext * ctx) noexcept -> void
      {
      if(hooks_off())
        return;
      ++own_depth;
      hook_work(
        [&]
        {
          ID3D11VertexShader * vs{};
          ctx->VSGetShader(&vs, nullptr, nullptr);
          if(vs)
            {
            classify(ctx, vs, current_pending);
            vs->Release();
            }
        },
        [&]() noexcept
        {
          current_pending = max_vs_seen;
        });
      --own_depth;
      }

    // ---- the probe (render thread) ----
    constexpr std::uint32_t probe_window_ms{250};
    constexpr std::uint32_t probe_max_lines{40};
    std::uint64_t probe_started_ms{};
    std::uint32_t probe_lines{};
    std::uint32_t probe_surface_draws{};
    void * adopted_in_probe{};  // the probe sees the adopted shader's draws until the next VSSetShader

    auto vs_hash_of(void * vs) noexcept -> std::uint64_t
      {
      std::uint32_t const reserved{seen_reserved.load(std::memory_order_relaxed)};
      std::uint32_t const n{reserved < max_vs_seen ? reserved : max_vs_seen};
      for(std::uint32_t i{}; i != n; ++i)
        if(seen_ptr[i].load(std::memory_order_acquire) == vs)
          return seen_hash[i];
      return 0;
      }

    // the sizes of the panels' interface surfaces seen so far (measured in the game at 2160 rows, 2026-10-04), and the
    // same drawn smaller or larger for another height (surface_scale); 2048x1280 only as it is: at another scale its
    // shape is any 16:10 target's
    auto panel_sized(std::uint32_t w, std::uint32_t h) noexcept -> bool
      {
      return (w == 2048 and h == 1280) or surface_scale(3072u, 660u, w, h) or surface_scale(2200u, 1800u, w, h)
             or surface_scale(1024u, 1534u, w, h);
      }

#if defined(EDWORLD_EHT)
    // ---- the galaxy map's names (src/galmap.h), edworld_eht: a few times a second, while no panel is drawn, the HUD
    // text shader's draws into a target taller than wide (the map's picture of its labels) are copied - their index
    // ranges and their quads' vertices - with the font they read; read back a frame or more later without waiting,
    // the glyphs told by their shape, the labels published to `galmap` in shm_dir ----
    std::atomic<void *> galmap_vs{};
    bool current_galmap{};

    constexpr std::uint32_t galmap_max_draws{256};  // 64 were met with more systems shown (2026-11-16)
    constexpr std::uint32_t galmap_ib_bytes{512u * 1024u};
    constexpr std::uint32_t galmap_vb_bytes{4u * 1024u * 1024u};
    constexpr std::uint32_t galmap_stride{24};  // x, y, ?, ?, u, v as floats (the dumps of 2026-11-16)
    constexpr LONGLONG galmap_collect_ms{20};   // the map's text draws come together at a frame's start
    constexpr LONGLONG galmap_font_ms{5000};    // the font is a cache the game fills: copied again this often

    struct galmap_draw_t
      {
      std::uint32_t ib_at, index_count, vb_at, vertex_count;
      bool wide_index;
      };

    enum struct galmap_phase_e : std::uint8_t
      {
      idle,
      collecting,
      waiting
      };

    struct galmap_state_t
      {
      ID3D11Buffer * ib{};
      ID3D11Buffer * vb{};
      ID3D11Texture2D * font{};
      void * font_source{};  ///< the game's font texture copied last, by its address only
      std::uint32_t font_width{}, font_height{};
      LONGLONG font_copied{};
      bool font_pending{};
      bool font_fresh{};  ///< the font was read back in this round: a glyph not known in it is not known
      bool font_wanted{}; ///< a glyph was not known in an older copy: copy it again in the next round
      std::vector<std::uint8_t> font_pixels;
      std::vector<std::pair<std::uint64_t, char>> chars;  ///< a glyph's rectangle in the font and its character
      std::vector<std::uint64_t> unknown_logged;
      galmap_draw_t draws[galmap_max_draws]{};
      std::uint32_t draw_count{}, ib_used{}, vb_used{};
      galmap_phase_e phase{};
      LONGLONG started{}, last{};
      galmap_t * share{};
      bool share_failed{}, staging_failed{};
      std::uint64_t logged_hash{};
      LONGLONG logged_at{};
      };
    galmap_state_t galmap_state;

    template<typename T>
    auto galmap_release(T *& p) noexcept -> void
      {
      if(p)
        p->Release();
      p = nullptr;
      }

    auto galmap_buffer(std::uint32_t bytes) noexcept -> ID3D11Buffer *
      {
      D3D11_BUFFER_DESC d{};
      d.ByteWidth = bytes;
      d.Usage = D3D11_USAGE_STAGING;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Buffer * b{};
      if(FAILED(device->CreateBuffer(&d, nullptr, &b)))
        return nullptr;
      return b;
      }

    auto galmap_open() noexcept -> galmap_t *
      {
      galmap_state_t & g{galmap_state};
      if(g.share or g.share_failed)
        return g.share;
      g.share_failed = true;
      std::wstring const & dir{start_fields().shm_dir};
      if(dir.empty())
        return nullptr;
      CreateDirectoryW(dir.c_str(), nullptr);
      std::wstring const path{stralgo::stl::merge(dir, L"\\galmap"sv)};
      // edplugin_common's: opened or made, grown to the record's size, mapped; a failure logged with the tag
      auto * const view{static_cast<galmap_t *>(edplugin_common::map_file_rw(path, sizeof(galmap_t), "galmap"))};
      if(not view)
        return nullptr;
      std::memset(view, 0, sizeof(galmap_t));
      view->magic = galmap_magic;
      view->version = galmap_version;
      view->size = sizeof(galmap_t);
      g.share = view;
      g.share_failed = false;
      log_line("galmap: publishing the galaxy map's names to {} ({} bytes)", wide(path), sizeof(galmap_t));
      return view;
      }

    ///\brief the draw writes into the map's picture of its labels: a target taller than wide, no panel's surface
    auto galmap_target(ID3D11DeviceContext * ctx) noexcept -> bool
      {
      ID3D11RenderTargetView * rtv{};
      ctx->OMGetRenderTargets(1, &rtv, nullptr);
      if(not rtv)
        return false;
      ID3D11Resource * res{};
      rtv->GetResource(&res);
      rtv->Release();
      if(not res)
        return false;
      bool labels{};
      ID3D11Texture2D * tex{};
      if(SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) and tex)
        {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        labels = d.Height > d.Width and d.Width >= 1024u and not panel_sized(d.Width, d.Height);
        tex->Release();
        }
      res->Release();
      return labels;
      }

    ///\brief the font the text draw reads (PS t1, one byte a pixel), copied when it is another or not since galmap_font_ms
    auto galmap_font(ID3D11DeviceContext * ctx, LONGLONG now) noexcept -> void
      {
      galmap_state_t & g{galmap_state};
      ID3D11ShaderResourceView * srv{};
      ctx->PSGetShaderResources(1, 1, &srv);
      if(not srv)
        return;
      ID3D11Resource * res{};
      srv->GetResource(&res);
      srv->Release();
      if(not res)
        return;
      ID3D11Texture2D * tex{};
      res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
      res->Release();
      if(not tex)
        return;
      if(tex != g.font_source or g.font_wanted or (now - g.font_copied) * 1000 / qpc_frequency >= galmap_font_ms)
        {
        g.font_wanted = false;
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if((d.Format == DXGI_FORMAT_A8_UNORM or d.Format == DXGI_FORMAT_R8_UNORM) and d.ArraySize == 1u and d.SampleDesc.Count == 1u)
          {
          if(not g.font or g.font_width != d.Width or g.font_height != d.Height)
            {
            galmap_release(g.font);
            D3D11_TEXTURE2D_DESC c{};
            c.Width = d.Width;
            c.Height = d.Height;
            c.MipLevels = c.ArraySize = 1;
            c.Format = d.Format;
            c.SampleDesc.Count = 1;
            c.Usage = D3D11_USAGE_STAGING;
            c.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if(FAILED(device->CreateTexture2D(&c, nullptr, &g.font)))
              g.font = nullptr;
            g.font_width = d.Width;
            g.font_height = d.Height;
            }
          if(g.font)
            {
            ctx->CopySubresourceRegion(g.font, 0, 0, 0, 0, tex, 0, nullptr);
            g.font_source = tex;
            g.font_copied = now;
            g.font_pending = true;
            }
          }
        }
      tex->Release();
      }

    ///\brief one text draw's indices and its quads' vertices (four a quad, from its base vertex) into the staging buffers
    auto galmap_take(ID3D11DeviceContext * ctx, std::uint32_t count, std::uint32_t start, std::int32_t base) noexcept -> void
      {
      galmap_state_t & g{galmap_state};
      if(g.draw_count == galmap_max_draws or count < 6u or base < 0)
        return;
      ID3D11Buffer * ib{};
      DXGI_FORMAT format{};
      UINT ib_offset{};
      ctx->IAGetIndexBuffer(&ib, &format, &ib_offset);
      ID3D11Buffer * vb{};
      UINT stride{}, vb_offset{};
      ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &vb_offset);
      if(ib and vb and stride == galmap_stride)
        {
        bool const wide{format == DXGI_FORMAT_R32_UINT};
        std::uint32_t const index_size{wide ? 4u : 2u};
        std::uint32_t const ib_bytes{count * index_size};
        std::uint32_t const vertices{count / 6u * 4u};
        std::uint32_t const vb_bytes{vertices * galmap_stride};
        D3D11_BUFFER_DESC ibd{}, vbd{};
        ib->GetDesc(&ibd);
        vb->GetDesc(&vbd);
        std::uint64_t const ib_from{ib_offset + std::uint64_t{start} * index_size};
        std::uint64_t const vb_from{vb_offset + static_cast<std::uint64_t>(base) * galmap_stride};
        if(g.ib_used + ib_bytes <= galmap_ib_bytes and g.vb_used + vb_bytes <= galmap_vb_bytes and ib_from + ib_bytes <= ibd.ByteWidth
           and vb_from + vb_bytes <= vbd.ByteWidth)
          {
          D3D11_BOX const ibox{static_cast<UINT>(ib_from), 0, 0, static_cast<UINT>(ib_from + ib_bytes), 1, 1};
          ctx->CopySubresourceRegion(g.ib, 0, g.ib_used, 0, 0, ib, 0, &ibox);
          D3D11_BOX const vbox{static_cast<UINT>(vb_from), 0, 0, static_cast<UINT>(vb_from + vb_bytes), 1, 1};
          ctx->CopySubresourceRegion(g.vb, 0, g.vb_used, 0, 0, vb, 0, &vbox);
          g.draws[g.draw_count++] = galmap_draw_t{g.ib_used, count, g.vb_used, vertices, wide};
          g.ib_used += ib_bytes;
          g.vb_used += vb_bytes;
          }
        }
      if(ib)
        ib->Release();
      if(vb)
        vb->Release();
      }

    ///\brief a glyph's character by its rectangle in the font, worked out once a rectangle (the font copied again
    /// forgets them); a shape no template knows is '?'. In a copy of the font older than this round the glyph may not
    /// be there yet (the game adds glyphs as it needs them): then '?' is not kept, and the font is copied again in the
    /// next round; in a fresh copy it is kept and logged once with its zones, to be named
    auto galmap_char(std::int32_t u0, std::int32_t v0, std::int32_t u1, std::int32_t v1) -> char
      {
      galmap_state_t & g{galmap_state};
      auto const w{static_cast<std::int32_t>(g.font_width)}, h{static_cast<std::int32_t>(g.font_height)};
      if(u0 < 0 or v0 < 0 or u1 > w or v1 > h or u1 <= u0 or v1 <= v0)
        return '?';
      std::uint64_t const key{static_cast<std::uint64_t>(u0) | static_cast<std::uint64_t>(v0) << 16 | static_cast<std::uint64_t>(u1) << 32
                              | static_cast<std::uint64_t>(v1) << 48};
      for(auto const & [k, c]: g.chars)
        if(k == key)
          return c;
      galmap_zones_t const zones{galmap_zones(g.font_pixels.data(), g.font_width, u0, v0, u1, v1)};
      galmap_match_t const m{galmap_match(zones)};
      if(m.c == '?' and not g.font_fresh)
        {
        g.font_wanted = true;
        return m.c;
        }
      g.chars.emplace_back(key, m.c);
      if(m.c == '?' and std::ranges::find(g.unknown_logged, key) == g.unknown_logged.end())
        {
        g.unknown_logged.push_back(key);
        char hex[galmap_zones_x * galmap_zones_y * 3 + 1]{};
        for(std::size_t i{}; i != zones.size(); ++i)
          format_into(hex + i * 3, 4, "{:02x} ", as_unsigned(zones[i]));
        log_line("galmap: a glyph not known yet at {},{} {}x{} (nearest {} apart): {}", u0, v0, u1 - u0, v1 - v0, m.distance, hex);
        }
      return m.c;
      }

    ///\brief the copies read back, if the GPU is done with them: true once done (published, or nothing to read)
    auto galmap_read(ID3D11DeviceContext * ctx) -> bool
      {
      galmap_state_t & g{galmap_state};
      if(g.font_pending)
        {
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT const hr{ctx->Map(g.font, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)};
        if(hr == DXGI_ERROR_WAS_STILL_DRAWING)
          return false;
        g.font_pending = false;
        if(SUCCEEDED(hr) and m.pData)
          {
          g.font_pixels.resize(static_cast<std::size_t>(g.font_width) * g.font_height);
          for(std::uint32_t y{}; y != g.font_height; ++y)
            std::memcpy(g.font_pixels.data() + static_cast<std::size_t>(y) * g.font_width, static_cast<std::uint8_t const *>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch,
                        g.font_width);
          ctx->Unmap(g.font, 0);
          g.chars.clear();
          g.font_fresh = true;
          }
        }
      if(g.font_pixels.empty() or g.draw_count == 0u)
        return true;
      D3D11_MAPPED_SUBRESOURCE mi{}, mv{};
      HRESULT hr{ctx->Map(g.ib, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mi)};
      if(hr == DXGI_ERROR_WAS_STILL_DRAWING)
        return false;
      if(FAILED(hr) or not mi.pData)
        return true;
      hr = ctx->Map(g.vb, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mv);
      if(hr == DXGI_ERROR_WAS_STILL_DRAWING or FAILED(hr) or not mv.pData)
        {
        ctx->Unmap(g.ib, 0);
        return hr != DXGI_ERROR_WAS_STILL_DRAWING;
        }
      float const fw{static_cast<float>(g.font_width)}, fh{static_cast<float>(g.font_height)};
      std::vector<std::string> names;
      std::vector<galmap_quad_t> quads;
      for(std::uint32_t d{}; d != g.draw_count; ++d)
        {
        galmap_draw_t const & draw{g.draws[d]};
        auto const * const indices{static_cast<std::uint8_t const *>(mi.pData) + draw.ib_at};
        auto const * const vertices{static_cast<std::uint8_t const *>(mv.pData) + draw.vb_at};
        quads.clear();
        for(std::uint32_t q{}; q != draw.index_count / 6u; ++q)
          {
          float x0{1e30f}, x1{-1e30f}, y0{1e30f}, y1{-1e30f}, u0{1e30f}, u1{-1e30f}, v0{1e30f}, v1{-1e30f};
          bool inside{true};
          for(std::uint32_t k{}; k != 6u; ++k)
            {
            std::uint32_t index{};
            if(draw.wide_index)
              std::memcpy(&index, indices + (q * 6u + k) * 4u, 4);
            else
              {
              std::uint16_t i16{};
              std::memcpy(&i16, indices + (q * 6u + k) * 2u, 2);
              index = i16;
              }
            if(index >= draw.vertex_count)
              {
              inside = false;
              break;
              }
            float v[6];
            std::memcpy(v, vertices + static_cast<std::size_t>(index) * galmap_stride, sizeof v);
            x0 = std::min(x0, v[0]);
            x1 = std::max(x1, v[0]);
            y0 = std::min(y0, v[1]);
            y1 = std::max(y1, v[1]);
            u0 = std::min(u0, v[4]);
            u1 = std::max(u1, v[4]);
            v0 = std::min(v0, v[5]);
            v1 = std::max(v1, v[5]);
            }
          if(not inside)
            continue;
          char const c{galmap_char(static_cast<std::int32_t>(std::lround(u0 * fw)), static_cast<std::int32_t>(std::lround(v0 * fh)),
                                   static_cast<std::int32_t>(std::lround(u1 * fw)), static_cast<std::int32_t>(std::lround(v1 * fh)))};
          quads.push_back(galmap_quad_t{x0, x1, y1 - y0, c});
          }
        for(std::string & name: galmap_labels(quads))
          if(not name.empty())
            names.push_back(std::move(name));
        }
      ctx->Unmap(g.vb, 0);
      ctx->Unmap(g.ib, 0);

      std::uint32_t unknown{};
      std::uint64_t hash{edplugin_common::fnv1a64_basis};
      for(std::string const & name: names)
        {
        unknown += static_cast<std::uint32_t>(std::count(name.begin(), name.end(), '?'));
        hash = fnv1a64(name.data(), name.size(), hash);  // going on over every name
        }
      if(galmap_t * const share{galmap_open()}; share)
        {
        std::atomic_ref<std::uint32_t> sequence{share->sequence};
        sequence.fetch_add(1u, std::memory_order_acq_rel);  // odd: inside
        share->unix_ms = unix_ms_now();
        auto const n{static_cast<std::uint32_t>(std::min<std::size_t>(names.size(), max_galmap_names))};
        for(std::uint32_t i{}; i != n; ++i)
          {
          std::memset(share->names[i].name, 0, sizeof share->names[i].name);
          std::memcpy(share->names[i].name, names[i].data(), std::min(names[i].size(), sizeof share->names[i].name - 1u));
          }
        share->count = n;
        share->unknown = unknown;
        sequence.fetch_add(1u, std::memory_order_acq_rel);  // even: done
        }
      // the log: when the names change, at most every 10 s
      LONGLONG const now{qpc_now()};
      if(hash != g.logged_hash and (now - g.logged_at) * 1000 / qpc_frequency >= 10000)
        {
        g.logged_hash = hash;
        g.logged_at = now;
        std::string first;
        for(std::size_t i{}; i != std::min<std::size_t>(names.size(), 4u); ++i)
          first += (i ? ", " : "") + names[i];
        log_line("galmap: {} name(s) from {} text draw(s), {} glyph(s) not known yet: {}{}", names.size(), g.draw_count, unknown,
                 first, names.size() > 4u ? ", ..." : "");
        }
      return true;
      }

    ///\brief a draw of the HUD's text shader: while no panel is drawn, its part of the galaxy map's names
    auto galmap_draw(ID3D11DeviceContext * ctx, std::uint32_t count, std::uint32_t start, std::int32_t base) -> void
      {
      LONGLONG const now{qpc_now()};
      if((now - last_draw_qpc) * 1000 / qpc_frequency < panel_less_ms)
        return;
      galmap_state_t & g{galmap_state};
      if(g.phase == galmap_phase_e::waiting)
        {
        if(galmap_read(ctx))
          {
          g.phase = galmap_phase_e::idle;
          g.last = now;
          }
        return;
        }
      if(g.phase == galmap_phase_e::collecting and (now - g.started) * 1000 / qpc_frequency >= galmap_collect_ms)
        {
        g.phase = galmap_phase_e::waiting;
        return;
        }
      // idle: the interval first, the target (four calls) only once it is due (D8, 2026-11-18)
      if(g.phase == galmap_phase_e::idle
         and ((now - g.last) * 1000 / qpc_frequency < std::max<LONGLONG>(100, start_fields().galmap_interval_ms) or g.staging_failed))
        return;
      if(not galmap_target(ctx))
        return;
      if(g.phase == galmap_phase_e::idle)
        {
        if(not g.ib)
          g.ib = galmap_buffer(galmap_ib_bytes);
        if(not g.vb)
          g.vb = galmap_buffer(galmap_vb_bytes);
        if(not g.ib or not g.vb)
          {
          g.staging_failed = true;
          log_line("galmap: no staging buffers; the galaxy map's names are not read");
          return;
          }
        g.phase = galmap_phase_e::collecting;
        g.started = now;
        g.draw_count = g.ib_used = g.vb_used = 0;
        g.font_fresh = false;
        galmap_font(ctx, now);
        }
      galmap_take(ctx, count, start, base);
      }

    auto guarded_galmap(ID3D11DeviceContext * ctx, std::uint32_t count, std::uint32_t start, std::int32_t base) noexcept -> void
      {
      ++own_depth;
      hook_work(
        [&]
        {
          galmap_draw(ctx, count, start, base);
        },
        no_handler);
      --own_depth;
      }
#endif

    auto panel_surface_at(ID3D11DeviceContext * ctx, std::uint32_t & slot, std::uint32_t & w, std::uint32_t & h) noexcept -> bool
      {
      ID3D11ShaderResourceView * srvs[2]{};
      ctx->PSGetShaderResources(1, 2, srvs);
      bool found{};
      for(std::uint32_t i{2}; i-- != 0;)  // t2 first, the panel family's slot
        {
        if(not srvs[i])
          continue;
        if(not found)
          {
          ID3D11Resource * res{};
          srvs[i]->GetResource(&res);
          ID3D11Texture2D * tex{};
          if(res and SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))))
            {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            if(panel_sized(d.Width, d.Height))
              {
              found = true;
              slot = i + 1;
              w = d.Width;
              h = d.Height;
              }
            tex->Release();
            }
          if(res)
            res->Release();
          }
        srvs[i]->Release();
        }
      return found;
      }

    auto probe_draw(ID3D11DeviceContext * ctx, std::uint32_t count, std::uint32_t instances) noexcept -> void
      {
      std::uint64_t const now{GetTickCount64()};
      std::uint32_t const n{probe_draws.fetch_add(1, std::memory_order_relaxed)};
      if(n == 0)
        {
        probe_started_ms = now;
        probe_lines = 0;
        probe_surface_draws = 0;
        log_line("probe: a jump charges, no panel draw for over 1 s; looking at the draws for {} ms", probe_window_ms);
        }
      ID3D11VertexShader * vs{};
      ctx->VSGetShader(&vs, nullptr, nullptr);
      ID3D11ShaderResourceView * srvs[16]{};
      ctx->PSGetShaderResources(0, 16, srvs);
      for(std::uint32_t slot{}; slot != 16; ++slot)
        {
        if(not srvs[slot])
          continue;
        ID3D11Resource * res{};
        srvs[slot]->GetResource(&res);
        ID3D11Texture2D * tex{};
        if(res and SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))))
          {
          D3D11_TEXTURE2D_DESC d{};
          tex->GetDesc(&d);
          if(panel_sized(d.Width, d.Height))
            {
            ++probe_surface_draws;
            // not watched (the probe sees only unwatched draws), whatever its verdict: from now on it is
            std::uint32_t const seen{vs ? table_get(vs) : max_vs_seen};
            bool const judged_panel{seen < max_vs_seen and verdict[seen].load(std::memory_order_relaxed) == verdict_e::panel};
            if(vs and (slot == 1 or slot == 2) and ctx == immediate.load(std::memory_order_relaxed) and adopted_in_probe != vs
               and not judged_panel)
              {
              adopted_in_probe = vs;
              adopt(vs, seen, "probe", slot, d.Width, d.Height);
              }
            if(probe_lines < probe_max_lines)
              {
              ++probe_lines;
              log_line("probe: draw {} ctx {} vs {:016X} ({}) PS t{} {}x{} surf {:08x} count {} inst {}", n,
                       ctx == immediate.load(std::memory_order_relaxed) ? "immediate" : "other",
                       static_cast<unsigned long long>(vs_hash_of(vs)), static_cast<void const *>(static_cast<void *>(vs)), slot, d.Width, d.Height,
                       static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(res) & 0xffffffffull), count, instances);
              }
            }
          tex->Release();
          }
        if(res)
          res->Release();
        srvs[slot]->Release();
        }
      if(vs)
        vs->Release();
      if(now - probe_started_ms > probe_window_ms)
        {
        log_line("probe: done, {} draw(s) seen, {} with a panel-sized surface at PS", n + 1, probe_surface_draws);
        probe_request.store(false, std::memory_order_relaxed);
        }
      }

    auto guarded_probe(ID3D11DeviceContext * ctx, std::uint32_t count, std::uint32_t instances) noexcept -> void
      {
      if(hooks_off())
        return;
      ++own_depth;
      hook_work(
        [&]
        {
          probe_draw(ctx, count, instances);
        },
        [&]() noexcept
        {
          probe_request.store(false);
        });
      --own_depth;
      }

    // ---- the probe (its own thread): arms it once per charge that sees no panel draw ----
    auto probe_body(void *) -> DWORD
      {
      bool armed_this_charge{};
      std::uint64_t armed_at{};
      for(;;)
        {
        Sleep(100);
        std::uint64_t const now{GetTickCount64()};
        if(not game_state().charging)
          {
          armed_this_charge = false;
          continue;
          }
        if(not armed_this_charge and now - last_watched_ms.load(std::memory_order_relaxed) > 1000)
          {
          armed_this_charge = true;
          armed_at = now;
          probe_draws.store(0);
          probe_request.store(true);
          }
        if(armed_at and now - armed_at > 2000 and probe_request.load() and probe_draws.load() == 0)
          {
          probe_request.store(false);
          log_line("probe: a jump charges, no panel draw, and no draw at all reached the hooks in 2 s");
          census_report("at the jump", false);
          armed_at = 0;
          }
        }
      }

    // ---- the target's hologram (PoC): the model drawn to a 512x512 target, its record's orientation read back ----
    // The game draws its holograms with one shader (station_vs), each into a 512x512 surface of its own laid over the
    // cockpit: the ship's own (a Kestrel: 27885 indices, record scale 0.0055) and the target's (a commercial outpost: 3576
    // indices, scale 0.0004; 2026-10-21). They come one right after the other; for each draw of such a burst (at most
    // holo_parts) edworld GPU-copies the VB0 entry and cb1 rows 270..273 (that hologram's camera), and the whole t33 pool,
    // and reads them back without waiting a few frames later. Each hologram has a surface of its own, which the HUD lays
    // over the screen in its pass: the ship's own first, the target's second (4 of 4 whole frames of both CMDRs,
    // 2026-10-21/22; the smallest scale took a Panther's 0.0024 over a construction site's). The target is a draw to the
    // surface laid over second, of those the smallest scale (a station's parts), the largest if several
    // (holo_target_part); with one laying over there is no target. hologram_math.h turns its record and camera into the
    // model's axes in the camera's frame. A burst at most every holo_every_ms.
    enum struct holo_kind_e : std::uint8_t
      {
      none,
      candidate,  ///< a model shader (input INSTANCEANDMODELDATAINDEX): looked at with station_vs = 0 until one is found
      hologram
      };

    std::atomic<holo_kind_e> holo_kind[max_vs_seen]{};
    std::uint32_t current_holo{max_vs_seen};  // the bound shader's seen slot when it is a candidate or the hologram's
    std::atomic<bool> holo_known{};            // a shader is taken for the hologram's model

    constexpr std::uint32_t holo_slots{4};
    constexpr std::uint32_t holo_min_indices{1000};  // a candidate's draw, for the search
    constexpr std::uint32_t holo_min_part{200};      // a hologram's draw worth reading (not its smallest bits)
    constexpr std::uint32_t holo_parts{6};
    constexpr LONGLONG holo_every_ms{50};
    constexpr LONGLONG holo_burst_ms{3};
    constexpr std::uint32_t holo_part_bytes{16u + 64u};  // the VB0 entry, then cb1 rows 270..273
    constexpr std::uint32_t holo_side_bytes{holo_parts * holo_part_bytes};
    constexpr std::uint32_t holo_cb1_row{270u};

    struct holo_slot_t
      {
      ID3D11Buffer * side{};
      ID3D11Buffer * pool{};
      std::uint32_t pool_capacity{};
      std::uint32_t pool_bytes{};
      std::uint32_t pool_first{};
      std::uint32_t pool_stride{};
      LONGLONG qpc{};
      std::uint32_t parts{};
      std::uint32_t count[holo_parts]{};
      std::uint32_t cb_first[holo_parts]{};
      bool target[holo_parts]{};  ///< drawn to the target's surface (holo_target_rt at the draw)
      bool own[holo_parts]{};     ///< drawn to the own hologram's surface (holo_own_rt at the draw)
      bool in_flight{};
      };

    struct holo_state_t
      {
      ID3D11Device * device{};
      holo_slot_t slot[holo_slots]{};
      std::uint32_t next{};
      LONGLONG last_copy{};
      int open{-1};               // the slot the current burst's draws go to
      std::uint32_t count{};      // the target's draw's indices (the log: which model)
      float scale{};
      LONGLONG search_window{};   // the search's budget: holo_search_checks a tenth of a second
      std::uint32_t search_checks{};
      hologram_view_t view{};
      quat_t q{};
      float rows[4][4]{};         // cb1 rows 270..273 the view came from, and their buffer's first constant (the log)
      std::uint32_t cb_first{};
      LONGLONG view_at{};
      LONGLONG logged_at{};
      bool shown{};               // the view was fresh at the last look (for the log's transitions)
      };

    holo_state_t hs;
    // the holograms' surfaces (compared by address only, never used) and the HUD's laying over of them this frame
    constexpr std::uint32_t holo_max_rts{4};
    constexpr UINT holo_laid_slot{4};  // the laying over's pixel shader reads the hologram's surface at t4 (2026-10-22)
    void * holo_rts[holo_max_rts]{};
    std::uint32_t holo_rt_count{};
    void * holo_laid[2]{};             // the surfaces laid over first and second in the current frame
    std::uint32_t holo_laid_count{};
    void * holo_target_rt{};           // the surface laid over second in the last frame: the target's; null none
    void * holo_own_rt{};              // the surface laid over first in the last frame: the own hologram's; null none
    bool holo_target_logged{};
    LONGLONG attach_qpc{};
    constexpr std::uint32_t holo_search_checks{64};

    auto holo_ms(LONGLONG from, LONGLONG to) noexcept -> LONGLONG { return (to - from) * 1000 / qpc_frequency; }

    // ---- the hologram models' registry (holo_models.h, PoC 2026-10-22) ----
    // Each index count of a session is resolved by the count at once, and sampled once: the draw's indices and the
    // vertices from its base vertex (VB1, stride 40, at most holo_sample_vertex_bytes) are copied and read back a few
    // frames later, one count at a time, at most one every holo_sample_every_ms - a single hitch per model, no cost per
    // frame (Artur 2026-10-22: single hitches are fine, a lasting drop of the frame rate is not). The fingerprint then
    // resolves the model again: the same model on another build, or another count that is a model's (remapped, its axes
    // turned onto the model's for the T1 table). A station model drawn to the own hologram's surface breaks the order the
    // target's choice stands on: logged once, and with a confirmed model the target's hologram is off for the session.
    // edworld_eht also writes each sample (indices, vertices, and the target's surface as a picture) to edworld_models\
    // for Artur to confirm the model on the picture.
    constexpr std::uint32_t holo_seen_max{64};
    constexpr std::uint32_t holo_sample_vertex_bytes{8u * 1024u * 1024u};
    constexpr LONGLONG holo_sample_every_ms{1000};
    constexpr LONGLONG holo_surface_wait_ms{3000};

    struct holo_seen_t
      {
      std::uint32_t count;
      holo_resolved_t r;
      bool sampled;
      };

    holo_seen_t holo_seen[holo_seen_max]{};
    std::uint32_t holo_seen_count{};
    bool holo_swapped_logged{};  // the target taken from the own surface (the HUD's order swapped): logged once a change

    ///\brief which surface a hologram's draw goes to, as the HUD laid them over in the frame before: the own (first), the
    /// target's (second), or one not laid over (yet, or at all: the surfaces change hands as a target is chosen)
    enum struct holo_on_e : std::uint8_t
      {
      own,
      target,
      other
      };

    auto holo_on_name(holo_on_e on) noexcept -> char const *
      {
      return on == holo_on_e::own ? "own" : on == holo_on_e::target ? "target's" : "other (not laid over)";
      }

    struct holo_sample_t
      {
      ID3D11Buffer * ib{};
      ID3D11Buffer * vb{};
      std::uint32_t count{};
      std::uint32_t index_size{};
      std::uint32_t ib_bytes{};
      std::uint32_t vb_bytes{};
      holo_on_e on{};
      bool in_flight{};
      LONGLONG qpc{};
      LONGLONG done_at{};
      ID3D11Texture2D * surface{};  // edworld_eht: the surface drawn to, copied as the HUD lays it over
      void * want_surface{};        // edworld_eht: that surface, until it is laid over (or holo_surface_wait_ms passed)
      LONGLONG want_since{};
      LONGLONG surface_qpc{};
      void * drawn_to{};            // the surface the sampled draw went to
      char stamp[32]{};
      };

    holo_sample_t holo_sample{};


    ///\brief the game's build as edworld tells it apart: the exe's size and last write (an update rewrites the exe)
    ///\brief the version VersionInfo.txt beside the exe names, to the log: a label for people only (a copied game may keep an
    /// older one, so the build stays the exe's size and last write)
    auto holo_log_version(wchar_t const * exe) noexcept -> void
      {
      std::wstring name{exe};
      std::size_t const slash{name.find_last_of(L"\\/")};
      if(slash == std::wstring::npos)
        return;
      name.resize(slash + 1);
      name += L"VersionInfo.txt";
      HANDLE const file{CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr)};
      if(file == INVALID_HANDLE_VALUE)
        {
        log_line("hologram: no VersionInfo.txt beside the exe");
        return;
        }
      char text[512]{};
      DWORD n{};
      if(not ReadFile(file, text, sizeof text - 1, &n, nullptr))
        n = 0;
      CloseHandle(file);
      text[n] = '\0';
      char const * const at{std::strstr(text, "\"Version\":\"")};
      char version[64]{};
      if(at)
        std::sscanf(at + 11, "%63[^\"]", version);
      log_line("hologram: VersionInfo.txt beside the exe names version {}", version[0] ? version : "(none read)");
      }

    constinit edplugin_common::once_flag_t holo_build_said;

    ///\brief the game's build (edplugin_common's, told once in the process: the exe's size and last write), "unknown"
    /// when the exe cannot be read; said in the log once, with the version beside the exe
    auto holo_build() noexcept -> char const *
      {
      std::string_view const key{edplugin_common::game_build_key()};  // its text ends with a NUL
      char const * const text{key.empty() ? "unknown" : key.data()};
      if(holo_build_said.first())
        {
        log_line("hologram: the game's build {} (the exe's size and last write)", text);
        holo_log_version(edplugin_common::module_path(nullptr).c_str());  // a long path too
        }
      return text;
      }

    auto holo_how(holo_resolve_e how) noexcept -> char const *
      {
      switch(how)
        {
        case holo_resolve_e::known: return "known";
        case holo_resolve_e::stable: return "the same as on another build";
        case holo_resolve_e::remapped: return "REMAPPED";
        case holo_resolve_e::ambiguous: return "AMBIGUOUS (fits several turns)";
        case holo_resolve_e::unknown: break;
        }
      return "unknown";
      }

    auto holo_seen_find(std::uint32_t count) noexcept -> holo_seen_t *
      {
      for(std::uint32_t i{}; i != holo_seen_count; ++i)
        if(holo_seen[i].count == count)
          return &holo_seen[i];
      return nullptr;
      }

    ///\brief the count's entry for the session, made and resolved by the count alone at its first draw; null when full
    auto holo_seen_of(std::uint32_t count, holo_on_e on) noexcept -> holo_seen_t *
      {
      if(holo_seen_t * const s{holo_seen_find(count)})
        return s;
      if(holo_seen_count == holo_seen_max)
        return nullptr;
      holo_seen_t & s{holo_seen[holo_seen_count++]};
      s = holo_seen_t{count, holo_resolve(holo_models.data(), holo_models.size(), count, holo_build(), nullptr), false};
      if(s.r.model >= 0)
        log_line("hologram: {} indices on the {} surface: {} ({}, {})", count, holo_on_name(on),
                 holo_models[s.r.model].name, holo_how(s.r.how),
                 holo_models[s.r.model].status == holo_status_e::confirmed ? "confirmed" : "measured");
      else
        {
        holo_model_t const * other{};
        for(holo_model_t const & m: holo_models)
          if(m.count == count)
            other = &m;
        if(other)
          log_line("hologram: {} indices on the {} surface: {}'s count on build {}, not this one - unknown until its "
                   "fingerprint says", count, holo_on_name(on), other->name, other->build);
        else
          log_line("hologram: {} indices on the {} surface: not in the registry", count, holo_on_name(on));
        }
      return &s;
      }

    ///\brief a staging copy of bytes [from, from + bytes) of a buffer; null when it cannot be made
    auto holo_staging_copy(ID3D11DeviceContext * ctx, ID3D11Buffer * source, std::uint64_t from, std::uint32_t bytes) noexcept
      -> ID3D11Buffer *
      {
      D3D11_BUFFER_DESC sd{};
      sd.ByteWidth = (bytes + 15u) & ~15u;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Buffer * staging{};
      if(FAILED(device->CreateBuffer(&sd, nullptr, &staging)) or not staging)
        return nullptr;
      D3D11_BOX const box{static_cast<UINT>(from), 0, 0, static_cast<UINT>(from + bytes), 1, 1};
      ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);
      return staging;
      }

    auto holo_sample_release() noexcept -> void
      {
      if(holo_sample.ib)
        holo_sample.ib->Release();
      if(holo_sample.vb)
        holo_sample.vb->Release();
      holo_sample.ib = nullptr;
      holo_sample.vb = nullptr;
      holo_sample.in_flight = false;
      }

    ///\brief the first draw of a count not sampled yet this session: its indices and vertices copied for holo_sample_read
    auto holo_sample_take(ID3D11DeviceContext * ctx, std::uint32_t count, std::uint32_t start_index, std::int32_t base_vertex,
                          void * surface, holo_on_e on, LONGLONG now) noexcept -> void
      {
      holo_seen_t * const seen{holo_seen_of(count, on)};
      if(not seen or seen->sampled or holo_sample.in_flight or holo_sample.want_surface or holo_sample.surface or
         holo_ms(holo_sample.done_at, now) < holo_sample_every_ms)
        return;
      seen->sampled = true;  // one try per count and session, even when the copy cannot be made
      ID3D11Buffer * ib{};
      DXGI_FORMAT ib_format{};
      UINT ib_offset{};
      ctx->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
      ID3D11Buffer * vbs[2]{};
      UINT strides[2]{}, offsets[2]{};
      ctx->IAGetVertexBuffers(0, 2, vbs, strides, offsets);
      if(vbs[0])
        vbs[0]->Release();
      char const * why{};
      if(not ib or not vbs[1])
        why = "no index or vertex buffer";
      else if(strides[1] != vertex_stride or base_vertex < 0)
        why = "not the vertex layout known (VB1 stride 40)";
      if(not why)
        {
        std::uint32_t const size{ib_format == DXGI_FORMAT_R16_UINT ? 2u : 4u};
        D3D11_BUFFER_DESC ibd{}, vbd{};
        ib->GetDesc(&ibd);
        vbs[1]->GetDesc(&vbd);
        std::uint64_t const ib_from{ib_offset + static_cast<std::uint64_t>(start_index) * size};
        std::uint64_t const vb_from{offsets[1] + static_cast<std::uint64_t>(base_vertex) * vertex_stride};
        std::uint64_t const ib_bytes{static_cast<std::uint64_t>(count) * size};
        if(ib_from + ib_bytes > ibd.ByteWidth or vb_from >= vbd.ByteWidth)
          why = "its ranges past their buffers";
        else
          {
          auto const vb_bytes{static_cast<std::uint32_t>(std::min<std::uint64_t>(holo_sample_vertex_bytes, vbd.ByteWidth - vb_from))};
          holo_sample.ib = holo_staging_copy(ctx, ib, ib_from, static_cast<std::uint32_t>(ib_bytes));
          holo_sample.vb = holo_staging_copy(ctx, vbs[1], vb_from, vb_bytes);
          if(holo_sample.ib and holo_sample.vb)
            {
            holo_sample.count = count;
            holo_sample.index_size = size;
            holo_sample.ib_bytes = static_cast<std::uint32_t>(ib_bytes);
            holo_sample.vb_bytes = vb_bytes;
            holo_sample.on = on;
            holo_sample.drawn_to = surface;
            holo_sample.qpc = now;
            holo_sample.in_flight = true;
            SYSTEMTIME t;
            GetSystemTime(&t);
            format_into(holo_sample.stamp, "{:04}{:02}{:02}T{:02}{:02}{:02}_{:03}Z", t.wYear, t.wMonth, t.wDay,
                          t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
            }
          else
            {
            holo_sample_release();
            why = "no staging copy";
            }
          }
        }
      if(ib)
        ib->Release();
      if(vbs[1])
        vbs[1]->Release();
      if(why)
        log_line("hologram: {} indices not sampled ({})", count, why);
      }

    ///\brief the fingerprint of a sample read back; false (logged) when its indices lie past the vertices read
    auto holo_sample_fingerprint(std::uint8_t const * indices, std::uint8_t const * vertices, holo_fingerprint_t & f) noexcept -> bool
      {
      try
        {
        std::vector<vec3_t> used;
        if(not holo_used_vertices(indices, holo_sample.index_size, holo_sample.count, vertices, holo_sample.vb_bytes, used))
          {
          log_line("hologram: {} indices not fingerprinted (an index past the {} vertices read)", holo_sample.count,
                   holo_sample.vb_bytes / vertex_stride);
          return false;
          }
        f = holo_fingerprint(used.data(), used.size(), holo_sample.count / 3u);
        return true;
        }
      catch(std::exception const & e)
        {
        log_line("hologram: {} indices not fingerprinted ({})", holo_sample.count, e.what());
        return false;
        }
      }

    auto holo_log_fingerprint(std::uint32_t count, holo_fingerprint_t const & f) noexcept -> void
      {
      // as an entry's fingerprint in holo_models.h
      char extreme[6 * 3 * 12]{};
      int at{};
      for(vec3_t const & v: f.extreme)
        at += format_into(extreme + at, sizeof extreme - static_cast<std::size_t>(at), "{}{{{:.4f}f, {:.4f}f, {:.4f}f}}", at ? ", " : "",
                            static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z));
      log_line("hologram model fingerprint {}: {{{}u, {}u, {{{:.4f}f, {:.4f}f, {:.4f}f}}, {{{{{}}}}}, {{{{0x{:016x}ull, 0x{:016x}ull, 0x{:016x}ull, "
               "0x{:016x}ull, 0x{:016x}ull, 0x{:016x}ull, 0x{:016x}ull, 0x{:016x}ull}}}}}}", count, f.vertices, f.triangles,
               static_cast<double>(f.extent[0]), static_cast<double>(f.extent[1]), static_cast<double>(f.extent[2]), extreme,
               as_unsigned(f.grid[0]), as_unsigned(f.grid[1]), as_unsigned(f.grid[2]), as_unsigned(f.grid[3]), as_unsigned(f.grid[4]), as_unsigned(f.grid[5]), as_unsigned(f.grid[6]), as_unsigned(f.grid[7]));
      }

    auto holo_models_dir() -> std::wstring { return stralgo::stl::merge(start_fields().dir, L"\\edworld_models"sv); }

    ///\brief the sample's copies once the GPU has made them: fingerprint, resolve again, log; edworld_eht writes them too
    auto holo_sample_read(ID3D11DeviceContext * ctx, LONGLONG now) -> void
      {
      if(not holo_sample.in_flight)
        return;
      D3D11_MAPPED_SUBRESOURCE mi{}, mv{};
      HRESULT hr{ctx->Map(holo_sample.ib, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mi)};
      if(SUCCEEDED(hr))
        {
        hr = ctx->Map(holo_sample.vb, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mv);
        if(FAILED(hr))
          ctx->Unmap(holo_sample.ib, 0);
        }
      if(hr == DXGI_ERROR_WAS_STILL_DRAWING and holo_ms(holo_sample.qpc, now) < 2000)
        return;
      holo_sample.done_at = now;
      if(FAILED(hr))
        {
        log_line("hologram: the sample of {} indices lost (hr 0x{:08X})", holo_sample.count, static_cast<unsigned long>(hr));
        holo_sample_release();
        return;
        }
      auto const * const indices{static_cast<std::uint8_t const *>(mi.pData)};
      auto const * const vertices{static_cast<std::uint8_t const *>(mv.pData)};
      holo_fingerprint_t f{};
      if(holo_sample_fingerprint(indices, vertices, f))
        {
        holo_resolved_t const r{holo_resolve(holo_models.data(), holo_models.size(), holo_sample.count, holo_build(), &f)};
        if(holo_seen_t * const seen{holo_seen_find(holo_sample.count)})
          seen->r = r;
        log_line("hologram model sample: {} indices ({} surface), {} vertices, {} triangles, build {}: {} {} (likeness {:.3f}, "
                 "next model {:.3f}, turn {})", holo_sample.count, holo_on_name(holo_sample.on), f.vertices, f.triangles,
                 holo_build(), r.model >= 0 ? holo_models[r.model].name : "-", holo_how(r.how), static_cast<double>(r.score),
                 static_cast<double>(r.second), r.turn);
        holo_log_fingerprint(holo_sample.count, f);
        if constexpr(with_eht)
          if(r.how == holo_resolve_e::unknown or r.how == holo_resolve_e::remapped or r.how == holo_resolve_e::ambiguous)
            sphere_note_new_model(holo_sample.count);  // a picture for Artur to confirm
        }
      if constexpr(with_eht)
        {
        CreateDirectoryW(holo_models_dir().c_str(), nullptr);
        wchar_t name[MAX_PATH];
        format_into(name, MAX_PATH, L"{}\\{}_{}_{}_ib_u{}.bin", holo_models_dir(), utf8(holo_build()), utf8(holo_sample.stamp),
                      holo_sample.count, holo_sample.index_size * 8u);
        queue_write(name, indices, holo_sample.ib_bytes);
        format_into(name, MAX_PATH, L"{}\\{}_{}_{}_vb_s40.bin", holo_models_dir(), utf8(holo_build()), utf8(holo_sample.stamp),
                      holo_sample.count);
        // only the vertices the indices reach (the copy is up to holo_sample_vertex_bytes)
        std::uint32_t top{};
        for(std::uint32_t i{}; i != holo_sample.count; ++i)
          {
          std::uint32_t at{};
          if(holo_sample.index_size == 2u)
            {
            std::uint16_t v{};
            std::memcpy(&v, indices + i * 2u, 2);
            at = v;
            }
          else
            std::memcpy(&at, indices + i * 4u, 4);
          if(at != 0xFFFFFFFFu)  // a strip cut, no vertex
            top = std::max(top, at);
          }
        // in 64 bits: a large index times the stride must not wrap to a short file
        std::uint64_t const wanted{(std::uint64_t{top} + 1u) * vertex_stride};
        queue_write(name, vertices, static_cast<std::uint32_t>(std::min<std::uint64_t>(holo_sample.vb_bytes, wanted)));
        // the game's files at the sample (Status.json, the journal's place), read by the writer thread
        char sample[96];
        std::size_t const length{format_into(sample, "{}_{}_{}", holo_build(), holo_sample.stamp, holo_sample.count)};
        format_into(name, MAX_PATH, L"{}\\{}_context.json", holo_models_dir(), utf8(sample));
        if(length > 0)
          queue_write(name, sample, static_cast<std::uint32_t>(length), false, nullptr, true);
        holo_sample.want_surface = holo_sample.drawn_to;
        holo_sample.want_since = now;
        }
      ctx->Unmap(holo_sample.vb, 0);
      ctx->Unmap(holo_sample.ib, 0);
      holo_sample_release();
      }

    ///\brief edworld_eht: the target's surface as the HUD lays it over, copied once for the sample's picture
    auto holo_surface_take(ID3D11DeviceContext * ctx, ID3D11Resource * res) noexcept -> void
      {
      holo_sample.want_surface = nullptr;
      ID3D11Texture2D * tex{};
      res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
      if(not tex)
        return;
      D3D11_TEXTURE2D_DESC d{};
      tex->GetDesc(&d);
      D3D11_TEXTURE2D_DESC sd{d};
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      if(d.SampleDesc.Count == 1 and d.ArraySize == 1 and d.MipLevels == 1 and
         SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &holo_sample.surface)))
        {
        ctx->CopyResource(holo_sample.surface, tex);
        holo_sample.surface_qpc = qpc_now();
        }
      else
        {
        holo_sample.surface = nullptr;
        log_line("hologram: the target's surface of {} indices not copied (format {}, samples {})", holo_sample.count,
                 static_cast<unsigned>(d.Format), d.SampleDesc.Count);
        }
      tex->Release();
      }

    ///\brief edworld_eht: the surface's depth as a grey picture (PGM; near light, far darker, nothing drawn black), as
    /// the pictures Artur looked at were made
    auto holo_surface_write(ID3D11DeviceContext * ctx, LONGLONG now) noexcept -> void
      {
      if(holo_sample.want_surface and holo_ms(holo_sample.want_since, now) > holo_surface_wait_ms)
        {
        log_line("hologram: no picture of {} indices (its surface was not laid over within {} ms)", holo_sample.count,
                 static_cast<long long>(holo_surface_wait_ms));
        holo_sample.want_surface = nullptr;
        }
      if(not holo_sample.surface)
        return;
      D3D11_MAPPED_SUBRESOURCE m{};
      HRESULT const hr{ctx->Map(holo_sample.surface, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)};
      if(hr == DXGI_ERROR_WAS_STILL_DRAWING and holo_ms(holo_sample.surface_qpc, now) < 2000)
        return;
      if(SUCCEEDED(hr) and m.pData)
        {
        D3D11_TEXTURE2D_DESC d{};
        holo_sample.surface->GetDesc(&d);
        constexpr std::uint32_t side{512u};
        constexpr char header[]{"P5\n512 512\n255\n"};
        constexpr std::uint32_t header_bytes{sizeof header - 1u};
        static std::uint8_t picture[header_bytes + side * side];
        std::memcpy(picture, header, header_bytes);
        auto const * const rows{static_cast<std::uint8_t const *>(m.pData)};
        auto const depth = [&](std::uint32_t x, std::uint32_t y) noexcept -> float
          {
          float v{};
          std::memcpy(&v, rows + static_cast<std::size_t>(y) * m.RowPitch + x * 4u, 4);
          return v;
          };
        bool const four_bytes{d.Format == DXGI_FORMAT_R32_TYPELESS or d.Format == DXGI_FORMAT_R32_FLOAT or
                              d.Format == DXGI_FORMAT_D32_FLOAT};
        if(four_bytes and d.Width == side and d.Height == side)
          {
          float lo{1e30f}, hi{-1e30f};
          for(std::uint32_t y{}; y != side; ++y)
            for(std::uint32_t x{}; x != side; ++x)
              if(float const v{depth(x, y)}; v < 1e9f)
                {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                }
          float const span{hi > lo ? hi - lo : 1.f};
          for(std::uint32_t y{}; y != side; ++y)
            for(std::uint32_t x{}; x != side; ++x)
              {
              float const v{depth(x, y)};
              picture[header_bytes + y * side + x] = v < 1e9f ? static_cast<std::uint8_t>(255.f - (v - lo) / span * 200.f) : 0u;
              }
          wchar_t name[MAX_PATH];
          format_into(name, MAX_PATH, L"{}\\{}_{}_{}_holo.pgm", holo_models_dir(), utf8(holo_build()), utf8(holo_sample.stamp),
                        holo_sample.count);
          queue_write(name, picture, sizeof picture, true);
          }
        else
          log_line("hologram: the target's surface of {} indices not pictured (format {}, {}x{})", holo_sample.count,
                   static_cast<unsigned>(d.Format), d.Width, d.Height);
        ctx->Unmap(holo_sample.surface, 0);
        }
      else
        log_line("hologram: the target's surface of {} indices lost (hr 0x{:08X})", holo_sample.count, static_cast<unsigned long>(hr));
      holo_sample.surface->Release();
      holo_sample.surface = nullptr;
      }

    ///\brief a model shader's bytecode names its first input INSTANCEANDMODELDATAINDEX (the hologram's does, 2026-10-20)
    auto holo_candidate(void const * bytecode, std::size_t length) noexcept -> bool
      {
      constexpr char name[]{"INSTANCEANDMODELDATAINDEX"};
      constexpr std::size_t n{sizeof name - 1};
      auto const * const b{static_cast<char const *>(bytecode)};
      for(std::size_t i{}; i + n <= length; ++i)
        if(b[i] == name[0] and std::memcmp(b + i, name, n) == 0)
          return true;
      return false;
      }

    ///\brief the bound render target when it is 512x512 (a hologram's surface), its address only; null otherwise
    auto holo_surface(ID3D11DeviceContext * ctx) noexcept -> void *
      {
      ID3D11RenderTargetView * rtv{};
      ctx->OMGetRenderTargets(1, &rtv, nullptr);
      if(not rtv)
        return nullptr;
      void * surface{};
      ID3D11Resource * res{};
      rtv->GetResource(&res);
      if(res)
        {
        D3D11_RESOURCE_DIMENSION dim{};
        res->GetType(&dim);
        if(dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          {
          D3D11_TEXTURE2D_DESC d{};
          static_cast<ID3D11Texture2D *>(res)->GetDesc(&d);
          if(d.Width == 512u and d.Height == 512u)
            surface = res;
          }
        res->Release();
        }
      rtv->Release();
      return surface;
      }

    ///\brief a surface a hologram is drawn to, kept to know the HUD's laying over of it
    auto holo_note_surface(void * surface) noexcept -> void
      {
      for(std::uint32_t i{}; i != holo_rt_count; ++i)
        if(holo_rts[i] == surface)
          return;
      if(holo_rt_count == holo_max_rts)
        holo_rt_count = 0;  // the game made new ones (a new device, a resize): the oldest go
      holo_rts[holo_rt_count++] = surface;
      log_line("hologram: a hologram's surface {} ({} known)", static_cast<void const *>(surface), holo_rt_count);
      }

    ///\brief a draw of the HUD's pass (6 indices) whose pixel shader reads a hologram's surface at t4 lays it over the
    /// screen: the first such of a frame is the ship's own, the second the target's
    auto holo_laying_over(ID3D11DeviceContext * ctx) noexcept -> void
      {
      if(holo_laid_count == 2u and not holo_sample.want_surface)
        return;
      ID3D11ShaderResourceView * srv{};
      ctx->PSGetShaderResources(holo_laid_slot, 1, &srv);
      if(not srv)
        return;
      ID3D11Resource * res{};
      srv->GetResource(&res);
      srv->Release();
      if(not res)
        return;
      if(holo_sample.want_surface == res)
        holo_surface_take(ctx, res);  // the sample's surface, complete as the HUD reads it
      if(holo_laid_count != 2u)
        for(std::uint32_t i{}; i != holo_rt_count; ++i)
          if(holo_rts[i] == res)
            {
            holo_laid[holo_laid_count++] = res;
            break;
            }
      res->Release();
      }

    auto guarded_laying_over(ID3D11DeviceContext * ctx) noexcept -> void
      {
      ++own_depth;
      hook_work(
        [&]
        {
          holo_laying_over(ctx);
        },
        no_handler);
      --own_depth;
      }

    ///\brief at a frame's start: the surface laid over second in the frame before is the target's (none with one)
    auto holo_end_laying_over() noexcept -> void
      {
      void * const target{holo_laid_count == 2u ? holo_laid[1] : nullptr};
      void * const own{holo_laid_count != 0u ? holo_laid[0] : nullptr};
      holo_laid_count = 0;
      if((target != nullptr) != holo_target_logged)
        {
        holo_target_logged = target != nullptr;
        if(target)
          log_line("hologram: the target's surface {} (the HUD lays it over second)", static_cast<void const *>(target));
        else
          log_line("hologram: no target's surface (the HUD lays over {})", holo_rt_count ? "one hologram" : "none");
        }
      holo_target_rt = target;
      holo_own_rt = own;
      }

    auto holo_release() noexcept -> void
      {
      for(holo_slot_t & s: hs.slot)
        {
        if(s.side)
          s.side->Release();
        if(s.pool)
          s.pool->Release();
        s = holo_slot_t{};
        }
      holo_sample_release();
      if(holo_sample.surface)
        holo_sample.surface->Release();
      holo_sample.surface = nullptr;
      holo_sample.want_surface = nullptr;
      }

    ///\brief the copies made a few frames ago, read without waiting; the newest one read sets the view
    auto holo_drain(ID3D11DeviceContext * ctx, LONGLONG now) noexcept -> void
      {
      for(std::uint32_t k{}; k != holo_slots; ++k)
        {
        std::uint32_t const at_slot{(hs.next + k) % holo_slots};  // oldest first
        holo_slot_t & s{hs.slot[at_slot]};
        if(not s.in_flight or static_cast<int>(at_slot) == hs.open)
          continue;
        if(holo_ms(s.qpc, now) > 500)
          {
          s.in_flight = false;  // never came back: dropped
          continue;
          }
        D3D11_MAPPED_SUBRESOURCE side{}, pool{};
        if(FAILED(ctx->Map(s.side, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &side)))
          continue;
        if(FAILED(ctx->Map(s.pool, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &pool)))
          {
          ctx->Unmap(s.side, 0);
          continue;
          }
        // the target's draw: a draw to the target's surface, the smallest scale, the most indices among equal ones
        auto const * const sb{static_cast<std::uint8_t const *>(side.pData)};
        auto const * const pb{static_cast<std::uint8_t const *>(pool.pData)};
        holo_part_t parts[holo_parts]{};
        for(std::uint32_t p{}; p != s.parts; ++p)
          {
          std::uint32_t index{};
          std::memcpy(&index, sb + p * holo_part_bytes, 4);
          std::uint64_t const at{(static_cast<std::uint64_t>(s.pool_first) + index) * s.pool_stride};
          if(at + 16u > s.pool_bytes)
            continue;
          float scale{};
          std::memcpy(&scale, pb + at + 4, 4);
          holo_seen_t const * const seen{holo_seen_find(s.count[p])};
          bool const place{seen and holo_place_of(seen->r, holo_models.data()) != holo_place_e::none};
          parts[p] = holo_part_t{scale, s.count[p], s.target[p], s.own[p], place};
          }
        int const best{holo_target_part(parts, s.parts)};
        if(bool const swapped{best >= 0 and not parts[best].target}; swapped != holo_swapped_logged)
          {
          holo_swapped_logged = swapped;
          if(swapped)
            log_line("hologram: the target's model by its kind: {} indices on the own surface (the HUD's order swapped)",
                     parts[best].count);
          else
            log_line("hologram: the target's model on the target's surface again");
          }
        float const best_scale{best >= 0 ? parts[best].scale : 0.f};
        if(best >= 0)
          {
          auto const p{static_cast<std::uint32_t>(best)};
          std::uint32_t index{};
          std::memcpy(&index, sb + p * holo_part_bytes, 4);
          float rows[4][4]{};
          std::memcpy(rows, sb + p * holo_part_bytes + 16, sizeof rows);
          quat_t const q{hologram_orientation(pb + (static_cast<std::uint64_t>(s.pool_first) + index) * s.pool_stride)};
          if(auto const cam{camera_axes(rows)}; cam)
            {
            hs.q = q;
            std::memcpy(hs.rows, rows, sizeof rows);
            hs.cb_first = s.cb_first[p];
            hs.count = s.count[p];
            hs.scale = best_scale;
            // the model the count resolved to on this build: its registered count for the tables, and when remapped its
            // axes (the T1 table's) turned back from the drawn ones; none until a count of another build is confirmed
            std::array<compass_direction_t, 3> axes{hologram_axes(q, *cam)};
            holo_seen_t const * const seen{holo_seen_find(s.count[p])};
            holo_resolved_t const r{seen ? seen->r : holo_resolved_t{}};
            if(r.how == holo_resolve_e::remapped)
              axes = holo_model_axes(axes, holo_turn_list[static_cast<std::size_t>(r.turn)]);
            bool const ship{holo_shows_ship(s.count[p], s.count, s.own, s.parts, r.model >= 0 ? &holo_models[r.model] : nullptr)};
            hs.view = hologram_view_t{.axes = axes,
                                      .count = s.count[p],
                                      .table = holo_table_count(r, holo_models.data()),
                                      .flat = holo_flat_of(r, holo_models.data()),
                                      .have = true,
                                      .ship = ship,
                                      .place = holo_place_of(r, holo_models.data())};
            hs.view_at = s.qpc;
            }
          }
        ctx->Unmap(s.pool, 0);
        ctx->Unmap(s.side, 0);
        s.in_flight = false;
        }
      }

    ///\brief GPU copies of what a hologram's draw reads (part p of the burst): VB0 entry, cb1 rows 270..273, the t33 pool
    auto holo_copy(ID3D11DeviceContext * ctx, holo_slot_t & s, std::uint32_t p, std::uint32_t start_instance) noexcept -> bool
      {
      if(not s.side)
        {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = holo_side_bytes;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if(FAILED(device->CreateBuffer(&d, nullptr, &s.side)))
          {
          s.side = nullptr;
          return false;
          }
        }
      ID3D11Buffer * vb{};
      UINT stride{}, offset{};
      ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
      if(not vb)
        return false;
      bool ok{};
      D3D11_BUFFER_DESC bd{};
      vb->GetDesc(&bd);
      std::uint64_t const from{offset + static_cast<std::uint64_t>(start_instance) * stride};
      if(stride and from + 8u <= bd.ByteWidth)
        {
        D3D11_BOX const box{static_cast<UINT>(from), 0, 0, static_cast<UINT>(from + 8u), 1, 1};
        ctx->CopySubresourceRegion(s.side, 0, p * holo_part_bytes, 0, 0, vb, 0, &box);
        ok = true;
        }
      vb->Release();
      if(not ok)
        return false;

      ID3D11Buffer * cb1{};
      UINT first{}, count{};
      if(immediate1)
        immediate1->VSGetConstantBuffers1(1, 1, &cb1, &first, &count);
      else
        ctx->VSGetConstantBuffers(1, 1, &cb1);
      if(not cb1)
        return false;
      ok = false;
      cb1->GetDesc(&bd);
      s.cb_first[p] = first;
      std::uint32_t const cb_from{(first + holo_cb1_row) * 16u};
      if(cb_from + 64u <= bd.ByteWidth)
        {
        D3D11_BOX const box{cb_from, 0, 0, cb_from + 64u, 1, 1};
        ctx->CopySubresourceRegion(s.side, 0, p * holo_part_bytes + 16u, 0, 0, cb1, 0, &box);
        ok = true;
        }
      cb1->Release();
      if(not ok)
        return false;

      ID3D11ShaderResourceView * srv{};
      ctx->VSGetShaderResources(33, 1, &srv);
      if(not srv)
        return false;
      D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
      srv->GetDesc(&vd);
      std::uint32_t first_element{};
      if(vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER)
        first_element = vd.Buffer.FirstElement;
      else if(vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX)
        first_element = vd.BufferEx.FirstElement;
      ID3D11Resource * res{};
      srv->GetResource(&res);
      srv->Release();
      if(not res)
        return false;
      D3D11_RESOURCE_DIMENSION dim{};
      res->GetType(&dim);
      ID3D11Buffer * buf{};
      if(dim == D3D11_RESOURCE_DIMENSION_BUFFER)
        res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void **>(&buf));
      res->Release();
      if(not buf)
        return false;
      buf->GetDesc(&bd);
      ok = bd.ByteWidth != 0 and bd.ByteWidth <= pool_limit;
      if(ok and s.pool_capacity < bd.ByteWidth)
        {
        if(s.pool)
          s.pool->Release();
        s.pool = nullptr;
        s.pool_capacity = 0;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bd.ByteWidth;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if(SUCCEEDED(device->CreateBuffer(&d, nullptr, &s.pool)))
          s.pool_capacity = bd.ByteWidth;
        else
          s.pool = nullptr;
        ok = s.pool != nullptr;
        }
      // each part copies the pool again (the second audit, 2026-11-18: one copy a series, at its first part, would miss
      // records the game writes between the own ship's hologram and the target's); the last copy is the one read
      if(ok)
        {
        D3D11_BOX const box{0, 0, 0, bd.ByteWidth, 1, 1};
        ctx->CopySubresourceRegion(s.pool, 0, 0, 0, 0, buf, 0, &box);
        s.pool_bytes = bd.ByteWidth;
        s.pool_first = first_element;
        s.pool_stride = bd.StructureByteStride ? bd.StructureByteStride : 336u;
        }
      buf->Release();
      return ok;
      }

    auto holo_log(LONGLONG now) noexcept -> void
      {
      bool const fresh{hs.view.have and holo_ms(hs.view_at, now) < 500};
      if(fresh != hs.shown)
        {
        hs.shown = fresh;
        log_line("{}", fresh ? "hologram: read" : "hologram: gone (no read for half a second)");
        }
      if(fresh and holo_ms(hs.logged_at, now) >= 2000)
        {
        hs.logged_at = now;
        auto const & a{hs.view.axes};
        // the target's draw's indices tell the model (3576: a commercial outpost, 2026-10-21)
        log_line("hologram: {} indices, scale {:.5f}, q {:+.4f} {:+.4f} {:+.4f} {:+.4f} | X {:+.3f} {:+.3f} {:+.3f} Y {:+.3f} {:+.3f} {:+.3f} Z {:+.3f} {:+.3f} {:+.3f}",
                 hs.count, static_cast<double>(hs.scale), hs.q.x, hs.q.y, hs.q.z, hs.q.w, a[0].x, a[0].y, a[0].z, a[1].x, a[1].y, a[1].z, a[2].x, a[2].y, a[2].z);
        auto const & r{hs.rows};
        log_line("hologram: cb1 first {}, rows 270..273 {:+.4f} {:+.4f} {:+.4f} {:+.4f} | {:+.4f} {:+.4f} {:+.4f} {:+.4f} | {:+.4f} {:+.4f} {:+.4f} {:+.4f} | {:+.4f} {:+.4f} "
                 "{:+.4f} {:+.4f}", hs.cb_first, r[0][0], r[0][1], r[0][2], r[0][3], r[1][0], r[1][1], r[1][2], r[1][3], r[2][0], r[2][1], r[2][2],
                 r[2][3], r[3][0], r[3][1], r[3][2], r[3][3]);
        }
      }

    ///\brief a draw of a candidate or of the hologram's shader on the immediate context
    auto hologram_draw(ID3D11DeviceContext * ctx, std::uint32_t index_count, std::uint32_t start_index, std::int32_t base_vertex,
                       std::uint32_t start_instance) -> void
      {
      if(index_count < holo_min_part or current_holo >= max_vs_seen)
        return;
      LONGLONG const now{qpc_now()};
      holo_kind_e const kind{holo_kind[current_holo].load(std::memory_order_relaxed)};
      if(kind == holo_kind_e::candidate)
        {
        // the search: only while no shader is known, from half a minute after the start, a few looks a tenth of a second
        if(index_count < holo_min_indices or holo_known.load(std::memory_order_relaxed) or holo_ms(attach_qpc, now) < 30000)
          return;
        if(holo_ms(hs.search_window, now) >= 100)
          {
          hs.search_window = now;
          hs.search_checks = 0;
          }
        if(hs.search_checks++ >= holo_search_checks or not holo_surface(ctx))
          return;
        holo_kind[current_holo].store(holo_kind_e::hologram, std::memory_order_relaxed);
        holo_known.store(true);
        log_line("hologram: vs {:016X} draws {} indices to a 512x512 target - taken for the hologram's model (station_vs)",
                 static_cast<unsigned long long>(seen_hash[current_holo]), index_count);
        }
      void * const surface{holo_surface(ctx)};
      if(not surface)
        return;  // the same shader draws models in the scene too
      holo_note_surface(surface);
      if(hs.device != device)
        {
        holo_release();
        hs = holo_state_t{};
        hs.device = device;
        }
      // a burst: the holograms' draws come one right after the other and share a slot
      if(hs.open >= 0 and holo_ms(hs.slot[hs.open].qpc, now) > holo_burst_ms)
        hs.open = -1;
      holo_drain(ctx, now);
      holo_log(now);
      holo_sample_read(ctx, now);
      holo_surface_write(ctx, now);
      if(index_count >= holo_min_part)
        holo_sample_take(ctx, index_count, start_index, base_vertex, surface,
                         holo_own_rt and surface == holo_own_rt         ? holo_on_e::own
                         : holo_target_rt and surface == holo_target_rt ? holo_on_e::target
                                                                        : holo_on_e::other,
                         now);
      if(hs.open < 0)
        {
        if(holo_ms(hs.last_copy, now) < holo_every_ms or hs.slot[hs.next].in_flight)
          return;  // not yet, or the ring is full (the reads lag)
        holo_slot_t & s{hs.slot[hs.next]};
        s.parts = 0;
        s.qpc = now;
        hs.open = static_cast<int>(hs.next);
        hs.next = (hs.next + 1) % holo_slots;
        hs.last_copy = now;
        }
      holo_slot_t & s{hs.slot[hs.open]};
      if(s.parts == holo_parts)
        return;
      if(holo_copy(ctx, s, s.parts, start_instance))
        {
        s.target[s.parts] = holo_target_rt and surface == holo_target_rt;
        s.own[s.parts] = holo_own_rt and surface == holo_own_rt;
        s.count[s.parts++] = index_count;
        s.in_flight = true;
        }
      }

    auto guarded_hologram(ID3D11DeviceContext * ctx, std::uint32_t index_count, std::uint32_t start_index, std::int32_t base_vertex,
                          std::uint32_t start_instance) noexcept -> void
      {
      ++own_depth;
      hook_work(
        [&]
        {
          hologram_draw(ctx, index_count, start_index, base_vertex, start_instance);
        },
        no_handler);
      --own_depth;
      }

    // ---- hooks ----
    HRESULT STDMETHODCALLTYPE hook_create_vs(
      ID3D11Device * self,
      void const * bytecode,
      SIZE_T length,
      ID3D11ClassLinkage * linkage,
      ID3D11VertexShader ** shader
    )
      {
      HRESULT const hr{orig_create_vs(self, bytecode, length, linkage, shader)};
      if(FAILED(hr) or not shader or not *shader or not bytecode or chain_refusal()[0])
        return hr;
      vs_created.fetch_add(1, std::memory_order_relaxed);
      std::uint64_t const hash{fnv1a64(static_cast<std::uint8_t const *>(bytecode), length)};
      if(std::uint32_t const at{seen_reserved.fetch_add(1, std::memory_order_relaxed)}; at < max_vs_seen)
        {
        // shaders may be created on several threads: a slot is reserved first, its pointer published last
        seen_hash[at] = hash;
        verdict[at].store(verdict_e::pending, std::memory_order_relaxed);
        verdict_looked[at] = 0;
        // edworld's own (ImGui's, the list's) are judged at once: they draw from copies of a panel's surface
        verdict[at].store(creating_own ? verdict_e::not_panel : verdict_e::pending, std::memory_order_relaxed);
        holo_kind_e kind{holo_kind_e::none};
        if(not creating_own and start_fields().station_axes)
          {
          if(start_fields().station_vs and hash == start_fields().station_vs)
            kind = holo_kind_e::hologram;
          else if(not start_fields().station_vs and holo_candidate(bytecode, length))
            kind = holo_kind_e::candidate;
          }
        holo_kind[at].store(kind, std::memory_order_relaxed);
        if(kind == holo_kind_e::hologram and not holo_known.exchange(true))
          log_line("hologram: vs {:016X} created (station_vs)", static_cast<unsigned long long>(hash));
        seen_ptr[at].store(*shader, std::memory_order_release);
        table_put(*shader, at);
        }
      if(creating_own)
        return hr;
#if defined(EDWORLD_EHT)
      shader_catalog_note(shader_stage_e::vertex, bytecode, length, hash);
#endif
      if(start_fields().log_all_vs)
        log_line("vs {:016X} {} bytes", static_cast<unsigned long long>(hash), static_cast<std::size_t>(length));
      if(in_dump_vs(hash))
        {
        bool known{};
        for(std::uint32_t k{}, n{std::min(kept_vs_reserved.load(), max_kept_vs)}; k != n; ++k)
          known = known or kept_vs[k].hash.load(std::memory_order_acquire) == hash;
        if(not known)
          if(std::uint32_t const k{kept_vs_reserved.fetch_add(1)}; k < max_kept_vs)
            if(void * const copy{std::malloc(length)})
              {
              std::memcpy(copy, bytecode, length);
              kept_vs[k].bytes = copy;
              kept_vs[k].length = static_cast<std::uint32_t>(length);
              kept_vs[k].hash.store(hash, std::memory_order_release);
              log_line("vs {:016X} kept for dumps ({} bytes)", static_cast<unsigned long long>(hash), static_cast<std::size_t>(length));
              }
        }
#if defined(EDWORLD_EHT)
      if(hash == galmap_text_vs and not creating_own)
        {
        static_cast<IUnknown *>(*shader)->AddRef();  // pinned, as the watched ones
        if(void * const before{galmap_vs.exchange(*shader, std::memory_order_relaxed)})
          static_cast<IUnknown *>(before)->Release();  // made again: the old one is compared with no more
        else
          log_line("galmap: the game made the text shader the galaxy map's names are read from");
        }
#endif
      if(hash == atmosphere_shell_vs and not creating_own)
        {
        static_cast<IUnknown *>(*shader)->AddRef();  // pinned, as the watched ones
        if(void * const before{shell_vs.exchange(*shader, std::memory_order_relaxed)})
          static_cast<IUnknown *>(before)->Release();  // made again: the old one is compared with no more
        else
          log_line("clouds: the game made the atmosphere shell's shader");
        }
      auto const & listed{start_fields().watch_vs};
      for(std::size_t i{}; i != listed.size(); ++i)
        if(listed[i] == hash)
          {
          if(std::uint32_t const at{table_get(*shader)}; at < max_vs_seen)
            verdict[at].store(verdict_e::panel, std::memory_order_relaxed);
          if(watch(*shader, static_cast<std::uint32_t>(i), false))
            log_line("vs {:016X} watched (list index {}, pointer {})", static_cast<unsigned long long>(hash), i, static_cast<void const *>(*shader));
          break;
          }
      return hr;
      }

#if defined(EDWORLD_EHT)
    // ---- the catalog's hooks: the game's creation, then a note; nothing else changes ----
    template<typename shader_t>
    using create_shader_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, void const *, SIZE_T, ID3D11ClassLinkage *, shader_t **);
    using create_gs_so_fn = HRESULT(STDMETHODCALLTYPE *)(
      ID3D11Device *, void const *, SIZE_T, D3D11_SO_DECLARATION_ENTRY const *, UINT, UINT const *, UINT, UINT,
      ID3D11ClassLinkage *, ID3D11GeometryShader **
    );
    using create_input_layout_fn = HRESULT(STDMETHODCALLTYPE *)(
      ID3D11Device *, D3D11_INPUT_ELEMENT_DESC const *, UINT, void const *, SIZE_T, ID3D11InputLayout **
    );
    create_shader_fn<ID3D11GeometryShader> orig_create_gs{};
    create_shader_fn<ID3D11PixelShader> orig_create_ps{};
    create_shader_fn<ID3D11HullShader> orig_create_hs{};
    create_shader_fn<ID3D11DomainShader> orig_create_ds{};
    create_shader_fn<ID3D11ComputeShader> orig_create_cs{};
    create_gs_so_fn orig_create_gs_so{};
    create_input_layout_fn orig_create_input_layout{};

    template<typename shader_t, create_shader_fn<shader_t> * original, shader_stage_e stage>
    HRESULT STDMETHODCALLTYPE hook_create_shader(
      ID3D11Device * self,
      void const * bytecode,
      SIZE_T length,
      ID3D11ClassLinkage * linkage,
      shader_t ** shader
    )
      {
      HRESULT const hr{(*original)(self, bytecode, length, linkage, shader)};
      if(SUCCEEDED(hr) and bytecode and not creating_own)
        shader_catalog_note(stage, bytecode, length, fnv1a64(static_cast<std::uint8_t const *>(bytecode), length));
      return hr;
      }

    HRESULT STDMETHODCALLTYPE hook_create_gs_so(
      ID3D11Device * self,
      void const * bytecode,
      SIZE_T length,
      D3D11_SO_DECLARATION_ENTRY const * entries,
      UINT entry_count,
      UINT const * strides,
      UINT stride_count,
      UINT rasterized,
      ID3D11ClassLinkage * linkage,
      ID3D11GeometryShader ** shader
    )
      {
      HRESULT const hr{orig_create_gs_so(self, bytecode, length, entries, entry_count, strides, stride_count, rasterized, linkage, shader)};
      if(SUCCEEDED(hr) and bytecode and not creating_own)
        shader_catalog_note(shader_stage_e::geometry, bytecode, length, fnv1a64(static_cast<std::uint8_t const *>(bytecode), length));
      return hr;
      }

    HRESULT STDMETHODCALLTYPE hook_create_input_layout(
      ID3D11Device * self,
      D3D11_INPUT_ELEMENT_DESC const * elements,
      UINT count,
      void const * signature,
      SIZE_T length,
      ID3D11InputLayout ** layout
    )
      {
      HRESULT const hr{orig_create_input_layout(self, elements, count, signature, length, layout)};
      if(SUCCEEDED(hr) and not creating_own)
        shader_catalog_note_layout(elements, count);
      return hr;
      }

    ///\brief the catalog's hooks on the device's table; one failing leaves the others (and edworld) as they are
    auto patch_catalog(ID3D11Device * dev) noexcept -> void
      {
      if(not start_fields().shader_catalog)
        return;
      bool ok{true};
      ok = patch_slot(dev, slot_create_pixel_shader,
                      reinterpret_cast<void *>(&hook_create_shader<ID3D11PixelShader, &orig_create_ps, shader_stage_e::pixel>),
                      reinterpret_cast<void **>(&orig_create_ps)) and ok;
      ok = patch_slot(dev, slot_create_compute_shader,
                      reinterpret_cast<void *>(&hook_create_shader<ID3D11ComputeShader, &orig_create_cs, shader_stage_e::compute>),
                      reinterpret_cast<void **>(&orig_create_cs)) and ok;
      ok = patch_slot(dev, slot_create_geometry_shader,
                      reinterpret_cast<void *>(&hook_create_shader<ID3D11GeometryShader, &orig_create_gs, shader_stage_e::geometry>),
                      reinterpret_cast<void **>(&orig_create_gs)) and ok;
      ok = patch_slot(dev, slot_create_hull_shader,
                      reinterpret_cast<void *>(&hook_create_shader<ID3D11HullShader, &orig_create_hs, shader_stage_e::hull>),
                      reinterpret_cast<void **>(&orig_create_hs)) and ok;
      ok = patch_slot(dev, slot_create_domain_shader,
                      reinterpret_cast<void *>(&hook_create_shader<ID3D11DomainShader, &orig_create_ds, shader_stage_e::domain>),
                      reinterpret_cast<void **>(&orig_create_ds)) and ok;
      ok = patch_slot(dev, slot_create_geometry_shader_so, reinterpret_cast<void *>(&hook_create_gs_so),
                      reinterpret_cast<void **>(&orig_create_gs_so)) and ok;
      ok = patch_slot(dev, slot_create_input_layout, reinterpret_cast<void *>(&hook_create_input_layout),
                      reinterpret_cast<void **>(&orig_create_input_layout)) and ok;
      log_line("shaders: the catalog {} (edworld_shaders beside the log, build {})",
               ok ? "notes every shader the game creates" : "could not patch every creation; it notes the ones it could",
               game_build_key());
      }
#endif

    void STDMETHODCALLTYPE hook_vs_set_shader(
      ID3D11DeviceContext * self,
      ID3D11VertexShader * shader,
      ID3D11ClassInstance * const * instances,
      UINT instance_count
    )
      {
      if(own_depth != 0)
        return orig_vs_set_shader(self, shader, instances, instance_count);
      if(self == immediate.load(std::memory_order_relaxed))
        {
        int found{-1};
        std::uint32_t pending{max_vs_seen};
        std::uint32_t holo{max_vs_seen};
        if(shader)
          {
          // a seen shader: its slot says whether it is watched; one made before edworld came (not in the table) is
          // looked for in the watched list
          if(std::uint32_t const at{table_get(shader)}; at < max_vs_seen)
            {
            if(std::uint8_t const w{slot_watched[at].load(std::memory_order_acquire)}; w != 0u)
              found = static_cast<int>(w) - 1;
            else
              {
              if(verdict[at].load(std::memory_order_relaxed) == verdict_e::pending)
                pending = at;
              if(holo_kind[at].load(std::memory_order_relaxed) != holo_kind_e::none)
                holo = at;
              }
            }
          else
            {
            std::uint32_t const reserved{watched_count.load(std::memory_order_acquire)};
            std::uint32_t const n{reserved < max_watched ? reserved : max_watched};
            for(std::uint32_t i{}; i != n; ++i)
              if(watched_ptr[i].load(std::memory_order_acquire) == shader)
                {
                found = static_cast<int>(i);
                break;
                }
            }
          }
        current_watched = found;
        current_shell = shader != nullptr and shader == shell_vs.load(std::memory_order_relaxed);
#if defined(EDWORLD_EHT)
        current_galmap = shader != nullptr and shader == galmap_vs.load(std::memory_order_relaxed);
#endif
        current_pending = pending;
        current_holo = holo;
        }
      orig_vs_set_shader(self, shader, instances, instance_count);
      }

    ///\brief a dump_vs draw of the whole frame: its index range, vertex buffers (from its base vertex) and the VS
    /// record pool at t33 (its records from the draw's start instance, as the panels'), and its shader's bytecode once
    /// per dump
    auto full_inputs(ID3D11DeviceContext * ctx, std::uint32_t a, std::uint64_t hash, std::uint32_t count, std::uint32_t start,
                     std::int32_t base, std::uint32_t start_instance) noexcept -> void
      {
      char name[64];
      ID3D11Buffer * ib{};
      DXGI_FORMAT ib_format{};
      UINT ib_offset{};
      ctx->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
      if(ib)
        {
        std::uint32_t const size{ib_format == DXGI_FORMAT_R16_UINT ? 2u : 4u};
        format_into(name, "a{}_ib_u{}", a, size * 8u);
        queue_buffer(ctx, ib, ib_offset + static_cast<std::uint64_t>(start) * size, static_cast<std::uint64_t>(count) * size, name);
        ib->Release();
        }
      ID3D11Buffer * vbs[4]{};
      UINT strides[4]{}, offsets[4]{};
      ctx->IAGetVertexBuffers(0, 4, vbs, strides, offsets);
      for(std::uint32_t v{}; v != 4; ++v)
        {
        if(not vbs[v])
          continue;
        format_into(name, "a{}_vb{}_s{}", a, v, strides[v]);
        // the input layout is not known: a buffer read from the base vertex, or else (past its end) an instance
        // stream read from the start instance, as the panels' slot 0
        D3D11_BUFFER_DESC bd{};
        vbs[v]->GetDesc(&bd);
        std::uint64_t from{offsets[v] + static_cast<std::uint64_t>(std::max(base, 0)) * strides[v]};
        if(from >= bd.ByteWidth)
          from = offsets[v] + static_cast<std::uint64_t>(start_instance) * strides[v];
        queue_buffer(ctx, vbs[v], from, buffer_window, name);
        vbs[v]->Release();
        }
      ID3D11ShaderResourceView * pool{};
      ctx->VSGetShaderResources(33, 1, &pool);
      if(pool)
        {
        ID3D11Resource * res{};
        pool->GetResource(&res);
        pool->Release();
        ID3D11Buffer * buffer{};
        if(res)
          {
          res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void **>(&buffer));
          res->Release();
          }
        if(buffer)
          {
          format_into(name, "a{}_t33", a);
          queue_buffer(ctx, buffer, 0, pool_window, name);
          buffer->Release();
          }
        }
      // what the draw's pixel shader reads: t0..t7, a texture's size and format or a buffer's bytes
      ID3D11ShaderResourceView * srvs[8]{};
      ctx->PSGetShaderResources(0, 8, srvs);
      for(std::uint32_t t{}; t != 8; ++t)
        {
        if(not srvs[t])
          continue;
        ID3D11Resource * res{};
        srvs[t]->GetResource(&res);
        srvs[t]->Release();
        if(not res)
          continue;
        D3D11_RESOURCE_DIMENSION dim{};
        res->GetType(&dim);
        auto const id{static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(static_cast<void *>(res)) & 0xffffffffull)};
        if(dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          {
          ID3D11Texture2D * tex{};
          if(SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) and tex)
            {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            full_line("  a{} ps t{} texture {:08x} {}x{} f{} mips {} array {}", a, t, as_unsigned(id), d.Width, d.Height,
                      static_cast<unsigned>(d.Format), d.MipLevels, d.ArraySize);
            // its picture too, as the draw reads it (its first mip): <stamp>_a<draw>_ps<slot>_...raw
            if(d.SampleDesc.Count == 1u and d.ArraySize == 1u and d.Width <= 4096u and d.Height <= 4096u and d.Width * d.Height > 1u)
              full_checkpoint(ctx, tex, d, a, t + 1u);
            tex->Release();
            }
          }
        else if(dim == D3D11_RESOURCE_DIMENSION_BUFFER)
          {
          ID3D11Buffer * buffer{};
          if(SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void **>(&buffer))) and buffer)
            {
            D3D11_BUFFER_DESC bd{};
            buffer->GetDesc(&bd);
            full_line("  a{} ps t{} buffer {:08x} {} bytes stride {}", a, t, as_unsigned(id), bd.ByteWidth, bd.StructureByteStride);
            buffer->Release();
            }
          }
        else
          full_line("  a{} ps t{} resource {:08x} dimension {}", a, t, as_unsigned(id), static_cast<unsigned>(dim));
        res->Release();
        }
      std::uint32_t const kept{std::min(kept_vs_reserved.load(), max_kept_vs)};
      for(std::uint32_t k{}; k != kept; ++k)
        if(kept_vs[k].hash.load(std::memory_order_acquire) == hash and not full_kept_written[k])
          {
          full_kept_written[k] = true;
          wchar_t file[MAX_PATH];
          format_into(file, MAX_PATH, L"{}\\edworld_dumps\\{}_vs_{:016X}.dxbc", start_fields().dir, utf8(dump_stamp),
                        static_cast<unsigned long long>(hash));
          queue_write(file, kept_vs[k].bytes, kept_vs[k].length, true);
          }
      }

    auto full_draw(ID3D11DeviceContext * ctx, char kind, std::uint32_t count, std::uint32_t instances, std::uint32_t start,
                   std::int32_t base, std::uint32_t start_instance) noexcept -> void
      {
      if(full_draws == full_max_draws or frame != full_frame)
        return;
      std::uint32_t const a{full_draws++};
      ID3D11VertexShader * vs{};
      ctx->VSGetShader(&vs, nullptr, nullptr);
      std::uint64_t const hash{vs ? vs_hash_of(vs) : 0};
      if(vs)
        vs->Release();
      D3D11_VIEWPORT vp{};
      UINT viewports{1};
      ctx->RSGetViewports(&viewports, &vp);
      ID3D11RenderTargetView * rtv{};
      ctx->OMGetRenderTargets(1, &rtv, nullptr);
      UINT rt_w{}, rt_h{}, rt_f{};
      void * rt_ptr{};
      if(rtv)
        {
        ID3D11Resource * res{};
        rtv->GetResource(&res);
        rt_ptr = res;
        ID3D11Texture2D * tex{};
        if(res and SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))))
          {
          D3D11_TEXTURE2D_DESC d{};
          tex->GetDesc(&d);
          rt_w = d.Width;
          rt_h = d.Height;
          rt_f = static_cast<UINT>(d.Format);
          bool const screen_sized{d.Width >= 2048u and d.SampleDesc.Count == 1u and d.ArraySize == 1u};
          // the target drawn to before is left: its pass is done, copied once
          if(full_prev_rt and full_prev_rt != tex)
            {
            bool seen{};
            for(std::uint32_t l{}; l != full_left_count; ++l)
              seen = seen or full_left[l] == full_prev_rt;
            if(not seen and full_left_count != full_max_left)
              {
              full_left[full_left_count++] = full_prev_rt;
              D3D11_TEXTURE2D_DESC pd{};
              full_prev_rt->GetDesc(&pd);
              full_checkpoint(ctx, full_prev_rt, pd, a);
              }
            full_prev_rt->Release();
            full_prev_rt = nullptr;
            }
          if(screen_sized and full_prev_rt != tex)
            {
            full_prev_rt = tex;
            full_prev_rt->AddRef();
            }
          if(screen_sized)
            {
            if(a <= full_early_draws and a % full_early_every == 0u)
              full_checkpoint(ctx, tex, d, a);
            else if(a >= full_next_checkpoint)
              {
              full_checkpoint(ctx, tex, d, a);
              full_next_checkpoint = a + full_checkpoint_every;
              }
            if(full_last_rt != tex)
              {
              if(full_last_rt)
                full_last_rt->Release();
              full_last_rt = tex;
              full_last_rt->AddRef();
              }
            }
          tex->Release();
          }
        if(res)
          res->Release();
        rtv->Release();
        }
      full_line("a{} {} vs {:016X} count {} inst {} start {} base {} sinst {} vp {:.0f},{:.0f} {:.0f}x{:.0f} rt {}x{} f{} at {:08x}", a, kind,
               static_cast<unsigned long long>(hash), count, instances, start, base, start_instance, static_cast<double>(vp.TopLeftX),
               static_cast<double>(vp.TopLeftY), static_cast<double>(vp.Width), static_cast<double>(vp.Height), rt_w, rt_h, rt_f,
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(rt_ptr) & 0xffffffffull));
      ID3D11Buffer * cbs[4]{};
      UINT firsts[4]{}, counts[4]{};
      if(immediate1)
        immediate1->VSGetConstantBuffers1(0, 4, cbs, firsts, counts);
      else
        ctx->VSGetConstantBuffers(0, 4, cbs);
      char name[64];
      bool const looked_into{in_dump_vs(hash)};
      for(std::uint32_t c{}; c != 4; ++c)
        {
        if(not cbs[c])
          continue;
        format_into(name, "a{}_cb{}", a, c);
        // a dump_vs draw's buffers whole from their first constant (the hologram's camera is row 270 of cb1)
        std::uint64_t const whole{counts[c] ? counts[c] * 16ull : 65536ull};
        std::uint64_t const bytes{looked_into ? whole : std::min<std::uint64_t>(whole, full_cb_bytes)};
        if(looked_into)
          full_line("  a{} cb{} first {} count {}", a, c, firsts[c], counts[c]);
        queue_buffer(ctx, cbs[c], firsts[c] * 16ull, bytes, name, false);
        cbs[c]->Release();
        }
      if(in_dump_vs(hash))
        {
        full_inputs(ctx, a, hash, count, start, base, start_instance);
        full_after_draw = a + 1u;  // its target is copied again right after the game's draw
        }
      }

    ///\brief after a dump_vs draw of the whole frame: its render target as the draw left it (`_a<draw>_rt_after_...`)
    auto full_after(ID3D11DeviceContext * ctx) noexcept -> void
      {
      std::uint32_t const a{full_after_draw - 1u};
      full_after_draw = 0;
      ID3D11RenderTargetView * rtv{};
      ctx->OMGetRenderTargets(1, &rtv, nullptr);
      if(not rtv)
        return;
      ID3D11Resource * res{};
      rtv->GetResource(&res);
      rtv->Release();
      if(not res)
        return;
      ID3D11Texture2D * tex{};
      res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
      res->Release();
      if(not tex)
        return;
      D3D11_TEXTURE2D_DESC d{};
      tex->GetDesc(&d);
      // up to 8192 wide: the game's main target (5310x1274 on 2026-10-21) too, so the draw that puts a pad's number on
      // the screen shows in its copy; the whole frame's checkpoints stay limited to full_max_checkpoints
      if(d.SampleDesc.Count == 1u and d.ArraySize == 1u and d.Width <= 8192u and d.Height <= 8192u)
        full_checkpoint(ctx, tex, d, a, rt_after_slot);
      tex->Release();
      }

    auto guarded_full_after(ID3D11DeviceContext * ctx) noexcept -> void
      {
      if(hooks_off())  // after 8 faults every guarded path stays off
        return;
      LONGLONG const began{qpc_now()};
      ++own_depth;
      hook_work(
        [&]
        {
          full_after(ctx);
        },
        [&]() noexcept
        {
          full_after_draw = 0;
        });
      --own_depth;
      // the copy of the game's main target after a dump_vs draw takes milliseconds: no gap of the game's either (it ended
      // each whole frame of 2026-10-22 right after the hologram's draws, at 18 draws)
      if(last_draw_qpc)
        last_draw_qpc += qpc_now() - began;
      }

    auto guarded_full(ID3D11DeviceContext * ctx, char kind, std::uint32_t count, std::uint32_t instances, std::uint32_t start,
                      std::int32_t base, std::uint32_t start_instance) noexcept -> void
      {
      if(hooks_off())
        return;
      LONGLONG const began{qpc_now()};
      ++own_depth;
      hook_work(
        [&]
        {
          full_draw(ctx, kind, count, instances, start, base, start_instance);
        },
        [&]() noexcept
        {
          full_armed.store(false, std::memory_order_relaxed);
        });
      --own_depth;
      // the dump's own work (copies of whole targets, milliseconds) is no gap of the game's: it must not end the frame
      if(last_draw_qpc)
        last_draw_qpc += qpc_now() - began;
      }

    // ---- the clouds (PoC): the game's draw of its shell of the atmosphere, once more, with shaders of ours ----
    ///\brief edupscale's jitter frame (its export edupscale_jitter_frame, 2026-11-20): the count of the frames it moved
    /// the camera in, -1 while it does not, or when edupscale (that export) is not loaded. Looked up once in 600 calls
    /// until found (the loader's lock under Wine), said in the log when found
    [[nodiscard]]
    auto edupscale_jitter_frame() noexcept -> int
      {
      using jitter_frame_fn = int (*)();
      static jitter_frame_fn found{};
      static std::uint32_t calls{};
      if(not found and calls++ % 600u == 0u)
        if(HMODULE const m{GetModuleHandleW(L"edupscale.dll")})
          if((found = reinterpret_cast<jitter_frame_fn>(reinterpret_cast<void *>(GetProcAddress(m, "edupscale_jitter_frame")))))
            log_line("clouds: edupscale's jitter frame found (clouds_noise_time)");
      return found ? found() : -1;
      }

    struct clouds_t
      {
      ID3D11VertexShader * vs{};
      ID3D11PixelShader * ps{};
      ID3D11BlendState * blend{};
      ID3D11RasterizerState * raster{};
      ID3D11Buffer * params{};
      bool tried{};
      bool told{};
      // the volume (PoC)
      ID3D11VertexShader * volume_vs{};
      ID3D11PixelShader * volume_ps{};
      ID3D11BlendState * volume_blend{};
      ID3D11RasterizerState * volume_raster{};
      ID3D11DepthStencilState * volume_depth_state{};
      ID3D11Buffer * volume_params{};
      // the volume's parameters, the wind's seconds among them, written again for each draw
      float volume_values[128]{};
      std::uint64_t draws_made{};  ///< the volume's draws, for clouds_noise_time 2 without edupscale's jitter
      ULONGLONG wind_start{};
      ID3D11Texture2D * depth_copy{};
      ID3D11ShaderResourceView * depth_view{};
      UINT depth_width{};
      UINT depth_height{};
      DXGI_FORMAT depth_format{};
      bool volume_failed{};
      // the volume at half size (PoC): marched into a target of half the picture's width and height, then laid over
      // the picture by the depth
      ID3D11VertexShader * up_vs{};
      ID3D11PixelShader * up_ps{};
      ID3D11BlendState * half_blend{};
      ID3D11Texture2D * half_target{};
      ID3D11RenderTargetView * half_rtv{};
      ID3D11ShaderResourceView * half_srv{};
      UINT half_width{};
      UINT half_height{};
      bool half_failed{};
      ID3D11ShaderResourceView * blue_noise{};  // where each pixel's steps start
      // whether the clouds fit the game's state at the shell's draws (fits.h): each reason once in the log
      misfit_log_t misfits{};
      bool told_full_size{};  ///< the half size off while the picture and the depth differ, told
      };
    clouds_t clouds;

    // the GPU time of the volume (the copy of the depth and the draw), read back a few draws late without waiting for
    // the GPU, and summed up in the log every 5 s with the pixels it shaded
    struct clouds_timing_t
      {
      struct slot_t
        {
        ID3D11Query * disjoint{};
        ID3D11Query * begin{};
        ID3D11Query * end{};
        ID3D11Query * stats{};
        bool pending{};
        };
      std::array<slot_t, 6> slots{};
      std::size_t next{};
      bool tried{};
      bool ready{};
      edworld::gpu_span_window_t window{};
      ULONGLONG window_start{};
      std::uint32_t busy{};  // draws not timed as every slot still waited for the GPU
      std::uint32_t calls{};  // the shell's draws with the volume, one in five of them timed
      };
    clouds_timing_t clouds_timing;

    auto make_clouds_timing(ID3D11DeviceContext * ctx) noexcept -> bool
      {
      if(clouds_timing.tried)
        return clouds_timing.ready;
      clouds_timing.tried = true;
      ID3D11Device * device{};
      ctx->GetDevice(&device);
      bool ok{device != nullptr};
      for(auto & s: clouds_timing.slots)
        {
        D3D11_QUERY_DESC q{};
        q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        ok = ok and SUCCEEDED(device->CreateQuery(&q, &s.disjoint));
        q.Query = D3D11_QUERY_TIMESTAMP;
        ok = ok and SUCCEEDED(device->CreateQuery(&q, &s.begin)) and SUCCEEDED(device->CreateQuery(&q, &s.end));
        q.Query = D3D11_QUERY_PIPELINE_STATISTICS;
        ok = ok and SUCCEEDED(device->CreateQuery(&q, &s.stats));
        }
      if(device)
        device->Release();
      clouds_timing.ready = ok;
      clouds_timing.window_start = GetTickCount64();
      log_line("clouds: GPU timing {}", ok ? "on, summed up every 5 s" : "could not be made, the clouds are drawn untimed");
      return ok;
      }

    ///\brief takes in the spans the GPU has finished, and writes the window's sum to the log once it is 5 s long
    auto collect_clouds_timing(ID3D11DeviceContext * ctx) noexcept -> void
      {
      for(auto & s: clouds_timing.slots)
        {
        if(not s.pending)
          continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 begin{};
        UINT64 end{};
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
        UINT const flags{D3D11_ASYNC_GETDATA_DONOTFLUSH};
        if(ctx->GetData(s.disjoint, &disjoint, sizeof disjoint, flags) != S_OK or ctx->GetData(s.begin, &begin, sizeof begin, flags) != S_OK
           or ctx->GetData(s.end, &end, sizeof end, flags) != S_OK or ctx->GetData(s.stats, &stats, sizeof stats, flags) != S_OK)
          continue;
        s.pending = false;
        if(not disjoint.Disjoint and disjoint.Frequency != 0u and end >= begin)
          edworld::add_span(clouds_timing.window, static_cast<float>(static_cast<double>(end - begin) * 1000. / static_cast<double>(disjoint.Frequency)),
                            static_cast<float>(static_cast<double>(stats.PSInvocations) / 1e6));
        }
      ULONGLONG const now{GetTickCount64()};
      if(now - clouds_timing.window_start < 5000u)
        return;
      auto const s{edworld::span_summary(clouds_timing.window)};
      log_line("clouds gpu 5s: draws {} (1 in 5 timed), ms median {:.2f} mean {:.2f} max {:.2f}, pixels {:.2f} M a draw, {:.2f} ms per million pixels, untimed {}",
               s.count, s.ms_median, s.ms_mean, s.ms_max, s.megapixels_mean, s.ms_per_megapixel, clouds_timing.busy);
      clouds_timing.window = {};
      clouds_timing.busy = 0u;
      clouds_timing.window_start = now;
      }

    ///\brief the slot that times this draw, or none while every slot still waits for the GPU
    auto start_clouds_timing(ID3D11DeviceContext * ctx) noexcept -> clouds_timing_t::slot_t *
      {
      if(not make_clouds_timing(ctx))
        return nullptr;
      // one draw in five timed, and the finished ones taken in only then (D11, 2026-11-18: four queries begun and up to
      // 24 asked for at each draw of a shell before); five, not four: with two shells a frame each is timed in turn
      if(clouds_timing.calls++ % 5u != 0u)
        return nullptr;
      collect_clouds_timing(ctx);
      auto & s{clouds_timing.slots[clouds_timing.next]};
      if(s.pending)
        {
        ++clouds_timing.busy;
        return nullptr;
        }
      clouds_timing.next = (clouds_timing.next + 1u) % clouds_timing.slots.size();
      ctx->Begin(s.disjoint);
      ctx->End(s.begin);
      ctx->Begin(s.stats);
      return &s;
      }

    auto stop_clouds_timing(ID3D11DeviceContext * ctx, clouds_timing_t::slot_t * s) noexcept -> void
      {
      if(not s)
        return;
      ctx->End(s->stats);
      ctx->End(s->end);
      ctx->End(s->disjoint);
      s->pending = true;
      }

    auto make_clouds(ID3D11DeviceContext * ctx) noexcept -> bool
      {
      if(clouds.tried)
        return clouds.params != nullptr;
      clouds.tried = true;
      ID3D11Device * device{};
      ctx->GetDevice(&device);
      if(not device)
        return false;
      creating_own = true;
      bool ok{SUCCEEDED(device->CreateVertexShader(g_clouds_vs, sizeof g_clouds_vs, nullptr, &clouds.vs))};
      ok = ok and SUCCEEDED(device->CreatePixelShader(g_clouds_ps, sizeof g_clouds_ps, nullptr, &clouds.ps));
      creating_own = false;

      D3D11_BLEND_DESC blend{};
      blend.RenderTarget[0].BlendEnable = TRUE;
      blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
      blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
      blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
      blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
      blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
      blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      ok = ok and SUCCEEDED(device->CreateBlendState(&blend, &clouds.blend));

      D3D11_RASTERIZER_DESC raster{};
      raster.FillMode = D3D11_FILL_SOLID;
      raster.CullMode = D3D11_CULL_NONE;
      raster.DepthClipEnable = TRUE;
      ok = ok and SUCCEEDED(device->CreateRasterizerState(&raster, &clouds.raster));

      float const values[4]{start_fields().clouds_height, start_fields().clouds_alpha, start_fields().clouds_cover, start_fields().clouds_light};
      D3D11_BUFFER_DESC desc{};
      desc.ByteWidth = sizeof values;
      desc.Usage = D3D11_USAGE_IMMUTABLE;
      desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA data{};
      data.pSysMem = values;
      ID3D11Buffer * params{};
      ok = ok and SUCCEEDED(device->CreateBuffer(&desc, &data, &params));
      device->Release();
      clouds.params = ok ? params : nullptr;
      log_line("clouds: {}", ok ? "ready" : "could not be made, none will be drawn");
      return ok;
      }

    auto make_volume(ID3D11DeviceContext * ctx) noexcept -> bool
      {
      if(clouds.volume_params)
        return true;
      if(clouds.volume_failed)
        return false;
      clouds.volume_failed = true;
      ID3D11Device * device{};
      ctx->GetDevice(&device);
      if(not device)
        return false;
      creating_own = true;
      bool ok{SUCCEEDED(device->CreateVertexShader(g_clouds3d_vs, sizeof g_clouds3d_vs, nullptr, &clouds.volume_vs))};
      ok = ok and SUCCEEDED(device->CreatePixelShader(g_clouds3d_ps, sizeof g_clouds3d_ps, nullptr, &clouds.volume_ps));
      clouds.half_failed = not(SUCCEEDED(device->CreateVertexShader(g_clouds_up_vs, sizeof g_clouds_up_vs, nullptr, &clouds.up_vs))
                               and SUCCEEDED(device->CreatePixelShader(g_clouds_up_ps, sizeof g_clouds_up_ps, nullptr, &clouds.up_ps)));
      creating_own = false;

      // what the shader gives is already weighed by how much of it there is
      D3D11_BLEND_DESC blend{};
      blend.RenderTarget[0].BlendEnable = TRUE;
      blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
      blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
      blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
      blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
      blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
      blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      ok = ok and SUCCEEDED(device->CreateBlendState(&blend, &clouds.volume_blend));
      // into the half target, cleared first, the march's colour and cover as they are
      D3D11_BLEND_DESC plain{};
      plain.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      clouds.half_failed = clouds.half_failed or FAILED(device->CreateBlendState(&plain, &clouds.half_blend));

      D3D11_RASTERIZER_DESC raster{};
      raster.FillMode = D3D11_FILL_SOLID;
      raster.CullMode = D3D11_CULL_NONE;
      raster.DepthClipEnable = FALSE;
      ok = ok and SUCCEEDED(device->CreateRasterizerState(&raster, &clouds.volume_raster));

      // the far half of the sphere lies behind the planet: the shader tests the depth along the ray instead
      D3D11_DEPTH_STENCIL_DESC depth{};
      depth.DepthEnable = FALSE;
      depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
      depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
      ok = ok and SUCCEEDED(device->CreateDepthStencilState(&depth, &clouds.volume_depth_state));

      try
        {
        auto const ranks{edworld::blue_noise_ranks(64)};
        std::vector<std::uint16_t> texels(ranks.size());
        for(std::size_t i{}; i != ranks.size(); ++i)
          texels[i] = static_cast<std::uint16_t>(ranks[i] * 65535u / (ranks.size() - 1u));
        D3D11_TEXTURE2D_DESC t{};
        t.Width = 64;
        t.Height = 64;
        t.MipLevels = 1;
        t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R16_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_IMMUTABLE;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = texels.data();
        init.SysMemPitch = 64u * sizeof(std::uint16_t);
        ID3D11Texture2D * noise{};
        ok = ok and SUCCEEDED(device->CreateTexture2D(&t, &init, &noise))
             and SUCCEEDED(device->CreateShaderResourceView(noise, nullptr, &clouds.blue_noise));
        if(noise)
          noise->Release();
        }
      catch(std::exception const & e)
        {
        log_line("clouds: the blue noise could not be made ({})", e.what());
        ok = false;
        }

      // 19..32 (the game's fade, near and far, the debug colour, the planet's size on the screen, the steps' most, the
      // edge, the haze) and 33..42 (the atmosphere's kind) are written again for each draw, as they change while the game
      // runs, 43 the lumps' size, 44..85 the storms, 86..91 the lumps by the layer and the weather, 92..111 the aurora,
      // 112..123 the snow, 124..127 the weather's R/L before the day was read and how far it has come in
      float const values[128]{
        start_fields().clouds_bottom, start_fields().clouds_top, start_fields().clouds_cover, start_fields().clouds_light,
        clouds_steps_held(start_fields().clouds_steps), start_fields().clouds_density, start_fields().clouds_opacity, start_fields().clouds_top_gain,
        start_fields().clouds_min_light, start_fields().clouds_under_gain, start_fields().clouds_detail, start_fields().clouds_detail_size,
        start_fields().clouds_detail_range, start_fields().clouds_detail_edge, start_fields().clouds_detail_gap, 0.f,
        start_fields().clouds_wind_low / 3.6f, start_fields().clouds_wind_high / 3.6f, 1.f, 0.f,
        0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f
      };  // the rest, 44..127, zero
      std::memcpy(clouds.volume_values, values, sizeof values);
      clouds.wind_start = GetTickCount64();
      D3D11_BUFFER_DESC desc{};
      desc.ByteWidth = sizeof values;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA data{};
      data.pSysMem = values;
      ID3D11Buffer * params{};
      ok = ok and SUCCEEDED(device->CreateBuffer(&desc, &data, &params));
      device->Release();
      if(ok)
        {
        clouds.volume_params = params;
        clouds.volume_failed = false;
        }
      log_line("clouds: the volume {}{}", ok ? "is ready" : "could not be made, the layer stays",
               ok and clouds.half_failed ? ", at full size (the half size could not be made)" : "");
      return ok;
      }

    ///\brief the half target for a picture of the size of the bound target, made again when that size changes;
    /// false when there is none, and the clouds are drawn at full size
    auto make_half(ID3D11Device * device, ID3D11RenderTargetView * picture) noexcept -> bool
      {
      if(clouds.half_failed or not device or not picture)
        return false;
      ID3D11Resource * resource{};
      picture->GetResource(&resource);
      ID3D11Texture2D * texture{};
      if(resource)
        {
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
        resource->Release();
        }
      if(not texture)
        return false;
      D3D11_TEXTURE2D_DESC d{};
      texture->GetDesc(&d);
      texture->Release();
      UINT const width{(d.Width + 1u) / 2u};
      UINT const height{(d.Height + 1u) / 2u};
      if(clouds.half_target and clouds.half_width == width and clouds.half_height == height)
        return true;
      for(IUnknown ** held: std::initializer_list<IUnknown **>{
            reinterpret_cast<IUnknown **>(&clouds.half_srv), reinterpret_cast<IUnknown **>(&clouds.half_rtv),
            reinterpret_cast<IUnknown **>(&clouds.half_target)
          })
        if(*held)
          {
          (*held)->Release();
          *held = nullptr;
          }
      D3D11_TEXTURE2D_DESC c{};
      c.Width = width;
      c.Height = height;
      c.MipLevels = 1;
      c.ArraySize = 1;
      c.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      c.SampleDesc.Count = 1;
      c.Usage = D3D11_USAGE_DEFAULT;
      c.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      bool const ok{SUCCEEDED(device->CreateTexture2D(&c, nullptr, &clouds.half_target))
                    and SUCCEEDED(device->CreateRenderTargetView(clouds.half_target, nullptr, &clouds.half_rtv))
                    and SUCCEEDED(device->CreateShaderResourceView(clouds.half_target, nullptr, &clouds.half_srv))};
      clouds.half_width = width;
      clouds.half_height = height;
      clouds.half_failed = not ok;
      log_line("clouds: half target {}x{} for a picture of {}x{} {}", width, height, d.Width, d.Height,
               ok ? "made" : "could not be made, the clouds are drawn at full size");
      return ok;
      }

    ///\brief this moment's depth of the scene as something a shader can read; false without one
    auto copy_depth(ID3D11DeviceContext * ctx) noexcept -> bool
      {
      ID3D11DepthStencilView * dsv{};
      ctx->OMGetRenderTargets(0, nullptr, &dsv);
      if(not dsv)
        return false;
      D3D11_DEPTH_STENCIL_VIEW_DESC view{};
      dsv->GetDesc(&view);
      ID3D11Resource * resource{};
      dsv->GetResource(&resource);
      dsv->Release();
      ID3D11Texture2D * texture{};
      resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
      bool ok{};
      if(texture)
        {
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        DXGI_FORMAT family{};
        DXGI_FORMAT read{};
        switch(view.Format)
          {
          case DXGI_FORMAT_D32_FLOAT: family = DXGI_FORMAT_R32_TYPELESS; read = DXGI_FORMAT_R32_FLOAT; break;
          case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            family = DXGI_FORMAT_R32G8X24_TYPELESS;
            read = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            break;
          case DXGI_FORMAT_D24_UNORM_S8_UINT: family = DXGI_FORMAT_R24G8_TYPELESS; read = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
          default: break;
          }
        if(family != DXGI_FORMAT_UNKNOWN and d.SampleDesc.Count == 1u)
          {
          if(not clouds.depth_copy or clouds.depth_width != d.Width or clouds.depth_height != d.Height or clouds.depth_format != family)
            {
            if(clouds.depth_view)
              clouds.depth_view->Release();
            if(clouds.depth_copy)
              clouds.depth_copy->Release();
            clouds.depth_view = nullptr;
            clouds.depth_copy = nullptr;
            ID3D11Device * device{};
            ctx->GetDevice(&device);
            D3D11_TEXTURE2D_DESC c{d};
            c.Format = family;
            c.Usage = D3D11_USAGE_DEFAULT;
            c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            c.CPUAccessFlags = 0;
            c.MiscFlags = 0;
            if(device and SUCCEEDED(device->CreateTexture2D(&c, nullptr, &clouds.depth_copy)))
              {
              D3D11_SHADER_RESOURCE_VIEW_DESC s{};
              s.Format = read;
              s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
              s.Texture2D.MipLevels = 1;
              device->CreateShaderResourceView(clouds.depth_copy, &s, &clouds.depth_view);
              }
            if(device)
              device->Release();
            clouds.depth_width = d.Width;
            clouds.depth_height = d.Height;
            clouds.depth_format = family;
            log_line("clouds: depth {}x{} format {}, mips {}, {}", d.Width, d.Height, static_cast<int>(view.Format), d.MipLevels,
                     clouds.depth_view ? "copied" : "cannot be read");
            }
          if(clouds.depth_view)
            {
            ctx->CopyResource(clouds.depth_copy, texture);
            ok = true;
            }
          }
        texture->Release();
        }
      resource->Release();
      return ok;
      }

    ///\brief the volume in place of the layer, with the shaders and states of the shell put back after it
    auto draw_volume(
      ID3D11DeviceContext * ctx,
      settings_t const & live,
      cb_binding_t const & object,
      cb_binding_t const & frame,
      UINT index_count,
      UINT instance_count,
      UINT start_index,
      INT base_vertex,
      UINT start_instance
    ) noexcept -> void
      {
      ID3D11DepthStencilState * game_depth{};
      UINT game_stencil{};
      ID3D11ShaderResourceView * game_t0{};
      ID3D11ShaderResourceView * game_t2{};
      ctx->OMGetDepthStencilState(&game_depth, &game_stencil);
      ctx->PSGetShaderResources(0, 1, &game_t0);
      ctx->PSGetShaderResources(2, 1, &game_t2);

      ID3D11RenderTargetView * game_rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
      ID3D11DepthStencilView * game_dsv{};
      ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, game_rtv, &game_dsv);
      // the pixel shader's UAVs share the targets' slots: binding the half target over them would unbind them (none
      // while the clouds fit, own_draw_now; kept so the targets go back as they were whatever comes)
      UINT game_rtvs{};
      for(UINT i{}; i != D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if(game_rtv[i])
          game_rtvs = i + 1;
      ID3D11UnorderedAccessView * game_uav[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
      if(game_rtvs < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, game_rtvs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT - game_rtvs,
                                                       game_uav + game_rtvs);
      bool game_any_uav{};
      for(ID3D11UnorderedAccessView * u: game_uav)
        game_any_uav = game_any_uav or u;
      D3D11_VIEWPORT game_ports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
      UINT game_port_count{D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE};
      ctx->RSGetViewports(&game_port_count, game_ports);
      ID3D11Device * device{};
      ctx->GetDevice(&device);
      // at half size only over a picture of the depth's size: clouds_up.hlsl reads the depth at the picture's pixels
      auto const picture_size{view_size(game_rtv[0])}, depth_size{view_size(game_dsv)};
      if(picture_size != depth_size and not clouds.told_full_size)
        {
        clouds.told_full_size = true;
        log_line("clouds: at full size while the picture {}x{} and the depth {}x{} differ", picture_size.first, picture_size.second,
                 depth_size.first, depth_size.second);
        }
      bool const half{game_port_count != 0u and picture_size == depth_size and make_half(device, game_rtv[0])};
      if(device)
        device->Release();

      // the buffer from the settings, the planet as read and the frame (clouds_values.h, the render test's too)
      clouds_frame_t now{};
      double const seconds{static_cast<double>(GetTickCount64() - clouds.wind_start) / 1000.};
      now.seconds = static_cast<float>(seconds);
      now.flicker_s = seconds;
      now.pixel = half ? 2.f : 1.f;
      now.port_width = game_port_count != 0u ? game_ports[0].Width : 0.f;
      std::uint32_t const noise_time{live.clouds_noise_time};
      now.noise_shift = noise_time == 0u ? 0.f : clouds_noise_shift(noise_time, edupscale_jitter_frame(), ++clouds.draws_made);
      atmo_snow_values(live, now.snow.data());
      clouds_planet_t const planet{atmo_clouds_planet()};
      std::array<float, clouds_value_count> values{};
      atmo_report_changes(live, planet, clouds_values(start_fields(), live, planet, now, values));
      std::copy(values.begin(), values.end(), clouds.volume_values);
      ctx->UpdateSubresource(clouds.volume_params, 0, nullptr, clouds.volume_values, 0, 0);
      if(half)
        {
        float const clear[4]{};
        ctx->ClearRenderTargetView(clouds.half_rtv, clear);
        ctx->OMSetRenderTargets(1, &clouds.half_rtv, nullptr);
        D3D11_VIEWPORT port{game_ports[0]};
        port.TopLeftX *= 0.5f;
        port.TopLeftY *= 0.5f;
        port.Width *= 0.5f;
        port.Height *= 0.5f;
        ctx->RSSetViewports(1, &port);
        }
      orig_vs_set_shader(ctx, clouds.volume_vs, nullptr, 0);
      ctx->PSSetShader(clouds.volume_ps, nullptr, 0);
      ctx->VSSetConstantBuffers(4, 1, &clouds.volume_params);
      set_cb(ctx, cb_stage_e::pixel, 0, object);  // the game's VS b0/b1 with their windows
      set_cb(ctx, cb_stage_e::pixel, 1, frame);
      ctx->PSSetConstantBuffers(4, 1, &clouds.volume_params);
      ctx->PSSetShaderResources(0, 1, &clouds.depth_view);
      ctx->PSSetShaderResources(2, 1, &clouds.blue_noise);
      ctx->OMSetBlendState(half ? clouds.half_blend : clouds.volume_blend, nullptr, 0xffffffffu);
      ctx->RSSetState(clouds.volume_raster);
      ctx->OMSetDepthStencilState(clouds.volume_depth_state, 0);
      orig_draw_indexed_instanced(ctx, index_count, instance_count, start_index, base_vertex, start_instance);

      if(half)
        {
        // over the picture: one triangle from the vertex's number, nothing from the game's vertex buffers
        ID3D11InputLayout * game_layout{};
        D3D11_PRIMITIVE_TOPOLOGY game_topology{};
        ID3D11ShaderResourceView * game_t1{};
        ctx->IAGetInputLayout(&game_layout);
        ctx->IAGetPrimitiveTopology(&game_topology);
        ctx->PSGetShaderResources(1, 1, &game_t1);
        if(game_any_uav)
          ctx->OMSetRenderTargetsAndUnorderedAccessViews(game_rtvs, game_rtv, game_dsv, game_rtvs,
                                                         D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT - game_rtvs, game_uav + game_rtvs, nullptr);
        else
          ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, game_rtv, game_dsv);
        ctx->RSSetViewports(game_port_count, game_ports);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        orig_vs_set_shader(ctx, clouds.up_vs, nullptr, 0);
        ctx->PSSetShader(clouds.up_ps, nullptr, 0);
        ctx->PSSetShaderResources(1, 1, &clouds.half_srv);
        ctx->OMSetBlendState(clouds.volume_blend, nullptr, 0xffffffffu);
        orig_draw(ctx, 3, 0);
        ctx->PSSetShaderResources(1, 1, &game_t1);
        ctx->IASetInputLayout(game_layout);
        ctx->IASetPrimitiveTopology(game_topology);
        if(game_layout)
          game_layout->Release();
        if(game_t1)
          game_t1->Release();
        }
      for(ID3D11RenderTargetView * held: game_rtv)
        if(held)
          held->Release();
      for(ID3D11UnorderedAccessView * held: game_uav)
        if(held)
          held->Release();
      if(game_dsv)
        game_dsv->Release();

      ctx->OMSetDepthStencilState(game_depth, game_stencil);
      ctx->PSSetShaderResources(0, 1, &game_t0);
      ctx->PSSetShaderResources(2, 1, &game_t2);
      if(game_depth)
        game_depth->Release();
      if(game_t0)
        game_t0->Release();
      if(game_t2)
        game_t2->Release();
      }

    auto draw_clouds(
      ID3D11DeviceContext * ctx,
      settings_t const & live,
      UINT index_count,
      UINT instance_count,
      UINT start_index,
      INT base_vertex,
      UINT start_instance
    ) noexcept -> void
      {
      if(not make_clouds(ctx))
        return;
      // only where the clouds fit the game's targets (fits.h): elsewhere the game draws its shell as without edworld
      if(misfit_e const fit{misfit_of(own_draw_now(ctx))}; fit != misfit_e::none)
        {
        (void)drawn_or_told("clouds", clouds.misfits, fit);
        return;
        }
      ID3D11VertexShader * game_vs{};
      ID3D11PixelShader * game_ps{};
      ID3D11BlendState * game_blend{};
      float game_factor[4]{};
      UINT game_mask{};
      ID3D11RasterizerState * game_raster{};
      ctx->VSGetShader(&game_vs, nullptr, nullptr);
      ctx->PSGetShader(&game_ps, nullptr, nullptr);
      ctx->OMGetBlendState(&game_blend, game_factor, &game_mask);
      ctx->RSGetState(&game_raster);
      // the game's slots with their windows into the buffers (ID3D11DeviceContext1), put back as they were
      cb_binding_t object_b{get_cb(ctx, cb_stage_e::vertex, 0)};
      cb_binding_t frame_b{get_cb(ctx, cb_stage_e::vertex, 1)};
      cb_binding_t game_vs_b4{get_cb(ctx, cb_stage_e::vertex, 4)};
      cb_binding_t game_ps_b0{get_cb(ctx, cb_stage_e::pixel, 0)};
      cb_binding_t game_ps_b1{get_cb(ctx, cb_stage_e::pixel, 1)};
      cb_binding_t game_ps_b4{get_cb(ctx, cb_stage_e::pixel, 4)};
      // the game's buffers handed to the clouds' shaders only when their windows hold the rows those read
      bool const buffers_fit{cb_fits(object_b, clouds_object_rows) and cb_fits(frame_b, clouds_frame_rows)};
      if(object_b.buffer and frame_b.buffer)
        (void)drawn_or_told("clouds", clouds.misfits, buffers_fit ? misfit_e::none : misfit_e::buffers);
      ID3D11Buffer * const object{buffers_fit ? object_b.buffer : nullptr};
      ID3D11Buffer * const frame{buffers_fit ? frame_b.buffer : nullptr};

      if(not clouds.told)
        {
        clouds.told = true;
        ID3D11DepthStencilState * depth{};
        UINT stencil{};
        ctx->OMGetDepthStencilState(&depth, &stencil);
        D3D11_DEPTH_STENCIL_DESC d{};
        if(depth)
          depth->GetDesc(&d);
        D3D11_BLEND_DESC b{};
        if(game_blend)
          game_blend->GetDesc(&b);
        ID3D11RenderTargetView * rtv{};
        ID3D11DepthStencilView * dsv{};
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        log_line("clouds: the shell's depth {} write {} func {}, view {}, blend {} src {} dest {}, frame constants {}",
                 d.DepthEnable, d.DepthWriteMask, d.DepthFunc, dsv ? "bound" : "none", b.RenderTarget[0].BlendEnable,
                 b.RenderTarget[0].SrcBlend, b.RenderTarget[0].DestBlend, frame ? "bound" : "none");
        if(depth)
          depth->Release();
        if(rtv)
          rtv->Release();
        if(dsv)
          dsv->Release();
        }

      refresh_live_settings();
      // the game draws the shell into a small target too, for its reflections: the volume is for the picture itself
      D3D11_VIEWPORT port{};
      UINT ports{1u};
      ctx->RSGetViewports(&ports, &port);
      bool const volume_wanted{object and frame and start_fields().clouds_volume and port.Width >= 1024.f and make_volume(ctx)};
      clouds_timing_t::slot_t * timing{volume_wanted ? start_clouds_timing(ctx) : nullptr};
      bool const volume{volume_wanted and copy_depth(ctx)};
      if(volume)
        draw_volume(ctx, live, object_b, frame_b, index_count, instance_count, start_index, base_vertex, start_instance);
      stop_clouds_timing(ctx, timing);
      if(not volume and object and frame)
        {
        orig_vs_set_shader(ctx, clouds.vs, nullptr, 0);
        ctx->PSSetShader(clouds.ps, nullptr, 0);
        ctx->VSSetConstantBuffers(4, 1, &clouds.params);
        set_cb(ctx, cb_stage_e::pixel, 0, object_b);
        set_cb(ctx, cb_stage_e::pixel, 1, frame_b);
        ctx->PSSetConstantBuffers(4, 1, &clouds.params);
        ctx->OMSetBlendState(clouds.blend, nullptr, 0xffffffffu);
        ctx->RSSetState(clouds.raster);
        orig_draw_indexed_instanced(ctx, index_count, instance_count, start_index, base_vertex, start_instance);
        }
      if(object and frame)
        {
        orig_vs_set_shader(ctx, game_vs, nullptr, 0);
        ctx->PSSetShader(game_ps, nullptr, 0);
        set_cb(ctx, cb_stage_e::vertex, 4, game_vs_b4);
        set_cb(ctx, cb_stage_e::pixel, 0, game_ps_b0);
        set_cb(ctx, cb_stage_e::pixel, 1, game_ps_b1);
        set_cb(ctx, cb_stage_e::pixel, 4, game_ps_b4);
        ctx->OMSetBlendState(game_blend, game_factor, game_mask);
        ctx->RSSetState(game_raster);
        }

      for(IUnknown * held: std::initializer_list<IUnknown *>{game_vs, game_ps, game_blend, game_raster})
        if(held)
          held->Release();
      for(cb_binding_t * b: {&object_b, &frame_b, &game_ps_b0, &game_ps_b1, &game_vs_b4, &game_ps_b4})
        b->release();
      }

    auto guarded_clouds(
      ID3D11DeviceContext * ctx,
      settings_t const & live,
      UINT index_count,
      UINT instance_count,
      UINT start_index,
      INT base_vertex,
      UINT start_instance
    ) noexcept -> void
      {
      ++own_depth;
      hook_work(
        [&]
        {
          draw_clouds(ctx, live, index_count, instance_count, start_index, base_vertex, start_instance);
        },
        no_handler);
      --own_depth;
      }

    void STDMETHODCALLTYPE hook_draw_indexed(ID3D11DeviceContext * self, UINT index_count, UINT start_index, INT base_vertex)
      {
      if(own_depth != 0)
        return orig_draw_indexed(self, index_count, start_index, base_vertex);
      census_note(self);
      if(not hooks_off())
        guarded_frame_without_panels(self);
      if(full_armed.load(std::memory_order_relaxed))
        {
        if(self == immediate.load(std::memory_order_relaxed))
          guarded_full(self, 'i', index_count, 1, start_index, base_vertex, 0);
        else
          note_full_other(self);
        }
      bool const watched{watched_draw(self)};
      if(index_count == 6u and self == immediate.load(std::memory_order_relaxed) and holo_rt_count
         and not hooks_off())
        guarded_laying_over(self);
      if(self == immediate.load(std::memory_order_relaxed) and current_holo < max_vs_seen and not hooks_off())
        guarded_hologram(self, index_count, start_index, base_vertex, 0);
      if(watched)
        guarded_observe(self, index_count, 1, start_index, base_vertex, 0);
      else
        {
        if(self == immediate.load(std::memory_order_relaxed) and current_pending < max_vs_seen)
          guarded_classify(self);
        if(probe_request.load(std::memory_order_relaxed))
          guarded_probe(self, index_count, 1);
        }
#if defined(EDWORLD_EHT)
      if(self == immediate.load(std::memory_order_relaxed) and current_galmap and start_fields().galmap and not hooks_off())
        guarded_galmap(self, index_count, start_index, base_vertex);
#endif
      orig_draw_indexed(self, index_count, start_index, base_vertex);
      if(self == immediate.load(std::memory_order_relaxed) and full_after_draw)
        guarded_full_after(self);
      if(watched)
        guarded_after(self, index_count, start_index, base_vertex, 0);
      note_draw_end(self);
      }

    void STDMETHODCALLTYPE hook_draw(ID3D11DeviceContext * self, UINT vertex_count, UINT start_vertex)
      {
      if(own_depth != 0)
        return orig_draw(self, vertex_count, start_vertex);
      census_note(self);
      if(not hooks_off())
        guarded_frame_without_panels(self);
      if(full_armed.load(std::memory_order_relaxed))
        {
        if(self == immediate.load(std::memory_order_relaxed))
          guarded_full(self, 'd', vertex_count, 1, start_vertex, 0, 0);
        else
          note_full_other(self);
        }
      if(watched_draw(self))
        guarded_observe(self, vertex_count, 1, start_vertex, 0, 0);
      else
        {
        if(self == immediate.load(std::memory_order_relaxed) and current_pending < max_vs_seen)
          guarded_classify(self);
        if(probe_request.load(std::memory_order_relaxed))
          guarded_probe(self, vertex_count, 1);
        }
      orig_draw(self, vertex_count, start_vertex);
      if(self == immediate.load(std::memory_order_relaxed) and full_after_draw)
        guarded_full_after(self);
      guarded_build_info(self);
      note_draw_end(self);
      }

    void STDMETHODCALLTYPE hook_draw_indexed_instanced(
      ID3D11DeviceContext * self,
      UINT index_count,
      UINT instance_count,
      UINT start_index,
      INT base_vertex,
      UINT start_instance
    )
      {
      if(own_depth != 0)
        return orig_draw_indexed_instanced(self, index_count, instance_count, start_index, base_vertex, start_instance);
      census_note(self);
      if(not hooks_off())
        guarded_frame_without_panels(self);
      if(census_on.load(std::memory_order_relaxed) and self == immediate.load(std::memory_order_relaxed) and current_shell)
        census_add(census_shell);
      if(full_armed.load(std::memory_order_relaxed))
        {
        if(self == immediate.load(std::memory_order_relaxed))
          guarded_full(self, 'I', index_count, instance_count, start_index, base_vertex, start_instance);
        else
          note_full_other(self);
        }
      bool const watched{watched_draw(self)};
      if(index_count == 6u and self == immediate.load(std::memory_order_relaxed) and holo_rt_count
         and not hooks_off())
        guarded_laying_over(self);
      if(self == immediate.load(std::memory_order_relaxed) and current_holo < max_vs_seen and not hooks_off())
        guarded_hologram(self, index_count, start_index, base_vertex, start_instance);
      if(watched)
        guarded_observe(self, index_count, instance_count, start_index, base_vertex, start_instance);
      else
        {
        if(self == immediate.load(std::memory_order_relaxed) and current_pending < max_vs_seen)
          guarded_classify(self);
        if(probe_request.load(std::memory_order_relaxed))
          guarded_probe(self, index_count, instance_count);
        }
#if defined(EDWORLD_EHT)
      if(self == immediate.load(std::memory_order_relaxed) and current_galmap and start_fields().galmap and not hooks_off())
        guarded_galmap(self, index_count, start_index, base_vertex);
#endif
      orig_draw_indexed_instanced(self, index_count, instance_count, start_index, base_vertex, start_instance);
      // the immediate context first: a deferred one may draw on another thread, which reads neither current_shell
      // (the immediate context's) nor a live setting (the render thread's, ini/live.h). The live settings taken once
      // here and handed down to all that reads them below
      if(self == immediate.load(std::memory_order_relaxed))
        {
        settings_t const & live{settings()};
        if(current_shell and atmo_period_s(live) and not hooks_off())
          guarded_atmo(self, live);
        if(current_shell and start_fields().clouds and not hooks_off())
          guarded_clouds(self, live, index_count, instance_count, start_index, base_vertex, start_instance);
        }
      if(self == immediate.load(std::memory_order_relaxed) and full_after_draw)
        guarded_full_after(self);
      if(watched)
        guarded_after(self, index_count, start_index, base_vertex, start_instance);
      note_draw_end(self);
      }

    void STDMETHODCALLTYPE hook_draw_instanced(
      ID3D11DeviceContext * self,
      UINT vertex_count,
      UINT instance_count,
      UINT start_vertex,
      UINT start_instance
    )
      {
      if(own_depth != 0)
        return orig_draw_instanced(self, vertex_count, instance_count, start_vertex, start_instance);
      census_note(self);
      if(not hooks_off())
        guarded_frame_without_panels(self);
      if(full_armed.load(std::memory_order_relaxed))
        {
        if(self == immediate.load(std::memory_order_relaxed))
          guarded_full(self, 'D', vertex_count, instance_count, start_vertex, 0, start_instance);
        else
          note_full_other(self);
        }
      if(watched_draw(self))
        guarded_observe(self, vertex_count, instance_count, start_vertex, 0, start_instance);
      else
        {
        if(self == immediate.load(std::memory_order_relaxed) and current_pending < max_vs_seen)
          guarded_classify(self);
        if(probe_request.load(std::memory_order_relaxed))
          guarded_probe(self, vertex_count, instance_count);
        }
      orig_draw_instanced(self, vertex_count, instance_count, start_vertex, start_instance);
      if(self == immediate.load(std::memory_order_relaxed) and full_after_draw)
        guarded_full_after(self);
      guarded_build_info(self);
      note_draw_end(self);
      }

    void STDMETHODCALLTYPE hook_draw_indexed_instanced_indirect(ID3D11DeviceContext * self, ID3D11Buffer * args, UINT offset)
      {
      if(census_on.load(std::memory_order_relaxed))
        census_add(census_indirect);
      orig_draw_indexed_instanced_indirect(self, args, offset);
      }

    void STDMETHODCALLTYPE hook_draw_instanced_indirect(ID3D11DeviceContext * self, ID3D11Buffer * args, UINT offset)
      {
      if(census_on.load(std::memory_order_relaxed))
        census_add(census_indirect);
      orig_draw_instanced_indirect(self, args, offset);
      }

    void STDMETHODCALLTYPE hook_execute_command_list(ID3D11DeviceContext * self, ID3D11CommandList * list, BOOL restore)
      {
      if(census_on.load(std::memory_order_relaxed))
        census_add(census_lists);
      orig_execute_command_list(self, list, restore);
      }

    ///\brief "in place", or each draw slot of the context's table another module took over and that module's file name
    auto census_hooks(ID3D11DeviceContext * ctx, char * out, std::size_t size) noexcept -> void
      {
      struct
        {
        int slot;
        void * hook;
        char const * name;
        } const slots[]{
        {slot_draw_indexed, reinterpret_cast<void *>(&hook_draw_indexed), "DrawIndexed"},
        {slot_draw, reinterpret_cast<void *>(&hook_draw), "Draw"},
        {slot_draw_indexed_instanced, reinterpret_cast<void *>(&hook_draw_indexed_instanced), "DrawIndexedInstanced"},
        {slot_draw_instanced, reinterpret_cast<void *>(&hook_draw_instanced), "DrawInstanced"},
      };
      void * const * const table{*reinterpret_cast<void * const * const *>(ctx)};
      std::size_t used{};
      out[0] = '\0';
      for(auto const & s: slots)
        {
        if(table[s.slot] == s.hook or used + 1 >= size)
          continue;
        char module[MAX_PATH]{"unknown"};
        HMODULE owner{};
        if(GetModuleHandleExA(
             GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
             static_cast<char const *>(table[s.slot]),
             &owner
           ))
          {
          char path[MAX_PATH]{};
          if(GetModuleFileNameA(owner, path, MAX_PATH))
            {
            char const * const slash{std::strrchr(path, '\\')};
            format_into(module, "{}", slash ? slash + 1 : path);
            }
          }
        used += format_into(out + used, size - used, "{}{} taken by {}", used ? ", " : "", s.name, module);
        }
      if(used == 0)
        format_into(out, size, "in place");
      }

    auto census_report(char const * when, bool counts) noexcept -> void
      {
      std::uint32_t const attached{std::min(census_attached.load(), census_contexts)};
      if(counts)
        {
        char own[160]{};
        std::size_t used{};
        for(std::uint32_t i{}; i != attached and used + 1 < sizeof own; ++i)
          {
          used += format_into(own + used, sizeof own - used, "{}{}", i ? " + " : "", census_draws[i].load());
          }
        log_line(
          "census {}: draws {} on the devices' own contexts (in the order they came), {} on other contexts, {} indirect, "
          "{} command lists run; the atmosphere shell drawn {} times",
          when,
          own,
          census_draws[census_contexts].load(),
          census_indirect.load(),
          census_lists.load(),
          census_shell.load()
        );
        }
      for(std::uint32_t i{}; i != attached; ++i)
        if(ID3D11DeviceContext * const ctx{census_context[i].load()})
          {
          char hooks[1024];
          census_hooks(ctx, hooks, sizeof hooks);
          log_line("census {}: device {}'s draw hooks {}", when, i + 1, hooks);
          }
      if(ID3D11Multithread * const mt{multithread.load()})
        {
        bool const now{mt->GetMultithreadProtected() != FALSE};
        log_line("census {}: the multithread protection is {}{}", when, now ? "on" : "off",
                 now != multithread_at_start ? " (it changed since the start)" : " (as at the start)");
        }
      if(context_hook == context_hook_e::own_table)
        {
        std::size_t moved{};
        for(std::size_t i{}; i != own_watched; ++i)
          if(own_table.runtime[own_watch[i].slot] != own_watch[i].at_start)
            ++moved;
        log_line("census {}: the runtime re-pointed {} of edworld's {} context slots in its own table since the start{}",
                 when, moved, own_watched, moved ? "; followed through edworld's table" : "");
        }
      }

    auto census_body(void *) -> DWORD
      {
      Sleep(30000);
      census_report("after 30 s", true);
      Sleep(270000);
      census_report("after 5 min", true);
      census_on.store(false);
      return 0;
      }
    }  // namespace

  auto game_build_key() noexcept -> char const *
    {
    return holo_build();
    }

  auto hologram_view() noexcept -> hologram_view_t
    {
    LONGLONG const now{qpc_now()};
    holo_log(now);
    if(not hs.view.have or holo_ms(hs.view_at, now) >= 500)
      return hologram_view_t{};
    return hs.view;
    }

  auto attach_to_device(ID3D11Device * dev) noexcept -> void
    {
    // One device: the first the game gets. A second (the device made again after a driver reset, or another of the
    // game's) is not followed: edworld's objects, hooks and records belong to the first, and mixing two devices or two
    // context tables would hand one device's objects to the other's context. Never seen in 407 sessions of two
    // commanders (2026-11-18); the log says so if it comes.
    static std::atomic<ID3D11Device *> first_device{};
    ID3D11Device * expected{};
    if(not first_device.compare_exchange_strong(expected, dev))
      {
      if(expected != dev)
        log_line("attach: device {} is a second device of the game; not followed, edworld stays with the first ({})",
                 static_cast<void const *>(static_cast<void *>(dev)), static_cast<void const *>(static_cast<void *>(expected)));
      return;
      }
    ID3D11DeviceContext * ctx{};
    dev->GetImmediateContext(&ctx);
    if(not ctx)
      {
      log_line("attach: device {} has no immediate context", static_cast<void const *>(dev));
      return;
      }
    log_device(dev);
    bool const refused{chain_refusal()[0] != '\0'};
    if(refused)
      disabled.store(true);  // edworld_eht off for the session (proxy.cc): its hooks pass, the corner says why
    {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpc_frequency = f.QuadPart ? f.QuadPart : 1;
    }
    note_multithread(dev, ctx);
    log_context_table(ctx);
    context_hook = pick_context_hook(ctx);
    if(context_hook == context_hook_e::own_table and not prepare_own_table(ctx))
      context_hook = context_hook_e::in_place;
    bool ok{true};
    ok = ok and patch_slot(dev, slot_create_vertex_shader, reinterpret_cast<void *>(&hook_create_vs), reinterpret_cast<void **>(&orig_create_vs));
    ok = ok and hook_context(ctx, slot_vs_set_shader, reinterpret_cast<void *>(&hook_vs_set_shader), reinterpret_cast<void **>(&orig_vs_set_shader));
    ok = ok and hook_context(ctx, slot_draw_indexed, reinterpret_cast<void *>(&hook_draw_indexed), reinterpret_cast<void **>(&orig_draw_indexed));
    ok = ok and hook_context(ctx, slot_draw, reinterpret_cast<void *>(&hook_draw), reinterpret_cast<void **>(&orig_draw));
    ok = ok and hook_context(ctx, slot_draw_indexed_instanced, reinterpret_cast<void *>(&hook_draw_indexed_instanced), reinterpret_cast<void **>(&orig_draw_indexed_instanced));
    ok = ok and hook_context(ctx, slot_draw_instanced, reinterpret_cast<void *>(&hook_draw_instanced), reinterpret_cast<void **>(&orig_draw_instanced));
    ok = ok and hook_context(ctx, slot_draw_indexed_instanced_indirect, reinterpret_cast<void *>(&hook_draw_indexed_instanced_indirect), reinterpret_cast<void **>(&orig_draw_indexed_instanced_indirect));
    ok = ok and hook_context(ctx, slot_draw_instanced_indirect, reinterpret_cast<void *>(&hook_draw_instanced_indirect), reinterpret_cast<void **>(&orig_draw_instanced_indirect));
    ok = ok and hook_context(ctx, slot_execute_command_list, reinterpret_cast<void *>(&hook_execute_command_list), reinterpret_cast<void **>(&orig_execute_command_list));
    if(ok and context_hook == context_hook_e::own_table and *reinterpret_cast<void ***>(ctx) != own_table.entries)
      *reinterpret_cast<void ***>(ctx) = own_table.entries;  // from here every call of this context goes through edworld's table
    if(not ok)
      {
      log_line("attach: could not patch the device/context tables; observing nothing");
      ctx->Release();
      return;
      }
#if defined(EDWORLD_EHT)
    if(not refused)
      patch_catalog(dev);
#endif

    ID3D11DeviceContext1 * ctx1{};
    if(FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&ctx1))))
      ctx1 = nullptr;

    for(slot_t & s: ring)
      {
      if constexpr(not with_eht)
        break;
      s.filled = false;
      s.count = 0;
      D3D11_BUFFER_DESC d{};
      d.ByteWidth = max_panels * record_bytes;
      d.Usage = D3D11_USAGE_STAGING;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      s.staging = nullptr;
      if(FAILED(dev->CreateBuffer(&d, nullptr, &s.staging)))
        s.staging = nullptr;
      d.ByteWidth = side_bytes;
      s.side = nullptr;
      if(FAILED(dev->CreateBuffer(&d, nullptr, &s.side)))
        s.side = nullptr;
      s.pool = nullptr;
      s.pool_capacity = 0;
      }
    recording = -1;
    device = dev;
    attach_qpc = qpc_now();
    // the gaps are measured from here: zero would make the first draw's gap the machine's uptime, which overflows
    // when turned to microseconds after some 10 days
    last_draw_qpc = attach_qpc;
    last_any_draw_qpc = attach_qpc;
    immediate1 = ctx1;
    immediate.store(ctx);  // the latest device wins; its context stays referenced for the process lifetime
    if(refused)
      {
      // no census, share, game state or probe: only the corner's line, through the draw hooks
      log_line("attach: device {} context {}: edworld_eht off for this session, only its corner's line drawn",
               static_cast<void const *>(dev), static_cast<void const *>(ctx));
      return;
      }
    if(std::uint32_t const order{census_attached.fetch_add(1)}; order < census_contexts)
      {
      census_context[order].store(ctx);
      if(order == 0)
        {
        census_on.store(true);
        auto const census_thread{[](LPVOID) -> DWORD { return run_guarded_thread("census", &census_body, nullptr, 1); }};
        if(HANDLE const t{CreateThread(nullptr, 0, census_thread, nullptr, 0, nullptr)})
          CloseHandle(t);
        }
      else
        {
        ID3D11DeviceContext * const first{census_context[0].load()};
        log_line(
          "attach: device {}'s context {} the first device's table",
          order + 1,
          *reinterpret_cast<void **>(ctx) == *reinterpret_cast<void **>(first) ? "shares" : "does not share");
        }
      }
#if defined(EDWORLD_EHT)
    if(not share)
      open_share();
#endif
    if(start_fields().compass)
      start_game_state();  // the compass's checking lines read the flight from Status.json
    if(start_fields().patch)
      {
      start_game_state();
      static std::atomic<bool> probe_started{};
      auto const probe_thread{[](LPVOID) -> DWORD { return run_guarded_thread("probe", &probe_body, nullptr); }};
      if(not probe_started.exchange(true))
        if(HANDLE const t{CreateThread(nullptr, 0, probe_thread, nullptr, 0, nullptr)})
          CloseHandle(t);
      }
    if(start_fields().patch and not start_fields().edsm)
      log_line("{}", with_eht ? "patch: on, edsm = 0 - allegiance from EHT's target only"
                        : "patch: on, but edsm = 0 - no allegiance, only the test frame can be drawn");
    std::uint32_t const n{watched_count.load()};
    log_line(
      "attach: device {} context {} (context1 {}), {} watched hash(es), {} already created",
      static_cast<void const *>(dev),
      static_cast<void const *>(ctx),
      ctx1 ? "yes" : "no",
      start_fields().watch_vs.size(),
      n
    );
    }
  }  // namespace edworld
