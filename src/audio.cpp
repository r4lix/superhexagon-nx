// Sound effects and streamed music behind the original's sound-object API (FAudio in the original).
#include "rt.h"
#include "hand_decl.h"
#include <SDL.h>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <math.h>
#include <string.h>

// stb_vorbis: include the implementation here, without the pushdata API
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"

float rt_music_volume();
float rt_sound_volume();

namespace {
struct Sfx {
    std::vector<int16_t> pcm;   // stereo interleaved, 44.1 kHz
};
struct Voice {
    const Sfx* s = nullptr;
    size_t pos = 0;
    float vol = 1.f;
};
struct Music {
    std::vector<uint8_t> data;
    stb_vorbis* v = nullptr;
    int channels = 2;
    int rate = 44100;
    bool playing = false;
    float vol = 1.f;
    // Variable-speed / reverse playback: the decoder runs forward into a ring of recent frames and the play position
    // moves through that history (negative rate = rewind), resampled linearly.
    std::vector<int16_t> hist;  // stereo frames, index = absolute frame % kHist
    long long decPos = 0;       // absolute index of the next frame the decoder produces
    long long oldest = 0;       // first absolute frame of this play (nothing older exists)
    double pos = 0;             // play position, absolute frames
    bool eof = false;
};
const int kHist = 44100 * 15;
std::atomic<float> g_rate{1.0f};

void decodeMore(Music& m)
{
    static float tmp[1024 * 2];
    int got = stb_vorbis_get_samples_float_interleaved(m.v, 2, tmp, 1024 * 2);
    if (got <= 0) {
        m.eof = true;
        return;
    }
    for (int i = 0; i < got; ++i) {
        size_t o = (size_t)((m.decPos + i) % kHist) * 2;
        for (int c = 0; c < 2; ++c) {
            float x = tmp[i * 2 + c] * 32767.f;
            m.hist[o + (size_t)c] = (int16_t)(x > 32767.f ? 32767 : (x < -32768.f ? -32768 : x));
        }
    }
    m.decPos += got;
}

SDL_AudioDeviceID g_dev = 0;
std::map<int, Sfx> g_sfx;       // blob object offset -> samples
std::map<int, Music> g_music;
std::vector<Voice> g_voices;
bool g_enabled = false;
bool g_halted = false;          // game paused / overlay open: silence + freeze music and sfx
const int kRate = 44100;

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

void mixCallback(void*, Uint8* stream, int len)
{
    float* out = (float*)stream;
    int frames = len / (int)(sizeof(float) * 2);
    memset(out, 0, (size_t)len);
    if (g_halted) {
        return;
    }
    float sv = rt_sound_volume();
    float mv = rt_music_volume();
    for (auto& vc : g_voices) {
        if (!vc.s) {
            continue;
        }
        size_t total = vc.s->pcm.size() / 2;
        for (int i = 0; i < frames && vc.pos < total; ++i, ++vc.pos) {
            out[i * 2] += vc.s->pcm[vc.pos * 2] / 32768.f * vc.vol * sv;
            out[i * 2 + 1] += vc.s->pcm[vc.pos * 2 + 1] / 32768.f * vc.vol * sv;
        }
        if (vc.pos >= total) {
            vc.s = nullptr;
        }
    }
    const double rate = g_rate.load();
    for (auto& kv : g_music) {
        Music& m = kv.second;
        if (!m.playing || !m.v || m.hist.empty()) {
            continue;
        }
        static bool first = true;
        if (first) { logf("mix: first music decode"); first = false; }
        for (int i = 0; i < frames; ++i) {
            if (rate > 0) {
                while (!m.eof && m.decPos < (long long)m.pos + 2) {
                    decodeMore(m);
                }
            }
            long long lo = m.decPos - kHist;
            if (lo < m.oldest) {
                lo = m.oldest;
            }
            if (m.pos < (double)lo) {
                m.pos = (double)lo;
                continue;                           // rewound past the history: hold (silent)
            }
            long long idx = (long long)m.pos;
            if (idx + 1 >= m.decPos) {
                if (m.eof && rate > 0) {
                    m.playing = false;
                    break;
                }
                continue;
            }
            float fr = (float)(m.pos - (double)idx);
            size_t a0 = (size_t)(idx % kHist) * 2, a1 = (size_t)((idx + 1) % kHist) * 2;
            for (int c = 0; c < 2; ++c) {
                float s0 = m.hist[a0 + (size_t)c], s1 = m.hist[a1 + (size_t)c];
                out[i * 2 + c] += (s0 + (s1 - s0) * fr) / 32768.f * m.vol * mv;
            }
            m.pos += rate;
        }
    }
}
}

bool audio_init()
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        logf("audio init failed: %s", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = kRate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mixCallback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        logf("audio device: %s", SDL_GetError());
        return false;
    }
    g_voices.resize(32);
    SDL_PauseAudioDevice(g_dev, 0);
    g_enabled = true;
    return true;
}

void audio_set_halted(bool h) { g_halted = h; }
void audio_set_rate(float r) { g_rate.store(r); }

// play position of a music object in ms, -1 when it is not playing
double audio_music_pos_ms(int obj)
{
    double r = -1;
    auto it = g_music.find(obj);
    if (g_enabled && it != g_music.end()) {
        SDL_LockAudioDevice(g_dev);
        if (it->second.playing) {
            r = it->second.pos * 1000.0 / kRate;
        }
        SDL_UnlockAudioDevice(g_dev);
    }
    return r;
}

void audio_shutdown()
{
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
    }
}

void audio_register_sfx(int obj, const std::string& path)
{
    std::vector<uint8_t> raw;
    if (!readFile(path, raw)) {
        logf("missing sound %s", path.c_str());
        return;
    }
    int ch = 0, rate = 0;
    short* pcm = nullptr;
    int n = stb_vorbis_decode_memory(raw.data(), (int)raw.size(), &ch, &rate, &pcm);
    if (n <= 0 || !pcm) {
        logf("cannot decode %s", path.c_str());
        return;
    }
    Sfx s;
    s.pcm.resize((size_t)n * 2);
    for (int i = 0; i < n; ++i) {
        s.pcm[(size_t)i * 2] = pcm[i * ch];
        s.pcm[(size_t)i * 2 + 1] = pcm[i * ch + (ch > 1 ? 1 : 0)];
    }
    free(pcm);
    g_sfx[obj] = std::move(s);
}

void audio_register_music(int obj, const std::string& path)
{
    Music& m = g_music[obj];
    if (!readFile(path, m.data)) {
        logf("missing music %s", path.c_str());
        return;
    }
    int err = 0;
    m.v = stb_vorbis_open_memory(m.data.data(), (int)m.data.size(), &err, nullptr);
    if (!m.v) {
        logf("cannot open %s (%d)", path.c_str(), err);
    }
}

// ---- the original's sound object API ---------------------------------------------------------------------
void FUN_00439190(int snd)
{
    logf("sfx play %d", snd);
    if (!g_enabled) {
        return;
    }
    auto it = g_sfx.find(snd);
    if (it != g_sfx.end()) {
        SDL_LockAudioDevice(g_dev);
        for (auto& v : g_voices) {
            if (!v.s) {
                v.s = &it->second;
                v.pos = 0;
                v.vol = 1.f;
                break;
            }
        }
        SDL_UnlockAudioDevice(g_dev);
    }
}

void FUN_0043a340(int snd, int offsetMs)
{
    logf("music play %d @%d ms", snd, offsetMs);
    auto it = g_music.find(snd);
    if (!g_enabled || it == g_music.end() || !it->second.v) {
        return;
    }
    SDL_LockAudioDevice(g_dev);
    Music& m = it->second;
    unsigned sample = (unsigned)((long long)offsetMs * kRate / 1000);
    stb_vorbis_seek(m.v, sample);
    if (m.hist.empty()) {
        m.hist.assign((size_t)kHist * 2, 0);
    }
    m.decPos = m.oldest = sample;
    m.pos = (double)sample;
    m.eof = false;
    m.playing = true;
    SDL_UnlockAudioDevice(g_dev);
}

void FUN_0043a4c0(int snd, float vol)
{
    auto it = g_music.find(snd);
    if (it != g_music.end()) {
        it->second.vol = vol;
    }
}

void FUN_0043cbd0(int snd)
{
    auto it = g_music.find(snd);
    if (it != g_music.end()) {
        SDL_LockAudioDevice(g_dev);
        it->second.playing = false;
        SDL_UnlockAudioDevice(g_dev);
    }
}
