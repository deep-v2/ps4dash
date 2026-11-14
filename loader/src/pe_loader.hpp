// Minimal x86-64 PE32+ loader. Maps the ORIGINAL GeometryDash.exe / libcocos2d.dll
// images into memory without the Windows loader. Platform specifics (allocating
// executable memory) are injected via PlatformHooks so the same code runs on a
// Windows test host and on PS4 (OpenOrbis).
// STATUS: written, NOT yet compiled (no C++ compiler on the dev machine yet).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gdps4 {

struct PlatformHooks {
    // Reserve+commit size bytes RWX (or RW then protect later). Return nullptr on failure.
    std::function<void*(size_t size)> alloc_exec;
    std::function<bool(void* p, size_t size, bool r, bool w, bool x)> protect;
};

// Resolver: (dll name lowercase, symbol name or "#ordinal") -> address, or nullptr.
using ImportResolver = std::function<void*(const std::string& dll, const std::string& sym)>;

struct LoadedImage {
    std::string name;
    uint8_t* base = nullptr;
    size_t size = 0;
    uint64_t entry_rva = 0;
    bool is_dll = false;
    std::unordered_map<std::string, uint32_t> exports;   // name -> rva
    std::unordered_map<uint32_t, uint32_t> ord_exports;   // ordinal -> rva
    std::vector<uint64_t> tls_callbacks;                  // absolute VAs
    uint64_t tls_index_va = 0, tls_raw_start = 0, tls_raw_end = 0;
    uint32_t tls_zero_fill = 0;
    uint32_t pdata_rva = 0, pdata_size = 0;               // needed for SEH/C++ exceptions
    void* find_export(const std::string& sym) const;
};

struct LoadResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> unresolved;                  // "dll!sym"
};

LoadResult load_pe(const std::vector<uint8_t>& file, const std::string& name,
                   const PlatformHooks& hooks, const ImportResolver& resolve,
                   LoadedImage& out, bool allow_unresolved);

} // namespace gdps4
