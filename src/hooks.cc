// edworld — the observer: which vertex shaders draw panels, and what each panel draw carries.
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
    std::atomic<std::uint32_t> watched_count{};
    std::atomic<std::uint64_t> vs_created{};

    // ---- render-thread state ----
    std::atomic<ID3D11DeviceContext *> immediate{};
    ID3D11Device * device{};
    ID3D11DeviceContext1 * immediate1{};
    int current_watched{-1};
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

    // ---- the published record ----
    auto open_share() noexcept -> void
      {
      std::wstring const & path{settings().share};
      if(path.empty())
        return;
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

    auto begin_frame(LONGLONG now) noexcept -> void
      {
      if(recording >= 0 and ring[recording].count)
        ring[recording].filled = true;
      recording = -1;
      ++frame;
      drain(now);
      if(pending_count)
        write_dumps(now);
      check_trigger(now);
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
        queue_dump(ctx);
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

    auto watched_draw(ID3D11DeviceContext * ctx) noexcept -> bool
      {
      return current_watched >= 0 and ctx == immediate.load(std::memory_order_relaxed)
             and not disabled.load(std::memory_order_relaxed);
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
      if(settings().log_all_vs)
        log_line("vs %016llX %zu bytes", static_cast<unsigned long long>(hash), static_cast<std::size_t>(length));
      auto const & watch{settings().watch_vs};
      for(std::size_t i{}; i != watch.size(); ++i)
        if(watch[i] == hash)
          {
          std::uint32_t const at{watched_count.load()};
          if(at < max_watched)
            {
            (*shader)->AddRef();  // pinned: a released pointer must never be reused for another shader we would match
            watched_index[at] = static_cast<std::uint32_t>(i);
            watched_ptr[at].store(*shader, std::memory_order_release);
            watched_count.store(at + 1, std::memory_order_release);
            log_line("vs %016llX watched (list index %zu, pointer %p)", static_cast<unsigned long long>(hash), i, *shader);
            }
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
        if(shader)
          {
          std::uint32_t const n{watched_count.load(std::memory_order_acquire)};
          for(std::uint32_t i{}; i != n; ++i)
            if(watched_ptr[i].load(std::memory_order_relaxed) == shader)
              {
              found = static_cast<int>(i);
              break;
              }
          }
        current_watched = found;
        }
      orig_vs_set_shader(self, shader, instances, instance_count);
      }

    void STDMETHODCALLTYPE hook_draw_indexed(ID3D11DeviceContext * self, UINT index_count, UINT start_index, INT base_vertex)
      {
      if(watched_draw(self))
        guarded_observe(self, index_count, 1, start_index, base_vertex, 0);
      orig_draw_indexed(self, index_count, start_index, base_vertex);
      }

    void STDMETHODCALLTYPE hook_draw(ID3D11DeviceContext * self, UINT vertex_count, UINT start_vertex)
      {
      if(watched_draw(self))
        guarded_observe(self, vertex_count, 1, start_vertex, 0, 0);
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
      if(watched_draw(self))
        guarded_observe(self, index_count, instance_count, start_index, base_vertex, start_instance);
      orig_draw_indexed_instanced(self, index_count, instance_count, start_index, base_vertex, start_instance);
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
    if(not share)
      open_share();
    if(settings().patch)
      start_game_state();
    if(settings().patch and not settings().edsm)
      log_line("patch: on, but edsm = 0 - no allegiance, only the test frame can be drawn");
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
