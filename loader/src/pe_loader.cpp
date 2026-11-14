#include "pe_loader.hpp"
#include <algorithm>
#include <cstring>

namespace gdps4 {
namespace {
template <class T> const T* at(const std::vector<uint8_t>& f, size_t off) {
    return off + sizeof(T) <= f.size() ? reinterpret_cast<const T*>(f.data() + off) : nullptr;
}
#pragma pack(push, 1)
struct DataDir { uint32_t rva, size; };
struct SectionHdr { char name[8]; uint32_t vsize, va, rawsize, rawptr, relocptr, lineptr; uint16_t nrel, nline; uint32_t chars; };
struct ImportDesc { uint32_t ilt, ts, fwd, name, iat; };
struct ExportDir { uint32_t chars, ts; uint16_t maj, min; uint32_t name, base, nfuncs, nnames, funcs, names, ords; };
struct TlsDir64 { uint64_t raw_start, raw_end, index_addr, callbacks; uint32_t zero_fill, chars; };
#pragma pack(pop)
enum { DIR_EXPORT = 0, DIR_IMPORT = 1, DIR_EXCEPTION = 3, DIR_BASERELOC = 5, DIR_TLS = 9 };
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
}

void* LoadedImage::find_export(const std::string& sym) const {
    if (!sym.empty() && sym[0] == '#') {
        auto it = ord_exports.find((uint32_t)std::stoul(sym.substr(1)));
        return it == ord_exports.end() ? nullptr : base + it->second;
    }
    auto it = exports.find(sym);
    return it == exports.end() ? nullptr : base + it->second;
}

std::string LoadedImage::forwarder(const std::string& sym) const {
    uint8_t* a = (uint8_t*)find_export(sym);
    if (!a || a < base + export_rva || a >= base + export_rva + export_size) return {};
    return std::string((char*)a);
}

LoadResult load_pe(const std::vector<uint8_t>& f, const std::string& name, const PlatformHooks& hooks,
                   const ImportResolver& resolve, LoadedImage& out, bool allow_unresolved) {
    LoadResult r;
    auto fail = [&](const char* m) { r.error = name + ": " + m; return r; };
    auto mz = at<uint16_t>(f, 0);
    if (!mz || *mz != 0x5A4D) return fail("not MZ");
    uint32_t peoff = *at<uint32_t>(f, 0x3C);
    auto sig = at<uint32_t>(f, peoff);
    if (!sig || *sig != 0x00004550) return fail("not PE");
    size_t coff = peoff + 4;
    uint16_t machine = *at<uint16_t>(f, coff), nsec = *at<uint16_t>(f, coff + 2),
             optsz = *at<uint16_t>(f, coff + 16), chars = *at<uint16_t>(f, coff + 18);
    if (machine != 0x8664) return fail("not x86-64");
    size_t opt = coff + 20;
    if (*at<uint16_t>(f, opt) != 0x20B) return fail("not PE32+");
    uint32_t entry = *at<uint32_t>(f, opt + 16);
    uint64_t pref_base = *at<uint64_t>(f, opt + 24);
    uint32_t img_size = *at<uint32_t>(f, opt + 56), hdr_size = *at<uint32_t>(f, opt + 60);
    uint32_t ndirs = *at<uint32_t>(f, opt + 108);
    auto dirs = reinterpret_cast<const DataDir*>(f.data() + opt + 112);
    auto dir = [&](int i) { return i < (int)ndirs ? dirs[i] : DataDir{0, 0}; };

    uint8_t* base = static_cast<uint8_t*>(hooks.alloc_exec(img_size));
    if (!base) return fail("alloc failed");
    std::memset(base, 0, img_size);
    std::memcpy(base, f.data(), std::min<size_t>(hdr_size, f.size()));
    auto secs = reinterpret_cast<const SectionHdr*>(f.data() + opt + optsz);
    for (int i = 0; i < nsec; i++) {
        const auto& s = secs[i];
        if (s.rawsize && s.rawptr + (size_t)s.rawsize <= f.size())
            std::memcpy(base + s.va, f.data() + s.rawptr, std::min(s.rawsize, std::max(s.vsize, s.rawsize)));
    }
    out.name = name; out.base = base; out.size = img_size; out.entry_rva = entry; out.is_dll = chars & 0x2000;

    // Relocations (both images have .reloc and are DYNAMIC_BASE).
    int64_t delta = (int64_t)((uint64_t)base - pref_base);
    if (auto rd = dir(DIR_BASERELOC); delta && rd.size) {
        uint8_t* p = base + rd.rva; uint8_t* end = p + rd.size;
        while (p < end) {
            uint32_t page = *(uint32_t*)p, bsz = *(uint32_t*)(p + 4);
            if (!bsz) break;
            for (uint16_t* e = (uint16_t*)(p + 8); (uint8_t*)e < p + bsz; e++) {
                int type = *e >> 12, off = *e & 0xFFF;
                if (type == 10) *(uint64_t*)(base + page + off) += delta;       // DIR64
                else if (type == 3) *(uint32_t*)(base + page + off) += (uint32_t)delta;
                else if (type != 0) return fail("unsupported reloc type");
            }
            p += bsz;
        }
    } else if (delta && !rd.size) return fail("image not relocatable and not at preferred base");

    // Exports.
    if (auto ed = dir(DIR_EXPORT); ed.size) {
        out.export_rva = ed.rva; out.export_size = ed.size;
        auto e = (ExportDir*)(base + ed.rva);
        auto funcs = (uint32_t*)(base + e->funcs); auto names = (uint32_t*)(base + e->names);
        auto ords = (uint16_t*)(base + e->ords);
        for (uint32_t i = 0; i < e->nfuncs; i++) if (funcs[i]) out.ord_exports[e->base + i] = funcs[i];
        for (uint32_t i = 0; i < e->nnames; i++) out.exports[(char*)(base + names[i])] = funcs[ords[i]];
    }

    // Imports.
    if (auto id = dir(DIR_IMPORT); id.size) {
        for (auto d = (ImportDesc*)(base + id.rva); d->name; d++) {
            std::string dll = lower((char*)(base + d->name));
            auto ilt = (uint64_t*)(base + (d->ilt ? d->ilt : d->iat));
            auto iat = (uint64_t*)(base + d->iat);
            for (; *ilt; ilt++, iat++) {
                std::string sym = (*ilt >> 63) ? "#" + std::to_string(*ilt & 0xFFFF)
                                               : std::string((char*)(base + (uint32_t)*ilt + 2));
                void* a = resolve(dll, sym);
                if (!a) r.unresolved.push_back(dll + "!" + sym);
                *iat = (uint64_t)a;
            }
        }
    }
    if (!r.unresolved.empty() && !allow_unresolved) return fail("unresolved imports");

    if (auto td = dir(DIR_TLS); td.size) {
        auto t = (TlsDir64*)(base + td.rva);   // fields already relocated
        out.tls_index_va = t->index_addr; out.tls_raw_start = t->raw_start; out.tls_raw_end = t->raw_end;
        out.tls_zero_fill = t->zero_fill;
        for (auto cb = (uint64_t*)t->callbacks; cb && *cb; cb++) out.tls_callbacks.push_back(*cb);
    }
    out.pdata_rva = dir(DIR_EXCEPTION).rva; out.pdata_size = dir(DIR_EXCEPTION).size;
    // TODO: per-section protection via hooks.protect once W^X policy on PS4 is settled.
    r.ok = true;
    return r;
}
} // namespace gdps4
