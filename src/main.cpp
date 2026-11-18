// RP main - window, game loop, camera, input, HUD, minimap.
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "Common.h"
#include "UnitTemplate.h"
#include "TileMap.h"
#include "Game.h"
#include "Renderer.h"
#include "Debug.h"

// The map shipped with the project. Kept as a relative key so it can be joined
// onto whichever assets root resolveAssetRoot() finds -- running rp.exe from the
// repo root (out/) and from the project root must both work.
static const char* MAP_REL = "maps/normal/l030;[demo]mission_3__-__Crossfire.tmx";

// Find the asset directory without depending on the process working directory.
// Candidates: an explicit override, then <exe dir>/assets, <exe dir>/../assets,
// and finally ./assets. A directory only counts when it actually holds maps/.
// Mirrors the reference project's FileHelper/cli.cpp resolveAssetRoot().
static std::string resolveAssetRoot() {
    auto looksLikeRoot = [](const std::string& d) {
        return DirectoryExists((d + "/maps").c_str()) || DirectoryExists((d + "/units").c_str());
    };
    std::vector<std::string> cand;
    std::string exeDir = GetApplicationDirectory();
    if (!exeDir.empty()) {
        cand.push_back(exeDir + "assets");
        cand.push_back(exeDir + "../assets");
    }
    cand.push_back("assets");
    cand.push_back("../assets");
    for (const std::string& d : cand) {
        std::string norm = d;
        while (norm.size() > 1 && (norm.back() == '/' || norm.back() == '\\')) norm.pop_back();
        if (looksLikeRoot(norm)) {
            TraceLog(LOG_INFO, TextFormat("Asset root: %s", norm.c_str()));
            return norm;
        }
    }
    TraceLog(LOG_WARNING, "Asset root not found; falling back to ./assets");
    return "assets";
}

static void LogToFile(int logLevel, const char* text, va_list args) {
    static FILE* f = nullptr;
    (void)logLevel;
    if (!f) f = fopen("rp_log.txt", "wt");
    if (f) { vfprintf(f, text, args); fputc('\n', f); fflush(f); }
}

Renderer g_render;

struct UI {
    float panelH = 132;
    bool overPanel = false;
    bool overMinimap = false;
    // placement
    bool placing = false;
    std::string placingName;
    Vec2 placingWorld;
    bool placingValid = false;
    // selection drag
    bool dragging = false;
    Vector2 dragA, dragB;
    // RWX ActionType.reclaimTarget pick mode (GameUI:1934-1955): the builder action bar arms it and the next
    // map click issues the command.
    bool reclaimPick = false;
    // command marker anim
    bool markerOn = false; Vector2 markerPos; float markerT = 0;
};
static UI ui;

// ---------------------------------------------------------------------------
// Main menu. Mirrors RWX's kotlin MainMenuSceneHost (core/.../io/github/rwx/ui/host/
// MainMenuSceneHost.kt) and its MainMenuViewModel / UiTheme.Layout constants:
//   title "RWX" + subtitle, a grid of 172x124 tiles (badge + label) and a fixed
//   About / Exit footer. Panel surface alpha is 0.52. RP lays the eight tiles out
//   as 4 columns x 2 rows (RWX's own grid is two columns); the badge/label/footer
//   styling still follows the Kotlin source.
// Buttons that need engine features not ported yet (replay/multiplayer/mods/...)
// fall back to a small "not available" toast; Single Player / Continue enter the game.
// ---------------------------------------------------------------------------
enum class MenuAction { None, Continue, SinglePlayer, WatchReplay, Sandbox, Multiplayer,
                        Mods, ResourceBrowser, Settings, About, Exit };

struct MenuButton {
    MenuAction action;
    const char* label;
    const char* badge;
};

struct MenuScene {
    static constexpr int COLS = 4;
    static constexpr float TILE_W = 172, TILE_H_BASE = 124, COL_GAP = 20, STAGGER = 18;
    static constexpr float FOOT_W = 220, FOOT_H = 56;
    Texture2D title{}; Texture2D glow{}; bool loaded = false;
    std::string toast; float toastT = 0;
    bool hot[16] = {};

    void load() {
        if (loaded) return;
        title = LoadTexture((g_assetsRoot + "/drawable/title.png").c_str());
        glow  = LoadTexture((g_assetsRoot + "/drawable/rounded_glow_button.png").c_str());
        loaded = true;
    }
    void close() {
        if (!loaded) return;
        UnloadTexture(title); UnloadTexture(glow); loaded = false;
    }

    // Labels/badges straight from RWX MainMenuViewModel + I18n.kt (en).
    static std::vector<MenuButton> primary(bool canResume) {
        std::vector<MenuButton> v;
        if (canResume) v.push_back({ MenuAction::Continue, "Continue", "GO" });
        v.push_back({ MenuAction::SinglePlayer, "Single Player", "SP" });
        v.push_back({ MenuAction::WatchReplay, "Watch Replay", "REP" });
        v.push_back({ MenuAction::Sandbox, "Sandbox", "SAN" });
        v.push_back({ MenuAction::Multiplayer, "Multiplayer", "MP" });
        v.push_back({ MenuAction::Mods, "Mods", "MOD" });
        v.push_back({ MenuAction::ResourceBrowser, "Resource Browser", "RES" });
        v.push_back({ MenuAction::Settings, "Settings", "SET" });
        return v;
    }

    static MenuAction drawTile(Rectangle r, const MenuButton& b, Texture2D glowTex,
                               bool hover, Color primaryC, Color secondaryC, Color border, Color bg) {
        DrawRectangleRec(r, hover ? Color{ 46, 52, 60, 240 } : bg);
        DrawRectangleLinesEx(r, 1, hover ? primaryC : border);
        // badge chip (RWX renders a small badge next to the label)
        Rectangle badge{ r.x + 10, r.y + 10, 40, 22 };
        DrawRectangleRec(badge, hover ? secondaryC : primaryC);
        int bw = MeasureText(b.badge, 12);
        DrawText(b.badge, (int)(badge.x + (badge.width - bw) / 2), (int)(badge.y + 5), 12, DARKGRAY);
        // label centered (wrap-free, RWX uses bodySmall centred)
        int tw = MeasureText(b.label, 16);
        int lx = (int)(r.x + (r.width - std::min<float>(tw, r.width - 16)) / 2);
        if (lx < r.x + 8) lx = (int)(r.x + 8);
        DrawText(b.label, lx, (int)(r.y + r.height - 26), 16, hover ? primaryC : WHITE);
        return hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) ? b.action : MenuAction::None;
    }

    // Returns the action chosen this frame, or None.
    MenuAction drawAndInput(bool canResume) {
        load();
        int SW = GetScreenWidth(), SH = GetScreenHeight();
        Vector2 mouse = GetMousePosition();
        Color primaryC{ 90, 190, 220, 255 }, secondaryC{ 240, 180, 70, 255 };
        Color border{ 90, 100, 110, 180 }, bg{ 24, 28, 34, 220 };

        // Full-screen menu surface (RWX PanelStyle.Menu over an opaque background).
        DrawRectangle(0, 0, SW, SH, Color{ 10, 12, 16, 255 });
        DrawRectangle(0, 0, SW, SH, Color{ 0, 0, 0, 133 }); // alpha 0.52

        // Title: RWX draws "RWX" in a display gradient; RP uses the shipped RW title art
        // plus the same subtitle text from MainMenuSceneHost.MENU_SUBTITLE.
        int titleW = title.id ? title.width : 0, titleH = title.id ? title.height : 0;
        float tscale = std::min(1.4f, (float)SW / 900.0f);
        int drawW = (int)(titleW * tscale), drawH = (int)(titleH * tscale);
        if (title.id) DrawTextureEx(title, Vector2{ (float)(SW - drawW) / 2, 24.0f }, 0, tscale, WHITE);
        else DrawText("RWX", SW / 2 - 90, 40, 72, primaryC);
        const char* subtitle = "Rusted Warfare Extension";
        int subW = MeasureText(subtitle, 18);
        DrawText(subtitle, (SW - subW) / 2, 24 + drawH + 8, 18, Color{ 190, 200, 210, 255 });

        auto items = primary(canResume);
        // 4 columns x 2 rows for this build. RWX clamps the menu viewport height to
        // 420dp; here tileH shrinks to fit above the footer and the whole grid is
        // centred in the space left below the title.
        constexpr float GAP_Y = 12;
        float colsW = COLS * TILE_W + (COLS - 1) * COL_GAP;
        float x0 = (SW - colsW) / 2;
        float footY = (float)SH - FOOT_H - 16;
        int rows = (int)((items.size() + COLS - 1) / COLS);
        float availTop = (float)(24 + drawH + 40);
        float availH = footY - 16 - availTop;
        float tileH = std::min<float>(TILE_H_BASE, (availH - STAGGER / 2 - (rows - 1) * GAP_Y) / rows);
        if (tileH < 56) tileH = 56;
        float gridH = rows * tileH + (rows - 1) * GAP_Y + STAGGER / 2;
        float y0 = availTop + std::max(0.0f, (availH - gridH) / 2);
        MenuAction chosen = MenuAction::None;
        for (size_t i = 0; i < items.size(); ++i) {
            int col = (int)i % COLS, row = (int)i / COLS;
            Rectangle r{ x0 + col * (TILE_W + COL_GAP),
                         y0 + row * (tileH + GAP_Y) + (col % 2 ? STAGGER / 2 : 0), TILE_W, tileH };
            bool hover = CheckCollisionPointRec(mouse, r);
            MenuAction a = drawTile(r, items[i], glow, hover, primaryC, secondaryC, border, bg);
            if (a != MenuAction::None) chosen = a;
        }

        // Footer: About / Exit, fixed width/height, centred (RWX MAIN_MENU_FOOTER_*).
        float footTotal = 2 * FOOT_W + COL_GAP;
        float fx = (SW - footTotal) / 2, fy = (float)SH - FOOT_H - 16;
        MenuButton footer[2] = { { MenuAction::About, "About", "?" }, { MenuAction::Exit, "Exit", "X" } };
        for (int i = 0; i < 2; ++i) {
            Rectangle r{ fx + i * (FOOT_W + COL_GAP), fy, FOOT_W, FOOT_H };
            bool hover = CheckCollisionPointRec(mouse, r);
            DrawRectangleRec(r, hover ? Color{ 120, 70, 50, 240 } : bg);
            DrawRectangleLinesEx(r, 1, hover ? secondaryC : border);
            int tw = MeasureText(footer[i].label, 20);
            DrawText(footer[i].label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + FOOT_H / 2 - 10), 20, WHITE);
            if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) chosen = footer[i].action;
        }

        if (!toast.empty()) {
            toastT -= GetFrameTime();
            if (toastT <= 0) toast.clear();
            else {
                int tw = MeasureText(toast.c_str(), 16);
                DrawRectangle(SW / 2 - tw / 2 - 12, SH - 100, tw + 24, 30, Color{ 0, 0, 0, 200 });
                DrawText(toast.c_str(), SW / 2 - tw / 2, SH - 93, 16, Color{ 240, 200, 100, 255 });
            }
        }
        return chosen;
    }
    void showToast(const char* s) { toast = s; toastT = 2.5f; }
};
static MenuScene g_menu;

// ---------------------------------------------------------------------------
// Level select, ported from the RWX Kotlin/Kool sources:
//   ui/host/LevelSelectSceneHost.kt   - scene "level-select", mode list, Back
//   ui/model/LevelSelectViewModel.kt  - scans maps/<assetSubdir>/, displayName(),
//                                       playerCount() from [pN]/Np, <name>_map.png
//   ui/component/ListComponents.kt    - 260x204 card: 138 preview / 34 name /
//                                       16 player label, 116x24 mode badge,
//                                       up to 6 columns, filter/sort controls
// RP keeps those data rules and card metrics; Kool's async loading and styled
// text field are replaced by direct raylib drawing.
// ---------------------------------------------------------------------------
namespace LevelUI {

enum class Mode { Campaign, Skirmish, Challenge, Survival };

struct ModeInfo { Mode mode; const char* label; const char* subdir; };
static const ModeInfo kModes[] = {
    { Mode::Campaign,  "Campaign",  "normal"    },
    { Mode::Skirmish,  "Skirmish",  "skirmish"  },
    { Mode::Challenge, "Challenge", "challenge" },
    { Mode::Survival,  "Survival",  "survival"  },
};

struct Entry {
    std::string path;    // maps/<subdir>/<file>.tmx, relative to the assets root
    std::string file;
    std::string display;
    std::string preview; // full path to <name>_map.png, empty when absent
    int players = 0;
    Mode mode = Mode::Skirmish;
};

static const ModeInfo& infoFor(Mode m) {
    for (const ModeInfo& i : kModes) if (i.mode == m) return i;
    return kModes[0];
}
static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static bool endsWithIgnoreCase(const std::string& s, const char* suffix) {
    std::string a = lower(s), b = lower(suffix);
    return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0;
}

// LevelSelectViewModel.MapEntry.displayName: strip "l030;" / "[p2]", turn
// underscores into spaces and collapse repeated whitespace.
static std::string displayName(std::string f) {
    if (endsWithIgnoreCase(f, ".tmx")) f.resize(f.size() - 4);
    if (f.size() > 2 && std::isalpha((unsigned char)f[0]) && std::isdigit((unsigned char)f[1])) {
        size_t i = 1;
        while (i < f.size() && std::isdigit((unsigned char)f[i])) ++i;
        if (i < f.size() && f[i] == ';') f.erase(0, i + 1);
    }
    if (!f.empty() && f[0] == '[') {
        size_t close = f.find(']');
        if (close != std::string::npos) {
            std::string tag = lower(f.substr(1, close - 1));
            if (tag.rfind("z;", 0) == 0) tag.erase(0, 2);
            if (!tag.empty() && (tag[0] == 'p' || tag[0] == 'o')) {
                size_t i = 1;
                while (i < tag.size() && std::isdigit((unsigned char)tag[i])) ++i;
                if (i > 1 && i == tag.size()) { f.erase(0, close + 1); f = trim(f); }
            }
        }
    }
    std::string out; bool sp = false;
    for (char c : f) {
        if (c == '_' || std::isspace((unsigned char)c)) { if (!sp) { out += ' '; sp = true; } }
        else { out += c; sp = false; }
    }
    return trim(out);
}

// LevelSelectViewModel.playerCount: leftmost pN or Np in the file name.
static int playerCount(const std::string& f) {
    std::string s = lower(f);
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] == 'p' && std::isdigit((unsigned char)s[i + 1])) {
            int v = 0; size_t j = i + 1;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) { v = v * 10 + (s[j] - '0'); ++j; }
            if (v > 0 && v < 32) return v;
        }
        if (std::isdigit((unsigned char)s[i]) && s[i + 1] == 'p') {
            size_t start = i;
            while (start > 0 && std::isdigit((unsigned char)s[start - 1])) --start;
            int v = 0;
            for (size_t j = start; j <= i; ++j) v = v * 10 + (s[j] - '0');
            if (v > 0 && v < 32) return v;
        }
    }
    return 0;
}

static std::string findPreview(const std::string& full) {
    std::string stem = full.substr(0, full.size() - 4); // drop .tmx
    static const char* suffix[] = { "_map.png", "_map.PNG", "_map.jpg", "_map.JPG",
                                    "_map.jpeg", "_map.JPEG", "_map.Png", "_map.Jpg", "_map.Jpeg" };
    for (const char* s : suffix) {
        std::string p = stem + s;
        if (FileExists(p.c_str())) return p;
    }
    return "";
}

// LevelSelectViewModel.builtInMapItems: list assets in maps/<assetSubdir>/,
// keep .tmx, sort by file name.
static std::vector<Entry> scan(Mode mode) {
    std::vector<Entry> out;
    const ModeInfo& mi = infoFor(mode);
    FilePathList list = LoadDirectoryFiles((g_assetsRoot + "/maps/" + mi.subdir).c_str());
    std::vector<std::string> files;
    files.reserve(list.count);
    for (unsigned int i = 0; i < list.count; ++i) files.push_back(list.paths[i]);
    UnloadDirectoryFiles(list);
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        size_t sa = a.find_last_of("/\\"), sb = b.find_last_of("/\\");
        const std::string fa = sa == std::string::npos ? a : a.substr(sa + 1);
        const std::string fb = sb == std::string::npos ? b : b.substr(sb + 1);
        return fa < fb;
    });
    for (const std::string& full : files) {
        if (!endsWithIgnoreCase(full, ".tmx")) continue;
        size_t slash = full.find_last_of("/\\");
        std::string file = slash == std::string::npos ? full : full.substr(slash + 1);
        Entry e;
        e.path = std::string("maps/") + mi.subdir + "/" + file;
        e.file = file;
        e.display = displayName(file);
        e.players = playerCount(file);
        e.preview = findPreview(full);
        e.mode = mode;
        out.push_back(e);
    }
    return out;
}

static std::string ellipsize(const std::string& s, int fs, float maxW) {
    if (MeasureText(s.c_str(), fs) <= maxW) return s;
    std::string t = s;
    while (!t.empty() && MeasureText((t + "...").c_str(), fs) > maxW) t.pop_back();
    return t + "...";
}
static void drawFit(Texture2D tex, Rectangle box) {
    float s = std::min(box.width / (float)tex.width, box.height / (float)tex.height);
    float w = tex.width * s, h = tex.height * s;
    DrawTextureEx(tex, Vector2{ box.x + (box.width - w) / 2, box.y + (box.height - h) / 2 }, 0, s, WHITE);
}

struct LevelSelectScene {
    static constexpr float CARD_W = 260, CARD_H = 204, PREVIEW_H = 138;
    static constexpr float GAP = 8, BACK_W = 56, CONTROL_H = 56, LABEL_W = 112;
    static constexpr int MAX_COLS = 6;

    Mode mode = Mode::Skirmish;
    std::vector<Entry> entries, visible;
    std::vector<int> filterPlayers;
    std::vector<std::string> filterLabels;
    std::string query;
    bool queryFocus = false, inited = false;
    int filterIdx = 0, sortIdx = 0;
    float scroll = 0;
    float eraseRepeat = 0, caretBlink = 0; // key repeat + caret feedback for the filter field
    std::unordered_map<std::string, Texture2D> previews;
    std::string toast; float toastT = 0;

    struct Result { bool back = false; const Entry* picked = nullptr; };

    // Erase one UTF-8 code point (not one byte) so a pasted non-ASCII filter
    // cannot be left as invalid bytes after pressing Backspace.
    static void popLastChar(std::string& s) {
        if (s.empty()) return;
        size_t i = s.size() - 1;
        while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
        s.erase(i);
    }
    static void popLastWord(std::string& s) {
        while (!s.empty() && std::isspace((unsigned char)s.back())) popLastChar(s);
        while (!s.empty() && !std::isspace((unsigned char)s.back())) popLastChar(s);
    }

    void init() { if (!inited) { inited = true; setMode(Mode::Skirmish); } }
    void close() {
        for (auto& kv : previews) if (kv.second.id) UnloadTexture(kv.second);
        previews.clear();
        inited = false;
    }
    void showToast(const char* s) { toast = s; toastT = 2.5f; }

    void setMode(Mode m) {
        mode = m;
        entries = scan(m);
        filterLabels.clear(); filterPlayers.clear();
        filterLabels.push_back("All maps");
        std::vector<int> counts;
        for (const Entry& e : entries)
            if (e.players > 0 && std::find(counts.begin(), counts.end(), e.players) == counts.end())
                counts.push_back(e.players);
        std::sort(counts.begin(), counts.end());
        for (int c : counts) { filterLabels.push_back(std::to_string(c) + "p maps"); filterPlayers.push_back(c); }
        filterIdx = 0; sortIdx = 0; query.clear(); queryFocus = false; scroll = 0;
        rebuildVisible();
    }
    void rebuildVisible() {
        visible.clear();
        std::string q = lower(trim(query));
        int want = (filterIdx > 0 && filterIdx - 1 < (int)filterPlayers.size())
                 ? filterPlayers[filterIdx - 1] : 0;
        for (const Entry& e : entries) {
            if (!q.empty() && lower(e.display).find(q) == std::string::npos &&
                lower(e.file).find(q) == std::string::npos) continue;
            if (want && e.players != want) continue;
            visible.push_back(e);
        }
        if (sortIdx == 1) {
            std::sort(visible.begin(), visible.end(), [](const Entry& a, const Entry& b) {
                return lower(a.display) < lower(b.display);
            });
        } else if (sortIdx == 2) {
            std::sort(visible.begin(), visible.end(), [](const Entry& a, const Entry& b) {
                int pa = a.players ? a.players : 999, pb = b.players ? b.players : 999;
                if (pa != pb) return pa < pb;
                return lower(a.display) < lower(b.display);
            });
        }
    }
    Texture2D previewFor(const std::string& path) {
        if (path.empty()) return Texture2D{};
        auto it = previews.find(path);
        if (it == previews.end()) {
            Texture2D t = LoadTexture(path.c_str());
            if (t.id == 0) TraceLog(LOG_WARNING, TextFormat("Level select: preview failed: %s", path.c_str()));
            it = previews.emplace(path, t).first;
        }
        return it->second;
    }

    Result drawAndInput() {
        init();
        Result res;
        int SW = GetScreenWidth(), SH = GetScreenHeight();
        Vector2 mouse = GetMousePosition();
        Color primary{ 151, 188, 98, 255 }, surface{ 27, 18, 18, 255 };
        Color raised{ 68, 68, 68, 255 }, sunken{ 20, 14, 14, 255 };
        Color text{ 255, 255, 255, 255 }, sub{ 200, 203, 200, 255 };

        DrawRectangle(0, 0, SW, SH, Color{ 53, 57, 53, 255 });
        DrawRectangle(0, 0, SW, SH, Color{ 27, 18, 18, 190 }); // PanelStyle.Menu overlay
        float contentW = std::max(280.0f, std::min<float>((float)SW - 96.0f, 1720.0f));
        float x0 = (SW - contentW) / 2;

        // Filter row: Back icon button + map-name text field (LevelSelectFilterInput).
        float y1 = 16;
        Rectangle back{ x0, y1, BACK_W, CONTROL_H };
        bool backHot = CheckCollisionPointRec(mouse, back);
        DrawRectangleRec(back, backHot ? Color{ 120, 70, 50, 255 } : surface);
        DrawRectangleLinesEx(back, 1, backHot ? primary : raised);
        DrawText("<", (int)(back.x + 22), (int)(back.y + 18), 24, text);
        if (backHot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) res.back = true;

        Rectangle lab{ back.x + BACK_W + 8, y1, LABEL_W, CONTROL_H };
        DrawText("Filter", (int)(lab.x + 4), (int)(lab.y + 20), 16, sub);
        Rectangle input{ lab.x + LABEL_W + 8, y1,
                         contentW - BACK_W - 8 - LABEL_W - 8, CONTROL_H };
        bool inputHot = CheckCollisionPointRec(mouse, input);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) queryFocus = inputHot;
        DrawRectangleRec(input, sunken);
        DrawRectangleLinesEx(input, 1, queryFocus ? primary : raised);
        std::string shown = ellipsize(query.empty() && !queryFocus ? "Map name" : query,
                                      16, input.width - 64);
        DrawText(shown.c_str(), (int)(input.x + 10), (int)(input.y + 18), 16,
                 query.empty() ? sub : text);
        if (queryFocus) {
            caretBlink += GetFrameTime();
            if (std::fmod(caretBlink, 1.0f) < 0.5f)
                DrawText("_", (int)(input.x + 11 + MeasureText(shown.c_str(), 16)),
                         (int)(input.y + 17), 16, primary);
        }
        // A visible clear affordance: clicking it always empties the filter,
        // independent of keyboard focus / IME state.
        if (!query.empty()) {
            Rectangle clr{ input.x + input.width - 36, input.y + (CONTROL_H - 28) / 2, 28, 28 };
            bool clrHot = CheckCollisionPointRec(mouse, clr);
            DrawRectangleRec(clr, clrHot ? Color{ 120, 70, 50, 255 } : Color{ 50, 40, 40, 255 });
            DrawRectangleLinesEx(clr, 1, clrHot ? primary : raised);
            DrawText("x", (int)(clr.x + 10), (int)(clr.y + 5), 18, text);
            if (clrHot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                query.clear();
                rebuildVisible();
            }
        }
        if (queryFocus) {
            // Deletion is driven by key state only. raylib reports Backspace as
            // char 8 on some platforms; also erasing on GetCharPressed()==8 here
            // would delete two characters for one keypress.
            int cp;
            while ((cp = GetCharPressed()) > 0) {
                if (cp >= 32 && cp < 127) query += (char)cp;
            }
            bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            bool eraseDown = IsKeyDown(KEY_BACKSPACE) || IsKeyDown(KEY_DELETE);
            if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_DELETE)) {
                if (ctrl) popLastWord(query); else popLastChar(query);
                eraseRepeat = 0.35f; // delay before key autorepeat kicks in
            } else if (eraseDown) {
                eraseRepeat -= GetFrameTime();
                if (eraseRepeat <= 0) {
                    if (ctrl) popLastWord(query); else popLastChar(query);
                    eraseRepeat = 0.06f;
                }
            } else {
                eraseRepeat = 0;
            }
            rebuildVisible();
        } else {
            eraseRepeat = 0;
        }

        // Option bar: Map Type / Players / Sort (LevelSelectOptionBar combos).
        float y2 = y1 + CONTROL_H + 12;
        float comboW = 210, colW = LABEL_W + comboW;
        float rowW = 3 * colW + 32;
        float cx = x0 + std::max(0.0f, (contentW - rowW) / 2);
        static const char* sortLabels[] = { "Default", "Name", "Players" };
        auto combo = [&](Rectangle r, const char* label, const char* value) {
            bool hot = CheckCollisionPointRec(mouse, r);
            DrawRectangleRec(r, hot ? raised : surface);
            DrawRectangleLinesEx(r, 1, hot ? primary : raised);
            DrawText(label, (int)(r.x + 8), (int)(r.y + (r.height - 14) / 2), 14, sub);
            int vw = MeasureText(value, 16);
            DrawText(value, (int)(r.x + r.width - vw - 24), (int)(r.y + (r.height - 16) / 2), 16, text);
            DrawText("v", (int)(r.x + r.width - 15), (int)(r.y + (r.height - 14) / 2), 14, primary);
            return hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
        };
        if (combo(Rectangle{ cx, y2, colW, CONTROL_H }, "Map Type", infoFor(mode).label)) {
            for (int i = 0; i < (int)(sizeof(kModes) / sizeof(kModes[0])); ++i)
                if (kModes[i].mode == mode) {
                    setMode(kModes[(i + 1) % 4].mode);
                    return res;
                }
        }
        cx += colW + 16;
        const char* playerLabel = filterIdx == 0 ? "All maps" : filterLabels[filterIdx].c_str();
        if (combo(Rectangle{ cx, y2, colW, CONTROL_H }, "Players", playerLabel)) {
            filterIdx = (filterIdx + 1) % (int)filterLabels.size();
            rebuildVisible();
        }
        cx += colW + 16;
        if (combo(Rectangle{ cx, y2, colW, CONTROL_H }, "Sort", sortLabels[sortIdx])) {
            sortIdx = (sortIdx + 1) % 3;
            rebuildVisible();
        }

        // Scrollable card grid.
        float gridTop = y2 + CONTROL_H + 36;
        char cap[128];
        snprintf(cap, sizeof(cap), "%s  -  %zu maps", infoFor(mode).label, visible.size());
        DrawText(cap, (int)(x0 + 8), (int)(gridTop - 24), 16, sub);
        Rectangle grid{ x0 + 4, gridTop, contentW - 8, (float)SH - 56 - gridTop };
        int cols = std::max(1, std::min(MAX_COLS, (int)(grid.width / (CARD_W + GAP))));
        float usedW = cols * CARD_W + (cols - 1) * GAP;
        float gx = grid.x + std::max(0.0f, (grid.width - usedW) / 2);
        int rows = (int)((visible.size() + cols - 1) / cols);
        float totalH = rows > 0 ? rows * (CARD_H + GAP) - GAP : 0.0f;
        float maxScroll = std::max(0.0f, totalH - grid.height);
        if (CheckCollisionPointRec(mouse, grid)) {
            float wheel = GetMouseWheelMove();
            if (wheel != 0) scroll -= wheel * 80.0f;
        }
        if (IsKeyPressed(KEY_DOWN)) scroll += 80;
        if (IsKeyPressed(KEY_UP)) scroll -= 80;
        if (IsKeyPressed(KEY_PAGE_DOWN)) scroll += grid.height * 0.8f;
        if (IsKeyPressed(KEY_PAGE_UP)) scroll -= grid.height * 0.8f;
        scroll = std::max(0.0f, std::min(scroll, maxScroll));

        BeginScissorMode((int)grid.x, (int)grid.y, (int)grid.width, (int)grid.height);
        int first = std::max(0, (int)(scroll / (CARD_H + GAP)));
        int last = std::min(rows, (int)((scroll + grid.height) / (CARD_H + GAP)) + 1);
        for (int row = first; row < last; ++row) {
            for (int col = 0; col < cols; ++col) {
                size_t idx = (size_t)row * cols + col;
                if (idx >= visible.size()) break;
                const Entry& e = visible[idx];
                Rectangle card{ gx + col * (CARD_W + GAP),
                                grid.y + row * (CARD_H + GAP) - scroll, CARD_W, CARD_H };
                bool hot = CheckCollisionPointRec(mouse, card);
                DrawRectangleRec(card, hot ? raised : sunken);
                DrawRectangleLinesEx(card, 1, hot ? primary : raised);
                // MapPreviewImage: FitContent inside a 138-high box.
                Rectangle pv{ card.x + 6, card.y + 6, CARD_W - 12, PREVIEW_H - 6 };
                DrawRectangleRec(pv, surface);
                DrawRectangleLinesEx(pv, 1, raised);
                Texture2D tex = previewFor(e.preview);
                if (tex.id) drawFit(tex, pv);
                else DrawText("No preview", (int)(pv.x + pv.width / 2 - 44),
                              (int)(pv.y + pv.height / 2 - 8), 14, Color{ 110, 110, 110, 255 });
                // MapModeBadge: 116x24 overlay. RWX derives it from map features;
                // RP has no feature detector, so it shows the selected map category.
                Rectangle badge{ pv.x + pv.width - 122, pv.y + 6, 116, 24 };
                DrawRectangleRec(badge, primary);
                DrawText(infoFor(e.mode).label, (int)(badge.x + 8), (int)(badge.y + 6), 14,
                         Color{ 0, 0, 0, 255 });
                std::string nm = ellipsize(e.display, 14, CARD_W - 16);
                DrawText(nm.c_str(), (int)(card.x + 8), (int)(card.y + PREVIEW_H + 6), 14, text);
                std::string pl = e.players > 0 ? std::to_string(e.players) + "p" : "Scenario";
                int pw = MeasureText(pl.c_str(), 12);
                DrawText(pl.c_str(), (int)(card.x + (CARD_W - pw) / 2), (int)(card.y + CARD_H - 24),
                         12, sub);
                if (hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) res.picked = &e;
            }
        }
        EndScissorMode();

        if (maxScroll > 0) {
            float sbX = grid.x + grid.width - 6;
            float sbH = std::max(30.0f, grid.height * grid.height / totalH);
            float sbY = grid.y + (grid.height - sbH) * (scroll / maxScroll);
            DrawRectangle(sbX, grid.y, 4, grid.height, Color{ 60, 60, 60, 180 });
            DrawRectangle(sbX, sbY, 4, sbH, primary);
        }
        DrawText("Click a map to start   |   Wheel / Up / Down to scroll   |   Backspace erases   |   Esc back",
                 (int)(x0 + 8), SH - 32, 14, sub);

        if (IsKeyPressed(KEY_ESCAPE)) {
            if (queryFocus) { queryFocus = false; query.clear(); rebuildVisible(); }
            else res.back = true;
        }
        if (!toast.empty()) {
            toastT -= GetFrameTime();
            if (toastT <= 0) toast.clear();
            else {
                int tw = MeasureText(toast.c_str(), 16);
                DrawRectangle(SW / 2 - tw / 2 - 12, SH - 70, tw + 24, 30, Color{ 0, 0, 0, 200 });
                DrawText(toast.c_str(), SW / 2 - tw / 2, SH - 63, 16, Color{ 240, 200, 100, 255 });
            }
        }
        return res;
    }
};
} // namespace LevelUI
static LevelUI::LevelSelectScene g_levels;

// RWX draws the zoomed-out hull as teamColoredIconTextures[team.getTeamColorIndex()] (CustomUnit.v():664-668),
// so the dot here takes the same slot of the same ten-colour table the sprites are dyed with.
static Color TC(int t) {
    uint32_t a = kTeamColors[teamColorIndex(t)];
    return Color{ (uint8_t)((a >> 16) & 0xFF), (uint8_t)((a >> 8) & 0xFF), (uint8_t)(a & 0xFF),
                  (uint8_t)((a >> 24) & 0xFF) };
}

static std::vector<const UnitTemplate*> buildableOptions(Game& g) {
    std::vector<const UnitTemplate*> out;
    auto sel = g.selectedUnits();
    if (sel.empty()) return out;
    const UnitTemplate* tpl = nullptr;
    for (auto* u : sel) if (u->tpl && (u->tpl->isFactory || u->tpl->isBuilder)) { tpl = u->tpl; break; }
    if (!tpl) return out;
    for (auto& bo : tpl->canBuild) {
        const UnitTemplate* it = g_templates.find(ToLower(bo.unitName));
        if (!it) continue;
        // builder lists buildings; factory lists non-buildings
        if (tpl->isBuilder && !it->isBuilding) continue;
        if (tpl->isFactory && it->isBuilding) continue;
        bool dup = false; for (auto* o : out) if (o->name == it->name || o == it) dup = true;
        if (!dup) out.push_back(it);
    }
    return out;
}

static void clearSelection(Game& g) { for (auto& u : g.units) u.selected = false; }
static void selectBox(Game& g, Vector2 a, Vector2 b) {
    clearSelection(g);
    Vector2 lo{ std::min(a.x, b.x), std::min(a.y, b.y) }, hi{ std::max(a.x, b.x), std::max(a.y, b.y) };
    int player = g.playerTeam();
    Unit* firstMatch = nullptr;
    for (auto& u : g.units) {
        if (u.team != player || !u.tpl || u.riding()) continue;
        if (u.tpl->isStatic() && (hi.x - lo.x) < 4 && (hi.y - lo.y) < 4) {} // allow buildings single select
        if (u.pos.x >= lo.x && u.pos.x <= hi.x && u.pos.y >= lo.y && u.pos.y <= hi.y) { u.selected = true; if (!firstMatch) firstMatch = &u; }
    }
    // if nothing of ours, try enemy single click for info (not selected)
    (void)firstMatch;
}

// HUD artwork. RWX loads these once in GameUI.initializeUIResources():629;
// the icons live in assets/drawable, the action glyphs in assets/units/shared.
struct HudArt {
    Texture2D metal{}, crystal{}, power{}, unitLand{}, pause{}, fast{}, replayPause{}, leaderboard{};
    Texture2D darkBox{}, lock{}, upgrade{}, buildIcon{};
    bool loaded = false;
    void load() {
        if (loaded) return;
        auto tex = [](const std::string& rel) {
            return LoadTexture((g_assetsRoot + "/" + rel).c_str());
        };
        metal       = tex("drawable/metal_dark.png");
        crystal     = tex("drawable/crystal.png");
        power       = tex("drawable/power.png");
        unitLand    = tex("drawable/unit_icon_land.png");
        pause       = tex("drawable/pause.png");
        fast        = tex("drawable/fast.png");
        replayPause = tex("drawable/replay_pause.png");
        leaderboard = tex("drawable/replay_leaderboard.png");
        darkBox     = tex("drawable/rounded_dark_box.png");
        lock        = tex("drawable/lock_icon_menu.png");
        upgrade     = tex("units/shared/icon_upgrade.png");
        buildIcon   = tex("units/shared/icon_build.png");
        loaded = true;
    }
    void close() {
        if (!loaded) return;
        Texture2D all[] = { metal, crystal, power, unitLand, pause, fast, replayPause,
                            leaderboard, darkBox, lock, upgrade, buildIcon };
        for (Texture2D& t : all) if (t.id) UnloadTexture(t);
        loaded = false;
    }
    // RWX uses 9-slices for its panels (GameUI.java:663-673). raylib draws them
    // with DrawTextureNPatch; fall back to a flat rect when the art is missing.
    void panel(Rectangle r, Color tint, int border = 24) const {
        if (!metal.id) { DrawRectangleRec(r, tint); return; }
        NPatchInfo np{ Rectangle{ 0, 0, (float)metal.width, (float)metal.height },
                       border, border, border, border, NPATCH_NINE_PATCH };
        DrawTextureNPatch(metal, np, r, Vector2{ 0, 0 }, 0, tint);
    }
};
static HudArt g_hudArt;

static void drawHudIcon(Texture2D t, Rectangle box, Color tint = WHITE, bool flipY = false) {
    if (!t.id || box.width <= 0 || box.height <= 0) return;
    float s = std::min(box.width / t.width, box.height / t.height);
    Rectangle dst{ box.x + (box.width - t.width * s) / 2, box.y + (box.height - t.height * s) / 2,
                   t.width * s, t.height * s };
    // A RenderTexture2D comes back Y-flipped (the framebuffer's origin is bottom-left), so a unit
    // preview drawn from one needs a negative-height source rect to stand upright.
    Rectangle src{ 0, 0, (float)t.width, flipY ? -(float)t.height : (float)t.height };
    DrawTexturePro(t, src, dst, Vector2{ 0, 0 }, 0, tint);
}

// Gameplay HUD, following RWX's GameUI / GameInterfaceRenderer / Minimap:
//   - right sidebar, width = clamp(screenHeight/2.5, 60, min(250, screenWidth/3))
//   - square minimap pinned to the sidebar top
//   - resource readout + selected-unit card below it
//   - bottom command bar with per-action buttons (icon, price, cooldown, queue)
//   - pause / speed / leaderboard icons on the top strip, group slots bottom-right
static void renderHUD(Game& g, Camera2D& cam) {
    g_hudArt.load();
    int SW = GetScreenWidth(), SH = GetScreenHeight();
    Vector2 mouse = GetMousePosition();
    ui.overPanel = false; ui.overMinimap = false;

    Team* me = nullptr;
    for (auto& t : g.teams) if (t.id == g.playerTeam()) me = &t;

    // RWX authors its HUD against a 1920x1080 surface: GameLogic.updateCameraSystem:2036-2049 sizes the
    // sidebar, GameInterfaceRenderer.h():1027-1029 the button height. So every metric below is a 1080p
    // constant times k, which keeps the strip's share of the screen the same at any window size.
    float k = std::min((float)SW / 1920.0f, (float)SH / 1080.0f);
    k = std::max(0.5f, std::min(2.0f, k));
    auto px = [&](float v) { return (int)std::lround(v * k); };
    float side = std::min(272.0f * k, (float)SW / 3.0f);
    float sideX = (float)SW - side;

    // ---- sidebar surface: dark panel, not the bright metal texture ----
    DrawRectangleRec(Rectangle{ sideX, 0, side, (float)SH }, Color{ 26, 28, 30, 245 });
    DrawRectangleLinesEx(Rectangle{ sideX, 0, side, (float)SH }, 1, Color{ 0, 0, 0, 255 });

    // ---- minimap: square, flush with the top-right corner ----
    Rectangle mm{ sideX, 0, side, side };
    DrawRectangleRec(mm, Color{ 10, 14, 18, 255 });
    if (CheckCollisionPointRec(mouse, mm)) ui.overMinimap = true;
    float tsx = g.map.w > 0 ? mm.width / (float)g.map.w : 1;
    float tsy = g.map.h > 0 ? mm.height / (float)g.map.h : 1;
    float sx = g.map.worldW() > 0 ? mm.width / g.map.worldW() : 1;
    float sy = g.map.worldH() > 0 ? mm.height / g.map.worldH() : 1;
    for (int y = 0; y < g.map.h; ++y) for (int x = 0; x < g.map.w; ++x) {
        const MapTile& t = g.map.at(x, y);
        Color c = t.water ? Color{ 40, 96, 160, 255 }
                : (t.blocked ? Color{ 86, 86, 92, 255 }
                : (t.resource ? Color{ 150, 140, 80, 255 } : Color{ 70, 130, 60, 255 }));
        DrawRectangle(mm.x + x * tsx, mm.y + y * tsy, std::ceil(tsx) + 0.5f, std::ceil(tsy) + 0.5f, c);
    }
    for (auto& u : g.units) {
        if (!u.tpl || u.riding()) continue;
        // Scenery is team -1 (the tileset says `team=none`) and RWX's Minimap only plots real units,
        // so a forest never turns into a field of red dots.
        if (u.team < 0 || u.tplKey == "tree") continue;
        Color c = u.team == g.playerTeam() ? Color{ 0, 255, 0, 255 } : Color{ 255, 60, 60, 255 };
        float sz = (u.tpl->isBuilding ? 3.0f : 2.0f) * std::max(1.0f, k);
        DrawRectangle(mm.x + u.pos.x * sx - sz / 2, mm.y + u.pos.y * sy - sz / 2, sz, sz, c);
    }
    Vector2 tl = GetScreenToWorld2D({ 0.0f, 0.0f }, cam), br = GetScreenToWorld2D({ (float)SW, (float)SH }, cam);
    Rectangle view{ mm.x + tl.x * sx, mm.y + tl.y * sy, (br.x - tl.x) * sx, (br.y - tl.y) * sy };
    view.x = std::max(mm.x, view.x); view.y = std::max(mm.y, view.y);
    view.width = std::min(view.width, mm.x + mm.width - view.x);
    view.height = std::min(view.height, mm.y + mm.height - view.y);
    DrawRectangleLinesEx(view, 1, WHITE);
    DrawRectangleLinesEx(mm, 1, Color{ 110, 110, 110, 255 });
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && ui.overMinimap) {
        Vector2 m = GetMousePosition();
        cam.target = { (m.x - mm.x) / sx, (m.y - mm.y) / sy };
    }

    // ---- top-left: small pause icon, green fps + status text ----
    Rectangle pauseBtn{ 8 * k, 8 * k, 27 * k, 27 * k };
    drawHudIcon(g_hudArt.pause, pauseBtn, Color{ 90, 220, 90, 255 });
    if (CheckCollisionPointRec(mouse, pauseBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
        g_debug.paused = !g_debug.paused;
    DrawText(TextFormat("%dfps", (int)GetFPS()), px(14), px(42), px(20), Color{ 90, 220, 90, 255 });
    DrawText(g_debug.paused ? "Paused" : "Editor Active", px(14), px(66), px(19),
             Color{ 90, 220, 90, 255 });

    // ---- credits at top-right of the map, left of the minimap (GameUI.highlightRect) ----
    float income = 0;
    for (auto& u : g.units)
        if (u.team == g.playerTeam() && u.tpl && !u.dead) income += u.tpl->resourceRate;
    income /= 40.0f;
    int credits = me ? (int)me->credits : 0;
    std::string cs = std::to_string(std::abs(credits));
    for (int i = (int)cs.size() - 3; i > 0; i -= 3) cs.insert(i, " ");
    char cbuf[96];
    snprintf(cbuf, sizeof(cbuf), "$%s(%+d)", cs.c_str(), (int)income);
    int rsize = px(26);
    int cw = MeasureText(cbuf, rsize);
    // subtle dark backing so the green text stays readable on light terrain
    DrawRectangle(sideX - cw - px(18), px(8), cw + px(14), px(36), Color{ 0, 0, 0, 90 });
    DrawText(cbuf, sideX - cw - px(12), px(12), rsize, Color{ 90, 220, 90, 255 });

    // ---- selected units ----
    auto sel = g.selectedUnits();

    // ---- command grid inside the sidebar: two compact columns ----
    struct PBtn { bool upgrade; bool reclaim = false; std::string label; float price; std::string key; };
    std::vector<PBtn> btns;
    for (auto* it : buildableOptions(g)) btns.push_back({ false, false, it->displayText, it->price, it->name });
    bool anyReclaimer = false;
    for (auto* s : sel) {
        if (s->team != g.playerTeam() || !s->tpl) continue;
        if (s->tpl->canReclaimResources) anyReclaimer = true;
        for (auto& a : s->tpl->actions) {
            bool dup = false;
            for (auto& o : btns) if (o.upgrade && o.key == a.convertTo) dup = true;
            if (!dup) btns.push_back({ true, false, a.text, a.price, a.convertTo });
        }
    }
    if (anyReclaimer) btns.push_back({ false, true, "Reclaim", 0, "" });

    if (CheckCollisionPointRec(mouse, Rectangle{ sideX, mm.height, side, SH - mm.height }))
        ui.overPanel = true;

    float cellW = (side - 16 * k) / 2.0f, cellH = 52 * k, gap = 4 * k;
    float gy = mm.height + 8 * k;
    // Selected-unit readout occupies the first row, like RWX's info cells.
    if (!sel.empty()) {
        const Unit* u = sel.front();
        char ib[64];
        if (sel.size() > 1)
            snprintf(ib, sizeof(ib), "%zux %s", sel.size(), u->tpl ? u->tpl->displayText.c_str() : "Units");
        else
            snprintf(ib, sizeof(ib), "%s", u->tpl ? u->tpl->displayText.c_str() : "Unit");
        DrawText(ib, (int)(sideX + 8 * k), (int)gy, px(19), Color{ 150, 220, 150, 255 });
        if (u->maxHp > 0) {
            Rectangle hb{ sideX + 8 * k, gy + 22 * k, side - 16 * k, 8 * k };
            DrawRectangleRec(hb, Color{ 60, 20, 20, 255 });
            DrawRectangle(hb.x, hb.y, hb.width * std::max(0.0f, u->hp / u->maxHp), hb.height,
                          Color{ 0, 200, 0, 255 });
            snprintf(ib, sizeof(ib), "%d/%d", (int)std::ceil(u->hp), (int)std::ceil(u->maxHp));
            DrawText(ib, (int)(sideX + side - 8 * k - MeasureText(ib, px(17))), (int)(gy + 22 * k),
                     px(17), WHITE);
        }
        gy += 38 * k;
    }

    int cols = 2;
    for (size_t i = 0; i < btns.size(); ++i) {
        int col = (int)i % cols, row = (int)i / cols;
        Rectangle btn{ sideX + 6 * k + col * (cellW + gap), gy + row * (cellH + gap), cellW, cellH };
        if (btn.y + cellH > SH - 2) break;
        PBtn& b = btns[i];
        bool hover = CheckCollisionPointRec(mouse, btn);
        bool afford = me && me->credits >= b.price;
        DrawRectangleRec(btn, hover ? Color{ 58, 58, 62, 255 } : Color{ 34, 34, 38, 255 });
        DrawRectangleLinesEx(btn, 1, afford ? Color{ 90, 90, 95, 255 } : Color{ 60, 50, 50, 255 });

        Texture2D icon{};
        bool drew = false;
        if (!b.upgrade && !b.reclaim) {
            const UnitTemplate* t = g_templates.find(ToLower(b.key));
            if (t && g_render.unitIcon(t->name, icon)) {
                drawHudIcon(icon, Rectangle{ btn.x + 5 * k, btn.y + 3 * k, btn.width - 10 * k,
                                             btn.height - 20 * k }, WHITE, true);
                drew = true;
            }
        }
        if (!drew)
            drawHudIcon(b.reclaim ? g_hudArt.buildIcon : g_hudArt.upgrade,
                        Rectangle{ btn.x + 10 * k, btn.y + 5 * k, btn.width - 20 * k, btn.height - 22 * k },
                        Color{ 200, 230, 200, 255 });

        if (i < 9) DrawText(TextFormat("%d", (int)i + 1), (int)(btn.x + 4 * k), (int)(btn.y + 3 * k),
                            px(15), Color{ 200, 200, 200, 220 });
        std::string label = b.label.size() > 8 ? b.label.substr(0, 7) + "." : b.label;
        int lsize = px(15);
        int lw = MeasureText(label.c_str(), lsize);
        DrawText(label.c_str(), (int)(btn.x + (btn.width - std::min<float>(lw, btn.width - 6 * k)) / 2),
                 (int)(btn.y + btn.height - 17 * k), lsize,
                 afford ? Color{ 150, 220, 150, 255 } : Color{ 150, 110, 110, 255 });
        if (!b.reclaim && b.price > 0) {
            char pc[24]; snprintf(pc, sizeof(pc), "%d", (int)b.price);
            DrawText(pc, (int)(btn.x + 4 * k), (int)(btn.y + 3 * k), px(15),
                     afford ? Color{ 240, 220, 90, 255 } : Color{ 160, 120, 120, 255 });
        }

        if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && afford) {
            if (b.reclaim) { ui.reclaimPick = true; ui.placing = false; }
            else if (b.upgrade) {
                for (auto* u : sel) {
                    if (!u->tpl) continue;
                    for (size_t k = 0; k < u->tpl->actions.size(); ++k)
                        if (ToLower(u->tpl->actions[k].convertTo) == ToLower(b.key)) {
                            g.orderUpgrade(u->id, (int)k); break;
                        }
                }
            } else if (g_templates.find(ToLower(b.key)) && g_templates.find(ToLower(b.key))->isBuilding) {
                ui.placing = true; ui.placingName = b.key;
            } else {
                for (auto* u : sel) if (u->tpl && u->tpl->isFactory) g.orderProduce(u->id, b.key);
            }
        }
    }

    // Selection circles are already drawn by Renderer::drawUnit; no extra HUD box.

    if (g.gameOver) {
        const char* msg = g.gameOverText == "VICTORY" ? "VICTORY  (R to restart)" : "DEFEAT  (R to restart)";
        DrawText(msg, (SW - MeasureText(msg, 28)) / 2, 60, 28,
                 g.gameOverText == "VICTORY" ? GREEN : RED);
    }
}

static void handlePlacement(Game& g, Vec2 world) {
    if (!ui.placing) return;
    int tx = (int)(world.x / TILE_SIZE), ty = (int)(world.y / TILE_SIZE);
    ui.placingWorld = world;
    const UnitTemplate* t = g_templates.find(ToLower(ui.placingName));
    ui.placingValid = false;
    if (t) {
        int fw = t->footprintTilesW(), fh = t->footprintTilesH();
        int minx = tx + t->footL, miny = ty + t->footU;
        bool ok = true;
        for (int y = miny; y < miny + fh && ok; ++y) for (int x = minx; x < minx + fw && ok; ++x)
            if (!g.map.inBounds(x, y) || !(t->placeOnlyOnWater ? g.map.canPlaceWaterBuilding(x, y) : g.map.canPlaceBuilding(x, y))) ok = false;
        if (ok && !g.buildingSpotFree(t->footprintPos(tx, ty), *t)) ok = false;
        // RWX: ghost turns red off the resource pool (center-tile rule, same as orderBuild)
        if (ok && t->placeOnlyOnResPool && !(g.map.inBounds(tx, ty) && g.map.at(tx, ty).resource)) ok = false;
        ui.placingValid = ok;
    }
}

static void tryPlace(Game& g) {
    if (!ui.placing) return;
    if (ui.placingValid) {
        auto sel = g.selectedUnits();
        for (auto* b : sel) if (b->tpl && b->tpl->isBuilder) {
            int tx = (int)(ui.placingWorld.x / TILE_SIZE), ty = (int)(ui.placingWorld.y / TILE_SIZE);
            if (g.orderBuild(b->id, ui.placingName, tx, ty)) break;
        }
    }
    ui.placing = false;
}

// Load a map chosen in the level browser into a fresh Game. On failure the
// caller's game is left untouched so the browser can show a toast and stay open.
static bool loadMap(Game& game, const std::string& rel) {
    std::string err;
    Game fresh;
    if (!fresh.init(g_assetsRoot + "/" + rel, &err)) {
        TraceLog(LOG_WARNING, TextFormat("Map load failed: %s - %s", rel.c_str(), err.c_str()));
        return false;
    }
    game = std::move(fresh);
    TraceLog(LOG_INFO, TextFormat("Map: %dx%d, spawns=%zu, units=%zu",
             game.map.w, game.map.h, game.map.spawns.size(), game.units.size()));
    return true;
}

static void buildFallback(Game& game) {
    game.map.setSize(80, 60);
    for (auto& t : game.map.tiles) t.color = TC_LAND;
    // carve a river
    for (int y = 0; y < game.map.h; ++y) { int wx = game.map.w / 2 + (int)(std::sin(y * 0.3f) * 4); for (int x = wx - 1; x <= wx + 1; ++x) if (game.map.inBounds(x, y)) { game.map.at(x, y).water = true; game.map.at(x, y).color = TC_WATER; } }
    // resource pools
    int spots[4][2] = { {12,12},{game.map.w-12,12},{12,game.map.h-12},{game.map.w-12,game.map.h-12} };
    for (auto& s : spots) for (int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){ int x=s[0]+dx,y=s[1]+dy; if(game.map.inBounds(x,y)) game.map.at(x,y).resource=true; }
    Team p; p.id = 0; p.isPlayer = true; p.credits = 4000; game.teams.push_back(p);
    Team e; e.id = 1; e.color = 1; e.credits = 3000; game.teams.push_back(e);
    auto done = [&](int id) { Unit* u = game.get(id); if (u) { u->underConstruction = false; u->buildProgress = 1; } };
    Vec2 baseP{ (game.map.w * 0.75f) * TILE_SIZE, (game.map.h * 0.8f) * TILE_SIZE };
    Vec2 baseE{ (game.map.w * 0.25f) * TILE_SIZE, (game.map.h * 0.2f) * TILE_SIZE };
    done(game.spawnUnit("commandCenter", 0, baseP));
    done(game.spawnUnit("landFactory", 0, baseP + Vec2{ TILE_SIZE * 3, 0 }));
    game.spawnUnit("builder", 0, baseP + Vec2{ 0, TILE_SIZE * 2 });
    done(game.spawnUnit("commandCenter", 1, baseE));
    done(game.spawnUnit("landFactory", 1, baseE + Vec2{ -TILE_SIZE * 3, 0 }));
    game.spawnUnit("builder", 1, baseE + Vec2{ 0, TILE_SIZE * 2 });
}

static void resetCamera(Camera2D& cam, Game& game, int SW, int SH) {
    cam.rotation = 0; cam.zoom = 1.0f;
    cam.offset = { SW / 2.0f, SH / 2.0f };
    Vector2 cc{ game.map.worldW() / 2, game.map.worldH() / 2 };
    for (auto& u : game.units)
        if (u.team == game.playerTeam() && u.tpl && u.tpl->name == "commandCenter") {
            cc = Vector2{ u.pos.x, u.pos.y }; break;
        }
    cam.target = cc;
}

int main(int argc, char** argv) {
    // Optional headless-ish screenshot: `rp.exe --shot out.png [N]` renders N frames
    // (default 3) then writes the screen buffer to PNG and exits. Normal play is untouched.
    std::string shotPath, mapOverride;
    int shotFrame = 3;
    bool startInGame = false, startInLevels = false;
    bool checkMaps = false;
    // RWX's HUD metrics are authored against 1920x1080, so that is the default surface; `--size WxH`
    // exists for the screenshot harness and for testing smaller windows.
    int winW = 1920, winH = 1080;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) { shotPath = argv[++i]; }
        else if (a == "--frames" && i + 1 < argc) { shotFrame = atoi(argv[++i]); }
        else if (a == "--game") startInGame = true;
        else if (a == "--levels") startInLevels = true;
        else if (a == "--checkmaps") checkMaps = true;
        else if (a == "--map" && i + 1 < argc) { mapOverride = argv[++i]; }
        else if (a == "--size" && i + 1 < argc) {
            // "--size 1920x1080" -- the surface RWX's HUD metrics are authored against
            const char* sz = argv[++i];
            const char* x = strchr(sz, 'x');
            if (x) { winW = atoi(sz); winH = atoi(x + 1); }
        }
    }
    const int SW = winW, SH = winH;
    SetTraceLogCallback(LogToFile);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(SW, SH, "RP - Rusted Warfare C++ remake");
    SetExitKey(KEY_NULL); // ESC cancels placement, must not close the window
    SetTargetFPS(120);

    g_assetsRoot = resolveAssetRoot();
    if (checkMaps) {
        // `--checkmaps` walks every shipped .tmx and reports the ones whose tileset descriptors could not
        // be resolved to a bitmap; that is exactly what makes a map render with blank / missing tiles.
        FilePathList fl = LoadDirectoryFilesEx((g_assetsRoot + "/maps").c_str(), ".tmx", true);
        int bad = 0;
        for (unsigned i = 0; i < fl.count; ++i) {
            std::string p = fl.paths[i];
            std::string rel = p.substr((g_assetsRoot + "/maps/").size());
            TileMap m;
            std::string err;
            if (!m.loadTmx(p, &err)) {
                TraceLog(LOG_WARNING, "MAPFAIL %-58s %s", rel.c_str(), err.c_str());
                ++bad; continue;
            }
            int noArt = 0;
            for (auto& ts : m.tilesets) {
                std::string bmp = g_assetsRoot + "/tilesets/bitmaps/" + ts.imageKey;
                if (ts.imageKey.empty() || !FileExists(bmp.c_str())) ++noArt;
            }
            int blocked = 0, cliff = 0, water = 0;
            for (auto& t : m.tiles) {
                if (t.blocked) ++blocked;
                if (t.cliff) ++cliff;
                if (t.water) ++water;
            }
            TraceLog(LOG_INFO, "MAP %-58s %3dx%-3d sets=%2zu noart=%d spawns=%zu blocked=%d(cliff=%d) water=%d",
                     rel.c_str(), m.w, m.h, m.tilesets.size(), noArt, m.spawns.size(), blocked, cliff, water);
            if (noArt) ++bad;
        }
        UnloadDirectoryFiles(fl);
        TraceLog(LOG_INFO, "checkmaps: %d of %u map(s) need attention", bad, fl.count);
        CloseWindow();
        return 0;
    }
    std::string currentMapRel = mapOverride.empty() ? MAP_REL : mapOverride;
    TraceLog(LOG_INFO, "Loading unit templates...");
    g_templates.loadAll(g_assetsRoot);
    TraceLog(LOG_INFO, TextFormat("Loaded %zu unit templates", g_templates.order.size()));

    Game game;
    if (!loadMap(game, currentMapRel)) {
        TraceLog(LOG_WARNING, "Using fallback terrain");
        buildFallback(game);
        TraceLog(LOG_INFO, TextFormat("Map: %dx%d, spawns=%zu, units=%zu",
                 game.map.w, game.map.h, game.map.spawns.size(), game.units.size()));
    }

    g_render.init();

    Camera2D cam{};
    resetCamera(cam, game, SW, SH);

    double acc = 0; double last = GetTime();
    int renderedFrames = 0;
    enum class Screen { Menu, Levels, Game };
    Screen screen = startInGame ? Screen::Game : (startInLevels ? Screen::Levels : Screen::Menu);

    while (!WindowShouldClose()) {
        // ---- main menu: RWX MainMenuSceneHost ----
        if (screen == Screen::Menu) {
            BeginDrawing();
            ClearBackground(Color{ 10, 12, 16, 255 });
            MenuAction act = g_menu.drawAndInput(true);
            EndDrawing();
            if (!shotPath.empty() && ++renderedFrames >= shotFrame) {
                Image img = LoadImageFromScreen(); ExportImage(img, shotPath.c_str()); UnloadImage(img);
                TraceLog(LOG_INFO, TextFormat("shot written: %s", shotPath.c_str()));
                g_menu.close(); g_levels.close(); g_hudArt.close(); g_render.close(); CloseWindow(); return 0;
            }
            if (act == MenuAction::SinglePlayer) {
                screen = Screen::Levels;
                TraceLog(LOG_INFO, "Main menu: opening level select");
            } else if (act == MenuAction::Continue) {
                screen = Screen::Game;
                TraceLog(LOG_INFO, "Main menu: entering game");
            } else if (act == MenuAction::Exit) {
                g_menu.close(); g_levels.close(); g_hudArt.close(); g_render.close(); CloseWindow(); return 0;
            } else if (act != MenuAction::None) {
                g_menu.showToast("Not available in this build");
            }
            continue;
        }

        // ---- level select: RWX LevelSelectSceneHost ----
        if (screen == Screen::Levels) {
            BeginDrawing();
            ClearBackground(Color{ 53, 57, 53, 255 });
            LevelUI::LevelSelectScene::Result res = g_levels.drawAndInput();
            EndDrawing();
            if (!shotPath.empty() && ++renderedFrames >= shotFrame) {
                Image img = LoadImageFromScreen(); ExportImage(img, shotPath.c_str()); UnloadImage(img);
                TraceLog(LOG_INFO, TextFormat("shot written: %s", shotPath.c_str()));
                g_menu.close(); g_levels.close(); g_hudArt.close(); g_render.close(); CloseWindow(); return 0;
            }
            if (res.back) {
                screen = Screen::Menu;
            } else if (res.picked) {
                std::string rel = res.picked->path;
                if (loadMap(game, rel)) {
                    currentMapRel = rel;
                    resetCamera(cam, game, SW, SH);
                    ui = UI{};
                    screen = Screen::Game;
                    TraceLog(LOG_INFO, TextFormat("Level select: starting %s", rel.c_str()));
                } else {
                    g_levels.showToast("Map could not be loaded");
                }
            }
            continue;
        }

        double now = GetTime();
        double dt = std::min(now - last, 0.25); // no catch-up ticks after time in the menus
        acc += dt * (g_debug.paused ? 0.0 : g_debug.speed); last = now;
        if (g_debug.step) { acc += 1.0 / TICK_RATE; g_debug.step = false; }
        while (acc >= 1.0 / TICK_RATE) { if (!game.gameOver) game.update(); acc -= 1.0 / TICK_RATE; }

        // camera pan (WASD doubles as the spawn picker's text input, so it yields while typing)
        float pan = 600.0f / cam.zoom * GetFrameTime();
        if (!g_debug.spawnOpen) {
            if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) cam.target.y -= pan;
            if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) cam.target.y += pan;
            if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) cam.target.x -= pan;
            if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) cam.target.x += pan;
        } else {
            if (IsKeyDown(KEY_UP)) cam.target.y -= pan;
            if (IsKeyDown(KEY_DOWN)) cam.target.y += pan;
            if (IsKeyDown(KEY_LEFT)) cam.target.x -= pan;
            if (IsKeyDown(KEY_RIGHT)) cam.target.x += pan;
        }
        Vector2 mouse = GetMousePosition();
        // wheel over the F2 inspector scrolls the panel instead of zooming the camera
        bool overInsp = g_debug.inspector && CheckCollisionPointRec(mouse, g_debug.inspRect);
        float wheel = overInsp ? 0.0f : GetMouseWheelMove();
        if (IsKeyDown(KEY_MINUS) || wheel < 0) cam.zoom = std::max(0.3f, cam.zoom - 0.1f);
        if (IsKeyDown(KEY_EQUAL) || wheel > 0) cam.zoom = std::min(3.0f, cam.zoom + 0.1f);
        // clamp target to world
        cam.target.x = std::max(0.0f, std::min(game.map.worldW(), cam.target.x));
        cam.target.y = std::max(0.0f, std::min(game.map.worldH(), cam.target.y));

        Vector2 worldV = GetScreenToWorld2D(mouse, cam);
        Vec2 world{ worldV.x, worldV.y };
        bool overUI = ui.overPanel || ui.overMinimap;

        debugHandleKeys(game);
        if (g_debug.spawnOpen || g_debug.spawnArmed) { ui.placing = false; ui.reclaimPick = false; }

        if (ui.markerOn) { ui.markerT += (float)GetFrameTime(); if (ui.markerT > 0.6f) ui.markerOn = false; }
        handlePlacement(game, world);

        // ---- selection drag (left mouse) ----
        bool dbgClick = debugMouseLeft(game, mouse, world, overUI);
        if (!dbgClick && !overUI && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (ui.placing) tryPlace(game);
            else { ui.dragging = true; ui.dragA = mouse; ui.dragB = mouse; }
        }
        if (ui.dragging) {
            ui.dragB = mouse;
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Vector2 a = GetScreenToWorld2D(ui.dragA, cam), b = GetScreenToWorld2D(ui.dragB, cam);
                float w = std::abs(b.x - a.x), h = std::abs(b.y - a.y);
                if (w * cam.zoom < 6 && h * cam.zoom < 6) {
                    // single click
                    Unit* best = nullptr; float bd = 1e9f;
                    for (auto& u : game.units) if (u.tpl && !u.riding()) { float d = Vec2{ a.x, a.y }.dist(u.pos); if (d < std::max(12.0f, u.tpl->radius) && d < bd) { bd = d; best = &u; } }
                    // GameUI:1934-1955: with the reclaim action armed the same click issues the command
                    // instead of selecting -- any unit under the cursor, whatever its team, since the general
                    // ReclaimTargetAction(true) isAvailableAndVisible():79-84 answers true for all of them.
                    // orderReclaim then drops the hulls RWX's canReclaimTarget:2179 would drop.
                    // Shift leaves the action armed for another target, as clearCurrentAction() is skipped.
                    if (ui.reclaimPick && best) {
                        for (auto* s : game.selectedUnits()) if (s->tpl && s->tpl->canReclaimResources) game.orderReclaim(s->id, best->id);
                        if (!IsKeyDown(KEY_LEFT_SHIFT) && !IsKeyDown(KEY_RIGHT_SHIFT)) ui.reclaimPick = false;
                    } else if (!ui.reclaimPick) {
                        clearSelection(game);
                        if (best && best->team == game.playerTeam()) best->selected = true;
                    }
                } else selectBox(game, a, b);
                ui.dragging = false;
            }
        }

        // ---- right click orders ----
        if (debugMouseRight()) {}
        else if (!overUI && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            if (ui.placing || ui.reclaimPick) { ui.placing = false; ui.reclaimPick = false; }
            else {
                auto sel = game.selectedUnits();
                if (!sel.empty()) {
                    // enemy at point? (RWX-style generous click radius)
                    Unit* enemy = nullptr; float bd = 1e9f;
                    for (auto& u : game.units) if (u.tpl && game.isEnemy(game.playerTeam(), u.team)) { float d = world.dist(u.pos); if (d < std::max(26.0f, u.tpl->radius + 10.0f) && d < bd) { bd = d; enemy = &u; } }
                    // RWX GameUI.handleUnitTargetClick:2653 - a friendly target that is damaged or
                    // unfinished takes the repair command instead of a move order. orderRepair only
                    // accepts it when every selected unit is a builder able to fix that kind of target.
                    Unit* site = nullptr; float sd = 1e9f;
                    for (auto& u : game.units) {
                        if (!u.tpl || u.dead || u.team != game.playerTeam()) continue;
                        if (u.operational() && u.hp >= u.maxHp) continue;
                        float d = world.dist(u.pos);
                        if (d < std::max(26.0f, u.tpl->radius + 10.0f) && d < sd) { sd = d; site = &u; }
                    }
                    std::vector<int> ids; for (auto* s : sel) ids.push_back(s->id);
                    // RWX GameUI.handleUnitTargetClick puts the transport branches after repair and before
                    // the plain move: :2728-2731 a friendly carrier any selected hull can board takes the
                    // loadInto order, and :2732-2735 (PC) a friendly hull a selected carrier can take aboard
                    // takes loadUp instead. canSelectedUnitsLoadInto:3856-3869 asks for *any* one of the
                    // selected units to qualify, then issueLoadIntoCommand gives it to all that do.
                    auto atPoint = [&](std::function<bool(const Unit& u)> ok) {
                        Unit* best = nullptr; float bd = 1e9f;
                        for (auto& u : game.units) {
                            if (!u.tpl || u.dead || u.team != game.playerTeam() || !ok(u)) continue;
                            float d = world.dist(u.pos);
                            if (d < std::max(26.0f, u.tpl->radius + 10.0f) && d < bd) { bd = d; best = &u; }
                        }
                        return best;
                    };
                    Unit* carrier = atPoint([&](const Unit& tr) {
                        if (!tr.tpl->canTransportUnits()) return false;
                        for (auto* s : sel) if (game.canLoadInto(tr, *s)) return true;
                        return false;
                    });
                    Unit* rideable = atPoint([&](const Unit& c) {
                        if (c.tpl->isStatic() || c.tpl->canTransportUnits()) return false;
                        for (auto* s : sel) if (game.canLoadInto(*s, c)) return true;
                        return false;
                    });
                    // GameUI.handleUnitTargetClick:2699-2726: right-clicking a [core] resourceRate target that
                    // *every* selected hull may reclaim issues the reclaim order instead of a move -- and the
                    // target needs no team check here, since OrderableUnit:2202 only refuses a cross-team one
                    // whose resourceRate is 0. RP keeps RWX's "all or nothing": one hull without
                    // [core] canReclaimResources and the whole click falls through to move.
                    Unit* harvest = nullptr; float hd = 1e9f;
                    for (auto& u : game.units) {
                        if (!u.tpl || u.dead || u.riding() || !(u.tpl->resourceRate > 0)) continue;
                        float d = world.dist(u.pos);
                        if (d < std::max(26.0f, u.tpl->radius + 10.0f) && d < hd) { hd = d; harvest = &u; }
                    }
                    bool allReclaim = !sel.empty();
                    for (auto* s : sel) if (!s->tpl || !s->tpl->canReclaimResources) allReclaim = false;
                    if (enemy && game.orderAttackTarget(ids, enemy->id) > 0) {} // attack order taken
                    else if (site && game.orderRepair(ids, site->id)) {}
                    else if (harvest && allReclaim) { for (int id : ids) game.orderReclaim(id, harvest->id); }
                    else if (carrier && game.orderLoadInto(ids, carrier->id) > 0) {}
                    else if (rideable && game.orderLoadUp(ids, rideable->id) > 0) {}
                    else if (IsKeyDown(KEY_LEFT_CONTROL)) game.orderAttackMove(ids, world);
                    else game.orderMove(ids, world);
                    ui.markerOn = true; ui.markerPos = worldV; ui.markerT = 0;
                }
            }
        }
        if (IsKeyPressed(KEY_ESCAPE)) { ui.placing = false; ui.reclaimPick = false; clearSelection(game); }
        if (!g_debug.spawnOpen) {
        if (IsKeyPressed(KEY_X)) { std::vector<int> ids; for (auto* s : game.selectedUnits()) ids.push_back(s->id); game.orderStop(ids); }
        // RWX puts unload / load-up in the command bar for every carrier with
        // transportUnitsAddUnloadOption (CustomUnitConfigParser.java:1844); RP triggers them from keys.
        if (IsKeyPressed(KEY_U) || IsKeyPressed(KEY_I)) {
            bool unload = IsKeyPressed(KEY_U);
            for (auto* s : game.selectedUnits()) {
                if (!s->tpl || !s->tpl->canTransportUnits()) continue;
                if (unload) game.orderUnload(s->id); else game.orderLoadUpAgain(s->id);
            }
        }
        if (IsKeyPressed(KEY_R) && game.gameOver) {
            if (loadMap(game, currentMapRel)) resetCamera(cam, game, SW, SH);
            for (auto& u : game.units) u.selected = false;
            game.gameOver = false;
        }
        } // !spawnOpen: game hotkeys yield to the picker's text input

        // ---- render ----
        ViewState view;
        view.cam = cam;
        view.placing = ui.placing; view.placingName = ui.placingName; view.placingWorld = ui.placingWorld; view.placingValid = ui.placingValid;
        view.selActive = ui.dragging; view.selA = { ui.dragA.x, ui.dragA.y }; view.selB = { ui.dragB.x, ui.dragB.y };
        view.cmdMarkerOn = ui.markerOn; view.cmdMarker = { ui.markerPos.x, ui.markerPos.y }; view.cmdMarkerT = ui.markerT;

        BeginDrawing();
        ClearBackground(Color{ 24, 28, 32, 255 });
        g_render.drawWorld(game, view);
        renderHUD(game, cam);
        debugDrawHUD(game, cam);
        // RWX keeps the armed action visible by drawing its tooltip on the cursor (drawActionTooltip,
        // GameUI:1935); RP has no tooltip art, so it prints the prompt instead.
        if (ui.reclaimPick) {
            Vector2 m2 = GetMousePosition();
            DrawText("Reclaim: click a target", m2.x + 14, m2.y + 16, 16, Color{ 255, 200, 90, 255 });
        }
        EndDrawing();
        if (!shotPath.empty() && ++renderedFrames >= shotFrame) {
            Image img = LoadImageFromScreen();
            ExportImage(img, shotPath.c_str());
            UnloadImage(img);
            TraceLog(LOG_INFO, TextFormat("shot written: %s", shotPath.c_str()));
            break;
        }
    }

    g_menu.close();
    g_levels.close();
    g_hudArt.close(); g_render.close();
    CloseWindow();
    return 0;
}
