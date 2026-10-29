// Game-side audio glue: asset registration (original FUN_0042e100) and the per-track beat tables
// (original FUN_0042ef20, whose CSV strings are baked into data/beats.bin).
#include "rt.h"
#include "hand_decl.h"
#include <string>
#include <vector>

bool audio_init();
void audio_register_sfx(int obj, const std::string& path);
void audio_register_music(int obj, const std::string& path);

static std::string g_dataDir = "data";
void rt_set_data_dir(const std::string& d) { g_dataDir = d; }
const std::string& rt_data_dir() { return g_dataDir; }

static std::vector<int> g_beats[6];   // beats.bin: index 0 = default, 1..5 = tracks

static void loadBeats()
{
    std::string path = g_dataDir + "/beats.bin";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        logf("missing %s", path.c_str());
        return;
    }
    for (int t = 0; t < 6; ++t) {
        uint32_t n = 0;
        if (fread(&n, 4, 1, f) != 1) {
            break;
        }
        g_beats[t].resize(n);
        if (n && fread(g_beats[t].data(), 4, n, f) != n) {
            break;
        }
    }
    fclose(f);
}

// original constructor of the audio object loads every file; we only register them
void audio_load_all()
{
    loadBeats();
    audio_init();
    for (int i = 0; i <= 5; ++i) {
        audio_register_music(AUDIO + 0x1d4d4 + 0x24 * i, g_dataDir + "/music/music" + std::to_string(i) + ".dat");
    }
    static const char* names[16] = {"begin", "excellent", "gameover", "start", "die", "rankup", "line", "triangle",
                                    "square", "pentagon", "hexagon", "menuselect", "menuchoose", "awesome",
                                    "wonderful", "superhexagon"};
    for (int j = 0; j < 16; ++j) {
        audio_register_sfx(AUDIO + 0x1d5ac + 0x24 * j, g_dataDir + "/sounds/" + names[j] + ".ogg");
    }
}

// original FUN_0042ef20(audio, track): fill the int table at audio+0xc used as "intensity per frame"
void FUN_0042ef20(int audio, int track)
{
    if (A(int, audio + 0x1d4d0) == track || track == 0) {
        return;
    }
    A(int, audio + 0x1d4d0) = track;
    int idx = (track >= 1 && track <= 5) ? track : 0;
    const std::vector<int>& v = g_beats[idx];
    int count = (int)v.size();
    int cap = 0x40000 / 4;     // generous: table lives in the audio object's reserved area
    for (int i = 0; i < count && i < cap; ++i) {
        if (track == 2) {
            A(int, audio + 0xc + i * 4) = (int)((double)v[i] * 0.6);
        } else if (track != 5) {
            A(int, audio + 0xc + i * 4) = v[i];
        } else if (i > 1) {
            int j = count - i;
            A(int, audio + 0xc + i * 4) = (j >= 0 && j < count) ? v[j] : 0;
        }
    }
}
