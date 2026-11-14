// fmod.dll for the PS4 runner: see fmod_shim.cpp.
#pragma once
#include <functional>
#include <string>

namespace gdps4::fmod {
struct Hooks {
    void (*log)(const char*) = nullptr;
    std::function<std::string(const std::string& win_path)> map_path;   // game path -> PS4 path
};
void init(const Hooks& h);
void* lookup(const std::string& sym);   // fmod.dll export, or nullptr
}  // namespace gdps4::fmod
