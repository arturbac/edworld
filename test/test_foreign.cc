// A stand-in for an overlay (Epic's EOS, Discord's, Steam's) that makes a graphics device of its own in the game's
// process, from a folder other than the game's: d3d11.dll by import, which is edworld. edworld must not follow it.
#include <windows.h>

#include <d3d11.h>

extern "C" __declspec(dllexport) HRESULT foreign_make_device()
  {
  ID3D11Device * device{};
  HRESULT const hr{
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)
  };
  if(device)
    device->Release();
  return hr;
  }
