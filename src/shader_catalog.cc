// edworld — the catalog of the game's shaders (shader_catalog.h): noted at creation, written by a thread of its own
// once the game has created none for a few seconds (loading, the shaders' build at start), so the game's threads only
// copy the bytes.
#include "shader_catalog.h"

#if defined(EDWORLD_EHT)
#include "panel_math.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <string_view>

#include <stralgo/stralgo.h>

namespace edworld
  {
  using namespace std::string_view_literals;

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
      std::wstring const root{stralgo::stl::merge(settings().dir, L"\\edworld_shaders"sv)};
      std::wstring const dir{stralgo::stl::merge(root, L'\\', widen(game_build_key()))};
      CreateDirectoryW(root.c_str(), nullptr);
      CreateDirectoryW(dir.c_str(), nullptr);
      std::string catalog;  // the batch's lines, added to catalog.tsv at once
      std::uint32_t added{}, known{};
      std::size_t bytes{};
      for(noted_t const & n: batch)
        {
        wchar_t name[64];
        format_into(name, L"\\{}_{:016X}.{}", utf8(stage_name(n.stage)), static_cast<unsigned long long>(n.hash),
                      n.stage == shader_stage_e::layout ? L"txt" : L"dxbc");
        std::wstring const path{stralgo::stl::merge(dir, std::wstring_view{name})};
        if(exists(path))
          {
          ++known;
          continue;
          }
        // a file not written whole is removed (and logged), so a later session writes it again
        if(not write_whole_file(path.c_str(), n.bytes.data(), n.bytes.size()))
          continue;
        char line[8192];
        int length{};
        if(n.stage == shader_stage_e::layout)
          length = static_cast<int>(format_into(line, "{:016X}\til\t{}\t-\t{}\t-\t-\t0\n", static_cast<unsigned long long>(n.hash),
                                                n.bytes.size(),
                                                std::string_view{reinterpret_cast<char const *>(n.bytes.data()), n.bytes.size()}));
        else
          {
          dxbc_shape_t const s{dxbc_shape(n.bytes.data(), n.bytes.size())};
          length = static_cast<int>(format_into(line, "{:016X}\t{}\t{}\t{}.{}\t{}\t{}\t{}\t{}\t{}\n", static_cast<unsigned long long>(n.hash),
                                 stage_name(n.stage), n.bytes.size(), s.major, s.minor, s.inputs.empty() ? "-" : s.inputs,
                                 s.outputs.empty() ? "-" : s.outputs, s.decls.empty() ? "-" : s.decls, s.instructions,
                                 s.ok ? s.chunks.c_str() : "UNREAD"));
          }
        if(length > 0)
          catalog.append(line, std::min<std::size_t>(static_cast<std::size_t>(length), sizeof line - 1));
        ++added;
        bytes += n.bytes.size();
        }
      if(not catalog.empty() and not append_to_file(stralgo::stl::merge(dir, L"\\catalog.tsv"sv).c_str(), catalog.data(), catalog.size()))
        log_line("shaders: catalog.tsv not written whole (error {})", GetLastError());
      log_line("shaders: {} new to edworld_shaders\\{} ({} bytes), {} there from before; created this session: vs {} ps {} "
               "cs {} gs {} hs {} ds {} layouts {}", added, game_build_key(), bytes, known,
               created[static_cast<int>(shader_stage_e::vertex)], created[static_cast<int>(shader_stage_e::pixel)],
               created[static_cast<int>(shader_stage_e::compute)], created[static_cast<int>(shader_stage_e::geometry)],
               created[static_cast<int>(shader_stage_e::hull)], created[static_cast<int>(shader_stage_e::domain)],
               created[static_cast<int>(shader_stage_e::layout)]);
      }

    auto writer_body(void *) -> DWORD
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
          log_line("shaders: the catalog stopped ({})", e.what());
          broken.store(true);
          return 0;
          }
        }
      }

    auto WINAPI writer(void *) -> DWORD { return run_guarded_thread("shader catalog", &writer_body, nullptr); }

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
        log_line("shaders: the catalog stopped ({})", e.what());
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
      at += static_cast<int>(format_into(text + at, sizeof text - static_cast<std::size_t>(at), "{}{}{}:f{}:s{}:o{}:{}{}", i ? " " : "",
                                         e.SemanticName ? e.SemanticName : "?", e.SemanticIndex, static_cast<unsigned>(e.Format), e.InputSlot,
                                         e.AlignedByteOffset, e.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? "i" : "v",
                                         e.InstanceDataStepRate));
      }
    auto const * const bytes{reinterpret_cast<std::uint8_t const *>(text)};
    note(shader_stage_e::layout, fnv1a64(bytes, static_cast<std::size_t>(at)), bytes, static_cast<std::size_t>(at));
    }
  }  // namespace edworld
#endif
