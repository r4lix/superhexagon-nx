// Persistence of best times / unlocks / settings / local high-score names. The original kept these in
// two xml files through a settings map; here they are plain fields of the game object, dumped to one file.
#include "rt.h"
#include "hand_decl.h"
#include <string.h>
#include <string>
#include <vector>

void nx_commit_save();     // flush the title save device (NSP install); no-op otherwise
static std::string g_savePath = "save.dat";
void rt_set_save_path(const std::string& p) { g_savePath = p; }

namespace {
struct Range { int off; int len; };
// offsets are relative to the game object (app + 0x10)
const Range kRanges[] = {
    {0x0010, 8},          // music / sound volume (0..10)
    {0x0018, 1},          // fullscreen flag
    {0x2988, 0x38},       // best times (normal + hyper variants)
    {0x54b0, 12},         // unlocked hyper modes
    {0x54c8, 12},         // finished hyper modes
    {0x5738, 4},          // tutorial flag
    {0x5aac, 0x78},       // local top-5 times, 6 modes x 5
    {0x5b3c, 0x18},       // best time snapshots
    {0x5b54, 1},          // arcade / hyper flag
    {0x10188, 8},         // vsync / vcrash
};
const int kNameBase = 0x57dc;   // 30 strings of 0x18 bytes
const int kNameCount = 30;
const int kPlayerName = 0xd8b0;
const uint32_t kMagic = 0x31584853;   // "SHX1"
}

void SavePersisted(int game)
{
    FILE* f = fopen(g_savePath.c_str(), "wb");
    if (!f) {
        logf("cannot write %s", g_savePath.c_str());
        return;
    }
    fwrite(&kMagic, 4, 1, f);
    for (const Range& r : kRanges) {
        fwrite(G + game + r.off, 1, (size_t)r.len, f);
    }
    for (int i = 0; i < kNameCount; ++i) {
        std::string s = bstr(game + kNameBase + i * 0x18);
        uint8_t n = (uint8_t)(s.size() > 40 ? 40 : s.size());
        fwrite(&n, 1, 1, f);
        fwrite(s.data(), 1, n, f);
    }
    std::string pn = bstr(game + kPlayerName);
    uint8_t n = (uint8_t)(pn.size() > 40 ? 40 : pn.size());
    fwrite(&n, 1, 1, f);
    fwrite(pn.data(), 1, n, f);
    fclose(f);
    nx_commit_save();
}

void GameLoadPersisted(int game)
{
    // defaults of the original's fresh install
    A(int32_t, game + 0x10) = 10;
    A(int32_t, game + 0x14) = 10;
    A(uint8_t, game + 0x18) = 1;
    A(int32_t, game + 0x10188) = 1;
    A(int32_t, game + 0x1018c) = 1;
    bstr_set(game + kPlayerName, "PLAYER");

    FILE* f = fopen(g_savePath.c_str(), "rb");
    if (!f) {
        return;
    }
    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != kMagic) {
        fclose(f);
        return;
    }
    for (const Range& r : kRanges) {
        if (fread(G + game + r.off, 1, (size_t)r.len, f) != (size_t)r.len) {
            fclose(f);
            return;
        }
    }
    for (int i = 0; i < kNameCount; ++i) {
        uint8_t n = 0;
        if (fread(&n, 1, 1, f) != 1) {
            break;
        }
        char buf[64] = {0};
        if (n && fread(buf, 1, n, f) != n) {
            break;
        }
        bstr_set(game + kNameBase + i * 0x18, std::string(buf, n));
    }
    uint8_t n = 0;
    if (fread(&n, 1, 1, f) == 1) {
        char buf[64] = {0};
        if (n && fread(buf, 1, n, f) == n) {
            bstr_set(game + kPlayerName, std::string(buf, n));
        }
    }
    fclose(f);
}

void FUN_00419290(int game) { SavePersisted(game); }   // variables
void FUN_00419930(int game) { SavePersisted(game); }   // settings
void FUN_00418640(int game) { SavePersisted(game); }   // scores
void FUN_00415e80(int) {}                              // (re)load score names from the map: fields already hold them
void FUN_00416e50(int) {}
char FUN_00417ef0(int) { return 1; }

// insertion of a finished run into the local top-5 of its mode (original FUN_004050d0)
void FUN_004050d0(int game)
{
    float t = A(float, game + 0x2914);
    int mode = A(int, game + 0x54a4) + (A(uint8_t, game + 0x54f8) ? 3 : 0);
    int base = game + 0x5aac + mode * 0x14;
    int nameBase = game + 0x57dc + mode * 0x78;
    A(int, game + 0x5b60) = mode * 5;
    int slot = 4;     // new rank index, 4 = out of the table
    int times[5];
    for (int i = 0; i < 5; ++i) {
        times[i] = A(int, base + i * 4);
    }
    if (t <= (float)times[4]) {
        return;       // not good enough
    }
    A(int, game + 0x5b5c) = 4;
    for (int i = 0; i < 5; ++i) {
        if ((float)times[i] < t) {
            slot = i;
            break;
        }
    }
    // (the original walks the list from the bottom; the effect is the usual insertion sort)
    for (int i = 4; i > slot; --i) {
        A(int, base + i * 4) = A(int, base + (i - 1) * 4);
        bstr_set(nameBase + i * 0x18, bstr(nameBase + (i - 1) * 0x18));
    }
    A(int, base + slot * 4) = (int)t;
    bstr_set(nameBase + slot * 0x18, bstr(game + 0x5b64));
    A(int, game + 0x5b5c) = slot;
    A(int, game + 0x5b58) = 0x1b;
}
