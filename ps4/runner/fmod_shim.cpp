// fmod.dll for the PS4 runner (M6 stage 1): FMOD's object model with real timing semantics, no audio
// output yet. Sounds report their true length (parsed from the OGG/MP3 file), channels advance with the
// clock (pitch, pause, setPosition, loops), DSP clocks/delays/fades are tracked, end callbacks fire.
// What's missing (stage 2): decoding + mixing + sceAudioOut, and real metering (GD's menu pulse).
// The 66 functions are exactly what GeometryDash.exe imports (docs/fmod_api.txt).
#include "fmod_shim.hpp"
#include "../../win32/platform.hpp"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#define MSABI __attribute__((ms_abi))

namespace gdps4::fmod {
namespace {
using FMOD_RESULT = int;
constexpr FMOD_RESULT OK = 0, ERR_FILE_NOTFOUND = 18, ERR_INVALID_PARAM = 31;
constexpr unsigned TIMEUNIT_MS = 1, TIMEUNIT_PCM = 2, TIMEUNIT_PCMBYTES = 4;
constexpr unsigned MODE_LOOP_NORMAL = 0x2, MODE_OPENMEMORY = 0x800, MODE_OPENMEMORY_POINT = 0x10000000;
constexpr int kRate = 44100;

Hooks g_h;
void log(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_h.log) g_h.log(b);
}
uint64_t now_us() { return plat::monotonic_ns() / 1000; }
uint64_t dsp_clock() { return now_us() * kRate / 1000000; }   // mixer clock in samples
std::recursive_mutex g_mx;

// ---- audio file length ----------------------------------------------------------------------------
struct Length { unsigned samples = 0; int rate = kRate; };
Length ogg_length(const std::vector<uint8_t>& d) {
    Length L;
    auto id = std::search(d.begin(), d.end(), (const uint8_t*)"\x01vorbis", (const uint8_t*)"\x01vorbis" + 7);
    if (id != d.end() && id + 16 <= d.end()) { const uint8_t* p = &*id + 12; L.rate = p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24; }
    for (size_t i = d.size() >= 14 ? d.size() - 14 : 0; i-- > 0;)   // last page's granule position = total samples
        if (d[i] == 'O' && d[i + 1] == 'g' && d[i + 2] == 'g' && d[i + 3] == 'S') {
            uint64_t g = 0; for (int k = 7; k >= 0; k--) g = (g << 8) | d[i + 6 + k];
            L.samples = (unsigned)g; break;
        }
    return L;
}
Length mp3_length(const std::vector<uint8_t>& d) {
    Length L;
    size_t i = 0;
    if (d.size() > 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3')   // skip ID3v2
        i = 10 + ((d[6] & 0x7F) << 21 | (d[7] & 0x7F) << 14 | (d[8] & 0x7F) << 7 | (d[9] & 0x7F));
    static const int br[2][16] = {{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},   // MPEG1 L3
                                  {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}};      // MPEG2/2.5 L3
    static const int sr[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    for (; i + 4 < d.size(); i++) {
        if (d[i] != 0xFF || (d[i + 1] & 0xE0) != 0xE0) continue;
        int ver = (d[i + 1] >> 3) & 3, layer = (d[i + 1] >> 1) & 3, bri = d[i + 2] >> 4, sri = (d[i + 2] >> 2) & 3;
        if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) continue;   // need a Layer III header
        bool mpeg1 = ver == 3; int rate = sr[ver == 3 ? 0 : ver == 2 ? 1 : 2][sri];
        int spf = mpeg1 ? 1152 : 576; L.rate = rate;
        // VBR: Xing/Info header carries the frame count
        for (size_t k = i + 4; k < i + 64 && k + 12 < d.size(); k++)
            if ((!memcmp(&d[k], "Xing", 4) || !memcmp(&d[k], "Info", 4)) && (d[k + 7] & 1)) {
                unsigned frames = d[k + 8] << 24 | d[k + 9] << 16 | d[k + 10] << 8 | d[k + 11];
                L.samples = frames * spf; return L;
            }
        int kbps = br[mpeg1 ? 0 : 1][bri];   // CBR estimate from the first frame
        L.samples = (unsigned)((double)(d.size() - i) * 8 / (kbps * 1000.0) * rate);
        return L;
    }
    return L;
}

// ---- objects ----------------------------------------------------------------------------------------
struct Sound { std::string path; unsigned length_samples = 0; int rate = kRate; int loop_count = 0; unsigned mode = 0; };
struct Group;
struct Channel {
    Sound* sound = nullptr; Group* group = nullptr;
    bool paused = false, stopped = false; float volume = 1, pitch = 1; void* user = nullptr;
    double pos_samples = 0; uint64_t last_us = 0;   // position at last_us (sound's own sample rate)
    int loop_count = 0; unsigned loop_start = 0, loop_end = 0;
    uint64_t delay_start = 0, delay_end = 0; bool delay_stop = false;
    std::vector<std::pair<uint64_t, float>> fades;
    void* callback = nullptr;
};
struct Group { std::string name; std::vector<Channel*> channels; float volume = 1, pitch = 1; bool paused = false; void* user = nullptr; void* callback = nullptr; Group* parent = nullptr; };
struct Dsp { int type = 0; bool metering_in = false, metering_out = false; };
struct System { unsigned version = 0; Group* master = nullptr; };
std::vector<Channel*> g_channels;

float group_pitch(const Group* g) { float p = 1; for (; g; g = g->parent) p *= g->pitch; return p; }
bool group_paused(const Group* g) { for (; g; g = g->parent) if (g->paused) return true; return false; }
// Advance a channel's position to now.
void advance(Channel* c) {
    uint64_t t = now_us();
    if (!c->stopped && !c->paused && !group_paused(c->group) && c->sound) {
        double dt = (t - c->last_us) / 1e6;
        c->pos_samples += dt * c->sound->rate * c->pitch * group_pitch(c->group);
        double len = c->sound->length_samples;
        if (len > 0 && c->pos_samples >= len) {
            bool loops = c->loop_count != 0 && ((c->sound->mode & MODE_LOOP_NORMAL) || c->loop_count < 0 || c->loop_count > 0);
            if (loops) {
                double ls = c->loop_start, le = c->loop_end ? c->loop_end + 1.0 : len;
                if (le <= ls) le = len;
                while (c->pos_samples >= le) { c->pos_samples -= (le - ls); if (c->loop_count > 0) c->loop_count--; if (!c->loop_count) break; }
            } else { c->pos_samples = len; c->stopped = true; }
        }
    }
    c->last_us = t;
}
using ChannelCallback = FMOD_RESULT (MSABI*)(void* chan, int type, int cbtype, void* d1, void* d2);
}  // namespace

// ================================ exports ===========================================================
namespace {
// --- C API
MSABI FMOD_RESULT FMOD_System_Create(System** out, unsigned headerversion) {
    auto* s = new System; s->version = headerversion; s->master = new Group; s->master->name = "master";
    *out = s;
    log("fmod: System_Create (header version %#x) - audio is SILENT in this build (no mixer/output yet)\n", headerversion);
    return OK;
}
MSABI FMOD_RESULT FMOD_Debug_Initialize(unsigned, int, void*, const char*) { return OK; }
MSABI FMOD_RESULT FMOD_Memory_GetStats(int* cur, int* max, int) { if (cur) *cur = 0; if (max) *max = 0; return OK; }
MSABI FMOD_RESULT FMOD_System_LockDSP(System*) { g_mx.lock(); return OK; }
MSABI FMOD_RESULT FMOD_System_UnlockDSP(System*) { g_mx.unlock(); return OK; }
MSABI FMOD_RESULT FMOD_Channel_GetDSPClock(Channel*, uint64_t* clock, uint64_t* parent) {
    uint64_t c = dsp_clock(); if (clock) *clock = c; if (parent) *parent = c; return OK;
}
MSABI FMOD_RESULT FMOD_Channel_GetDelay(Channel* c, uint64_t* start, uint64_t* end, int* stop) {
    if (start) *start = c->delay_start; if (end) *end = c->delay_end; if (stop) *stop = c->delay_stop; return OK;
}
MSABI FMOD_RESULT FMOD_Channel_GetFadePoints(Channel* c, unsigned* num, uint64_t* clocks, float* vols) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    if (num) *num = (unsigned)c->fades.size();
    for (size_t i = 0; i < c->fades.size(); i++) { if (clocks) clocks[i] = c->fades[i].first; if (vols) vols[i] = c->fades[i].second; }
    return OK;
}
MSABI FMOD_RESULT FMOD_Channel_RemoveFadePoints(Channel* c, uint64_t a, uint64_t b) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    c->fades.erase(std::remove_if(c->fades.begin(), c->fades.end(), [&](auto& f) { return f.first >= a && f.first <= b; }), c->fades.end());
    return OK;
}

// --- System
MSABI FMOD_RESULT Sys_init(System*, int, unsigned, void*) { return OK; }
MSABI FMOD_RESULT Sys_close(System*) { return OK; }
MSABI FMOD_RESULT Sys_release(System*) { return OK; }
MSABI FMOD_RESULT Sys_update(System*) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    for (Channel* c : g_channels) {
        bool was = c->stopped; advance(c);
        if (!was && c->stopped && c->callback)   // FMOD_CHANNELCONTROL_CALLBACK_END
            ((ChannelCallback)c->callback)(c, 0 /*CHANNEL*/, 0 /*END*/, nullptr, nullptr);
    }
    return OK;
}
MSABI FMOD_RESULT Sys_getVersion(System* s, unsigned* v) { *v = s->version; return OK; }
MSABI FMOD_RESULT Sys_lockDSP(System*) { g_mx.lock(); return OK; }
MSABI FMOD_RESULT Sys_unlockDSP(System*) { g_mx.unlock(); return OK; }
MSABI FMOD_RESULT Sys_mixerSuspend(System*) { return OK; }
MSABI FMOD_RESULT Sys_mixerResume(System*) { return OK; }
MSABI FMOD_RESULT Sys_setDSPBufferSize(System*, unsigned, int) { return OK; }
MSABI FMOD_RESULT Sys_getDSPBufferSize(System*, unsigned* len, int* num) { if (len) *len = 1024; if (num) *num = 4; return OK; }
MSABI FMOD_RESULT Sys_setSoftwareFormat(System*, int, int, int) { return OK; }
MSABI FMOD_RESULT Sys_getSoftwareFormat(System*, int* rate, int* mode, int* raw) { if (rate) *rate = kRate; if (mode) *mode = 3 /*STEREO*/; if (raw) *raw = 2; return OK; }
MSABI FMOD_RESULT Sys_getStreamBufferSize(System*, unsigned* size, unsigned* unit) { if (size) *size = 16384; if (unit) *unit = TIMEUNIT_PCMBYTES; return OK; }
MSABI FMOD_RESULT Sys_getCPUUsage(System*, float* usage) { memset(usage, 0, 6 * sizeof(float)); return OK; }
MSABI FMOD_RESULT Sys_createChannelGroup(System* s, const char* name, Group** out) {
    auto* g = new Group; g->name = name ? name : ""; g->parent = s->master; *out = g; return OK;
}
MSABI FMOD_RESULT Sys_createDSPByType(System*, int type, Dsp** out) { auto* d = new Dsp; d->type = type; *out = d; return OK; }
FMOD_RESULT create_sound(const char* name, unsigned mode, const uint8_t* exinfo, Sound** out) {
    std::vector<uint8_t> data;
    std::string path;
    if (mode & (MODE_OPENMEMORY | MODE_OPENMEMORY_POINT)) {
        unsigned len = exinfo ? *(const unsigned*)(exinfo + 4) : 0;
        data.assign((const uint8_t*)name, (const uint8_t*)name + len);
        path = "<memory>";
    } else {
        path = g_h.map_path ? g_h.map_path(name) : name;
        std::ifstream f(path, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(f), {});
        if (data.empty()) { log("fmod: cannot open %s (%s)\n", name, path.c_str()); return ERR_FILE_NOTFOUND; }
    }
    auto* s = new Sound; s->path = path; s->mode = mode;
    bool ogg = data.size() > 4 && !memcmp(data.data(), "OggS", 4);
    Length L = ogg ? ogg_length(data) : mp3_length(data);
    s->length_samples = L.samples; s->rate = L.rate;
    *out = s;
    return OK;
}
MSABI FMOD_RESULT Sys_createSound(System*, const char* name, unsigned mode, const uint8_t* exinfo, Sound** out) { return create_sound(name, mode, exinfo, out); }
MSABI FMOD_RESULT Sys_createStream(System*, const char* name, unsigned mode, const uint8_t* exinfo, Sound** out) { return create_sound(name, mode, exinfo, out); }
MSABI FMOD_RESULT Sys_playSound(System* sys, Sound* snd, Group* g, bool paused, Channel** out) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    auto* c = new Channel; c->sound = snd; c->group = g ? g : sys->master; c->paused = paused; c->last_us = now_us();
    c->loop_count = snd ? snd->loop_count : 0; if (snd && (snd->mode & MODE_LOOP_NORMAL) && !c->loop_count) c->loop_count = -1;
    c->group->channels.push_back(c); g_channels.push_back(c);
    if (out) *out = c;
    return OK;
}

// --- Sound
MSABI FMOD_RESULT Snd_getLength(Sound* s, unsigned* len, unsigned unit) {
    if (unit == TIMEUNIT_MS) *len = (unsigned)((uint64_t)s->length_samples * 1000 / s->rate);
    else if (unit == TIMEUNIT_PCMBYTES) *len = s->length_samples * 4;
    else *len = s->length_samples;
    return OK;
}
MSABI FMOD_RESULT Snd_getOpenState(Sound*, int* state, unsigned* pct, bool* starving, bool* diskbusy) {
    if (state) *state = 0 /*READY*/; if (pct) *pct = 100; if (starving) *starving = false; if (diskbusy) *diskbusy = false; return OK;
}
MSABI FMOD_RESULT Snd_release(Sound*) { return OK; }   // channels may still reference it; kept alive
MSABI FMOD_RESULT Snd_setLoopCount(Sound* s, int n) { s->loop_count = n; return OK; }

// --- ChannelControl (Channel and ChannelGroup share these entry points; tell them apart by object)
bool is_group(void* p) { std::lock_guard<std::recursive_mutex> l(g_mx); return std::find(g_channels.begin(), g_channels.end(), (Channel*)p) == g_channels.end(); }
MSABI FMOD_RESULT CC_setPaused(void* p, bool v) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    if (is_group(p)) ((Group*)p)->paused = v; else { advance((Channel*)p); ((Channel*)p)->paused = v; }
    return OK;
}
MSABI FMOD_RESULT CC_getPaused(void* p, bool* v) { *v = is_group(p) ? ((Group*)p)->paused : ((Channel*)p)->paused; return OK; }
MSABI FMOD_RESULT CC_setVolume(void* p, float v) { if (is_group(p)) ((Group*)p)->volume = v; else ((Channel*)p)->volume = v; return OK; }
MSABI FMOD_RESULT CC_getVolume(void* p, float* v) { *v = is_group(p) ? ((Group*)p)->volume : ((Channel*)p)->volume; return OK; }
MSABI FMOD_RESULT CC_setVolumeRamp(void*, bool) { return OK; }
MSABI FMOD_RESULT CC_setPitch(void* p, float v) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    if (is_group(p)) { for (Channel* c : ((Group*)p)->channels) advance(c); ((Group*)p)->pitch = v; }
    else { advance((Channel*)p); ((Channel*)p)->pitch = v; }
    return OK;
}
MSABI FMOD_RESULT CC_getPitch(void* p, float* v) { *v = is_group(p) ? ((Group*)p)->pitch : ((Channel*)p)->pitch; return OK; }
MSABI FMOD_RESULT CC_stop(void* p) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    if (is_group(p)) { for (Channel* c : ((Group*)p)->channels) c->stopped = true; }
    else ((Channel*)p)->stopped = true;
    return OK;
}
MSABI FMOD_RESULT CC_setUserData(void* p, void* u) { if (is_group(p)) ((Group*)p)->user = u; else ((Channel*)p)->user = u; return OK; }
MSABI FMOD_RESULT CC_getUserData(void* p, void** u) { *u = is_group(p) ? ((Group*)p)->user : ((Channel*)p)->user; return OK; }
MSABI FMOD_RESULT CC_setCallback(void* p, void* cb) { if (is_group(p)) ((Group*)p)->callback = cb; else ((Channel*)p)->callback = cb; return OK; }
MSABI FMOD_RESULT CC_getDSPClock(void*, uint64_t* clock, uint64_t* parent) { uint64_t c = dsp_clock(); if (clock) *clock = c; if (parent) *parent = c; return OK; }
MSABI FMOD_RESULT CC_setDelay(void* p, uint64_t start, uint64_t end, bool stop) {
    if (!is_group(p)) { auto* c = (Channel*)p; c->delay_start = start; c->delay_end = end; c->delay_stop = stop; }
    return OK;
}
MSABI FMOD_RESULT CC_addFadePoint(void* p, uint64_t clock, float vol) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    if (!is_group(p)) ((Channel*)p)->fades.push_back({clock, vol});
    return OK;
}
Dsp g_head_dsp;
MSABI FMOD_RESULT CC_addDSP(void*, int, Dsp*) { return OK; }
MSABI FMOD_RESULT CC_removeDSP(void*, Dsp*) { return OK; }
MSABI FMOD_RESULT CC_getDSP(void*, int, Dsp** out) { *out = &g_head_dsp; return OK; }
MSABI FMOD_RESULT CC_getNumDSPs(void*, int* n) { *n = 1; return OK; }

// --- Channel
MSABI FMOD_RESULT Ch_getPosition(Channel* c, unsigned* pos, unsigned unit) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    advance(c);
    int rate = c->sound ? c->sound->rate : kRate;
    if (unit == TIMEUNIT_MS) *pos = (unsigned)(c->pos_samples * 1000.0 / rate);
    else if (unit == TIMEUNIT_PCMBYTES) *pos = (unsigned)c->pos_samples * 4;
    else *pos = (unsigned)c->pos_samples;
    return OK;
}
MSABI FMOD_RESULT Ch_setPosition(Channel* c, unsigned pos, unsigned unit) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    int rate = c->sound ? c->sound->rate : kRate;
    c->pos_samples = unit == TIMEUNIT_MS ? pos * (double)rate / 1000.0 : unit == TIMEUNIT_PCMBYTES ? pos / 4.0 : pos;
    c->last_us = now_us(); c->stopped = false;
    return OK;
}
MSABI FMOD_RESULT Ch_getCurrentSound(Channel* c, Sound** s) { *s = c->sound; return OK; }
MSABI FMOD_RESULT Ch_setLoopCount(Channel* c, int n) { c->loop_count = n; return OK; }
MSABI FMOD_RESULT Ch_setLoopPoints(Channel* c, unsigned s, unsigned su, unsigned e, unsigned eu) {
    int rate = c->sound ? c->sound->rate : kRate;
    auto conv = [&](unsigned v, unsigned u) { return u == TIMEUNIT_MS ? (unsigned)((uint64_t)v * rate / 1000) : u == TIMEUNIT_PCMBYTES ? v / 4 : v; };
    c->loop_start = conv(s, su); c->loop_end = conv(e, eu);
    return OK;
}
MSABI FMOD_RESULT Ch_getLoopPoints(Channel* c, unsigned* s, unsigned su, unsigned* e, unsigned eu) {
    int rate = c->sound ? c->sound->rate : kRate;
    auto conv = [&](unsigned v, unsigned u) { return u == TIMEUNIT_MS ? (unsigned)((uint64_t)v * 1000 / rate) : u == TIMEUNIT_PCMBYTES ? v * 4 : v; };
    if (s) *s = conv(c->loop_start, su); if (e) *e = conv(c->loop_end, eu);
    return OK;
}

// --- ChannelGroup
MSABI FMOD_RESULT CG_addGroup(Group* g, Group* child, bool, void** conn) { child->parent = g; if (conn) *conn = nullptr; return OK; }
MSABI FMOD_RESULT CG_getNumChannels(Group* g, int* n) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    int k = 0; for (Channel* c : g->channels) { advance(c); if (!c->stopped) k++; }
    *n = k; return OK;
}
MSABI FMOD_RESULT CG_getChannel(Group* g, int idx, Channel** out) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    int k = 0; for (Channel* c : g->channels) if (!c->stopped && k++ == idx) { *out = c; return OK; }
    *out = nullptr; return ERR_INVALID_PARAM;
}
MSABI FMOD_RESULT CG_release(Group*) { return OK; }

// --- DSP
MSABI FMOD_RESULT Dsp_release(Dsp*) { return OK; }
MSABI FMOD_RESULT Dsp_setMeteringEnabled(Dsp* d, bool in, bool out) { d->metering_in = in; d->metering_out = out; return OK; }
MSABI FMOD_RESULT Dsp_getMeteringInfo(Dsp*, uint8_t* in, uint8_t* out) {
    // FMOD_DSP_METERING_INFO {int numsamples; float peak[32]; float rms[32]; short numchannels}: silence for now.
    if (in) memset(in, 0, 264); if (out) { memset(out, 0, 264); *(short*)(out + 260) = 2; }
    return OK;
}
MSABI FMOD_RESULT Dsp_setParameterBool(Dsp*, int, bool) { return OK; }
MSABI FMOD_RESULT Dsp_setParameterFloat(Dsp*, int, float) { return OK; }

const std::map<std::string, void*>& exports() {
    static const std::map<std::string, void*> m = {
        {"FMOD_System_Create", (void*)&FMOD_System_Create}, {"FMOD_Debug_Initialize", (void*)&FMOD_Debug_Initialize},
        {"FMOD_Memory_GetStats", (void*)&FMOD_Memory_GetStats}, {"FMOD_System_LockDSP", (void*)&FMOD_System_LockDSP},
        {"FMOD_System_UnlockDSP", (void*)&FMOD_System_UnlockDSP}, {"FMOD_Channel_GetDSPClock", (void*)&FMOD_Channel_GetDSPClock},
        {"FMOD_Channel_GetDelay", (void*)&FMOD_Channel_GetDelay}, {"FMOD_Channel_GetFadePoints", (void*)&FMOD_Channel_GetFadePoints},
        {"FMOD_Channel_RemoveFadePoints", (void*)&FMOD_Channel_RemoveFadePoints},
        // C++ API (mangled names as imported by GeometryDash.exe)
        {"?init@System@FMOD@@QEAA?AW4FMOD_RESULT@@HIPEAX@Z", (void*)&Sys_init},
        {"?close@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&Sys_close},
        {"?release@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&Sys_release},
        {"?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&Sys_update},
        {"?getVersion@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAI@Z", (void*)&Sys_getVersion},
        {"?lockDSP@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&Sys_lockDSP},
        {"?unlockDSP@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&Sys_unlockDSP},
        {"?mixerSuspend@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&Sys_mixerSuspend},
        {"?mixerResume@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&Sys_mixerResume},
        {"?setDSPBufferSize@System@FMOD@@QEAA?AW4FMOD_RESULT@@IH@Z", (void*)&Sys_setDSPBufferSize},
        {"?getDSPBufferSize@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAIPEAH@Z", (void*)&Sys_getDSPBufferSize},
        {"?setSoftwareFormat@System@FMOD@@QEAA?AW4FMOD_RESULT@@HW4FMOD_SPEAKERMODE@@H@Z", (void*)&Sys_setSoftwareFormat},
        {"?getSoftwareFormat@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAHPEAW4FMOD_SPEAKERMODE@@0@Z", (void*)&Sys_getSoftwareFormat},
        {"?getStreamBufferSize@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAI0@Z", (void*)&Sys_getStreamBufferSize},
        {"?getCPUUsage@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAUFMOD_CPU_USAGE@@@Z", (void*)&Sys_getCPUUsage},
        {"?createChannelGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDPEAPEAVChannelGroup@2@@Z", (void*)&Sys_createChannelGroup},
        {"?createDSPByType@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_DSP_TYPE@@PEAPEAVDSP@2@@Z", (void*)&Sys_createDSPByType},
        {"?createSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z", (void*)&Sys_createSound},
        {"?createStream@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z", (void*)&Sys_createStream},
        {"?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVSound@2@PEAVChannelGroup@2@_NPEAPEAVChannel@2@@Z", (void*)&Sys_playSound},
        {"?getLength@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@PEAII@Z", (void*)&Snd_getLength},
        {"?getOpenState@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@PEAW4FMOD_OPENSTATE@@PEAIPEA_N2@Z", (void*)&Snd_getOpenState},
        {"?release@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&Snd_release},
        {"?setLoopCount@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@H@Z", (void*)&Snd_setLoopCount},
        {"?setPaused@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z", (void*)&CC_setPaused},
        {"?getPaused@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@PEA_N@Z", (void*)&CC_getPaused},
        {"?setVolume@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z", (void*)&CC_setVolume},
        {"?getVolume@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@PEAM@Z", (void*)&CC_getVolume},
        {"?setVolumeRamp@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z", (void*)&CC_setVolumeRamp},
        {"?setPitch@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z", (void*)&CC_setPitch},
        {"?getPitch@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@PEAM@Z", (void*)&CC_getPitch},
        {"?stop@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&CC_stop},
        {"?setUserData@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@PEAX@Z", (void*)&CC_setUserData},
        {"?getUserData@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAX@Z", (void*)&CC_getUserData},
        {"?setCallback@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@P6A?AW43@PEAUFMOD_CHANNELCONTROL@@W4FMOD_CHANNELCONTROL_TYPE@@W4FMOD_CHANNELCONTROL_CALLBACK_TYPE@@PEAX3@Z@Z", (void*)&CC_setCallback},
        {"?getDSPClock@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@PEA_K0@Z", (void*)&CC_getDSPClock},
        {"?setDelay@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_K0_N@Z", (void*)&CC_setDelay},
        {"?addFadePoint@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_KM@Z", (void*)&CC_addFadePoint},
        {"?addDSP@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAVDSP@2@@Z", (void*)&CC_addDSP},
        {"?removeDSP@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVDSP@2@@Z", (void*)&CC_removeDSP},
        {"?getDSP@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAPEAVDSP@2@@Z", (void*)&CC_getDSP},
        {"?getNumDSPs@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", (void*)&CC_getNumDSPs},
        {"?getPosition@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAII@Z", (void*)&Ch_getPosition},
        {"?setPosition@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@II@Z", (void*)&Ch_setPosition},
        {"?getCurrentSound@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVSound@2@@Z", (void*)&Ch_getCurrentSound},
        {"?setLoopCount@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@H@Z", (void*)&Ch_setLoopCount},
        {"?setLoopPoints@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@IIII@Z", (void*)&Ch_setLoopPoints},
        {"?getLoopPoints@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAII0I@Z", (void*)&Ch_getLoopPoints},
        {"?addGroup@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@PEAV12@_NPEAPEAVDSPConnection@2@@Z", (void*)&CG_addGroup},
        {"?getNumChannels@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", (void*)&CG_getNumChannels},
        {"?getChannel@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAPEAVChannel@2@@Z", (void*)&CG_getChannel},
        {"?release@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&CG_release},
        {"?release@DSP@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", (void*)&Dsp_release},
        {"?setMeteringEnabled@DSP@FMOD@@QEAA?AW4FMOD_RESULT@@_N0@Z", (void*)&Dsp_setMeteringEnabled},
        {"?getMeteringInfo@DSP@FMOD@@QEAA?AW4FMOD_RESULT@@PEAUFMOD_DSP_METERING_INFO@@0@Z", (void*)&Dsp_getMeteringInfo},
        {"?setParameterBool@DSP@FMOD@@QEAA?AW4FMOD_RESULT@@H_N@Z", (void*)&Dsp_setParameterBool},
        {"?setParameterFloat@DSP@FMOD@@QEAA?AW4FMOD_RESULT@@HM@Z", (void*)&Dsp_setParameterFloat},
    };
    return m;
}
}  // namespace

void init(const Hooks& h) { g_h = h; }
void* lookup(const std::string& sym) {
    auto& m = exports();
    auto it = m.find(sym);
    return it == m.end() ? nullptr : it->second;
}

}  // namespace gdps4::fmod
