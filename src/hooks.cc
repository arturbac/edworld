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
#include "edworld_share.h"
#include "game_state.h"
#include "hologram.h"
#include "holo_models.h"
#include "panel_math.h"
#include "panel_patch.h"
#include "runtime.h"
#include "shader_catalog.h"

#include <d3d11_1.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <string>
#include <vector>

namespace edworld
  {
  namespace
    {
    // ---- vtable slots (the D3D11 ABI; IUnknown 0..2, ID3D11DeviceChild 3..6) ----
    constexpr int slot_create_vertex_shader{12};
    constexpr int slot_vs_set_shader{11};
    constexpr int slot_draw_indexed{12};
    constexpr int slot_draw{13};
    constexpr int slot_draw_indexed_instanced{20};
    constexpr int slot_draw_instanced{21};
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

    create_vs_fn orig_create_vs{};
    vs_set_shader_fn orig_vs_set_shader{};
    draw_indexed_fn orig_draw_indexed{};
    draw_fn orig_draw{};
    draw_indexed_instanced_fn orig_draw_indexed_instanced{};
    draw_instanced_fn orig_draw_instanced{};

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
    // the bytecode of the shaders in settings().dump_vs, kept from creation for a whole-frame dump (a slot counts once
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
    std::atomic<std::uint32_t> table_slot[slot_table_size]{};
    std::atomic<verdict_e> verdict[max_vs_seen]{};
    std::uint8_t verdict_looked[max_vs_seen]{};  // render thread only

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
          table_slot[i].store(slot, std::memory_order_release);  // a reused pointer takes its new shader's slot
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
          return table_slot[i].load(std::memory_order_acquire);
        if(not k)
          break;
        }
      return max_vs_seen;
      }

    // ---- the probe: a jump charges but no panel draw comes; log what is drawn instead ----
    std::atomic<std::uint64_t> last_watched_ms{};  // GetTickCount64 of the latest panel draw
    std::atomic<bool> probe_request{};             // set by the probe thread, cleared by the render thread
    std::atomic<std::uint32_t> probe_draws{};

    // ---- render-thread state ----
    std::atomic<ID3D11DeviceContext *> immediate{};
    ID3D11Device * device{};
    ID3D11DeviceContext1 * immediate1{};
    int current_watched{-1};
    std::uint32_t current_pending{max_vs_seen};  // the bound shader's seen slot while its verdict is pending
    std::atomic<bool> disabled{false};
    std::uint32_t faults{};

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
      ID3D11Buffer * pool{};  ///< the whole t33 record pool of the frame
      std::uint32_t pool_capacity{};
      std::uint32_t pool_bytes{};
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
    LONGLONG qpc_frequency{1};

    // ---- statistics for the log ----
    std::uint64_t stat_draws{}, stat_published{}, stat_dropped{}, stat_full{};
    LONGLONG last_log_qpc{};

    // ---- the published file ----
    share_t * share{};

    struct surface_cache_t
      {
      void * resource;
      std::uint32_t width, height, format;
      };

    surface_cache_t surfaces[64]{};
    std::uint32_t surface_next{};

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

    // ---- the published record (edworld_eht) ----
#if defined(EDWORLD_EHT)
    auto open_share() noexcept -> void
      {
      std::wstring const & dir{settings().shm_dir};
      if(dir.empty())
        return;
      CreateDirectoryW(dir.c_str(), nullptr);
      std::wstring const path{dir + L"\\panels"};
      HANDLE const file{CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      )};
      if(file == INVALID_HANDLE_VALUE)
        {
        log_line("share: cannot open %S (error %lu); not publishing", path.c_str(), GetLastError());
        return;
        }
      HANDLE const mapping{CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, sizeof(share_t), nullptr)};
      CloseHandle(file);
      if(not mapping)
        {
        log_line("share: cannot map %S (error %lu); not publishing", path.c_str(), GetLastError());
        return;
        }
      share = static_cast<share_t *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(share_t)));
      CloseHandle(mapping);
      if(not share)
        {
        log_line("share: cannot view %S (error %lu); not publishing", path.c_str(), GetLastError());
        return;
        }
      std::memset(share, 0, sizeof(share_t));
      share->magic = share_magic;
      share->version = share_version;
      share->size = sizeof(share_t);
      share->writer_pid = GetCurrentProcessId();
      log_line("share: publishing to %S (%zu bytes)", path.c_str(), sizeof(share_t));
      }
#endif

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
      std::uint64_t const at{(static_cast<std::uint64_t>(slot.pool_first) + index) * slot.pool_stride};
      if(at + 28u > slot.pool_bytes)
        return false;
      out = decode_record(m.pool + at);
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
        "frame %llu: %u panel draw(s); totals draws %llu published %llu dropped %llu full %llu faults %u vs %llu pool %u",
        static_cast<unsigned long long>(slot.frame),
        slot.count,
        static_cast<unsigned long long>(stat_draws),
        static_cast<unsigned long long>(stat_published),
        static_cast<unsigned long long>(stat_dropped),
        static_cast<unsigned long long>(stat_full),
        faults,
        static_cast<unsigned long long>(vs_created.load()),
        m.pool ? slot.pool_bytes : 0u
      );
      for(std::uint32_t i{}; i != slot.count; ++i)
        {
        panel_t p{};
        describe(slot, m, i, p);
        auto const ndc{clip_to_ndc(p.anchor_clip)};
        log_line(
          "  #%u vs%u surf %08llx %ux%u n%u inst%u rec%s%u pos %.3f %.3f %.3f s%.3f anchor %s %.4f %.4f w %.4f",
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
          if(settings().log_interval_ms
             and (now - last_log_qpc) * 1000 / qpc_frequency >= settings().log_interval_ms)
            {
            last_log_qpc = now;
            log_summary(*oldest, m);
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
    std::atomic<bool> writer_started{};
    std::atomic<std::uint32_t> writes_queued{}, writes_done{};

    DWORD WINAPI writer_thread(LPVOID)
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
              log_line("dump: %S without its context (%s)", item->path, e.what());
              }
          if(item->context and context.empty())
            {
            // logged above: nothing to write
            }
          else if(std::FILE * f{_wfopen(item->path, L"wb")})
            {
            if(item->context)
              std::fwrite(context.data(), 1, context.size(), f);
            else
              std::fwrite(reinterpret_cast<char const *>(item + 1), 1, item->bytes, f);
            std::fclose(f);
            if(item->rename_to[0] and not MoveFileExW(item->path, item->rename_to, MOVEFILE_REPLACE_EXISTING))
              log_line("dump: %S could not replace %S (error %lu)", item->path, item->rename_to, GetLastError());
            else if(item->loud)
              log_line("dump: wrote %S", item->path);
            }
          else
            log_line("dump: %S cannot be written", item->path);
          _aligned_free(item);
          writes_done.fetch_add(1, std::memory_order_release);
          }
        }
      }

    ///\brief a file of these bytes, written later by the writer thread; false when it cannot be queued (logged)
    auto queue_write(wchar_t const * path, void const * data, std::uint32_t bytes, bool loud = false,
                     wchar_t const * rename_to = nullptr, bool context = false) noexcept -> bool
      {
      if(not writer_started.exchange(true))
        {
        InitializeSListHead(&write_queue);
        write_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if(HANDLE const t{CreateThread(nullptr, 0, &writer_thread, nullptr, 0, nullptr)})
          CloseHandle(t);
        else
          log_line("dump: the writer thread could not start (error %lu)", GetLastError());
        }
      auto * item{static_cast<write_item_t *>(_aligned_malloc(sizeof(write_item_t) + bytes, MEMORY_ALLOCATION_ALIGNMENT))};
      if(not item)
        {
        log_line("dump: %S not written (no memory for %u bytes)", path, bytes);
        return false;
        }
      std::memset(item, 0, sizeof(write_item_t));
      std::swprintf(item->path, MAX_PATH, L"%ls", path);
      if(rename_to)
        std::swprintf(item->rename_to, MAX_PATH, L"%ls", rename_to);
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

    auto trigger_path() -> std::wstring { return settings().dir + L"\\edworld_dump"; }

    auto check_trigger(LONGLONG now) noexcept -> void
      {
      if(dump_armed)
        {
        if(frame > dump_frame)  // one frame of panel draws asked; done
          {
          dump_armed = false;
          DeleteFileW(trigger_path().c_str());
          log_line("dump: %u surface(s) queued", dumped_count);
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
      std::snprintf(dump_stamp, sizeof dump_stamp, "%04u%02u%02uT%02u%02u%02u_%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour,
                    t.wMinute, t.wSecond, t.wMilliseconds);
      CreateDirectoryW((settings().dir + L"\\edworld_dumps").c_str(), nullptr);
      log_line("dump: armed (%s)", dump_stamp);
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
        log_line("dump: surface %08llx %ux%u skipped (samples %u, array %u)", static_cast<unsigned long long>(id & 0xffffffffull),
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
            std::swprintf(name, MAX_PATH, L"%ls\\edworld_dumps\\%hs_a%u_rt_after_%ux%u_f%u_pitch%u.raw", settings().dir.c_str(),
                          dump_stamp, p.checkpoint - 1u, p.width, p.height, p.format, m.RowPitch);
          else if(p.checkpoint and p.ps_slot)
            std::swprintf(name, MAX_PATH, L"%ls\\edworld_dumps\\%hs_a%u_ps%u_%ux%u_f%u_pitch%u.raw", settings().dir.c_str(),
                          dump_stamp, p.checkpoint - 1u, p.ps_slot - 1u, p.width, p.height, p.format, m.RowPitch);
          else if(p.checkpoint)
            std::swprintf(name, MAX_PATH, L"%ls\\edworld_dumps\\%hs_c%04u_%ux%u_f%u_pitch%u.raw", settings().dir.c_str(),
                          dump_stamp, p.checkpoint - 1u, p.width, p.height, p.format, m.RowPitch);
          else
            std::swprintf(name, MAX_PATH, L"%ls\\edworld_dumps\\%hs_%08llx_%ux%u_f%u_pitch%u.raw", settings().dir.c_str(),
                          dump_stamp, static_cast<unsigned long long>(p.id & 0xffffffffull), p.width, p.height, p.format,
                          m.RowPitch);
          queue_write(name, m.pData, m.RowPitch * p.height, true);
          immediate.load()->Unmap(p.staging, 0);
          }
        else
          log_line("dump: surface %08llx lost (hr 0x%08lX)", static_cast<unsigned long long>(p.id & 0xffffffffull),
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
          std::swprintf(name, MAX_PATH, L"%ls\\edworld_dumps\\%hs_%hs.bin", settings().dir.c_str(), dump_stamp, p.name);
          queue_write(name, m.pData, p.bytes);
          immediate.load()->Unmap(p.staging, 0);
          }
        else
          log_line("dump: buffer %s lost (hr 0x%08lX)", p.name, static_cast<unsigned long>(hr));
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
        log_line("dump: %s starts past its buffer (%llu of %u)", name, static_cast<unsigned long long>(from), bd.ByteWidth);
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
      std::snprintf(p.name, sizeof p.name, "%s", name);
      p.qpc = qpc_now();
      if(loud)
        log_line("dump: %s = %u byte(s) from %llu of a buffer of %u (stride %u, bind 0x%x, misc 0x%x)", name, p.bytes,
               static_cast<unsigned long long>(from), bd.ByteWidth, bd.StructureByteStride, bd.BindFlags, bd.MiscFlags);
      }

    // ---- the whole frame (discovery, full_dump_request): every draw of one frame on the immediate context, one log
    // line each (its kind, the vertex shader's hash, counts, viewport, render target) and the first full_cb_bytes of
    // its VS constant buffers 0..3 as edworld_dumps/<stamp>_a<draw>_cb<slot>.bin; the surfaces of that frame too.
    // Made to find what is not a panel - the target's hologram, the camera, the light - by what changes with it.
    // 12000: at a carrier (2026-10-22) the frame held more than 4000 draws before the holograms' (drawn near its end)
    constexpr std::uint32_t full_max_draws{12000};
    constexpr std::uint32_t full_cb_bytes{1024};
    bool full_armed{};
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
        log_line("dump: checkpoint before a%u not made (no staging %ux%u f%u)", a, d.Width, d.Height, static_cast<unsigned>(d.Format));
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

    ///rief a draw of another context than the immediate one while a whole frame is armed: counted, not recorded
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

    auto full_line(char const * fmt, ...) noexcept -> void
      {
      if(full_text_bytes - full_text_used < 2u)
        return;
      va_list args;
      va_start(args, fmt);
      int const n{std::vsnprintf(full_text + full_text_used, full_text_bytes - full_text_used - 1u, fmt, args)};
      va_end(args);
      if(n < 0)
        return;
      full_text_used = std::min<std::uint32_t>(full_text_used + static_cast<std::uint32_t>(n), full_text_bytes - 2u);
      full_text[full_text_used++] = '\n';
      }

    auto in_dump_vs(std::uint64_t hash) noexcept -> bool
      {
      for(std::uint64_t const h: settings().dump_vs)
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
      std::swprintf(name, MAX_PATH, L"%ls\\edworld_dumps\\%hs_draws.txt", settings().dir.c_str(), dump_stamp);
      queue_write(name, full_text, full_text_used, true);
#if defined(EDWORLD_EHT)
      if(not settings().shm_dir.empty())
        {
        char text[64];
        int const n{std::snprintf(text, sizeof text, "%s %u\n", dump_stamp, full_done_draws)};
        std::wstring const done{settings().shm_dir + L"\\dump_done"};
        queue_write((done + L".part").c_str(), text, static_cast<std::uint32_t>(n), false, done.c_str());
        }
#endif
      log_line("dump: whole frame's files queued (%u written of %u so far)", writes_done.load(), writes_queued.load());
      }

    auto full_trigger_path() -> std::wstring { return settings().dir + L"\\edworld_dump_all"; }

    auto check_full(LONGLONG now) noexcept -> void
      {
      if(full_armed)
        {
        if(frame > full_frame)
          {
          full_armed = false;
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
          log_line("dump: whole frame %llu, %u draw(s) recorded", static_cast<unsigned long long>(full_frame), full_draws);
          std::uint32_t contexts{};
          for(std::atomic<ID3D11DeviceContext *> const & slot: full_other)
            contexts += slot.load(std::memory_order_relaxed) ? 1u : 0u;
          log_line("dump: the frame ended %.2f ms after arming, at a panel draw after a gap of %lld us (frame_gap_us %u); "
                   "draws of other contexts meanwhile %u in %u context(s)%s (not recorded)",
                   static_cast<double>(now - full_armed_qpc) * 1000.0 / static_cast<double>(qpc_frequency),
                   frame_gap_qpc * 1000000 / qpc_frequency, settings().frame_gap_us, full_other_draws.load(), contexts,
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
      // the surfaces of the same frame, as edworld_dump makes them (unless that dump is armed already)
      if(not dump_armed)
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
      full_armed = true;
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
      std::snprintf(dump_stamp, sizeof dump_stamp, "%04u%02u%02uT%02u%02u%02u_%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour,
                    t.wMinute, t.wSecond, t.wMilliseconds);
      CreateDirectoryW((settings().dir + L"\\edworld_dumps").c_str(), nullptr);
      log_line("dump: whole frame armed (%s)", dump_stamp);
      }

    auto holo_end_laying_over() noexcept -> void;

    auto begin_frame(LONGLONG now) noexcept -> void
      {
      if(recording >= 0 and ring[recording].count)
        ring[recording].filled = true;
      recording = -1;
      ++frame;
      holo_end_laying_over();
      if constexpr(not with_eht)
        {
        if(settings().log_interval_ms and (now - last_log_qpc) * 1000 / qpc_frequency >= settings().log_interval_ms)
          {
          last_log_qpc = now;
          log_line("frame %llu: totals draws %llu faults %u vs %llu", static_cast<unsigned long long>(frame),
                   static_cast<unsigned long long>(stat_draws), faults, static_cast<unsigned long long>(vs_created.load()));
          }
        }
      drain(now);
      if(pending_count)
        write_dumps(now);
      if(pending_buffer_count)
        write_buffers(now);
      check_trigger(now);
      check_full(now);
      if(full_done_waiting and not full_armed and pending_count == 0 and pending_buffer_count == 0)
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
      void * const key{res};
      for(surface_cache_t const & c: surfaces)
        if(c.resource == key)
          {
          out = c;
          res->Release();
          return true;
          }
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
      surfaces[surface_next++ % 64] = out;
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
      log_line("dump: g%u draw: indices %u from %u, base vertex %d, instances %u from %u, surface %08llx %ux%u", g,
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
        std::snprintf(name, sizeof name, "g%u_ib_u%u", g, size * 8u);
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
        std::snprintf(name, sizeof name, "g%u_vb%u_s%u", g, v, strides[v]);
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
        std::snprintf(name, sizeof name, "g%u_cb%u", g, c);
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
          std::snprintf(name, sizeof name, "g%u_t33", g);
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
      D3D11_BOX const box{0, 0, 0, bd.ByteWidth, 1, 1};
      ctx->CopySubresourceRegion(slot.pool, 0, 0, 0, 0, buf, 0, &box);
      buf->Release();
      slot.pool_bytes = bd.ByteWidth;
      slot.pool_first = first_element;
      slot.pool_stride = bd.StructureByteStride ? bd.StructureByteStride : 336u;
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
    ) noexcept -> void
      {
      LONGLONG const now{qpc_now()};
      if((now - last_draw_qpc) * 1000000 / qpc_frequency > settings().frame_gap_us)
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
      if(settings().compass)
        panel_compass(ctx, device, frame);
      if(settings().pad_text_samples or settings().pad_text_read)
        panel_pad_text(ctx, device, frame);
      if(watched_found[current_watched])
        {
        // found in the game: its draws carry the panel surface, but whether its records decode like the family's
        // is not known, so it gets the patch and publishes nothing
        if(settings().patch)
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
        if(settings().patch)
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

      if(settings().patch)
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
      __try
        {
        observe_panel(ctx, index_count, instance_count, start_index, base_vertex, start_instance);
        }
      __except(EXCEPTION_EXECUTE_HANDLER)
        {
        if(++faults >= 8)
          disabled.store(true);
        }
      }

    // the list under the jump panel, right after the game's draw of it; SEH as around the observation
    auto guarded_after(
      ID3D11DeviceContext * ctx, std::uint32_t index_count, std::uint32_t start_index, std::int32_t base_vertex, std::uint32_t start_instance
    ) noexcept -> void
      {
      bool const list{settings().patch and settings().list};
      bool const spheres{settings().compass and settings().compass_spheres};
      if(not list and not spheres)
        return;
      __try
        {
        if(list)
          panel_list_after(ctx, frame, index_count, start_index, base_vertex, start_instance);
        if(spheres)
          panel_compass_after(ctx, frame, index_count, start_index, base_vertex, start_instance);
        }
      __except(EXCEPTION_EXECUTE_HANDLER)
        {
        if(++faults >= 8)
          disabled.store(true);
        }
      }

    auto watched_draw(ID3D11DeviceContext * ctx) noexcept -> bool
      {
      return current_watched >= 0 and ctx == immediate.load(std::memory_order_relaxed)
             and not disabled.load(std::memory_order_relaxed);
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
      bool const ok{watch(vs, static_cast<std::uint32_t>(settings().watch_vs.size()), true)};
      log_line("vs %016llX (%p) draws a panel surface (PS t%u %ux%u) and is not in watch_vs: %s (%s)",
               static_cast<unsigned long long>(hash), vs, slot, w, h, ok ? "watched from now on" : "watched list full", how);
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
      __try
        {
        ID3D11VertexShader * vs{};
        ctx->VSGetShader(&vs, nullptr, nullptr);
        if(vs)
          {
          classify(ctx, vs, current_pending);
          vs->Release();
          }
        }
      __except(EXCEPTION_EXECUTE_HANDLER)
        {
        current_pending = max_vs_seen;
        if(++faults >= 8)
          disabled.store(true);
        }
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

    // the sizes of the panels' interface surfaces seen so far (measured in the game, 2026-10-04)
    auto panel_sized(std::uint32_t w, std::uint32_t h) noexcept -> bool
      {
      return (w == 3072 and h == 660) or (w == 2048 and h == 1280) or (w == 2200 and h == 1800) or (w == 1024 and h == 1534);
      }

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
        log_line("probe: a jump charges, no panel draw for over 1 s; looking at the draws for %u ms", probe_window_ms);
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
              log_line("probe: draw %u ctx %s vs %016llX (%p) PS t%u %ux%u surf %08llx count %u inst %u", n,
                       ctx == immediate.load(std::memory_order_relaxed) ? "immediate" : "other",
                       static_cast<unsigned long long>(vs_hash_of(vs)), static_cast<void *>(vs), slot, d.Width, d.Height,
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
        log_line("probe: done, %u draw(s) seen, %u with a panel-sized surface at PS", n + 1, probe_surface_draws);
        probe_request.store(false, std::memory_order_relaxed);
        }
      }

    auto guarded_probe(ID3D11DeviceContext * ctx, std::uint32_t count, std::uint32_t instances) noexcept -> void
      {
      __try
        {
        probe_draw(ctx, count, instances);
        }
      __except(EXCEPTION_EXECUTE_HANDLER)
        {
        probe_request.store(false);
        if(++faults >= 8)
          disabled.store(true);
        }
      }

    // ---- the probe (its own thread): arms it once per charge that sees no panel draw ----
    DWORD WINAPI probe_thread(LPVOID)
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
      FILE * const file{_wfopen(name.c_str(), L"rb")};
      if(not file)
        {
        log_line("hologram: no VersionInfo.txt beside the exe");
        return;
        }
      char text[512]{};
      std::size_t const n{std::fread(text, 1, sizeof text - 1, file)};
      std::fclose(file);
      text[n] = '\0';
      char const * const at{std::strstr(text, "\"Version\":\"")};
      char version[64]{};
      if(at)
        std::sscanf(at + 11, "%63[^\"]", version);
      log_line("hologram: VersionInfo.txt beside the exe names version %s", version[0] ? version : "(none read)");
      }

    auto holo_build() noexcept -> char const *
      {
      static char key[48]{};
      if(key[0])
        return key;
      wchar_t path[MAX_PATH]{};
      WIN32_FILE_ATTRIBUTE_DATA a{};
      if(GetModuleFileNameW(nullptr, path, MAX_PATH) and GetFileAttributesExW(path, GetFileExInfoStandard, &a))
        std::snprintf(key, sizeof key, "%lu-%08lx%08lx", a.nFileSizeLow, a.ftLastWriteTime.dwHighDateTime,
                      a.ftLastWriteTime.dwLowDateTime);
      else
        std::snprintf(key, sizeof key, "unknown");
      log_line("hologram: the game's build %s (the exe's size and last write)", key);
      holo_log_version(path);
      return key;
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
        log_line("hologram: %u indices on the %s surface: %s (%s, %s)", count, holo_on_name(on),
                 holo_models[s.r.model].name, holo_how(s.r.how),
                 holo_models[s.r.model].status == holo_status_e::confirmed ? "confirmed" : "measured");
      else
        {
        holo_model_t const * other{};
        for(holo_model_t const & m: holo_models)
          if(m.count == count)
            other = &m;
        if(other)
          log_line("hologram: %u indices on the %s surface: %s's count on build %s, not this one - unknown until its "
                   "fingerprint says", count, holo_on_name(on), other->name, other->build);
        else
          log_line("hologram: %u indices on the %s surface: not in the registry", count, holo_on_name(on));
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
            std::snprintf(holo_sample.stamp, sizeof holo_sample.stamp, "%04u%02u%02uT%02u%02u%02u_%03uZ", t.wYear, t.wMonth, t.wDay,
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
        log_line("hologram: %u indices not sampled (%s)", count, why);
      }

    ///\brief the fingerprint of a sample read back; false (logged) when its indices lie past the vertices read
    auto holo_sample_fingerprint(std::uint8_t const * indices, std::uint8_t const * vertices, holo_fingerprint_t & f) noexcept -> bool
      {
      try
        {
        std::vector<vec3_t> used;
        if(not holo_used_vertices(indices, holo_sample.index_size, holo_sample.count, vertices, holo_sample.vb_bytes, used))
          {
          log_line("hologram: %u indices not fingerprinted (an index past the %u vertices read)", holo_sample.count,
                   holo_sample.vb_bytes / vertex_stride);
          return false;
          }
        f = holo_fingerprint(used.data(), used.size(), holo_sample.count / 3u);
        return true;
        }
      catch(std::exception const & e)
        {
        log_line("hologram: %u indices not fingerprinted (%s)", holo_sample.count, e.what());
        return false;
        }
      }

    auto holo_log_fingerprint(std::uint32_t count, holo_fingerprint_t const & f) noexcept -> void
      {
      // as an entry's fingerprint in holo_models.h
      char extreme[6 * 3 * 12]{};
      int at{};
      for(vec3_t const & v: f.extreme)
        at += std::snprintf(extreme + at, sizeof extreme - static_cast<std::size_t>(at), "%s{%.4ff, %.4ff, %.4ff}", at ? ", " : "",
                            static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z));
      log_line("hologram model fingerprint %u: {%uu, %uu, {%.4ff, %.4ff, %.4ff}, {{%s}}, {{0x%016llxull, 0x%016llxull, 0x%016llxull, "
               "0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull}}}", count, f.vertices, f.triangles,
               static_cast<double>(f.extent[0]), static_cast<double>(f.extent[1]), static_cast<double>(f.extent[2]), extreme,
               f.grid[0], f.grid[1], f.grid[2], f.grid[3], f.grid[4], f.grid[5], f.grid[6], f.grid[7]);
      }

    auto holo_models_dir() -> std::wstring { return settings().dir + L"\\edworld_models"; }

    ///\brief the sample's copies once the GPU has made them: fingerprint, resolve again, log; edworld_eht writes them too
    auto holo_sample_read(ID3D11DeviceContext * ctx, LONGLONG now) noexcept -> void
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
        log_line("hologram: the sample of %u indices lost (hr 0x%08lX)", holo_sample.count, static_cast<unsigned long>(hr));
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
        log_line("hologram model sample: %u indices (%s surface), %u vertices, %u triangles, build %s: %s %s (likeness %.3f, "
                 "next model %.3f, turn %d)", holo_sample.count, holo_on_name(holo_sample.on), f.vertices, f.triangles,
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
        std::swprintf(name, MAX_PATH, L"%ls\\%hs_%hs_%u_ib_u%u.bin", holo_models_dir().c_str(), holo_build(), holo_sample.stamp,
                      holo_sample.count, holo_sample.index_size * 8u);
        queue_write(name, indices, holo_sample.ib_bytes);
        std::swprintf(name, MAX_PATH, L"%ls\\%hs_%hs_%u_vb_s40.bin", holo_models_dir().c_str(), holo_build(), holo_sample.stamp,
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
          top = std::max(top, at);
          }
        queue_write(name, vertices, std::min(holo_sample.vb_bytes, (top + 1u) * vertex_stride));
        // the game's files at the sample (Status.json, the journal's place), read by the writer thread
        char sample[96];
        int const length{std::snprintf(sample, sizeof sample, "%s_%s_%u", holo_build(), holo_sample.stamp, holo_sample.count)};
        std::swprintf(name, MAX_PATH, L"%ls\\%hs_context.json", holo_models_dir().c_str(), sample);
        if(length > 0)
          queue_write(name, sample, static_cast<std::uint32_t>(std::min<int>(length, sizeof sample - 1)), false, nullptr, true);
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
        log_line("hologram: the target's surface of %u indices not copied (format %u, samples %u)", holo_sample.count,
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
        log_line("hologram: no picture of %u indices (its surface was not laid over within %lld ms)", holo_sample.count,
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
          std::swprintf(name, MAX_PATH, L"%ls\\%hs_%hs_%u_holo.pgm", holo_models_dir().c_str(), holo_build(), holo_sample.stamp,
                        holo_sample.count);
          queue_write(name, picture, sizeof picture, true);
          }
        else
          log_line("hologram: the target's surface of %u indices not pictured (format %u, %ux%u)", holo_sample.count,
                   static_cast<unsigned>(d.Format), d.Width, d.Height);
        ctx->Unmap(holo_sample.surface, 0);
        }
      else
        log_line("hologram: the target's surface of %u indices lost (hr 0x%08lX)", holo_sample.count, static_cast<unsigned long>(hr));
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
      log_line("hologram: a hologram's surface %p (%u known)", surface, holo_rt_count);
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
      __try
        {
        holo_laying_over(ctx);
        }
      __except(EXCEPTION_EXECUTE_HANDLER)
        {
        if(++faults >= 8)
          disabled.store(true);
        }
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
          log_line("hologram: the target's surface %p (the HUD lays it over second)", target);
        else
          log_line("hologram: no target's surface (the HUD lays over %s)", holo_rt_count ? "one hologram" : "none");
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
            log_line("hologram: the target's model by its kind: %u indices on the own surface (the HUD's order swapped)",
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
            hs.view = hologram_view_t{true, axes, s.count[p], holo_table_count(r, holo_models.data()), ship,
                                      holo_place_of(r, holo_models.data()), holo_flat_of(r, holo_models.data())};
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
        log_line(fresh ? "hologram: read" : "hologram: gone (no read for half a second)");
        }
      if(fresh and holo_ms(hs.logged_at, now) >= 2000)
        {
        hs.logged_at = now;
        auto const & a{hs.view.axes};
        // the target's draw's indices tell the model (3576: a commercial outpost, 2026-10-21)
        log_line("hologram: %u indices, scale %.5f, q %+.4f %+.4f %+.4f %+.4f | X %+.3f %+.3f %+.3f Y %+.3f %+.3f %+.3f Z %+.3f %+.3f %+.3f",
                 hs.count, static_cast<double>(hs.scale), hs.q.x, hs.q.y, hs.q.z, hs.q.w, a[0].x, a[0].y, a[0].z, a[1].x, a[1].y, a[1].z, a[2].x, a[2].y, a[2].z);
        auto const & r{hs.rows};
        log_line("hologram: cb1 first %u, rows 270..273 %+.4f %+.4f %+.4f %+.4f | %+.4f %+.4f %+.4f %+.4f | %+.4f %+.4f %+.4f %+.4f | %+.4f %+.4f "
                 "%+.4f %+.4f", hs.cb_first, r[0][0], r[0][1], r[0][2], r[0][3], r[1][0], r[1][1], r[1][2], r[1][3], r[2][0], r[2][1], r[2][2],
                 r[2][3], r[3][0], r[3][1], r[3][2], r[3][3]);
        }
      }

    ///\brief a draw of a candidate or of the hologram's shader on the immediate context
    auto hologram_draw(ID3D11DeviceContext * ctx, std::uint32_t index_count, std::uint32_t start_index, std::int32_t base_vertex,
                       std::uint32_t start_instance) noexcept -> void
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
        log_line("hologram: vs %016llX draws %u indices to a 512x512 target - taken for the hologram's model (station_vs)",
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
      __try
        {
        hologram_draw(ctx, index_count, start_index, base_vertex, start_instance);
        }
      __except(EXCEPTION_EXECUTE_HANDLER)
        {
        if(++faults >= 8)
          disabled.store(true);
        }
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
      if(FAILED(hr) or not shader or not *shader or not bytecode)
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
        verdict[at].store(creating_own.load() ? verdict_e::not_panel : verdict_e::pending, std::memory_order_relaxed);
        holo_kind_e kind{holo_kind_e::none};
        if(not creating_own.load() and settings().station_axes)
          {
          if(settings().station_vs and hash == settings().station_vs)
            kind = holo_kind_e::hologram;
          else if(not settings().station_vs and holo_candidate(bytecode, length))
            kind = holo_kind_e::candidate;
          }
        holo_kind[at].store(kind, std::memory_order_relaxed);
        if(kind == holo_kind_e::hologram and not holo_known.exchange(true))
          log_line("hologram: vs %016llX created (station_vs)", static_cast<unsigned long long>(hash));
        seen_ptr[at].store(*shader, std::memory_order_release);
        table_put(*shader, at);
        }
      if(creating_own.load())
        return hr;
#if defined(EDWORLD_EHT)
      shader_catalog_note(shader_stage_e::vertex, bytecode, length, hash);
#endif
      if(settings().log_all_vs)
        log_line("vs %016llX %zu bytes", static_cast<unsigned long long>(hash), static_cast<std::size_t>(length));
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
              log_line("vs %016llX kept for dumps (%zu bytes)", static_cast<unsigned long long>(hash), static_cast<std::size_t>(length));
              }
        }
      auto const & listed{settings().watch_vs};
      for(std::size_t i{}; i != listed.size(); ++i)
        if(listed[i] == hash)
          {
          if(std::uint32_t const at{table_get(*shader)}; at < max_vs_seen)
            verdict[at].store(verdict_e::panel, std::memory_order_relaxed);
          if(watch(*shader, static_cast<std::uint32_t>(i), false))
            log_line("vs %016llX watched (list index %zu, pointer %p)", static_cast<unsigned long long>(hash), i, *shader);
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
      if(SUCCEEDED(hr) and bytecode and not creating_own.load())
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
      if(SUCCEEDED(hr) and bytecode and not creating_own.load())
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
      if(SUCCEEDED(hr) and not creating_own.load())
        shader_catalog_note_layout(elements, count);
      return hr;
      }

    ///\brief the catalog's hooks on the device's table; one failing leaves the others (and edworld) as they are
    auto patch_catalog(ID3D11Device * dev) noexcept -> void
      {
      if(not settings().shader_catalog)
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
      log_line("shaders: the catalog %s (edworld_shaders beside the log, build %s)",
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
      if(self == immediate.load(std::memory_order_relaxed))
        {
        int found{-1};
        std::uint32_t pending{max_vs_seen};
        std::uint32_t holo{max_vs_seen};
        if(shader)
          {
          std::uint32_t const reserved{watched_count.load(std::memory_order_acquire)};
          std::uint32_t const n{reserved < max_watched ? reserved : max_watched};
          for(std::uint32_t i{}; i != n; ++i)
            if(watched_ptr[i].load(std::memory_order_acquire) == shader)
              {
              found = static_cast<int>(i);
              break;
              }
          if(found < 0)
            if(std::uint32_t const at{table_get(shader)}; at < max_vs_seen)
              {
              if(verdict[at].load(std::memory_order_relaxed) == verdict_e::pending)
                pending = at;
              if(holo_kind[at].load(std::memory_order_relaxed) != holo_kind_e::none)
                holo = at;
              }
          }
        current_watched = found;
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
        std::snprintf(name, sizeof name, "a%u_ib_u%u", a, size * 8u);
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
        std::snprintf(name, sizeof name, "a%u_vb%u_s%u", a, v, strides[v]);
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
          std::snprintf(name, sizeof name, "a%u_t33", a);
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
            full_line("  a%u ps t%u texture %08llx %ux%u f%u mips %u array %u", a, t, id, d.Width, d.Height,
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
            full_line("  a%u ps t%u buffer %08llx %u bytes stride %u", a, t, id, bd.ByteWidth, bd.StructureByteStride);
            buffer->Release();
            }
          }
        else
          full_line("  a%u ps t%u resource %08llx dimension %u", a, t, id, static_cast<unsigned>(dim));
        res->Release();
        }
      std::uint32_t const kept{std::min(kept_vs_reserved.load(), max_kept_vs)};
      for(std::uint32_t k{}; k != kept; ++k)
        if(kept_vs[k].hash.load(std::memory_order_acquire) == hash and not full_kept_written[k])
          {
          full_kept_written[k] = true;
          wchar_t file[MAX_PATH];
          std::swprintf(file, MAX_PATH, L"%ls\\edworld_dumps\\%hs_vs_%016llX.dxbc", settings().dir.c_str(), dump_stamp,
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
      full_line("a%u %c vs %016llX count %u inst %u start %u base %d sinst %u vp %.0f,%.0f %.0fx%.0f rt %ux%u f%u at %08llx", a, kind,
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
        std::snprintf(name, sizeof name, "a%u_cb%u", a, c);
        // a dump_vs draw's buffers whole from their first constant (the hologram's camera is row 270 of cb1)
        std::uint64_t const whole{counts[c] ? counts[c] * 16ull : 65536ull};
        std::uint64_t const bytes{looked_into ? whole : std::min<std::uint64_t>(whole, full_cb_bytes)};
        if(looked_into)
          full_line("  a%u cb%u first %u count %u", a, c, firsts[c], counts[c]);
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
      LONGLONG const began{qpc_now()};
      __try
        {
        full_after(ctx);
        }
      __except(EXCEPTION_EXECUTE_HANDLER)
        {
        full_after_draw = 0;
        if(++faults >= 8)
          disabled.store(true);
        }
      // the copy of the game's main target after a dump_vs draw takes milliseconds: no gap of the game's either (it ended
      // each whole frame of 2026-10-22 right after the hologram's draws, at 18 draws)
      if(last_draw_qpc)
        last_draw_qpc += qpc_now() - began;
      }

    auto guarded_full(ID3D11DeviceContext * ctx, char kind, std::uint32_t count, std::uint32_t instances, std::uint32_t start,
                      std::int32_t base, std::uint32_t start_instance) noexcept -> void
      {
      LONGLONG const began{qpc_now()};
      __try
        {
        full_draw(ctx, kind, count, instances, start, base, start_instance);
        }
      __except(EXCEPTION_EXECUTE_HANDLER)
        {
        full_armed = false;
        if(++faults >= 8)
          disabled.store(true);
        }
      // the dump's own work (copies of whole targets, milliseconds) is no gap of the game's: it must not end the frame
      if(last_draw_qpc)
        last_draw_qpc += qpc_now() - began;
      }

    void STDMETHODCALLTYPE hook_draw_indexed(ID3D11DeviceContext * self, UINT index_count, UINT start_index, INT base_vertex)
      {
      if(full_armed)
        {
        if(self == immediate.load(std::memory_order_relaxed))
          guarded_full(self, 'i', index_count, 1, start_index, base_vertex, 0);
        else
          note_full_other(self);
        }
      bool const watched{watched_draw(self)};
      if(index_count == 6u and holo_rt_count and not disabled.load(std::memory_order_relaxed) and
         self == immediate.load(std::memory_order_relaxed))
        guarded_laying_over(self);
      if(current_holo < max_vs_seen and not disabled.load(std::memory_order_relaxed) and self == immediate.load(std::memory_order_relaxed))
        guarded_hologram(self, index_count, start_index, base_vertex, 0);
      if(watched)
        guarded_observe(self, index_count, 1, start_index, base_vertex, 0);
      else
        {
        if(current_pending < max_vs_seen and self == immediate.load(std::memory_order_relaxed))
          guarded_classify(self);
        if(probe_request.load(std::memory_order_relaxed))
          guarded_probe(self, index_count, 1);
        }
      orig_draw_indexed(self, index_count, start_index, base_vertex);
      if(full_after_draw and self == immediate.load(std::memory_order_relaxed))
        guarded_full_after(self);
      if(watched)
        guarded_after(self, index_count, start_index, base_vertex, 0);
      }

    void STDMETHODCALLTYPE hook_draw(ID3D11DeviceContext * self, UINT vertex_count, UINT start_vertex)
      {
      if(full_armed)
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
        if(current_pending < max_vs_seen and self == immediate.load(std::memory_order_relaxed))
          guarded_classify(self);
        if(probe_request.load(std::memory_order_relaxed))
          guarded_probe(self, vertex_count, 1);
        }
      orig_draw(self, vertex_count, start_vertex);
      if(full_after_draw and self == immediate.load(std::memory_order_relaxed))
        guarded_full_after(self);
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
      if(full_armed)
        {
        if(self == immediate.load(std::memory_order_relaxed))
          guarded_full(self, 'I', index_count, instance_count, start_index, base_vertex, start_instance);
        else
          note_full_other(self);
        }
      bool const watched{watched_draw(self)};
      if(index_count == 6u and holo_rt_count and not disabled.load(std::memory_order_relaxed) and
         self == immediate.load(std::memory_order_relaxed))
        guarded_laying_over(self);
      if(current_holo < max_vs_seen and not disabled.load(std::memory_order_relaxed) and self == immediate.load(std::memory_order_relaxed))
        guarded_hologram(self, index_count, start_index, base_vertex, start_instance);
      if(watched)
        guarded_observe(self, index_count, instance_count, start_index, base_vertex, start_instance);
      else
        {
        if(current_pending < max_vs_seen and self == immediate.load(std::memory_order_relaxed))
          guarded_classify(self);
        if(probe_request.load(std::memory_order_relaxed))
          guarded_probe(self, index_count, instance_count);
        }
      orig_draw_indexed_instanced(self, index_count, instance_count, start_index, base_vertex, start_instance);
      if(full_after_draw and self == immediate.load(std::memory_order_relaxed))
        guarded_full_after(self);
      if(watched)
        guarded_after(self, index_count, start_index, base_vertex, start_instance);
      }

    void STDMETHODCALLTYPE hook_draw_instanced(
      ID3D11DeviceContext * self,
      UINT vertex_count,
      UINT instance_count,
      UINT start_vertex,
      UINT start_instance
    )
      {
      if(full_armed)
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
        if(current_pending < max_vs_seen and self == immediate.load(std::memory_order_relaxed))
          guarded_classify(self);
        if(probe_request.load(std::memory_order_relaxed))
          guarded_probe(self, vertex_count, instance_count);
        }
      orig_draw_instanced(self, vertex_count, instance_count, start_vertex, start_instance);
      if(full_after_draw and self == immediate.load(std::memory_order_relaxed))
        guarded_full_after(self);
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
    ID3D11DeviceContext * ctx{};
    dev->GetImmediateContext(&ctx);
    if(not ctx)
      {
      log_line("attach: device %p has no immediate context", dev);
      return;
      }
    {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpc_frequency = f.QuadPart ? f.QuadPart : 1;
    }
    bool ok{true};
    ok = ok and patch_slot(dev, slot_create_vertex_shader, reinterpret_cast<void *>(&hook_create_vs), reinterpret_cast<void **>(&orig_create_vs));
    ok = ok and patch_slot(ctx, slot_vs_set_shader, reinterpret_cast<void *>(&hook_vs_set_shader), reinterpret_cast<void **>(&orig_vs_set_shader));
    ok = ok and patch_slot(ctx, slot_draw_indexed, reinterpret_cast<void *>(&hook_draw_indexed), reinterpret_cast<void **>(&orig_draw_indexed));
    ok = ok and patch_slot(ctx, slot_draw, reinterpret_cast<void *>(&hook_draw), reinterpret_cast<void **>(&orig_draw));
    ok = ok and patch_slot(ctx, slot_draw_indexed_instanced, reinterpret_cast<void *>(&hook_draw_indexed_instanced), reinterpret_cast<void **>(&orig_draw_indexed_instanced));
    ok = ok and patch_slot(ctx, slot_draw_instanced, reinterpret_cast<void *>(&hook_draw_instanced), reinterpret_cast<void **>(&orig_draw_instanced));
    if(not ok)
      {
      log_line("attach: could not patch the device/context tables; observing nothing");
      ctx->Release();
      return;
      }
#if defined(EDWORLD_EHT)
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
    immediate1 = ctx1;
    immediate.store(ctx);  // the latest device wins; its context stays referenced for the process lifetime
#if defined(EDWORLD_EHT)
    if(not share)
      open_share();
#endif
    if(settings().compass)
      start_game_state();  // the compass's checking lines read the flight from Status.json
    if(settings().patch)
      {
      start_game_state();
      static std::atomic<bool> probe_started{};
      if(not probe_started.exchange(true))
        if(HANDLE const t{CreateThread(nullptr, 0, &probe_thread, nullptr, 0, nullptr)})
          CloseHandle(t);
      }
    if(settings().patch and not settings().edsm)
      log_line(with_eht ? "patch: on, edsm = 0 - allegiance from EHT's target only"
                        : "patch: on, but edsm = 0 - no allegiance, only the test frame can be drawn");
    std::uint32_t const n{watched_count.load()};
    log_line(
      "attach: device %p context %p (context1 %s), %zu watched hash(es), %u already created",
      dev,
      ctx,
      ctx1 ? "yes" : "no",
      settings().watch_vs.size(),
      n
    );
    }
  }  // namespace edworld
