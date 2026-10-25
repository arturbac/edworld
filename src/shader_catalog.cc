// edworld — the catalog of the game's shaders (shader_catalog.h): noted at creation, written by a thread of its own
// once the game has created none for a few seconds (loading, the shaders' build at start), so the game's threads only
// copy the bytes.
#include "shader_catalog.h"

#if defined(EDWORLD_EHT)
#include "panel_math.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace edworld
  {
  namespace
    {
    struct noted_t
      {
      shader_stage_e stage;
      std::uint64_t hash;
      std::vector<std::uint8_t> bytes;  ///< the bytecode; for a layout, its elements as text
      };

    constexpr ULONGLONG quiet_ms{5000};  ///< no shader created for this long: the batch is written

    std::mutex lock;
    std::unordered_set<std::uint64_t> seen;  ///< this session's, so each is copied once
    std::vector<noted_t> pending;
    ULONGLONG last_note{};
    bool started{};
    std::atomic<bool> broken{};
    std::uint32_t created[8]{};  ///< by stage, this session

    auto widen(char const * s) -> std::wstring
      {
      std::wstring w;
      for(; *s; ++s)
        w += static_cast<wchar_t>(static_cast<unsigned char>(*s));
      return w;
      }

    auto exists(std::wstring const & path) noexcept -> bool
      {
      return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
      }

    ///\brief the batch to the build's directory: a shader already there (an earlier session) is skipped
    auto write(std::vector<noted_t> const & batch) -> void
      {
      std::wstring const root{settings().dir + L"\\edworld_shaders"};
      std::wstring const dir{root + L"\\" + widen(game_build_key())};
      CreateDirectoryW(root.c_str(), nullptr);
      CreateDirectoryW(dir.c_str(), nullptr);
      FILE * catalog{};
      std::uint32_t added{}, known{};
      std::size_t bytes{};
      for(noted_t const & n: batch)
        {
        wchar_t name[64];
        std::swprintf(name, std::size(name), L"\\%hs_%016llX.%ls", stage_name(n.stage), static_cast<unsigned long long>(n.hash),
                      n.stage == shader_stage_e::layout ? L"txt" : L"dxbc");
        std::wstring const path{dir + name};
        if(exists(path))
          {
          ++known;
          continue;
          }
        if(FILE * const f{_wfopen(path.c_str(), L"wb")})
          {
          std::fwrite(n.bytes.data(), 1, n.bytes.size(), f);
          std::fclose(f);
          }
        else
          {
          log_line("shaders: could not write %ls", path.c_str());
          continue;
          }
        if(not catalog)
          catalog = _wfopen((dir + L"\\catalog.tsv").c_str(), L"ab");
        if(catalog)
          {
          if(n.stage == shader_stage_e::layout)
            std::fprintf(catalog, "%016llX\til\t%zu\t-\t%.*s\t-\t-\t0\n", static_cast<unsigned long long>(n.hash), n.bytes.size(),
                         static_cast<int>(n.bytes.size()), reinterpret_cast<char const *>(n.bytes.data()));
          else
            {
            dxbc_shape_t const s{dxbc_shape(n.bytes.data(), n.bytes.size())};
            std::fprintf(catalog, "%016llX\t%s\t%zu\t%u.%u\t%s\t%s\t%s\t%u\t%s\n", static_cast<unsigned long long>(n.hash),
                         stage_name(n.stage), n.bytes.size(), s.major, s.minor, s.inputs.empty() ? "-" : s.inputs.c_str(),
                         s.outputs.empty() ? "-" : s.outputs.c_str(), s.decls.empty() ? "-" : s.decls.c_str(), s.instructions,
                         s.ok ? s.chunks.c_str() : "UNREAD");
            }
          }
        ++added;
        bytes += n.bytes.size();
        }
      if(catalog)
        std::fclose(catalog);
      log_line("shaders: %u new to edworld_shaders\\%s (%zu bytes), %u there from before; created this session: vs %u ps %u "
               "cs %u gs %u hs %u ds %u layouts %u", added, game_build_key(), bytes, known,
               created[static_cast<int>(shader_stage_e::vertex)], created[static_cast<int>(shader_stage_e::pixel)],
               created[static_cast<int>(shader_stage_e::compute)], created[static_cast<int>(shader_stage_e::geometry)],
               created[static_cast<int>(shader_stage_e::hull)], created[static_cast<int>(shader_stage_e::domain)],
               created[static_cast<int>(shader_stage_e::layout)]);
      }

    auto WINAPI writer(void *) -> DWORD
      {
      for(;;)
        {
        Sleep(1000);
        std::vector<noted_t> batch;
        {
        std::lock_guard const g{lock};
        if(pending.empty() or GetTickCount64() - last_note < quiet_ms)
          continue;
        batch.swap(pending);
        }
        try
          {
          write(batch);
          }
        catch(std::exception const & e)
          {
          log_line("shaders: the catalog stopped (%s)", e.what());
          broken.store(true);
          return 0;
          }
        }
      }

    auto note(shader_stage_e stage, std::uint64_t hash, std::uint8_t const * bytes, std::size_t length) noexcept -> void
      {
      if(broken.load(std::memory_order_relaxed))
        return;
      try
        {
        std::lock_guard const g{lock};
        ++created[static_cast<int>(stage)];
        last_note = GetTickCount64();
        if(not seen.insert(hash).second)
          return;
        pending.push_back(noted_t{stage, hash, std::vector<std::uint8_t>(bytes, bytes + length)});
        if(not started)
          {
          started = true;
          if(HANDLE const t{CreateThread(nullptr, 0, &writer, nullptr, 0, nullptr)})
            CloseHandle(t);
          else
            {
            log_line("shaders: no thread for the catalog; none kept");
            broken.store(true);
            }
          }
        }
      catch(std::exception const & e)
        {
        log_line("shaders: the catalog stopped (%s)", e.what());
        broken.store(true);
        }
      }
    }  // namespace

  auto shader_catalog_note(shader_stage_e stage, void const * bytecode, std::size_t length, std::uint64_t hash) noexcept -> void
    {
    if(not settings().shader_catalog or not bytecode)
      return;
    note(stage, hash, static_cast<std::uint8_t const *>(bytecode), length);
    }

  auto shader_catalog_note_layout(D3D11_INPUT_ELEMENT_DESC const * elements, UINT count) noexcept -> void
    {
    if(not settings().shader_catalog or not elements)
      return;
    char text[4096];
    int at{};
    for(UINT i{}; i != count and at < static_cast<int>(sizeof text) - 128; ++i)
      {
      D3D11_INPUT_ELEMENT_DESC const & e{elements[i]};
      int const n{std::snprintf(text + at, sizeof text - static_cast<std::size_t>(at), "%s%s%u:f%u:s%u:o%u:%s%u", i ? " " : "",
                                e.SemanticName ? e.SemanticName : "?", e.SemanticIndex, static_cast<unsigned>(e.Format), e.InputSlot,
                                e.AlignedByteOffset, e.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? "i" : "v", e.InstanceDataStepRate)};
      if(n < 0)
        break;
      at = std::min(at + n, static_cast<int>(sizeof text) - 1);
      }
    auto const * const bytes{reinterpret_cast<std::uint8_t const *>(text)};
    note(shader_stage_e::layout, fnv1a64(bytes, static_cast<std::size_t>(at)), bytes, static_cast<std::size_t>(at));
    }
  }  // namespace edworld
#endif
