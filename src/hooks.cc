// edworld — the observer: which vertex shaders draw panels, and what each panel draw carries.
//
// Read-only by construction: every hook calls the game's call through unchanged, and what we add is our own
// copies into our own staging buffers. Lessons taken from EDVR (MIT) and its Proton issue 65:
//  - per-draw work is a pointer compare; Get* calls only for the few matched draws;
//  - never read a mapped dynamic buffer on the CPU (write-combined under DXVK); copy on the GPU into staging
//    and map it frames later with DO_NOT_WAIT;
//  - GetType before GetDesc; a fault in our code disables us, never the game call.
#include "edworld_share.h"
#include "panel_math.h"
#include "runtime.h"

#include <d3d11_1.h>

#include <atomic>
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

    struct slot_t
      {
      ID3D11Buffer * staging{};
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

    auto publish(slot_t const & slot, std::uint8_t const * bytes) noexcept -> void
      {
      if(not share)
        return;
      std::atomic_ref<std::uint32_t> sequence{share->sequence};
      sequence.fetch_add(1u, std::memory_order_acq_rel);  // odd: inside
      share->frame = frame;
      share->source_frame = slot.frame;
      share->unix_ms = unix_ms_now();
      share->panel_count = slot.count;
      for(std::uint32_t i{}; i != slot.count; ++i)
        {
        panel_t & p{share->panels[i]};
        meta_t const & m{slot.meta[i]};
        p.surface_id = m.surface_id;
        p.surface_width = m.surface_width;
        p.surface_height = m.surface_height;
        p.surface_format = m.surface_format;
        p.vs_index = m.vs_index;
        p.index_count = m.index_count;
        p.instance_count = m.instance_count;
        p.start_index = m.start_index;
        p.base_vertex = m.base_vertex;
        p.start_instance = m.start_instance;
        p.ordinal = i;
        std::memcpy(p.cb0, bytes + i * record_bytes, record_bytes);
        anchor_from_cb0(p.cb0, p.anchor_clip);
        }
      sequence.fetch_add(1u, std::memory_order_acq_rel);  // even: done
      }

    auto log_summary(slot_t const & slot, std::uint8_t const * bytes) noexcept -> void
      {
      log_line(
        "frame %llu: %u panel draw(s); totals draws %llu published %llu dropped %llu full %llu faults %u vs %llu",
        static_cast<unsigned long long>(slot.frame),
        slot.count,
        static_cast<unsigned long long>(stat_draws),
        static_cast<unsigned long long>(stat_published),
        static_cast<unsigned long long>(stat_dropped),
        static_cast<unsigned long long>(stat_full),
        faults,
        static_cast<unsigned long long>(vs_created.load())
      );
      for(std::uint32_t i{}; i != slot.count; ++i)
        {
        meta_t const & m{slot.meta[i]};
        float cb0[cb0_rows][4];
        std::memcpy(cb0, bytes + i * record_bytes, record_bytes);
        float clip[4];
        anchor_from_cb0(cb0, clip);
        auto const ndc{clip_to_ndc(clip)};
        log_line(
          "  #%u vs%u surf %08llx %ux%u f%u n%u x%u si%u bv%d inst%u anchor %s %.4f %.4f w %.4f",
          i,
          m.vs_index,
          static_cast<unsigned long long>(m.surface_id & 0xffffffffull),
          m.surface_width,
          m.surface_height,
          m.surface_format,
          m.index_count,
          m.instance_count,
          m.start_index,
          m.base_vertex,
          m.start_instance,
          ndc ? "ndc" : "behind",
          ndc ? ndc->x : clip[0],
          ndc ? ndc->y : clip[1],
          clip[3]
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
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT const hr{immediate.load()->Map(oldest->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)};
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
        if(FAILED(hr) or not m.pData)
          {
          oldest->filled = false;
          ++stat_dropped;
          continue;
          }
        auto const * const bytes{static_cast<std::uint8_t const *>(m.pData)};
        bool const stale{(now - oldest->qpc) * 1000 / qpc_frequency > 100};
        if(not stale)
          {
          publish(*oldest, bytes);
          ++stat_published;
          if(settings().log_interval_ms
             and (now - last_log_qpc) * 1000 / qpc_frequency >= settings().log_interval_ms)
            {
            last_log_qpc = now;
            log_summary(*oldest, bytes);
            }
          }
        else
          ++stat_dropped;
        immediate.load()->Unmap(oldest->staging, 0);
        oldest->filled = false;
        }
      }

    auto begin_frame(LONGLONG now) noexcept -> void
      {
      if(recording >= 0 and ring[recording].count)
        ring[recording].filled = true;
      recording = -1;
      ++frame;
      drain(now);
      for(int i{}; i != static_cast<int>(ring_slots); ++i)
        if(not ring[i].filled and ring[i].staging)
          {
          recording = i;
          ring[i].count = 0;
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
      }
    recording = -1;
    device = dev;
    immediate1 = ctx1;
    immediate.store(ctx);  // the latest device wins; its context stays referenced for the process lifetime
    if(not share)
      open_share();
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
