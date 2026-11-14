"""Enumerate every import of the GD PE images and classify who must provide it on PS4.
Usage: python -I import_inventory.py <gd_dir> <out.csv>"""
import pefile, sys, os, csv, collections
IMAGES = ["GeometryDash.exe","libcocos2d.dll","libExtensions.dll"]
LOADED_AS_PE = {"libcocos2d.dll","libextensions.dll"}  # original RobTop DLLs, loaded by our PE loader
def cls(dll):
    d=dll.lower()
    if d in LOADED_AS_PE: return "pe-image (load original)"
    if d.startswith("api-ms-win-crt") or d.startswith("msvcp") or d.startswith("vcruntime"): return "crt (load MS redist PE or reimplement)"
    if d in ("opengl32.dll","glew32.dll"): return "shim -> Piglet GLES2"
    if d=="fmod.dll": return "shim -> sceAudioOut + decoders"
    if d in ("kernel32.dll","advapi32.dll","shell32.dll"): return "shim -> orbis libkernel"
    if d in ("user32.dll","gdi32.dll","xinput1_4.dll"): return "shim -> window/pad/font"
    if d=="steam_api64.dll": return "stub (drop Steam)"
    return "shim -> port open-source lib (zlib/png/jpeg/tiff/curl/sqlite/pthreads/lws)"
gd=sys.argv[1]; rows=[]; summary=collections.Counter()
for img in IMAGES:
    p=pefile.PE(os.path.join(gd,img),fast_load=True); p.parse_data_directories(directories=[1])
    for e in p.DIRECTORY_ENTRY_IMPORT:
        dll=e.dll.decode(); c=cls(dll)
        for i in e.imports:
            rows.append([img,dll,(i.name or b"#%d"%i.ordinal).decode(),c]); summary[c]+=1
with open(sys.argv[2],"w",newline="") as f:
    w=csv.writer(f); w.writerow(["image","dll","symbol","provider"]); w.writerows(rows)
for k,v in summary.most_common(): print(f"{v:5d}  {k}")
print(f"{len(rows):5d}  TOTAL")
