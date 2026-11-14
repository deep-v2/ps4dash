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
| M2 | Host test: run GD on Windows via our loader with pass-through shims (validates loader/TEB/SEH logic) | PC | **Done (2026-11-14).** Original GeometryDash.exe + libcocos2d.dll + libExtensions.dll run from our loader with pass-through shims. Hand-tested by the project owner: menus, playing levels and sound worked normally. Logs from the session: 0 exceptions, 0 GL errors over ~6,600 frames, all main levels read, save written to the sandbox. |

### The "loading stall" was GD's pause-on-focus-loss
The window sometimes showed the loading frame for 40+ s. Diagnosis (watchdog, `GDPS4_WATCHDOG_MS`):
- `LoadingLayer` is replaced by `MenuLayer` about 1 s after start; no thread is blocked.
- `CCDirector::isPaused()` flips to 1 exactly when the window loses the foreground; frames stop and the
  window keeps its last image. A real click on the window resumes it; losing focus again pauses again.
- Something on the dev desktop takes focus ~2 s after launch, so the game paused itself. This is the game's own
  behavior, not a loader bug, and doesn't apply to PS4 (no window focus there).
- Side finding: shims that return a `FILE*` must call **ucrtbase's** functions. A `FILE*` from the loader's own
  CRT makes the game's CRT fast-fail (`0xC0000409`).

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
./build/loader_run.exe "<path to your GD 2.208 folder>"   # logs: loader_run.log, exceptions.log, glfw_errors.log, files.log, gl.log
# Optional diagnostics: GDPS4_WATCHDOG_MS=500 GDPS4_WATCH_SECS=10 [GDPS4_FOCUS_AT=5] -> stacks.log
```
| M3 | PS4 hello: eboot that maps an image RWX, sets gs base, calls into PE code | PS4 | **Done (2026-11-14), all checks PASS on the owner's jailbroken PS4.** RWX flexible memory executes; `sysarch(AMD64_SET_GSBASE)` works and MSVC-style `gs:[0x30]` reads hit our fake TEB; our PE loader maps libcocos2d.dll on PS4 (5875 exports); RobTop's `CCPoint` ctor, `operator+` and `ccpLength` return correct results via the MS x64 ABI. |

### M3 hardware results
```
[PASS] exec_memory: alloc=sceKernelMapNamedFlexibleMemory RWX at 0x200368000, call returned 42, signal=0
[PASS] gs_sysarch: sysarch(AMD64_SET_GSBASE) ret=0 signal=0
[PASS] gs_teb_read: gs:[0x30]=0x51c430 expected 0x51c430 signal=0
[PASS] pe_load: base=0x2003bc000 size=0x1c5000 exports=5875 err= signal=0
[PASS] robtop_code: CCPoint(3,4)=(3,4) +(1.5,-2)=(4.5,2) ccpLength=5 signal=0
```
Risk #1 (gs-segment TEB) and #2 (executable memory) from section 4 are resolved: no kernel patch or
instruction rewriting needed. Still open: the gs base is per thread, so every thread the game creates must
set it (handled in the CreateThread shim in M4).

### M3 probe: build, install, run
Build (Windows, Git Bash, LLVM + OpenOrbis, `OO_PS4_TOOLCHAIN` set):
```bash
bash ps4/m3_probe/build.sh   # -> ps4/m3_probe/build/IV0000-GDPS00003_00-GDPS4M3PROBE0000.pkg
```
On the PS4 (GoldHEN or similar homebrew enabler):
1. Copy `libcocos2d.dll` from your GD 2.208 folder to `/data/gdps4/libcocos2d.dll` (FTP).
2. Install the PKG (GoldHEN Package Installer / Debug Settings), then start "gdps4 M3 probe".
3. A notification lists the results; full details are in `/data/gdps4/m3_results.txt`.

What it tests: `exec_memory` (RWX map + run), `gs_sysarch` / `gs_wrgsbase` (setting the gs base),
`gs_teb_read` (reading `gs:[0x30]` like MSVC code), `pe_load` (our PE loader mapping libcocos2d.dll on
PS4) and `robtop_code` (calling RobTop's `CCPoint(3,4)`, `operator+` and `ccpLength` via the MS x64 ABI;
expects (3,4), (4.5,2) and 5).
Notes: `-std=gnu++17` is required (OpenOrbis libc++ needs BSD extensions), and `PkgTool.Core` needs
`DOTNET_ROLL_FORWARD=Major` on machines without .NET Core 3.0.
| M4 | kernel32/CRT shims, DllMain/TLS, reach `cocos2d::CCApplication::run` | PS4 | **Done on console (2026-11-14).** All 15 images load on PS4, all 14 `DllMain`s succeed (MS CRT/STL, media libs, libcocos2d, libExtensions), `GeometryDash.exe` runs its startup and calls into the controller API. Open: SEH dispatcher on PS4 (no exception thrown so far). |

### M4 approach: Microsoft's own runtimes + a portable Win32 layer
- **M4a:** `ucrtbase`, `msvcp140`, `vcruntime140(_1)` (from System32) and the game's zlib/libpng/jpeg/tiff/
  pthreads/sqlite/glew/lzma/turbojpeg are mapped by **our** loader too (recursive loading, apiset mapping,
  forwarded exports, dependency-first `DllMain`). 15 images, 0 unresolved imports, menu at 60 fps on PC.
  This replaces ~650 CRT/STL functions with 253 OS functions (`docs/win32_core_todo.txt`).
- **M4b:** `win32/` is a portable kernel32/advapi32/shell32 (all 253) in C++17 with hand-written Windows types
  (`WCHAR` = `char16_t`) and MS-ABI exports, over a tiny backend (`platform_posix.cpp` for PS4,
  `platform_win.cpp` for PC validation). With `GDPS4_WIN32=1` the PC test routes every kernel import there:
  the game reaches `MenuLayer` at 60 fps; the only kernel calls still going to Windows are the 8
  exception-dispatch functions (by design on PC).
- Compiles for PS4 (OpenOrbis) as well; not yet run on the console.

### M4c: PS4 runner (`ps4/runner/`)
Loads the same 15 images on PS4 from `/data/gdps4/game`, links them against the Win32 layer, builds a Win32
TEB + static-TLS array per thread (gs base), runs TLS callbacks and `DllMain`s in Windows order, then enters
`GeometryDash.exe`. Path mapping: `C:\GD\...` -> `/data/gdps4/game/...` (case-insensitive lookup),
`C:\GD\save` (the game's LocalAppData) -> `/data/gdps4/save`. Imports from DLLs not implemented yet
(user32, gdi32, opengl32, fmod, ws2_32, ...) resolve to stubs that **stop the run and report the function
name**; the first one reached is the next thing to build. Crashes report image+offset.
```bash
bash tools/stage_ps4.sh "<GD 2.208 folder>"   # -> stage/game (19 MB: game binaries + MS runtimes)
bash ps4/runner/build.sh                      # -> ps4/runner/build/IV0000-GDPS00004_00-GDPS4RUNNER00000.pkg
```
On the PS4: upload the contents of `stage/game` to `/data/gdps4/game`, install the PKG, start
"Geometry Dash (gdps4)". Result: notification + `/data/gdps4/runner.log`.
Console runs (owner's PS4):
1. Stopped in `libExtensions` `DllMain` at `libcurl!curl_easy_init`. Fixed with an **offline network**
   shim: curl's API with every transfer failing (`CURLE_COULDNT_CONNECT`), websocket contexts not created.
   GD treats this like no internet (online levels/leaderboards/sync unavailable) until a real PS4
   network backend exists.
2. All `DllMain`s OK, entered `GeometryDash.exe`, stopped at `xinput1_4!#2` (`XInputGetState`).
   Implemented on `scePad`: the DualShock 4 is XInput controller 0 (Cross=A, Circle=B, Square=X,
   Triangle=Y, L1/R1, L3/R3, D-pad, OPTIONS=Start, touchpad click=Back, sticks, L2/R2 triggers).
3. Pad opens (`scePadOpen` returns a handle), the game queries its save folder (-> `/data/gdps4/save`)
   and stops at `user32!SystemParametersInfoW`: GLFW (inside libcocos2d) starting window creation.
   **M4 complete; M5 (window + OpenGL) starts here.**

PS4 toolchain notes: OpenOrbis' `sa_sigaction` macro is broken (use `__sa_handler.__sa_sigaction`), its
`mcontext_t` is Linux's layout (FreeBSD `mc_rip` is at ucontext+176), and libc lacks
`__cxa_thread_atexit_impl` (provided in `platform_posix.cpp`).
- Known gaps: SEH/C++ exception dispatch on PS4 (`RaiseException`/`RtlUnwindEx`/`RtlVirtualUnwind` stop
  with a message), `ExitThread`, process creation, writable file mappings, real time zones (UTC).
| M5 | Piglet GL + fake window → first rendered frame (menu) | PS4 | **Built, waiting for a console run.** |

### M5: window + OpenGL on the PS4
Measured on PC (`GDPS4_GLTRACE=1` wraps every window/GL function with a first-call logger): to reach the
menu the game calls 90 window/GL functions (not the 140 it imports), and only 23 runtime-loaded GL
functions, all core GLES2.
- `ps4/runner/desktop.cpp`: fake desktop for GLFW. Real window classes, windows (styles, longs, extra
  bytes, props), per-thread message queues (Peek/Get/Post/Send/Dispatch), `WM_NCCREATE`/`WM_CREATE` with a
  real `CREATESTRUCT`, activation/focus/size messages when the window is shown (so GD doesn't pause), one
  1920x1080 60 Hz monitor, one RGBA8 + depth24/stencil8 double-buffered GL pixel format.
- `ps4/runner/gl_piglet.cpp`: WGL on EGL (Piglet `libScePigletv2VSH` + runtime shader compiler
  `libSceShaccVSH`), presents as desktop GL 2.1. Every GLES2 function gets an MS-ABI wrapper generated by
  `tools/gen_gl_wrappers.py`. Desktop fixups: `#version 100` + default precision in shaders, sized
  internal formats -> GLES formats, desktop-only enables ignored, `glClearDepth`/`glDepthRange` doubles.
  Desktop-only GL entry points return stubs that stop with their name if called (glew needs them non-null).
- Not yet: GDI text rendering (cocos2d TTF labels), mouse/touch input, real keyboard.
| M6 | FMOD shim, DS4 input, save data → playable levels | PS4 | Pending |
| M7 | PKG + install docs | PS4 | Pending |
| M8 | Geode.dll loading | PS4 | Pending |

## 8. Legal note

Only your own install is used as input; no game files are redistributed in this project. The PKG you build will contain your copy of the game's binaries and assets — keep it for personal use.
