# GD 2.208 → PS4: Porting Strategy (Phase 1–2 report)

## 1. What's in the install (measured, not guessed)

| Component | Facts |
|---|---|
| `GeometryDash.exe` | PE32+ x86-64, MSVC 14.x, **has .reloc** (loadable at any base), no TLS. Very thin Win32 surface: 26 kernel32, 4 user32 (clipboard), 4 advapi32, 1 shell32, 4 GL + 2 glew, 66 FMOD, 8 Steam. Everything else goes through **811 imports from libcocos2d.dll**. |
| `libcocos2d.dll` | RobTop's modified cocos2d-x 2.2.x, 5875 exports, has TLS + .reloc. Embeds a GLFW-style Win32 window layer (77 user32, 21 gdi32 for window + CCLabelTTF font rendering), GL via glew (63), zlib/png/jpeg/tiff, pthreads, XInput. |
| `libExtensions.dll` | cocos2d extensions (CCHttpClient via libcurl, websockets, sqlite, CCScale9Sprite, color picker). |
| Third-party DLLs | fmod, glew32, zlib1, libpng16, jpeg62, tiff, libcurl, sqlite3, pthreadVC*, websockets, uv, openssl 3, XInput. All have open-source equivalents except FMOD. |
| Resources | ~9000 files: 1002 png, 191 plist, 186 fnt, 34 mp3, 31 ogg + songs/sfx. Fully platform-neutral, reusable as-is. |
| Geode | `Geode.dll` + 118 `.geode` mods. Mods are Windows DLLs inside zips, hooking GD by address. |
| Not used | `ffmpeg/ffplay/ffprobe.exe`, `gd-request-viewer`, Steam emulator files. Steam is dropped on PS4. |

Full per-symbol inventory: [imports.csv](imports.csv) (2137 imports, produced by `tools/import_inventory.py`).

## 2. The core constraint

GD is **closed source**. There is no game source code in the install (Geode ships bindings = function addresses + signatures, not implementations). So "source-level adaptation" of the game logic is not possible; reimplementing it would be exactly the clone you said not to make.

What *is* possible: PS4 is **x86-64 (AMD Jaguar)**, the same ISA as the Windows binary. The game's machine code can run natively on the PS4 CPU — no emulation, no Wine. What has to change is only what's *around* the code: the loader, the ABI boundary and the OS/graphics/audio APIs.

## 3. Chosen architecture: native PE host ("re-hosting")

```
 PS4 SELF/eboot.bin  (OpenOrbis, FreeBSD/SysV ABI)
 ├─ PE loader          maps the ORIGINAL GeometryDash.exe, libcocos2d.dll, libExtensions.dll
 ├─ Win64 runtime      TEB/gs-base, TLS, SEH unwinder (.pdata), heap, threads, files
 ├─ Shim DLLs          ms_abi functions exported under Windows DLL names:
 │    kernel32/advapi32/shell32 → libkernel
 │    opengl32/glew32           → Piglet (GLES2, libScePigletv2VSH)
 │    user32/gdi32/xinput       → fake window + DualShock 4 (scePad) + stb_truetype for CCLabelTTF
 │    fmod                      → sceAudioOut mixer + minimp3/stb_vorbis
 │    zlib/png/jpeg/tiff/sqlite/curl/pthreads → real libs compiled for PS4, ms_abi wrappers
 │    steam_api64               → stub
 │    MSVC CRT (ucrtbase/msvcp140/vcruntime) → load Microsoft redist PEs or implement
 └─ Assets             /data/gd/Resources (copied from your install, unmodified)
```

This uses 100% of RobTop's real code (game logic, physics, cocos2d rendering paths, menus, level format) unchanged, which is the only way to keep physics/timing/level compatibility bit-exact.

## 4. Hard problems, honestly ranked

1. **gs-segment TEB.** MSVC code reads `gs:[0x30]`, `gs:[0x58]` (TLS array). PS4 userland uses `fs` for TLS; setting the gs base needs `sysarch(AMD64_SET_GSBASE)` which may be blocked — likely needs a GoldHEN kernel patch or binary rewriting of gs accesses at load. **Biggest risk; must be tested on hardware first.**
2. **Executable memory.** Homebrew needs RWX/JIT mappings (`sceKernelJitCreateSharedMemory` or mmap exec with jailbreak patches). Known to work under GoldHEN.
3. **C++ exceptions/SEH.** MSVC `throw` → `RaiseException` → table-based unwinding over `.pdata`. Must implement `RtlLookupFunctionEntry`/`RtlVirtualUnwind`/dispatcher (Wine's code is the reference).
4. **ABI bridging.** Every shim export must be `__attribute__((ms_abi))` (clang supports this on SysV targets). Varargs (printf-family) and `long` = 32-bit on Win64 need care.
5. **OpenGL → GLES2.** cocos2d 2.x is GLES2-era, so mostly direct. Desktop-only calls (`glDrawBuffer`, some formats) need mapping. Shaders: Piglet runtime compile requires the shader compiler module to load.
6. **FMOD.** 66 functions; no homebrew FMOD exists. Reimplement the subset GD uses (streams, channels, volume, speed for practice music, effects maybe).
7. **Input.** GD's touch/click model → DualShock 4: X/R2 = jump, touchpad = cursor for menus.
8. **Save data.** CCFileUtils writable path → `/data/gd/save/` (CCGameManager.dat / CCLocalLevels.dat format unchanged).

## 5. Geode (Phase 4, after vanilla)

Geode's loader + mods are Windows DLLs that hook GD by **Windows addresses** — that's actually favorable here, because we run the *Windows* GD binary, so all Geode Windows bindings stay valid. The same PE loader can load `Geode.dll` and mod DLLs; their extra imports (more Win32, DbgHelp, etc.) get shimmed. Hooking (tulip-hook, writing jmp patches) works once code pages are writable. So the path is: vanilla first, then load Geode through the same host — no recompilation of mods needed in principle.

## 6. Toolchain status on this PC

- OpenOrbis PS4Toolchain: **present** at `C:\OpenOrbis\OpenOrbis\PS4Toolchain` (create-fself, create-gp4, PkgTool → PKG packaging covered).
- LLVM/clang 23.1.5: **installed** (`C:\Program Files\LLVM`). Host build works.

### Build & run M1 host test
```bash
clang++ -std=c++17 -O1 -Wall -Wextra -o build/loader_host.exe loader/src/pe_loader.cpp loader/host/host_main.cpp
./build/loader_host.exe "<path to your GD 2.208 folder>"
```
- The PATH also references an official Sony ORBIS SDK; this project does not use it — OpenOrbis only.

## 7. Milestones

| # | Milestone | Testable where | Status |
|---|---|---|---|
| M0 | Inventory of all imports + classification | PC | **Done** (imports.csv) |
| M1 | PE loader maps GD.exe/libcocos2d.dll, relocs, exports, imports, reports unresolved | PC (host build) | **Done**: builds with clang 23, `-Wall -Wextra` clean. Maps all 3 images, applies relocs, 1239/1239 RobTop cross-image imports resolve, 898 left for shims. No game code executed yet. |
| M2 | Host test: run GD on Windows via our loader with pass-through shims (validates loader/TEB/SEH logic) | PC | **In progress.** Original GeometryDash.exe + libcocos2d.dll + libExtensions.dll run from our loader, open the window, create the GL context and render the real loading screen. Loading then stalls (low CPU, no crash). |

### M2 findings (each one carries over to the PS4 runtime)
- **Module identity:** the game derives its save folder from its exe name and GLFW needs a real `HINSTANCE`. `GetModuleHandle*`/`GetModuleFileName*`/`GetProcAddress`/`LoadLibrary` are shimmed.
- **Save data:** `SHGetFolderPathA` is shimmed to a sandbox (`sandbox_localappdata/`), so the user's real saves are never touched. On PS4 this becomes `/data/gd`.
- **Steam:** the game exits unless `SteamAPI_Init` succeeds, then calls virtual methods on Steam interfaces. Offline Steam stub: Init = OK, every interface is a dummy whose methods return 0.
- **Static TLS:** swapping ntdll's per-thread TLS arrays corrupts the heap (on thread exit and on TLS growth). Solved on the Windows host with *TLS donor DLLs*: one tiny DLL per mapped image gets a real slot from ntdll, and its template is overwritten with the image's template. On PS4 we own the TEB entirely, so this is host-only.
- **MSVC thread-safe statics** depend on the TLS epoch value (`INT_MIN` in the template). A wrong TLS block shows up as constructors silently skipped and null objects.
- **Vanilla redirects:** Geode's `XInput1_4.dll` proxy and the request viewer's `libcurl.dll` proxy are bypassed (System32 XInput, `libcurl_orig.dll`).

### Build & run M2
```bash
clang++ -O1 -shared -o build/tls_donor.dll loader/host/tls_donor.cpp
clang++ -std=c++17 -O1 -g -Wall -Wextra -o build/loader_run.exe loader/src/pe_loader.cpp loader/host/run_main.cpp -luser32
./build/loader_run.exe "<path to your GD 2.208 folder>"   # logs: loader_run.log, exceptions.log, glfw_errors.log
```
| M3 | PS4 hello: eboot that maps an image RWX, sets gs base, calls into PE code | PS4 | **Blocked on clang + hardware test** |
| M4 | kernel32/CRT shims, DllMain/TLS, reach `cocos2d::CCApplication::run` | PS4 | Pending |
| M5 | Piglet GL + fake window → first rendered frame (menu) | PS4 | Pending |
| M6 | FMOD shim, DS4 input, save data → playable levels | PS4 | Pending |
| M7 | PKG + install docs | PS4 | Pending |
| M8 | Geode.dll loading | PS4 | Pending |

## 8. Legal note

Only your own install is used as input; no game files are redistributed in this project. The PKG you build will contain your copy of the game's binaries and assets — keep it for personal use.
