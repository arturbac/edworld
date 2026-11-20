// A stand-in for 3Dmigoto (EDHM) standing before edworld in the chain: its version resource names 3Dmigoto
// (fake_migoto.rc), its D3D11CreateDevice asks for "d3d11.dll" by name, which in the test's directory is edworld, and
// foreign_make_device makes a device through it, as the game's call through EDHM would (tools/test.sh, EDWORLD_TEST_PASSER).
#include <windows.h>

#include <d3d11.h>

using create_device_fn = HRESULT(WINAPI *)(
  IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, D3D_FEATURE_LEVEL const *, UINT, UINT, ID3D11Device **,
  D3D_FEATURE_LEVEL *, ID3D11DeviceContext **
);

extern "C" HRESULT WINAPI fake_migoto_D3D11CreateDevice(
  IDXGIAdapter * a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, D3D_FEATURE_LEVEL const * l, UINT n, UINT v,
  ID3D11Device ** d, D3D_FEATURE_LEVEL * o, ID3D11DeviceContext ** c)
  {
  HMODULE const by_name{LoadLibraryW(L"d3d11.dll")};
  auto const create{reinterpret_cast<create_device_fn>(GetProcAddress(by_name, "D3D11CreateDevice"))};
  return create ? create(a, t, s, f, l, n, v, d, o, c) : E_FAIL;
  }

extern "C" __declspec(dllexport) HRESULT foreign_make_device()
  {
  ID3D11Device * device{};
  HRESULT const hr{fake_migoto_D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                                 &device, nullptr, nullptr)};
  if(device)
    device->Release();
  return hr;
  }
