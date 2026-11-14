"""Import closure if the MS C/C++ runtime DLLs are loaded as PE images too.
Prints which OS-level functions (kernel32/kernelbase/ntdll/apisets) a PS4 shim must provide.
Usage: python -I closure.py <gd_dir>"""
import pefile, sys, os, collections
GD = sys.argv[1]; SYS = r"C:\Windows\System32"
# Images we load ourselves (game + MS runtimes). apiset api-ms-win-crt-* -> ucrtbase.
PE_IMAGES = {"geometrydash.exe": GD, "libcocos2d.dll": GD, "libextensions.dll": GD,
             "ucrtbase.dll": SYS, "msvcp140.dll": SYS, "vcruntime140.dll": SYS, "vcruntime140_1.dll": SYS,
             # third-party libraries shipped with the game, also loaded as PE images
             "zlib1.dll": GD, "libpng16.dll": GD, "jpeg62.dll": GD, "tiff.dll": GD, "pthreadvce3.dll": GD,
             "sqlite3.dll": GD, "libcurl_orig.dll": GD, "websockets.dll": GD, "uv.dll": GD, "liblzma.dll": GD,
             "turbojpeg.dll": GD, "libcrypto-3-x64.dll": GD, "libssl-3-x64.dll": GD, "glew32.dll": GD}
def canon(d):
    d = d.lower()
    if d.startswith("api-ms-win-crt-"): return "ucrtbase.dll"
    if d == "libcurl.dll": return "libcurl_orig.dll"   # vanilla: bypass request-viewer proxy
    return d
need = collections.defaultdict(set)
for img, folder in PE_IMAGES.items():
    p = pefile.PE(os.path.join(folder, img), fast_load=True); p.parse_data_directories(directories=[1, 13])
    entries = list(getattr(p, "DIRECTORY_ENTRY_IMPORT", [])) + list(getattr(p, "DIRECTORY_ENTRY_DELAY_IMPORT", []))
    for e in entries:
        d = canon(e.dll.decode())
        if d in PE_IMAGES: continue
        for i in e.imports: need[d].add((i.name or b"#%d" % i.ordinal).decode())
groups = collections.Counter()
for d, syms in sorted(need.items()):
    k = ("OS core (kernel32/kernelbase/ntdll/api-ms-win-core)" if d.startswith(("kernel32", "kernelbase", "ntdll", "api-ms-win-core", "api-ms-win-eventing", "api-ms-win-security", "advapi32"))
         else d)
    groups[k] += len(syms)
for k, n in groups.most_common(): print(f"{n:5d}  {k}")
os_syms = sorted({s for d, ss in need.items() if d.startswith(("kernel32","kernelbase","ntdll","api-ms-win-core","api-ms-win-eventing","api-ms-win-security","advapi32")) for s in ss})
open(os.path.join(os.path.dirname(__file__), "..", "docs", "os_closure.txt"), "w").write("\n".join(os_syms) + "\n")
print(len(os_syms), "unique OS-core symbols -> docs/os_closure.txt")
