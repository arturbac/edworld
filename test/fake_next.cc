// A stand-in for a chained proxy (3Dmigoto/EDHM): its D3D11CreateDevice asks for "d3d11.dll" BY NAME, which
// in the game directory is edworld itself. Without edworld's depth guard this recurses until the stack runs out.
#include <windows.h>

#include <d3d11.h>

using create_device_fn = HRESULT(WINAPI *)(
  IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, D3D_FEATURE_LEVEL const *, UINT, UINT, ID3D11Device **,
  D3D_FEATURE_LEVEL *, ID3D11DeviceContext **
);

static LONG calls{};

extern "C" __declspec(dllexport) LONG fake_next_calls() { return calls; }

extern "C" HRESULT WINAPI fake_D3D11CreateDevice(
  IDXGIAdapter * a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, D3D_FEATURE_LEVEL const * l, UINT n, UINT v,
  ID3D11Device ** d, D3D_FEATURE_LEVEL * o, ID3D11DeviceContext ** c)
  {
  InterlockedIncrement(&calls);
  HMODULE const by_name{LoadLibraryW(L"d3d11.dll")};
  auto const create{reinterpret_cast<create_device_fn>(GetProcAddress(by_name, "D3D11CreateDevice"))};
  return create ? create(a, t, s, f, l, n, v, d, o, c) : E_FAIL;
  }
