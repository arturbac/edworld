// edworld — a constant buffer slot as the game bound it: the buffer and, through ID3D11DeviceContext1, the window into
// it (first constant, count). edworld puts the game's slots back after its own draws and hands the game's buffers to
// its own shaders; without the window a buffer bound with an offset would be read from its start.
#pragma once

#include <d3d11_1.h>

namespace edworld
  {
  enum struct cb_stage_e : unsigned char
    {
    vertex,
    pixel
    };

  ///\brief a slot's binding; `first` 0 and `count` 0 when bound without a window (or the context has no 11.1 calls)
  struct cb_binding_t
    {
    ID3D11Buffer * buffer{};  ///< referenced (Get*), released by release()
    UINT first{};
    UINT count{};

    auto release() noexcept -> void
      {
      if(buffer)
        buffer->Release();
      buffer = nullptr;
      }
    };

  ///\brief the context's 11.1 interface, kept for the last context asked about (edworld draws on the immediate one)
  [[nodiscard]]
  inline auto context1_of(ID3D11DeviceContext * ctx) noexcept -> ID3D11DeviceContext1 *
    {
    static ID3D11DeviceContext * last{};
    static ID3D11DeviceContext1 * last1{};
    if(ctx != last)
      {
      if(last1)
        last1->Release();
      last1 = nullptr;
      last = ctx;
      if(ctx)
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&last1));
      }
    return last1;
    }

  [[nodiscard]]
  inline auto get_cb(ID3D11DeviceContext * ctx, cb_stage_e stage, UINT slot) noexcept -> cb_binding_t
    {
    cb_binding_t b{};
    if(ID3D11DeviceContext1 * const c1{context1_of(ctx)})
      {
      if(stage == cb_stage_e::vertex)
        c1->VSGetConstantBuffers1(slot, 1, &b.buffer, &b.first, &b.count);
      else
        c1->PSGetConstantBuffers1(slot, 1, &b.buffer, &b.first, &b.count);
      }
    else if(stage == cb_stage_e::vertex)
      ctx->VSGetConstantBuffers(slot, 1, &b.buffer);
    else
      ctx->PSGetConstantBuffers(slot, 1, &b.buffer);
    return b;
    }

  ///\brief binds `b` at `slot` of `stage`: with its window when it had one (first past 0), else the whole buffer
  inline auto set_cb(ID3D11DeviceContext * ctx, cb_stage_e stage, UINT slot, cb_binding_t const & b) noexcept -> void
    {
    ID3D11DeviceContext1 * const c1{b.buffer and b.first != 0 ? context1_of(ctx) : nullptr};
    if(c1)
      {
      if(stage == cb_stage_e::vertex)
        c1->VSSetConstantBuffers1(slot, 1, &b.buffer, &b.first, &b.count);
      else
        c1->PSSetConstantBuffers1(slot, 1, &b.buffer, &b.first, &b.count);
      }
    else if(stage == cb_stage_e::vertex)
      ctx->VSSetConstantBuffers(slot, 1, &b.buffer);
    else
      ctx->PSSetConstantBuffers(slot, 1, &b.buffer);
    }
  }  // namespace edworld
