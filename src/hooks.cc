// edworld — the observer: which vertex shaders draw panels, and what each panel draw carries.
// edworld alone only finds the panels' draws (for the patch); edworld_eht also copies what each carries and
// publishes it to EHT (`panels`).
//
// Read-only by construction: every hook calls the game's call through unchanged, and what we add is our own
// copies into our own staging buffers. Lessons taken from EDVR (MIT) and its Proton issue 65:
//  - per-draw work is a pointer compare; Get* calls only for the few matched draws;
//  - never read a mapped dynamic buffer on the CPU (write-combined under DXVK); copy on the GPU into staging
//    and map it frames later with DO_NOT_WAIT;
//  - GetType before GetDesc; a fault in our code disables us, never the game call.
#include "edworld_share.h"
#include "game_state.h"
#include "panel_math.h"
#include "panel_patch.h"
#include "runtime.h"

#include <d3d11_1.h>

#include <atomic>
#include <cstdio>
#include <cstring>

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

    // ---- surface dumps, on request (discovery): which panel shows what ----
    struct pending_dump_t
      {
      ID3D11Texture2D * staging;
      std::uint64_t id;
      std::uint32_t width, height, format;
      LONGLONG qpc;
      };

    constexpr std::uint32_t max_dumps{16};
    pending_dump_t pending_dumps[max_dumps]{};
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
      if(pending_count == max_dumps or dumped_count == max_dumps)
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
      pending_dumps[pending_count++] = pending_dump_t{staging, id, d.Width, d.Height, static_cast<std::uint32_t>(d.Format), qpc_now()};
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
          std::swprintf(name, MAX_PATH, L"%ls\\edworld_dumps\\%hs_%08llx_%ux%u_f%u_pitch%u.raw", settings().dir.c_str(),
                        dump_stamp, static_cast<unsigned long long>(p.id & 0xffffffffull), p.width, p.height, p.format,
                        m.RowPitch);
          if(std::FILE * f{_wfopen(name, L"wb")})
            {
            std::fwrite(m.pData, 1, static_cast<std::size_t>(m.RowPitch) * p.height, f);
            std::fclose(f);
            log_line("dump: wrote %S", name);
            }
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

    constexpr std::uint32_t max_buffers{128};
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
          if(std::FILE * f{_wfopen(name, L"wb")})
            {
            std::fwrite(m.pData, 1, p.bytes, f);
            std::fclose(f);
            }
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
                      char const * name) noexcept -> void
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
      log_line("dump: %s = %u byte(s) from %llu of a buffer of %u (stride %u, bind 0x%x, misc 0x%x)", name, p.bytes,
               static_cast<unsigned long long>(from), bd.ByteWidth, bd.StructureByteStride, bd.BindFlags, bd.MiscFlags);
      }

    auto begin_frame(LONGLONG now) noexcept -> void
      {
      if(recording >= 0 and ring[recording].count)
        ring[recording].filled = true;
      recording = -1;
      ++frame;
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
        begin_frame(now);
      last_draw_qpc = now;
      ++stat_draws;
      last_watched_ms.store(GetTickCount64(), std::memory_order_relaxed);
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

    // SEH around our own work only: a fault counts against us and, past a few, switches us off.
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
      if(not settings().patch or not settings().list)
        return;
      __try
        {
        panel_list_after(ctx, frame, index_count, start_index, base_vertex, start_instance);
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
      static_cast<IUnknown *>(shader)->AddRef();  // pinned: a released pointer must never be reused for another shader we would match
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
        seen_ptr[at].store(*shader, std::memory_order_release);
        table_put(*shader, at);
        }
      if(creating_own.load())
        return hr;
      if(settings().log_all_vs)
        log_line("vs %016llX %zu bytes", static_cast<unsigned long long>(hash), static_cast<std::size_t>(length));
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
            if(std::uint32_t const at{table_get(shader)};
               at < max_vs_seen and verdict[at].load(std::memory_order_relaxed) == verdict_e::pending)
              pending = at;
          }
        current_watched = found;
        current_pending = pending;
        }
      orig_vs_set_shader(self, shader, instances, instance_count);
      }

    void STDMETHODCALLTYPE hook_draw_indexed(ID3D11DeviceContext * self, UINT index_count, UINT start_index, INT base_vertex)
      {
      bool const watched{watched_draw(self)};
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
      if(watched)
        guarded_after(self, index_count, start_index, base_vertex, 0);
      }

    void STDMETHODCALLTYPE hook_draw(ID3D11DeviceContext * self, UINT vertex_count, UINT start_vertex)
      {
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
      bool const watched{watched_draw(self)};
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
      }
    }  // namespace

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
    immediate1 = ctx1;
    immediate.store(ctx);  // the latest device wins; its context stays referenced for the process lifetime
#if defined(EDWORLD_EHT)
    if(not share)
      open_share();
#endif
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
