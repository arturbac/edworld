// edworld render test (2026-11-18, Artur: flying to planets to check each change costs too much): the clouds' volume
// shader (shaders/clouds3d.hlsl) drawn with the system d3d11 into a float target for scenes of several atmospheres -
// from orbit by day and by night (the aurora), at the limb, inside the layer, on the ground (the snow) - its buffer
// made by clouds_values (src/clouds_values.h), the same as the game's draw. Checked by numbers that do not depend on
// the driver (no NaN, clouds on the disc, the aurora at night only and stronger in a storm, no jump between two frames)
// and against reference pictures (test/data/render/*.png, made again on purpose with --update).
// Usage: render_test.exe <out dir> [<reference dir>] [--update] [--shaders <dir>]; exit 0 when all checks pass
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>

#include "blue_noise.h"
#include "clouds_values.h"
#include "snow.h"

#include "clouds3d_ps.h"
#include "clouds3d_vs.h"
#include "clouds_up_ps.h"
#include "clouds_up_vs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace
  {
  using namespace edworld;

  constexpr UINT width{320u};
  constexpr UINT height{160u};
  int failures{};

  auto check(bool ok, char const * what) -> void
    {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if(not ok)
      ++failures;
    }

  struct vec_t
    {
    float x, y, z;
    };

  auto operator-(vec_t a, vec_t b) -> vec_t { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
  auto operator*(vec_t a, float s) -> vec_t { return {a.x * s, a.y * s, a.z * s}; }
  auto dot(vec_t a, vec_t b) -> float { return a.x * b.x + a.y * b.y + a.z * b.z; }
  auto cross(vec_t a, vec_t b) -> vec_t { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
  auto normalized(vec_t a) -> vec_t { return a * (1.f / std::sqrt(dot(a, a))); }

  ///\brief one picture: the planet (its frame the world's axes, at the origin), the eye, where it looks, the star
  struct scene_t
    {
    std::string name;
    atmosphere_kind_e kind;
    float radius;      ///< metres
    vec_t eye;         ///< in radii from the planet's centre
    vec_t look;        ///< the point looked at, in radii
    vec_t sun{1.f, 0.f, 0.f};  ///< towards the star
    float activity{-1.f};      ///< aurora_activity, -1 = by the time
    float field{-1.f};         ///< aurora_field
    bool aurora{true};
    bool snow{};
    float seconds{1000.f};
    float day_fade{1.f};
    float show{1.f};  ///< the picture's brightness (the night's: the aurora seen)
    };

  struct gpu_t
    {
    ID3D11Device * device{};
    ID3D11DeviceContext * ctx{};
    ID3D11VertexShader * vs{};
    ID3D11PixelShader * ps{};
    ID3D11InputLayout * layout{};
    ID3D11Buffer * vb{};
    ID3D11Buffer * ib{};
    UINT index_count{};
    ID3D11Buffer * object{};
    ID3D11Buffer * frame{};
    ID3D11Buffer * clouds{};
    ID3D11ShaderResourceView * depth{};
    ID3D11ShaderResourceView * noise{};
    ID3D11Texture2D * target{};
    ID3D11RenderTargetView * rtv{};
    ID3D11Texture2D * staging{};
    ID3D11RasterizerState * raster{};
    ID3D11DepthStencilState * no_depth{};
    };

  auto make_buffer(ID3D11Device * device, UINT bytes, UINT bind, void const * data = nullptr) -> ID3D11Buffer *
    {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    ID3D11Buffer * b{};
    device->CreateBuffer(&d, data ? &init : nullptr, &b);
    return b;
    }

  auto read_file(std::string const & path) -> std::vector<std::uint8_t>
    {
    std::vector<std::uint8_t> bytes;
    if(FILE * const f{std::fopen(path.c_str(), "rb")})
      {
      std::uint8_t buf[65536];
      for(std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) != 0;)
        bytes.insert(bytes.end(), buf, buf + n);
      std::fclose(f);
      }
    return bytes;
    }

  ///\brief shaders: edworld's own (built in), or another's from <dir>\clouds3d_vs.cso and clouds3d_ps.cso (an older
  /// build's, to compare)
  auto make_gpu(std::string const & shaders) -> gpu_t
    {
    gpu_t g{};
    std::vector<std::uint8_t> vs_code(g_clouds3d_vs, g_clouds3d_vs + sizeof g_clouds3d_vs);
    std::vector<std::uint8_t> ps_code(g_clouds3d_ps, g_clouds3d_ps + sizeof g_clouds3d_ps);
    if(not shaders.empty())
      {
      vs_code = read_file(shaders + "\\clouds3d_vs.cso");
      ps_code = read_file(shaders + "\\clouds3d_ps.cso");
      }
    D3D_FEATURE_LEVEL level{};
    if(FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.device, &level,
                                &g.ctx)))
      return g;
    if(vs_code.empty() or ps_code.empty())
      return g;
    g.device->CreateVertexShader(vs_code.data(), vs_code.size(), nullptr, &g.vs);
    g.device->CreatePixelShader(ps_code.data(), ps_code.size(), nullptr, &g.ps);
    D3D11_INPUT_ELEMENT_DESC const element{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    g.device->CreateInputLayout(&element, 1, vs_code.data(), vs_code.size(), &g.layout);
    // a unit sphere, as the game's shell
    constexpr int rings{48}, sectors{96};
    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    for(int r{}; r <= rings; ++r)
      for(int s{}; s <= sectors; ++s)
        {
        float const theta{3.14159265f * static_cast<float>(r) / rings};
        float const phi{6.2831853f * static_cast<float>(s) / sectors};
        vertices.insert(vertices.end(), {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)});
        }
    for(int r{}; r != rings; ++r)
      for(int s{}; s != sectors; ++s)
        {
        std::uint32_t const a{static_cast<std::uint32_t>(r * (sectors + 1) + s)}, b{a + sectors + 1u};
        indices.insert(indices.end(), {a, b, a + 1u, a + 1u, b, b + 1u});
        }
    g.index_count = static_cast<UINT>(indices.size());
    g.vb = make_buffer(g.device, static_cast<UINT>(vertices.size() * sizeof(float)), D3D11_BIND_VERTEX_BUFFER, vertices.data());
    g.ib = make_buffer(g.device, static_cast<UINT>(indices.size() * sizeof(std::uint32_t)), D3D11_BIND_INDEX_BUFFER, indices.data());
    g.object = make_buffer(g.device, 13u * 16u, D3D11_BIND_CONSTANT_BUFFER);
    g.frame = make_buffer(g.device, 336u * 16u, D3D11_BIND_CONSTANT_BUFFER);
    g.clouds = make_buffer(g.device, static_cast<UINT>(clouds_value_count * sizeof(float)), D3D11_BIND_CONSTANT_BUFFER);
    // the scene's depth: nothing in front (reversed depth, 0 = far)
    {
    D3D11_TEXTURE2D_DESC t{};
    t.Width = width;
    t.Height = height;
    t.MipLevels = t.ArraySize = 1;
    t.Format = DXGI_FORMAT_R32_FLOAT;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<float> zero(width * height, 0.f);
    D3D11_SUBRESOURCE_DATA init{zero.data(), width * sizeof(float), 0};
    ID3D11Texture2D * tex{};
    g.device->CreateTexture2D(&t, &init, &tex);
    if(tex)
      {
      g.device->CreateShaderResourceView(tex, nullptr, &g.depth);
      tex->Release();
      }
    }
    // the blue noise, as edworld makes it
    {
    auto const ranks{blue_noise_ranks(64)};
    std::vector<std::uint16_t> texels(ranks.size());
    for(std::size_t i{}; i != ranks.size(); ++i)
      texels[i] = static_cast<std::uint16_t>(ranks[i] * 65535u / (ranks.size() - 1u));
    D3D11_TEXTURE2D_DESC t{};
    t.Width = t.Height = 64;
    t.MipLevels = t.ArraySize = 1;
    t.Format = DXGI_FORMAT_R16_UNORM;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_IMMUTABLE;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{texels.data(), 64u * sizeof(std::uint16_t), 0};
    ID3D11Texture2D * tex{};
    g.device->CreateTexture2D(&t, &init, &tex);
    if(tex)
      {
      g.device->CreateShaderResourceView(tex, nullptr, &g.noise);
      tex->Release();
      }
    }
    D3D11_TEXTURE2D_DESC t{};
    t.Width = width;
    t.Height = height;
    t.MipLevels = t.ArraySize = 1;
    t.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_RENDER_TARGET;
    g.device->CreateTexture2D(&t, nullptr, &g.target);
    if(g.target)
      g.device->CreateRenderTargetView(g.target, nullptr, &g.rtv);
    t.Usage = D3D11_USAGE_STAGING;
    t.BindFlags = 0;
    t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g.device->CreateTexture2D(&t, nullptr, &g.staging);
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    g.device->CreateRasterizerState(&raster, &g.raster);
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    g.device->CreateDepthStencilState(&depth, &g.no_depth);
    return g;
    }

  ///\brief the scene's constants written and the volume's pipeline bound, for a picture view_w x view_h (pixel: 2
  /// when the volume is drawn at half size, as in the game); the target, the viewport and t0 are the caller's
  auto set_scene(gpu_t const & g, scene_t const & s, UINT view_w, UINT view_h, float pixel) -> void
    {
    float const r{s.radius};
    vec_t const eye{s.eye * r};
    vec_t const forward{normalized(s.look * r - eye)};
    // the up of the picture: the planet's y from afar, the local up near the ground (the horizon level)
    bool const near_ground{dot(s.eye, s.eye) < 1.2f * 1.2f};
    vec_t const up_hint{near_ground ? normalized(s.eye)
                                    : std::fabs(forward.y) > 0.95f ? vec_t{0.f, 0.f, 1.f} : vec_t{0.f, 1.f, 0.f}};
    vec_t const right{normalized(cross(up_hint, forward))};
    vec_t const up{cross(forward, right)};
    float const sy{1.f / std::tan(0.5f * 1.0471976f)};  // 60 degrees high
    float const sx{sy * static_cast<float>(view_h) / static_cast<float>(view_w)};

    // the game's object constants: rows 9..11 the sphere turned (none here), scaled to the radius and placed from the eye
    std::array<float, 13 * 4> object{};
    vec_t const centre{vec_t{0.f, 0.f, 0.f} - eye};
    float const rows[3][4]{{r, 0.f, 0.f, centre.x}, {0.f, r, 0.f, centre.y}, {0.f, 0.f, r, centre.z}};
    for(int i{}; i != 3; ++i)
      for(int k{}; k != 4; ++k)
        object[(9 + i) * 4 + k] = rows[i][k];
    // the frame's: 270..273 the projection (reversed depth, from the eye's axes), 291 the light's way, 293 its colour
    // (a G star), 90 y the exposure, 126 the radius and the game's fade
    std::vector<float> frame(336 * 4, 0.f);
    float const rx[3]{right.x, right.y, right.z}, ux[3]{up.x, up.y, up.z}, fx[3]{forward.x, forward.y, forward.z};
    for(int i{}; i != 3; ++i)
      {
      frame[(270 + i) * 4 + 0] = sx * rx[i];
      frame[(270 + i) * 4 + 1] = sy * ux[i];
      frame[(270 + i) * 4 + 2] = 0.f;
      frame[(270 + i) * 4 + 3] = fx[i];
      }
    frame[273 * 4 + 2] = 1.f;
    vec_t const sun{normalized(s.sun)};
    frame[291 * 4 + 0] = -sun.x;
    frame[291 * 4 + 1] = -sun.y;
    frame[291 * 4 + 2] = -sun.z;
    frame[293 * 4 + 0] = 480.f;
    frame[293 * 4 + 1] = 468.f;
    frame[293 * 4 + 2] = 440.f;
    frame[90 * 4 + 1] = 0.004f;
    frame[126 * 4 + 0] = r;
    frame[126 * 4 + 2] = 1.f;

    settings_t settings{};
    settings.aurora = s.aurora;
    settings.aurora_activity = s.activity;
    settings.aurora_field = s.field;
    clouds_planet_t planet{};
    planet.kind = s.kind;
    planet.radius = r;
    planet.kind_weight = 1.f;
    planet.day_s = 30.f * 3600.f;
    planet.day_fade = s.day_fade;
    planet.spin = -1.f;
    planet.light_rg = 480.f / 468.f;
    planet.light_bg = 440.f / 468.f;
    planet.light_g = 468.f;
    planet.utc_s = 1.8e9;
    clouds_frame_t now{};
    now.seconds = s.seconds;
    now.flicker_s = s.seconds;
    now.port_width = static_cast<float>(view_w);
    now.pixel = pixel;
    if(s.snow)
      {
      // on foot, under the clouds: the crystals of the kind, the eye still at its reference
      snow_t const snow{snow_of(s.kind, false)};
      now.snow[0] = snow.under_clouds * snow_most;
      now.snow[1] = snow.clear * snow_most;
      now.snow[2] = snow_fall_speed(1.3, snow.air_density);
      now.snow[9] = snow_parts_all + 4.f;
      }
    std::array<float, clouds_value_count> values{};
    (void)clouds_values(settings, planet, now, values);

    g.ctx->UpdateSubresource(g.object, 0, nullptr, object.data(), 0, 0);
    g.ctx->UpdateSubresource(g.frame, 0, nullptr, frame.data(), 0, 0);
    g.ctx->UpdateSubresource(g.clouds, 0, nullptr, values.data(), 0, 0);
    g.ctx->RSSetState(g.raster);
    g.ctx->OMSetDepthStencilState(g.no_depth, 0);
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    UINT const stride{12u}, offset{0u};
    g.ctx->IASetVertexBuffers(0, 1, &g.vb, &stride, &offset);
    g.ctx->IASetIndexBuffer(g.ib, DXGI_FORMAT_R32_UINT, 0);
    g.ctx->IASetInputLayout(g.layout);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(g.vs, nullptr, 0);
    g.ctx->PSSetShader(g.ps, nullptr, 0);
    ID3D11Buffer * const buffers[2]{g.object, g.frame};
    g.ctx->VSSetConstantBuffers(0, 2, buffers);
    g.ctx->PSSetConstantBuffers(0, 2, buffers);
    g.ctx->VSSetConstantBuffers(4, 1, &g.clouds);
    g.ctx->PSSetConstantBuffers(4, 1, &g.clouds);
    g.ctx->PSSetShaderResources(2, 1, &g.noise);
    }

  ///\brief the scene drawn: premultiplied colour and cover, width x height x 4 floats
  auto draw(gpu_t const & g, scene_t const & s) -> std::vector<float>
    {
    set_scene(g, s, width, height, 1.f);
    float const clear[4]{};
    g.ctx->ClearRenderTargetView(g.rtv, clear);
    g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
    D3D11_VIEWPORT const port{0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
    g.ctx->RSSetViewports(1, &port);
    g.ctx->PSSetShaderResources(0, 1, &g.depth);
    g.ctx->DrawIndexed(g.index_count, 0, 0);
    g.ctx->CopyResource(g.staging, g.target);
    std::vector<float> out(width * height * 4u, 0.f);
    D3D11_MAPPED_SUBRESOURCE m{};
    if(SUCCEEDED(g.ctx->Map(g.staging, 0, D3D11_MAP_READ, 0, &m)))
      {
      for(UINT y{}; y != height; ++y)
        std::memcpy(out.data() + y * width * 4u, static_cast<std::uint8_t const *>(m.pData) + y * m.RowPitch, width * 16u);
      g.ctx->Unmap(g.staging, 0);
      }
    return out;
    }

  struct measure_t
    {
    bool finite{true};
    double cover{};  ///< the mean cover
    double light{};  ///< the mean of r + g + b
    };

  auto measure(std::vector<float> const & p) -> measure_t
    {
    measure_t m{};
    for(std::size_t i{}; i != p.size(); i += 4u)
      {
      for(std::size_t k{}; k != 4u; ++k)
        if(not std::isfinite(p[i + k]))
          m.finite = false;
      m.cover += p[i + 3];
      m.light += p[i] + p[i + 1] + p[i + 2];
      }
    double const n{static_cast<double>(p.size() / 4u)};
    m.cover /= n;
    m.light /= n;
    return m;
    }

  auto mean_difference(std::vector<float> const & a, std::vector<float> const & b) -> double
    {
    double sum{};
    for(std::size_t i{}; i != a.size(); ++i)
      sum += std::fabs(a[i] - b[i]);
    return sum / static_cast<double>(a.size());
    }

  ///\brief the picture as 8 bits a channel over black (premultiplied), the colour brought up so it is seen
  auto to_rgb8(std::vector<float> const & p, float show) -> std::vector<std::uint8_t>
    {
    std::vector<std::uint8_t> out(width * height * 3u);
    for(std::size_t i{}, o{}; i != p.size(); i += 4u, o += 3u)
      for(std::size_t k{}; k != 3u; ++k)
        {
        float const v{std::sqrt(std::max(p[i + k] * show, 0.f))};  // a gamma of 2
        out[o + k] = static_cast<std::uint8_t>(std::lround(std::min(v, 1.f) * 255.f));
        }
    return out;
    }

  auto wic() -> IWICImagingFactory *
    {
    static IWICImagingFactory * factory{[]
                                          {
                                            IWICImagingFactory * f{};
                                            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                                            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                             IID_PPV_ARGS(&f));
                                            return f;
                                          }()};
    return factory;
    }

  auto write_png(std::wstring const & path, std::vector<std::uint8_t> const & rgb) -> bool
    {
    IWICImagingFactory * const f{wic()};
    if(not f)
      return false;
    IWICStream * stream{};
    IWICBitmapEncoder * encoder{};
    IWICBitmapFrameEncode * frame{};
    bool ok{SUCCEEDED(f->CreateStream(&stream)) and SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))
            and SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
            and SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache))
            and SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) and SUCCEEDED(frame->Initialize(nullptr))
            and SUCCEEDED(frame->SetSize(width, height))};
    // PNG's encoder takes blue first
    WICPixelFormatGUID format{GUID_WICPixelFormat24bppBGR};
    ok = ok and SUCCEEDED(frame->SetPixelFormat(&format)) and IsEqualGUID(format, GUID_WICPixelFormat24bppBGR);
    std::vector<std::uint8_t> row_major{rgb};
    for(std::size_t i{}; i + 2u < row_major.size(); i += 3u)
      std::swap(row_major[i], row_major[i + 2u]);
    ok = ok and SUCCEEDED(frame->WritePixels(height, width * 3u, static_cast<UINT>(row_major.size()), row_major.data()))
         and SUCCEEDED(frame->Commit()) and SUCCEEDED(encoder->Commit());
    for(IUnknown * u: std::initializer_list<IUnknown *>{frame, encoder, stream})
      if(u)
        u->Release();
    return ok;
    }

  auto read_png(std::wstring const & path, std::vector<std::uint8_t> & rgb) -> bool
    {
    IWICImagingFactory * const f{wic()};
    if(not f)
      return false;
    IWICBitmapDecoder * decoder{};
    IWICBitmapFrameDecode * frame{};
    IWICFormatConverter * converter{};
    UINT w{}, h{};
    bool ok{SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
            and SUCCEEDED(decoder->GetFrame(0, &frame)) and SUCCEEDED(frame->GetSize(&w, &h)) and w == width and h == height
            and SUCCEEDED(f->CreateFormatConverter(&converter))
            and SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.,
                                                WICBitmapPaletteTypeCustom))};
    rgb.assign(width * height * 3u, 0u);
    ok = ok and SUCCEEDED(converter->CopyPixels(nullptr, width * 3u, static_cast<UINT>(rgb.size()), rgb.data()));
    for(std::size_t i{}; i + 2u < rgb.size(); i += 3u)
      std::swap(rgb[i], rgb[i + 2u]);
    for(IUnknown * u: std::initializer_list<IUnknown *>{converter, frame, decoder})
      if(u)
        u->Release();
    return ok;
    }


  ///\brief the time on the graphics card of the clouds as the game's draw does them (hooks.cc draw_volume): the copy
  /// of the picture's depth, the volume at half size into a cleared target, then laid over the picture full size; the
  /// median over `frames` after a few unmeasured, in ms, and the volume pass's pixels (millions), as `clouds gpu 5s`
  struct bench_t
    {
    double median_ms{}, min_ms{}, megapixels{};
    };

  auto bench(gpu_t const & g, scene_t const & s, UINT full_w, UINT full_h, int frames) -> bench_t
    {
    bench_t result{};
    UINT const half_w{full_w / 2u}, half_h{full_h / 2u};
    auto const texture{[&](UINT w, UINT h, DXGI_FORMAT f, UINT bind) -> ID3D11Texture2D *
                         {
                           D3D11_TEXTURE2D_DESC d{};
                           d.Width = w;
                           d.Height = h;
                           d.MipLevels = d.ArraySize = 1;
                           d.Format = f;
                           d.SampleDesc.Count = 1;
                           d.BindFlags = bind;
                           ID3D11Texture2D * tex{};
                           g.device->CreateTexture2D(&d, nullptr, &tex);
                           return tex;
                         }};
    // the picture's depth: nothing in front (reversed depth, 0 = far), as the render test's
    ID3D11Texture2D * depth_src{};
    {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = full_w;
    d.Height = full_h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<float> zero(static_cast<std::size_t>(full_w) * full_h, 0.f);
    D3D11_SUBRESOURCE_DATA init{zero.data(), static_cast<UINT>(full_w * sizeof(float)), 0};
    g.device->CreateTexture2D(&d, &init, &depth_src);
    }
    ID3D11Texture2D * const depth_copy{texture(full_w, full_h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE)};
    ID3D11Texture2D * const half{texture(half_w, half_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)};
    ID3D11Texture2D * const full{texture(full_w, full_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET)};
    ID3D11ShaderResourceView * depth_view{};
    ID3D11ShaderResourceView * half_view{};
    ID3D11RenderTargetView * half_rtv{};
    ID3D11RenderTargetView * full_rtv{};
    ID3D11VertexShader * up_vs{};
    ID3D11PixelShader * up_ps{};
    ID3D11BlendState * over{};
    if(depth_copy)
      g.device->CreateShaderResourceView(depth_copy, nullptr, &depth_view);
    if(half)
      {
      g.device->CreateShaderResourceView(half, nullptr, &half_view);
      g.device->CreateRenderTargetView(half, nullptr, &half_rtv);
      }
    if(full)
      g.device->CreateRenderTargetView(full, nullptr, &full_rtv);
    g.device->CreateVertexShader(g_clouds_up_vs, sizeof g_clouds_up_vs, nullptr, &up_vs);
    g.device->CreatePixelShader(g_clouds_up_ps, sizeof g_clouds_up_ps, nullptr, &up_ps);
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g.device->CreateBlendState(&blend, &over);
    ID3D11Query * disjoint{};
    ID3D11Query * begin{};
    ID3D11Query * end{};
    ID3D11Query * stats{};
    D3D11_QUERY_DESC q{};
    q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    g.device->CreateQuery(&q, &disjoint);
    q.Query = D3D11_QUERY_TIMESTAMP;
    g.device->CreateQuery(&q, &begin);
    g.device->CreateQuery(&q, &end);
    q.Query = D3D11_QUERY_PIPELINE_STATISTICS;
    g.device->CreateQuery(&q, &stats);
    bool const ready{depth_src and depth_view and half_rtv and half_view and full_rtv and up_vs and up_ps and over and disjoint and begin
                     and end and stats};
    std::vector<double> times;
    for(int i{}; ready and i != frames + 10; ++i)
      {
      g.ctx->Begin(disjoint);
      g.ctx->End(begin);
      // the volume: the depth copied, the half target cleared and marched into
      g.ctx->CopyResource(depth_copy, depth_src);
      float const clear[4]{};
      g.ctx->ClearRenderTargetView(half_rtv, clear);
      set_scene(g, s, full_w, full_h, 2.f);
      g.ctx->OMSetRenderTargets(1, &half_rtv, nullptr);
      D3D11_VIEWPORT const half_port{0.f, 0.f, static_cast<float>(half_w), static_cast<float>(half_h), 0.f, 1.f};
      g.ctx->RSSetViewports(1, &half_port);
      g.ctx->PSSetShaderResources(0, 1, &depth_view);
      g.ctx->Begin(stats);
      g.ctx->DrawIndexed(g.index_count, 0, 0);
      g.ctx->End(stats);
      // then over the picture: one triangle from the vertex's number
      g.ctx->OMSetRenderTargets(1, &full_rtv, nullptr);
      D3D11_VIEWPORT const full_port{0.f, 0.f, static_cast<float>(full_w), static_cast<float>(full_h), 0.f, 1.f};
      g.ctx->RSSetViewports(1, &full_port);
      g.ctx->IASetInputLayout(nullptr);
      g.ctx->VSSetShader(up_vs, nullptr, 0);
      g.ctx->PSSetShader(up_ps, nullptr, 0);
      g.ctx->PSSetShaderResources(1, 1, &half_view);
      g.ctx->OMSetBlendState(over, nullptr, 0xffffffffu);
      g.ctx->Draw(3, 0);
      ID3D11ShaderResourceView * const none[2]{};
      g.ctx->PSSetShaderResources(0, 2, none);
      g.ctx->End(end);
      g.ctx->End(disjoint);
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
      UINT64 a{}, b{};
      D3D11_QUERY_DATA_PIPELINE_STATISTICS ps{};
      while(g.ctx->GetData(disjoint, &dj, sizeof dj, 0) != S_OK)
        Sleep(0);
      while(g.ctx->GetData(begin, &a, sizeof a, 0) != S_OK)
        Sleep(0);
      while(g.ctx->GetData(end, &b, sizeof b, 0) != S_OK)
        Sleep(0);
      while(g.ctx->GetData(stats, &ps, sizeof ps, 0) != S_OK)
        Sleep(0);
      if(i >= 10 and not dj.Disjoint and dj.Frequency and b >= a)
        {
        times.push_back(static_cast<double>(b - a) * 1000. / static_cast<double>(dj.Frequency));
        result.megapixels = static_cast<double>(ps.PSInvocations) / 1e6;
        }
      }
    if(not times.empty())
      {
      std::sort(times.begin(), times.end());
      result.median_ms = times[times.size() / 2u];
      result.min_ms = times.front();
      }
    for(IUnknown * u: std::initializer_list<IUnknown *>{stats, end, begin, disjoint, over, up_ps, up_vs, full_rtv, half_rtv, half_view,
                                                         depth_view, full, half, depth_copy, depth_src})
      if(u)
        u->Release();
    return result;
    }

  auto widen(std::string const & s) -> std::wstring { return std::wstring(s.begin(), s.end()); }
  }  // namespace

auto main(int argc, char ** argv) -> int
  {
  std::vector<std::string> plain;
  bool update{};
  std::string shaders;
  UINT bench_w{}, bench_h{};
  for(int i{1}; i < argc; ++i)
    {
    std::string const a{argv[i]};
    if(a == "--update")
      update = true;
    else if(a == "--shaders" and i + 1 < argc)
      shaders = argv[++i];
    else if(a == "--bench" and i + 2 < argc)
      {
      bench_w = static_cast<UINT>(std::atoi(argv[++i]));
      bench_h = static_cast<UINT>(std::atoi(argv[++i]));
      }
    else
      plain.push_back(a);
    }
  if(plain.empty())
    {
    std::printf("usage: render_test.exe <out dir> [<reference dir>] [--update] [--shaders <dir>]\n");
    return 2;
    }
  std::string const out_dir{plain[0]};
  std::string const reference_dir{plain.size() > 1u ? plain[1] : ""};
  CreateDirectoryA(out_dir.c_str(), nullptr);
  gpu_t const g{make_gpu(shaders)};
  check(g.device and g.vs and g.ps and g.layout and g.rtv and g.staging and g.depth and g.noise, "render: the device, the shaders and the targets made");
  if(failures)
    return 1;

  if(bench_w and bench_h)
    {
    // tools/bench.sh: the scenes of the two planets the game's numbers are taken at (Sjona's, 2026-11-18), by day from
    // orbit, by night (the aurora at K 7), inside the layer by day; the game's picture's size
    struct planet_t
      {
      char const * name;
      atmosphere_kind_e kind;
      float radius;
      };
    IDXGIDevice * dxgi{};
    IDXGIAdapter * adapter{};
    DXGI_ADAPTER_DESC desc{};
    if(SUCCEEDED(g.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi))) and dxgi
       and SUCCEEDED(dxgi->GetAdapter(&adapter)) and adapter)
      adapter->GetDesc(&desc);
    std::printf("bench gpu: card %ls (vendor %04X device %04X)\n", desc.Description, desc.VendorId, desc.DeviceId);
    if(adapter)
      adapter->Release();
    if(dxgi)
      dxgi->Release();
    planet_t const planets[]{{"IY-B_c16-6_1_co2", atmosphere_kind_e::carbon_dioxide, 3687046.f},
                             {"PW-D_b32-6_2_argon", atmosphere_kind_e::argon, 2329626.f}};
    for(planet_t const & p: planets)
      {
      atmosphere_clouds_t const c{atmosphere_clouds(p.kind, 1.f)};
      float const inside_h{1.f + 0.5f * (c.bottom + c.top)};
      // inside the aurora (the game's 2026-11-18 scene): by night at 63 degrees, where the oval lies at K 7, halfway up
      // its curtains (the layer towering on a planet with storms), looking along the ground towards the pole
      aurora_height_t const band{aurora_height(p.kind, p.radius, 2.f)};
      float const up{1.f + 0.5f * (band.floor + band.ceiling)};
      float const lat{63.f * 0.017453293f};
      vec_t const in_aurora{-std::cos(lat) * up, std::sin(lat) * up, 0.f};
      vec_t const ahead{-std::cos(lat) * up + 0.3f * std::sin(lat), std::sin(lat) * up + 0.3f * std::cos(lat), 0.f};
      scene_t const scenes[]{
        {.name = "orbit_day_2R", .kind = p.kind, .radius = p.radius, .eye = {2.f, 0.3f, 0.2f}, .look = {0.f, 0.f, 0.f}, .activity = 7.f},
        {.name = "orbit_night_2R_k7", .kind = p.kind, .radius = p.radius, .eye = {-1.7f, 1.0f, 0.2f}, .look = {0.f, 0.2f, 0.f}, .activity = 7.f},
        {.name = "inside_day", .kind = p.kind, .radius = p.radius, .eye = {inside_h, 0.f, 0.f}, .look = {inside_h, 0.05f, 0.3f}, .activity = 7.f},
        {.name = "orbit_day_2R_no_aurora", .kind = p.kind, .radius = p.radius, .eye = {2.f, 0.3f, 0.2f}, .look = {0.f, 0.f, 0.f}, .aurora = false},
        {.name = "orbit_night_2R_no_aurora", .kind = p.kind, .radius = p.radius, .eye = {-1.7f, 1.0f, 0.2f}, .look = {0.f, 0.2f, 0.f}, .aurora = false},
        {.name = "inside_aurora_k7", .kind = p.kind, .radius = p.radius, .eye = in_aurora, .look = ahead, .activity = 7.f, .field = 1.f},
        {.name = "inside_aurora_no_aurora", .kind = p.kind, .radius = p.radius, .eye = in_aurora, .look = ahead, .aurora = false},
      };
      for(scene_t const & s: scenes)
        {
        bench_t const b{bench(g, s, bench_w, bench_h, 100)};
        std::printf("bench gpu: %s %s %ux%u: median %.3f ms, min %.3f ms, %.2f M pixels in the volume pass\n", p.name, s.name.c_str(),
                    bench_w, bench_h, b.median_ms, b.min_ms, b.megapixels);
        }
      }
    return 0;
    }

  struct kind_t
    {
    atmosphere_kind_e kind;
    char const * name;
    bool snow;
    };
  kind_t const kinds[]{
    {atmosphere_kind_e::neon, "neon", false},
    {atmosphere_kind_e::ammonia, "ammonia", true},
    {atmosphere_kind_e::carbon_dioxide, "co2", false},
    {atmosphere_kind_e::argon, "argon", false},
    {atmosphere_kind_e::methane, "methane", true},
    {atmosphere_kind_e::water, "water", false},
  };
  int pictures{}, compared{}, differed{};
  auto const picture{[&](scene_t const & s, std::vector<float> const & p)
                       {
                         std::vector<std::uint8_t> const rgb{to_rgb8(p, s.show)};
                         std::string const file{s.name + ".png"};
                         if(not write_png(widen(out_dir + "\\" + file), rgb))
                           {
                           std::printf("FAIL: render: %s not written\n", file.c_str());
                           ++failures;
                           }
                         ++pictures;
                         if(reference_dir.empty())
                           return;
                         if(update)
                           {
                           write_png(widen(reference_dir + "\\" + file), rgb);
                           return;
                           }
                         std::vector<std::uint8_t> ref;
                         if(not read_png(widen(reference_dir + "\\" + file), ref))
                           {
                           std::printf("note: no reference %s\n", file.c_str());
                           return;
                           }
                         // the driver may round a little differently: a mean under 1.5 of 255 and few pixels off by more than 24
                         double sum{};
                         std::size_t off{};
                         for(std::size_t i{}; i != rgb.size(); ++i)
                           {
                           int const d{std::abs(int{rgb[i]} - int{ref[i]})};
                           sum += d;
                           if(d > 24)
                             ++off;
                           }
                         double const mean{sum / static_cast<double>(rgb.size())};
                         double const share{static_cast<double>(off) / static_cast<double>(rgb.size())};
                         ++compared;
                         bool const same{mean < 1.5 and share < 0.005};
                         if(not same)
                           ++differed;
                         std::printf("%s: render: %s like its reference (mean difference %.2f, %.2f%% of channels off by more than 24)\n",
                                     same ? "ok  " : "FAIL", file.c_str(), mean, share * 100.);
                         if(not same)
                           ++failures;
                       }};

  for(kind_t const & k: kinds)
    {
    float const r{atmosphere_physics_table[static_cast<std::size_t>(k.kind) - 1u].radius_km * 1000.f};
    std::string const n{k.name};
    // by day from orbit: clouds on the lit disc, no aurora there
    scene_t day{.name = n + "_orbit_day", .kind = k.kind, .radius = r, .eye = {3.f, 0.9f, 0.6f}, .look = {0.f, 0.f, 0.f},
                .activity = 7.f, .field = 1.f};
    std::vector<float> const day_p{draw(g, day)};
    measure_t const day_m{measure(day_p)};
    scene_t day_no_aurora{day};
    day_no_aurora.aurora = false;
    measure_t const day_plain{measure(draw(g, day_no_aurora))};
    picture(day, day_p);
    // the next frame, 1/60 s on: nothing jumps
    scene_t next{day};
    next.seconds += 1.f / 60.f;
    double const step{mean_difference(day_p, draw(g, next))};
    // by night from orbit over the north, a storm (K 7) on a planet with a field of its own, then K 3, then none
    scene_t night{.name = n + "_orbit_night_k7", .kind = k.kind, .radius = r, .eye = {-2.2f, 1.3f, 0.3f}, .look = {0.f, 0.3f, 0.f},
                  .activity = 7.f, .field = 1.f, .show = 40.f};
    std::vector<float> const night_p{draw(g, night)};
    picture(night, night_p);
    scene_t quiet{night};
    quiet.name = n + "_orbit_night_k3";
    quiet.activity = 3.f;
    std::vector<float> const quiet_p{draw(g, quiet)};
    picture(quiet, quiet_p);
    scene_t dark{night};
    dark.aurora = false;
    measure_t const night_m{measure(night_p)}, quiet_m{measure(quiet_p)}, dark_m{measure(draw(g, dark))};
    scene_t nofield{night};
    nofield.name = n + "_orbit_night_nofield";
    nofield.field = 0.f;
    std::vector<float> const nofield_p{draw(g, nofield)};
    picture(nofield, nofield_p);
    // the limb by day, the layer edge on
    scene_t limb{.name = n + "_limb", .kind = k.kind, .radius = r, .eye = {1.25f, 0.05f, 0.f}, .look = {0.6f, 0.f, 0.8f},
                 .activity = 3.f, .field = 1.f};
    std::vector<float> const limb_p{draw(g, limb)};
    picture(limb, limb_p);
    // inside the layer by day, looking along it
    atmosphere_clouds_t const c{atmosphere_clouds(k.kind, 1.f)};
    float const inside_h{1.f + 0.5f * (c.bottom + c.top)};
    scene_t inside{.name = n + "_inside", .kind = k.kind, .radius = r, .eye = {inside_h, 0.f, 0.f}, .look = {inside_h, 0.05f, 0.3f},
                   .activity = 3.f, .field = 1.f};
    std::vector<float> const inside_p{draw(g, inside)};
    picture(inside, inside_p);
    // the weather and the storms as the day is just read (they come in over 4 s): drawn, nothing broken
    scene_t early{day};
    early.day_fade = 0.f;
    measure_t const early_m{measure(draw(g, early))};

    std::string const what{n + ": "};
    check(day_m.finite and night_m.finite and quiet_m.finite and measure(limb_p).finite and measure(inside_p).finite
            and measure(nofield_p).finite and early_m.finite,
          (what + "no NaN or infinity in any picture").c_str());
    // over the whole picture, the disc a fifth of it: a thin haze (neon) 0.001, a deck (water) 0.02 (2026-11-18)
    check(day_m.cover > 0.0005 and day_m.light > 0.0, (what + "clouds on the lit disc from orbit").c_str());
    check(std::fabs(day_m.light - day_plain.light) <= 0.05 * day_m.light + 1e-6,
          (what + "by day no aurora to speak of").c_str());
    // the aurora's light over the night side's own (the clouds' faint light there): at least three times as much
    check(night_m.light > 3.0 * dark_m.light + 1e-6, (what + "the aurora by night with a field of its own (K 7)").c_str());
    check(night_m.light > quiet_m.light, (what + "the aurora stronger at K 7 than at K 3").c_str());
    check(step < 0.02 * (day_m.light + day_m.cover) + 1e-6, (what + "the next frame differs a little only").c_str());
    std::printf("      %s cover %.4f light %.5f | night k7 %.6f k3 %.6f none %.6f | step %.6f | inside cover %.4f | limb cover %.4f\n",
                n.c_str(), day_m.cover, day_m.light, night_m.light, quiet_m.light, dark_m.light, step, measure(inside_p).cover,
                measure(limb_p).cover);

    if(k.snow)
      {
      // on the ground under the clouds, looking a little up: the crystals near the eye
      float const ground{1.f + 2.f / r};
      scene_t snow{.name = n + "_ground_snow", .kind = k.kind, .radius = r, .eye = {ground, 0.f, 0.f},
                   .look = {ground + 0.2f / r, 0.f, 1.f / r}, .activity = 3.f, .field = 1.f, .snow = true};
      std::vector<float> const snow_p{draw(g, snow)};
      picture(snow, snow_p);
      scene_t no_snow{snow};
      no_snow.snow = false;
      measure_t const snow_m{measure(snow_p)};
      check(snow_m.finite and mean_difference(snow_p, draw(g, no_snow)) > 0.0, (what + "the snow on the ground").c_str());
      }
    }
  std::printf("render: %d picture(s) in %s; %d compared with a reference, %d different%s\n", pictures, out_dir.c_str(), compared,
              differed, update ? " (references made again)" : "");
  std::printf(failures ? "FAILED (%d failure(s))\n" : "PASSED (%d failure(s))\n", failures);
  return failures ? 1 : 0;
  }
