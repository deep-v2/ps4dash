// M1 host test: map the ORIGINAL GD images with our own loader (not LoadLibrary),
// link them against each other, and report every import a shim must still provide.
// Executes no game code. Usage: loader_host <gd_dir>
#include "../src/pe_loader.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>

using namespace gdps4;

static std::vector<uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <gd_dir>\n", argv[0]); return 2; }
    std::string dir = argv[1];
    PlatformHooks hooks;
    hooks.alloc_exec = [](size_t n) { return VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE); };
    hooks.protect = [](void*, size_t, bool, bool, bool) { return true; };

    std::map<std::string, LoadedImage> loaded;   // key: lowercase dll name
    ImportResolver resolve = [&](const std::string& dll, const std::string& sym) -> void* {
        auto it = loaded.find(dll);
        return it == loaded.end() ? nullptr : it->second.find_export(sym);
    };

    // Dependency order: cocos2d has no RobTop deps, extensions needs cocos2d, exe needs both.
    const char* order[] = {"libcocos2d.dll", "libExtensions.dll", "GeometryDash.exe"};
    std::map<std::string, int> missing_per_dll;
    int failures = 0;
    for (const char* name : order) {
        auto file = read_file(dir + "\\" + name);
        if (file.empty()) { std::printf("FAIL %s: cannot read\n", name); return 1; }
        LoadedImage img;
        auto r = load_pe(file, name, hooks, resolve, img, /*allow_unresolved=*/true);
        if (!r.ok) { std::printf("FAIL %s\n", r.error.c_str()); return 1; }
        std::printf("OK   %-18s base=%p size=0x%zx exports=%zu tls_cb=%zu pdata=%u unresolved=%zu\n",
                    name, (void*)img.base, img.size, img.exports.size(), img.tls_callbacks.size(),
                    img.pdata_size / 12, r.unresolved.size());
        for (auto& u : r.unresolved) {
            std::string d = u.substr(0, u.find('!'));
            missing_per_dll[d]++;
            if (d == "libcocos2d.dll" || d == "libextensions.dll") {   // must never happen
                std::printf("     BROKEN LINK %s\n", u.c_str()); failures++;
            }
        }
        std::string key = name; std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        loaded[key] = std::move(img);
    }
    std::printf("\nImports still needing a shim, by DLL:\n");
    int total = 0;
    for (auto& [d, n] : missing_per_dll) { std::printf("  %5d  %s\n", n, d.c_str()); total += n; }
    std::printf("  %5d  TOTAL\nRobTop cross-image links broken: %d\n", total, failures);
    return failures ? 1 : 0;
}
