// tools/doc.sh's program: the build named by its argument (edworld.dll or edworld_eht.dll) loaded beside it and its
// export edworld_write_settings_doc called, which makes the log and the ini as at the game's start with no device, no
// hook and no next d3d11. Exit code 0 when the export said done.
#include <windows.h>

#include <cstdio>

auto wmain(int argc, wchar_t ** argv) -> int
  {
  if(argc != 2)
    {
    std::printf("FAIL: usage: edworld_doc <edworld.dll | edworld_eht.dll>\n");
    return 1;
    }
  HMODULE const plugin{LoadLibraryW(argv[1])};
  if(not plugin)
    {
    std::printf("FAIL: %ls not loaded (error %lu)\n", argv[1], GetLastError());
    return 1;
    }
  using write_t = int(WINAPI *)();
  auto const write{reinterpret_cast<write_t>(reinterpret_cast<void *>(GetProcAddress(plugin, "edworld_write_settings_doc")))};
  if(not write)
    {
    std::printf("FAIL: no export edworld_write_settings_doc in %ls\n", argv[1]);
    return 1;
    }
  int const rc{write()};
  std::printf(rc == 0 ? "ok  : edworld_write_settings_doc\n" : "FAIL: edworld_write_settings_doc said %d\n", rc);
  return rc;
  }
