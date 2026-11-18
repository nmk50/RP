// RP main - window, game loop, camera, input, HUD, minimap.
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>
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
#include "Mods.h"

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
    // HUD tooltip text set while hovering the UnitInfo button (GameUI.drawActionTooltip).
    std::string hudTip;
};
static UI ui;

// ---------------------------------------------------------------------------
// Main menu. Mirrors RWX's kotlin MainMenuSceneHost (core/.../io/github/rwx/ui/host/
// MainMenuSceneHost.kt) and its MainMenuViewModel / UiTheme.Layout constants:
//   title "RWX" + subtitle, a grid of 172x124 tiles (badge + label) and a fixed
//   About / Exit footer. Panel surface alpha is 0.52. RP lays the eight tiles out
//   as 4 columns x 2 rows (RWX's own grid is two columns); the badge/label/footer
//   styling still follows the Kotlin source.
// Buttons that need engine features not ported yet (replay/multiplayer/...) fall back to a small
// "not available" toast; Single Player / Continue enter the game and Mods opens the mod screen.
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

// ---------------------------------------------------------------------------
// Mods screen, ported from RWX's Kotlin/Kool sources:
//   ui/host/ModsSceneHost.kt          - scene layout: filter field + Back, one scrollable list
//                                       grouped "Enabled" / "Disabled", Reload / Import File /
//                                       Disable All / Apply action bar
//   ui/component/ModsComponents.kt    - ModCard:160-248: 88dp card, 68dp thumbnail, title,
//                                       author/version line, description clamped to 45dp,
//                                       Enable/Disable button
//   ui/UiTheme.kt:451-460             - content width 1180 (320..1560), viewport 560 (260..720),
//                                       card 88, thumbnail 68, description 45
//   app/ModsController.kt             - toggle/reload/disableAll/apply wiring
// The data side is ModRegistry (src/Mods.h). Apply = ModManager.applyAndSaveMods():510-520, i.e.
// persist the selection and reload every custom unit.
// Delete is deliberately absent: ModInfo.canBeDeleted():584-595 answers false for anything that is
// not a built-in archive, so on PC it is dead for every folder the user could point at.
// Import File is a drag-and-drop here instead of a native file dialog -- dropping a folder or a
// .rwmod onto the window copies it into assets/mods/units/ (ModRepository.importMod:68-75).
// ---------------------------------------------------------------------------
namespace ModUI {

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

static std::string ellipsize(const std::string& s, int fs, float maxW) {
    if (MeasureText(s.c_str(), fs) <= maxW) return s;
    std::string t = s;
    while (!t.empty() && MeasureText((t + "...").c_str(), fs) > maxW) t.pop_back();
    return t + "...";
}
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

// Greedy word wrap that keeps a hard cap on the line count; the tail is folded into the last line
// with an ellipsis, which is what ClampedText does in ModsComponents.kt:219-227.
static std::vector<std::string> wrapText(const std::string& text, int fs, float maxW, int maxLines) {
    std::vector<std::string> out;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '\n') {
            out.push_back(line); line.clear(); ++i;
            if ((int)out.size() >= maxLines) { line.clear(); break; }
            continue;
        }
        if (!line.empty() && MeasureText((line + c).c_str(), fs) > maxW) {
            size_t sp = line.find_last_of(' ');
            if (c != ' ' && sp != std::string::npos && sp > 0) {
                out.push_back(line.substr(0, sp));
                line = line.substr(sp + 1);
                if ((int)out.size() >= maxLines) { line.clear(); break; }
                continue;                        // retry this character on the fresh line
            }
            out.push_back(line);
            line.clear();
            if ((int)out.size() >= maxLines) break;
            if (c == ' ') { ++i; continue; }
            continue;
        }
        line.push_back(c);
        ++i;
    }
    if (!line.empty()) {
        if ((int)out.size() < maxLines) out.push_back(line);
        else if (!out.empty()) out.back() = ellipsize(out.back() + " " + line, fs, maxW);
    }
    return out;
}

struct ModScene {
    static constexpr float CARD_H = 88, THUMB = 68, GAP = 6, HEADER_H = 30;
    static constexpr float TOP_H = 46, BAR_H = 54, BACK_W = 96, BTN_W = 118, BTN_H = 34;

    // A flattened list row: either a group header or an index into ModRegistry::all().
    struct Row { int mod = -1; int header = 0; }; // header 1 = Enabled, 2 = Disabled

    std::string query;
    bool queryFocus = false, inited = false;
    float scroll = 0, eraseRepeat = 0, caretBlink = 0;
    std::string toast; float toastT = 0;
    bool dirty = false;
    std::vector<Row> rows;
    std::unordered_map<std::string, Texture2D> thumbs;

    struct Result { bool back = false; bool applied = false; };

    void init() { if (!inited) { inited = true; rebuild(); } }
    void close() {
        for (auto& kv : thumbs) if (kv.second.id) UnloadTexture(kv.second);
        thumbs.clear();
        inited = false;
    }
    void showToast(const char* s) { toast = s; toastT = 2.5f; }

    bool matches(const ModEntry& e) const {
        std::string q = lower(Trim(query));
        if (q.empty()) return true;
        return lower(e.displayTitle()).find(q) != std::string::npos ||
               lower(e.dirName).find(q) != std::string::npos ||
               lower(e.description).find(q) != std::string::npos;
    }

    void rebuild() {
        rows.clear();
        const auto& all = g_mods.all();
        for (int pass = 1; pass <= 2; ++pass) {
            bool wantEnabled = (pass == 1);
            int count = 0;
            for (const ModEntry& e : all) if (e.enabled == wantEnabled && matches(e)) ++count;
            if (count == 0) continue;
            rows.push_back({ -1, pass });
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i].enabled == wantEnabled && matches(all[i])) rows.push_back({ (int)i, 0 });
        }
    }

    // Rescan the four roots but keep whatever the user has toggled in this session; a mod that was
    // not on disk before takes its default state (ModManager.loadAllMods only resets new entries).
    void reload() {
        std::unordered_map<std::string, bool> keep;
        for (const ModEntry& e : g_mods.all()) if (e.canToggle()) keep[e.uuid] = e.enabled;
        g_mods.scan(g_assetsRoot);
        for (ModEntry& e : g_mods.mutableAll()) {
            auto it = keep.find(e.uuid);
            if (it != keep.end()) e.enabled = it->second;
        }
        dirty = false;
        scroll = 0;
        rebuild();
        showToast("Rescanned mods folder");
    }

    Texture2D* thumbFor(const ModEntry& e) {
        if (e.thumbnail.empty()) return nullptr;
        auto it = thumbs.find(e.thumbnail);
        if (it == thumbs.end()) {
            Texture2D t = LoadTexture(e.thumbnail.c_str());
            it = thumbs.emplace(e.thumbnail, t).first;
        }
        return it->second.id ? &it->second : nullptr;
    }

    Result drawAndInput() {
        Result res;
        init();
        int SW = GetScreenWidth(), SH = GetScreenHeight();
        float k = clampf(std::min(SW / 1920.0f, SH / 1080.0f), 0.5f, 2.0f);
        Vector2 mouse = GetMousePosition();

        Color primary{ 90, 190, 220, 255 }, accent{ 240, 180, 70, 255 };
        Color border{ 90, 100, 110, 180 }, surface{ 30, 35, 42, 235 }, sunken{ 20, 23, 28, 255 };
        Color raised{ 46, 52, 60, 255 }, text{ 225, 230, 235, 255 }, sub{ 150, 160, 170, 255 };
        Color danger{ 220, 100, 90, 255 };

        DrawRectangle(0, 0, SW, SH, Color{ 10, 12, 16, 255 });
        DrawRectangle(0, 0, SW, SH, Color{ 0, 0, 0, 133 });

        float contentW = clampf(1180 * k, 320, 1560);
        contentW = std::min(contentW, (float)SW - 40 * k);
        float x0 = (SW - contentW) / 2;

        int titleFs = (int)(26 * k);
        DrawText("Mods", (int)x0, (int)(12 * k), titleFs, WHITE);
        {
            char st[192];
            snprintf(st, sizeof(st), "%d of %d mod(s) enabled  |  %zu templates  |  drop a folder here to install",
                     g_mods.enabledCount() - 1, g_mods.toggleableCount(), g_templates.order.size());
            DrawText(st, (int)(x0 + MeasureText("Mods", titleFs) + 16 * k), (int)(18 * k), (int)(16 * k), sub);
        }

        // ---- top row: Back + filter (ModsFilterField) ----
        float topY = (float)(12 * k) + titleFs + 10 * k;
        Rectangle backR{ x0, topY, BACK_W * k, TOP_H * k };
        bool backHot = CheckCollisionPointRec(mouse, backR);
        DrawRectangleRec(backR, backHot ? raised : surface);
        DrawRectangleLinesEx(backR, 1, backHot ? primary : border);
        DrawText("Back", (int)(backR.x + (backR.width - MeasureText("Back", (int)(17 * k))) / 2),
                 (int)(backR.y + (backR.height - 17 * k) / 2), (int)(17 * k), backHot ? primary : text);
        if (backHot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) res.back = true;

        Rectangle input{ backR.x + backR.width + 10 * k, topY, contentW - backR.width - 10 * k, TOP_H * k };
        bool inputHot = CheckCollisionPointRec(mouse, input);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) queryFocus = inputHot;
        DrawRectangleRec(input, sunken);
        DrawRectangleLinesEx(input, 1, queryFocus ? primary : border);
        {
            int fs = (int)(16 * k);
            std::string shown = ellipsize(query.empty() && !queryFocus ? "Search mods" : query,
                                          fs, input.width - 60 * k);
            DrawText(shown.c_str(), (int)(input.x + 10 * k), (int)(input.y + (input.height - fs) / 2), fs,
                     query.empty() ? sub : text);
            if (queryFocus) {
                caretBlink += GetFrameTime();
                if (std::fmod(caretBlink, 1.0f) < 0.5f)
                    DrawText("_", (int)(input.x + 11 * k + MeasureText(shown.c_str(), fs)),
                             (int)(input.y + (input.height - fs) / 2 - 1), fs, primary);
            }
            if (!query.empty()) {
                Rectangle clr{ input.x + input.width - 32 * k, input.y + (input.height - 26 * k) / 2, 26 * k, 26 * k };
                bool clrHot = CheckCollisionPointRec(mouse, clr);
                DrawRectangleRec(clr, clrHot ? Color{ 120, 70, 50, 255 } : Color{ 50, 40, 40, 255 });
                DrawRectangleLinesEx(clr, 1, clrHot ? primary : border);
                DrawText("x", (int)(clr.x + 9 * k), (int)(clr.y + 4 * k), (int)(17 * k), text);
                if (clrHot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) { query.clear(); rebuild(); }
            }
        }
        if (queryFocus) {
            // Deletion is driven by key state only; raylib reports Backspace as char 8 on some
            // platforms, so handling both would delete two characters per press.
            int cp;
            bool typed = false;
            while ((cp = GetCharPressed()) > 0) {
                if (cp >= 32 && cp < 127) { query += (char)cp; typed = true; }
            }
            bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            bool eraseDown = IsKeyDown(KEY_BACKSPACE) || IsKeyDown(KEY_DELETE);
            if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_DELETE)) {
                if (ctrl) popLastWord(query); else popLastChar(query);
                eraseRepeat = 0.35f; typed = true;
            } else if (eraseDown) {
                eraseRepeat -= GetFrameTime();
                if (eraseRepeat <= 0) {
                    if (ctrl) popLastWord(query); else popLastChar(query);
                    eraseRepeat = 0.06f; typed = true;
                }
            } else {
                eraseRepeat = 0;
            }
            if (typed) rebuild();
        } else {
            eraseRepeat = 0;
        }

        // ---- bottom action bar (ModsActionBar) ----
        float barY = (float)SH - BAR_H * k - 12 * k;
        float listY = topY + TOP_H * k + 12 * k;
        float listH = barY - 12 * k - listY;

        // ---- scrollable list ----
        float contentH = 0;
        for (const Row& r : rows) contentH += (r.header ? HEADER_H : CARD_H) * k + GAP * k;
        float maxScroll = std::max(0.0f, contentH - listH);
        if (CheckCollisionPointRec(mouse, Rectangle{ x0, listY, contentW, listH })) {
            float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) scroll -= wheel * 60 * k;
        }
        if (IsKeyDown(KEY_DOWN) && !queryFocus) scroll += 400 * k * GetFrameTime();
        if (IsKeyDown(KEY_UP) && !queryFocus) scroll -= 400 * k * GetFrameTime();
        scroll = clampf(scroll, 0.0f, maxScroll);

        BeginScissorMode((int)x0, (int)listY, (int)contentW, (int)listH);
        float ry = listY - scroll;
        for (const Row& r : rows) {
            float h = (r.header ? HEADER_H : CARD_H) * k;
            if (ry + h >= listY && ry <= listY + listH) {
                if (r.header) drawHeader(Rectangle{ x0, ry, contentW, h }, r.header, k, text, sub, accent, primary);
                else drawCard(Rectangle{ x0, ry, contentW, h }, r.mod, k, mouse,
                              primary, accent, border, surface, sunken, raised, text, sub, danger);
            }
            ry += h + GAP * k;
        }
        if (rows.empty())
            DrawText("No mods match the filter", (int)(x0 + 8 * k), (int)(listY + 12 * k), (int)(17 * k), sub);
        EndScissorMode();

        // scrollbar hint
        if (maxScroll > 0.0f) {
            float trackX = x0 + contentW + 6 * k;
            float thumbH = std::max(24 * k, listH * (listH / contentH));
            float t = scroll / maxScroll;
            DrawRectangle((int)trackX, (int)(listY + t * (listH - thumbH)), (int)(4 * k), (int)thumbH,
                          Color{ 120, 130, 140, 200 });
        }

        // ---- action bar: Reload / Disable All / Apply ----
        struct BarBtn { const char* label; int id; };
        BarBtn bar[3] = { { "Reload", 0 }, { "Disable All", 1 }, { "Apply", 2 } };
        float bw = (contentW - 2 * 10 * k) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            Rectangle r{ x0 + i * (bw + 10 * k), barY, bw, BAR_H * k };
            bool hot = CheckCollisionPointRec(mouse, r);
            bool strong = (bar[i].id == 2 && dirty);
            Color fill = hot ? Color{ 60, 90, 110, 255 } : (strong ? Color{ 40, 70, 90, 255 } : surface);
            DrawRectangleRec(r, fill);
            DrawRectangleLinesEx(r, 1, hot || strong ? primary : border);
            int fs = (int)(18 * k);
            int tw = MeasureText(bar[i].label, fs);
            DrawText(bar[i].label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + (r.height - fs) / 2), fs,
                     hot || strong ? WHITE : text);
            if (hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                if (bar[i].id == 0) reload();
                else if (bar[i].id == 1) {
                    g_mods.disableAll();
                    dirty = true;
                    rebuild();
                } else {
                    g_mods.saveSelection();
                    res.applied = true;
                }
            }
        }

        // ---- drag & drop import (ModRepository.importMod:68-75) ----
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            int imported = 0;
            for (unsigned i = 0; i < dropped.count; ++i) {
                std::string src = dropped.paths[i];
                size_t slash = src.find_last_of("/\\");
                std::string name = (slash == std::string::npos) ? src : src.substr(slash + 1);
                if (name.empty()) continue;
                std::string dst = g_assetsRoot + "/mods/units/" + name;
                std::error_code ec;
                if (DirectoryExists(src.c_str())) {
                    std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive |
                                                     std::filesystem::copy_options::overwrite_existing, ec);
                } else if (EndsWith(lower(src), ".rwmod") || EndsWith(lower(src), ".zip")) {
                    // An archive has to be unpacked, which RP cannot do without a zip reader; tell
                    // the user to extract it rather than silently doing nothing.
                    showToast("Extract the archive into assets/mods/units/ first");
                    continue;
                } else {
                    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
                }
                if (!ec) ++imported;
            }
            UnloadDroppedFiles(dropped);
            if (imported > 0) {
                reload();
                showToast("Imported - enable it above, then Apply");
            }
        }

        // ---- toast ----
        if (!toast.empty()) {
            toastT -= GetFrameTime();
            if (toastT <= 0) toast.clear();
            else {
                int fs = (int)(16 * k);
                int tw = MeasureText(toast.c_str(), fs);
                DrawRectangle(SW / 2 - tw / 2 - 12, (int)(barY - 40 * k), tw + 24, (int)(30 * k),
                              Color{ 0, 0, 0, 210 });
                DrawText(toast.c_str(), SW / 2 - tw / 2, (int)(barY - 33 * k), fs, accent);
            }
        }
        return res;
    }

    void drawHeader(Rectangle r, int header, float k, Color text, Color sub, Color accent, Color primary) {
        bool enabledGroup = (header == 1);
        const char* label = enabledGroup ? "Enabled" : "Disabled";
        int n = 0;
        for (const ModEntry& e : g_mods.all()) if (e.enabled == enabledGroup && matches(e)) ++n;
        int fs = (int)(18 * k);
        DrawRectangle((int)r.x, (int)(r.y + r.height - 2 * k), (int)r.width, (int)(1 * k),
                      Color{ 70, 80, 90, 160 });
        DrawText(label, (int)(r.x + 4 * k), (int)(r.y + (r.height - fs) / 2), fs,
                 enabledGroup ? primary : sub);
        char cnt[32];
        snprintf(cnt, sizeof(cnt), "(%d)", n);
        DrawText(cnt, (int)(r.x + 8 * k + MeasureText(label, fs)), (int)(r.y + (r.height - fs) / 2), fs, accent);
    }

    void drawCard(Rectangle r, int index, float k, Vector2 mouse,
                  Color primary, Color accent, Color border, Color surface, Color sunken,
                  Color raised, Color text, Color sub, Color danger) {
        if (index < 0 || index >= (int)g_mods.all().size()) return;
        ModEntry& e = g_mods.mutableAll()[index];
        bool hot = CheckCollisionPointRec(mouse, r);

        DrawRectangleRec(r, hot ? Color{ 36, 42, 50, 245 } : surface);
        DrawRectangleLinesEx(r, 1, e.error.empty() ? (hot ? primary : border) : danger);

        float pad = 10 * k;
        float x = r.x + pad;
        // thumbnail (ModCardThumbnail:251-272)
        if (Texture2D* t = thumbFor(e)) {
            Rectangle box{ x, r.y + (r.height - THUMB * k) / 2, THUMB * k, THUMB * k };
            DrawRectangleRec(box, Color{ 18, 20, 24, 255 });
            DrawRectangleLinesEx(box, 1, border);
            float s = std::min((box.width - 4 * k) / t->width, (box.height - 4 * k) / t->height);
            DrawTextureEx(*t, Vector2{ box.x + (box.width - t->width * s) / 2,
                                       box.y + (box.height - t->height * s) / 2 }, 0, s, WHITE);
            x += THUMB * k + 12 * k;
        }

        // Enable/Disable button, right aligned (ModCardButton)
        float btnW = (e.canToggle() ? BTN_W : 96) * k;
        Rectangle btn{ r.x + r.width - btnW - pad, r.y + (r.height - BTN_H * k) / 2, btnW, BTN_H * k };
        float textW = btn.x - x - 10 * k;

        int titleFs = (int)(18 * k);
        DrawText(ellipsize(e.displayTitle(), titleFs, textW).c_str(), (int)x, (int)(r.y + 8 * k), titleFs, text);

        // meta line: kind + unit count + minVersion
        {
            char meta[192];
            if (e.minVersion.empty())
                snprintf(meta, sizeof(meta), "%s  |  %d ini", e.kindLabel(), e.iniCount);
            else
                snprintf(meta, sizeof(meta), "%s  |  %d ini  |  needs %s", e.kindLabel(), e.iniCount,
                         e.minVersion.c_str());
            DrawText(ellipsize(meta, (int)(14 * k), textW).c_str(), (int)x, (int)(r.y + 30 * k),
                     (int)(14 * k), sub);
        }
        // description, clamped (modsCardDescriptionMaxHeight = 45dp)
        {
            int fs = (int)(14 * k);
            std::string body = e.error.empty() ? e.description : e.error;
            if (!body.empty()) {
                auto lines = wrapText(body, fs, textW, 2);
                for (size_t i = 0; i < lines.size(); ++i)
                    DrawText(lines[i].c_str(), (int)x, (int)(r.y + 50 * k + i * (fs + 2 * k)), fs,
                             e.error.empty() ? sub : danger);
            } else {
                DrawText(e.relPath.c_str(), (int)x, (int)(r.y + 50 * k), fs, Color{ 110, 120, 130, 255 });
            }
        }

        // toggle button
        bool btnHot = CheckCollisionPointRec(mouse, btn);
        bool on = e.enabled;
        Color fill = !e.canToggle() ? Color{ 40, 44, 50, 255 }
                                  : (btnHot ? (on ? Color{ 120, 70, 50, 255 } : Color{ 40, 90, 70, 255 })
                                            : (on ? Color{ 70, 45, 38, 255 } : Color{ 30, 60, 48, 255 }));
        DrawRectangleRec(btn, fill);
        DrawRectangleLinesEx(btn, 1, !e.canToggle() ? border : (btnHot ? accent : (on ? accent : primary)));
        const char* lbl = !e.canToggle() ? "Always on" : (on ? "Disable" : "Enable");
        int fs = (int)(16 * k);
        int tw = MeasureText(lbl, fs);
        DrawText(lbl, (int)(btn.x + (btn.width - tw) / 2), (int)(btn.y + (btn.height - fs) / 2), fs,
                 e.canToggle() ? text : sub);
        if (btnHot && e.canToggle() && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            e.enabled = !e.enabled;
            dirty = true;
            rebuild();
        }
    }
};

} // namespace ModUI
static ModUI::ModScene g_modScene;


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
        // Whether an option shows up at all: tech hook plus placement-vs-production, decided by the
        // target. Both roles can live on one hull (combatEngineer is builder and factory), so asking
        // the owner's own flags with a mutual-exclusion test used to blank the whole menu.
        if (!buildOptionAllowed(*tpl, *it, bo.tech)) continue;
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
        // Minimap:440-470 -- the terrain under fog is washed out, and a tile that was never seen is
        // not drawn at all, which is what leaves the black border around the explored part.
        if (g.fogEnabled && !g.fog.empty()) {
            uint8_t f = g.fog[(size_t)y * g.map.w + x];
            if (f >= Game::FOG_UNSEEN) continue;
            if (f >= Game::FOG_EXPLORED) c = Color{ (uint8_t)(c.r / 3), (uint8_t)(c.g / 3), (uint8_t)(c.b / 3), 255 };
        }
        DrawRectangle(mm.x + x * tsx, mm.y + y * tsy, std::ceil(tsx) + 0.5f, std::ceil(tsy) + 0.5f, c);
    }
    for (auto& u : g.units) {
        if (!u.tpl || u.riding()) continue;
        // Scenery is team -1 (the tileset says `team=none`) and RWX's Minimap only plots real units,
        // so a forest never turns into a field of red dots.
        if (u.team < 0 || u.tplKey == "tree") continue;
        // Minimap:465 -- a blip only shows while its tile is in sight, so an enemy army does not
        // announce itself from inside the fog.
        if (!g.visibleToPlayer(u)) continue;
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

    // ---- team resources (GameUI.processTouchInput:1348-1360 / drawTeamResources:1403-1417) ----
    // Always show the player's team; when the first controllable selected unit belongs to a
    // different team, RWX draws that team's resources as a second row as well.
    auto sel = g.selectedUnits();
    // BaseUnit.getCreditIncomeRate (CustomUnit:1215-1221) is the generation lump scaled by
    // CustomUnitConfigParser:1527 generationRate = 40 / generationDelay, i.e. a per-40-tick
    // figure. Per second that is lump * (40/delay) * 60/40 = lump * 60/delay; at the default
    // delay of 40 it collapses to the familiar 1.5x.
    auto incomePerSec = [](const UnitTemplate& t) -> float {
        if (t.creditIncomeRate == 0.0f) return 0.0f;
        float d = (float)(t.generationDelay > 0 ? t.generationDelay : 40);
        return t.creditIncomeRate * (40.0f / d) * 1.5f;
    };
    // Utility.padString:602-607 / toHexString:610-615 -- a whole number loses its decimal,
    // anything else keeps one digit. Both the button text and the tooltip use it.
    auto fmtRate = [](float v) -> std::string {
        char b[32];
        float a = std::fabs(v);
        if (a == (float)(int)a) snprintf(b, sizeof(b), "%d", (int)a);
        else snprintf(b, sizeof(b), "%.1f", a);
        return std::string(b);
    };
    auto teamIncome = [&](int teamId) {
        float v = 0;
        for (auto& u : g.units)
            if (u.team == teamId && u.tpl && !u.dead) v += incomePerSec(*u.tpl);
        return (int)std::lround(v);
    };
    auto drawTeamRow = [&](const Team* tm, int row, const char* prefix, Color col) {
        if (!tm) return;
        int inc = teamIncome(tm->id);
        std::string cs = std::to_string((int)std::abs(tm->credits));
        for (int i = (int)cs.size() - 3; i > 0; i -= 3) cs.insert(i, " ");
        char b[128];
        snprintf(b, sizeof(b), "%s$%s(%+d)", prefix, cs.c_str(), inc);
        int fs = px(26);
        int w = MeasureText(b, fs);
        DrawRectangle(sideX - w - px(18), px(8) + row * px(32), w + px(14), px(32), Color{ 0, 0, 0, 90 });
        DrawText(b, sideX - w - px(12), px(10) + row * px(32), fs, col);
    };
    drawTeamRow(me, 0, "", Color{ 90, 220, 90, 255 });
    const Unit* firstSel = sel.empty() ? nullptr : sel.front();
    if (firstSel && firstSel->team != g.playerTeam()) {
        Team* other = nullptr;
        for (auto& t : g.teams) if (t.id == firstSel->team) other = &t;
        drawTeamRow(other, 1, "Enemy ", Color{ 240, 130, 110, 255 });
    }

    // ---- command grid inside the sidebar: two compact columns ----
    std::string detailTip;
    struct PBtn { bool upgrade; bool reclaim = false; bool selType = false; int info = 0; std::string label; float price; std::string key; };
    std::vector<PBtn> btns;
    // GameUI.setupInterfaceFlags:610-615 -- bO is true on PC, and SettingsEngine:188
    // defaults showSelectedUnitsList to true, so every selection prepends one
    // SelectUnitTypeAction per unit type (GameInterfaceRenderer:904-949). The label is
    // "UnitName xN" (SelectUnitTypeAction:99-105); count desc is the PC sort order
    // (:939-945: sort, reverse, then insert at 0).
    std::vector<std::pair<std::string, int>> typeCounts;
    for (auto* s : sel) {
        if (!s || !s->tpl) continue;
        std::string k = ToLower(s->tpl->name);
        bool found = false;
        for (auto& e : typeCounts) if (e.first == k) { ++e.second; found = true; break; }
        if (!found) typeCounts.push_back({ k, 1 });
    }
    std::sort(typeCounts.begin(), typeCounts.end(),
              [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
                  return a.second > b.second;
              });
    for (auto& e : typeCounts) {
        const UnitTemplate* t = g_templates.find(e.first);
        if (!t) continue;
        PBtn b; b.upgrade = false; b.selType = true;
        b.label = t->displayText + " x" + std::to_string(e.second);
        b.price = 0; b.key = e.first;
        btns.push_back(b);
    }
    // GameInterfaceRenderer:850-853 inserts UnitInfoAction(false) on PC. Its button
    // text is the compact BaseUnit text (:2084-2112) and it stands for the selected
    // hull, so RP draws that hull's own icon on it. The team-info action is intentionally
    // omitted: the command grid must not show team information here. The detailed text
    // (:2144-2238) is a hover tooltip from UnitInfoAction.getDescription():153-168.
    ui.hudTip.clear();
    if (!sel.empty()) {
        const Unit* rep = nullptr;
        for (auto* s : sel) if (s->team == g.playerTeam() && s->tpl && !s->dead) { rep = s; break; }
        if (!rep && sel.front()->tpl) rep = sel.front();
        if (rep && rep->tpl) {
            // GameInterfaceRenderer.a(BaseUnit,boolean):2084-2112 is the button's display text
            // (UnitInfoAction.d():86-101). It is a multi-line block: hp/maxHp (or the price form
            // for a resource node), then "(shield/maxShield)", then one line per non-zero
            // generation rate -- credits as "+$3" / "-$2" (Resource.a(double,true,false):285-305
            // prints "$" for the credit resource). A zero rate prints nothing at all (:2106).
            char ib[192];
            if (rep->tpl->resourceRate > 0.0f) {
                float frac = rep->maxHp > 0 ? rep->hp / rep->maxHp : 0.0f;
                snprintf(ib, sizeof(ib), "$%.0f", rep->tpl->price * frac);
            } else {
                snprintf(ib, sizeof(ib), "%.0f/%d", std::ceil(rep->hp), (int)std::ceil(rep->maxHp));
            }
            std::string ilabel = ib;
            if (rep->shieldMax > 0) {
                snprintf(ib, sizeof(ib), "\n(%.0f/%.0f)", rep->shield, rep->shieldMax);
                ilabel += ib;
            }
            float perSec = incomePerSec(*rep->tpl);
            if (perSec != 0.0f) {
                ilabel += perSec > 0 ? "\n+$" : "\n-$";
                ilabel += fmtRate(perSec);
            }
            PBtn ibtn; ibtn.info = 1; ibtn.label = ilabel; ibtn.price = 0;
            ibtn.key = rep->tpl->name;   // this button stands for the selected hull: show its own art
            btns.push_back(ibtn);

            const UnitTemplate& t = *rep->tpl;
            detailTip = t.displayText;
            snprintf(ib, sizeof(ib), "\nHP: %d/%d", (int)std::ceil(rep->hp), (int)std::ceil(rep->maxHp));
            detailTip += ib;
            if (rep->shieldMax > 0) {
                snprintf(ib, sizeof(ib), "\nShield: %d/%d", (int)rep->shield, (int)rep->shieldMax);
                detailTip += ib;
            }
            // :2182-2193 -- "Income: +$X" / "Income: -$X", and the whole line is skipped at 0.
            float income = incomePerSec(t);
            if (income != 0.0f) {
                snprintf(ib, sizeof(ib), "\nIncome: %s$%s", income > 0 ? "+" : "-", fmtRate(income).c_str());
                detailTip += ib;
            }
            if (t.maxEnergy > 0) {
                snprintf(ib, sizeof(ib), "\nEnergy: %.0f/%.0f", rep->energy, t.maxEnergy);
                detailTip += ib;
            }
            if (t.moveSpeed > 0) { snprintf(ib, sizeof(ib), "\nSpeed: %.2f", t.moveSpeed); detailTip += ib; }
            if (t.canAttack) {
                float dmg = 0;
                if (!t.turrets.empty()) {
                    int pi = t.turrets[0].projectile;
                    if (pi >= 0 && pi < (int)t.projectiles.size()) dmg = t.projectiles[pi].directDamage;
                }
                snprintf(ib, sizeof(ib), "\nAttack: %.0f", dmg);
                detailTip += ib;
                if (t.attackRange > 0) { snprintf(ib, sizeof(ib), "\nRange: %.0f", t.attackRange); detailTip += ib; }
            }
        }
    }
    for (auto* it : buildableOptions(g)) btns.push_back({ false, false, false, 0, it->displayText, it->price, it->name });
    bool anyReclaimer = false;
    for (auto* s : sel) {
        if (s->team != g.playerTeam() || !s->tpl) continue;
        if (s->tpl->canReclaimResources) anyReclaimer = true;
        for (auto& a : s->tpl->actions) {
            bool dup = false;
            for (auto& o : btns) if (o.upgrade && o.key == a.convertTo) dup = true;
            if (!dup) btns.push_back({ true, false, false, 0, a.text, a.price, a.convertTo });
        }
    }
    if (anyReclaimer) btns.push_back({ false, true, false, 0, "Reclaim", 0, "" });

    if (CheckCollisionPointRec(mouse, Rectangle{ sideX, mm.height, side, SH - mm.height }))
        ui.overPanel = true;

    float cellW = (side - 16 * k) / 2.0f, cellH = 52 * k, gap = 4 * k;
    // RWX GameInterfaceRenderer.d():1241/1267-1269 -- the command grid starts at
    // minimap.bottom + 2 + i() + 2, where i() is 36 at 1080p (:1965-1966). On PC the
    // reserved strip stays empty: bQ is true (GameUI:614), so the unselect-all button
    // and compact info text at :2030-2051 are skipped entirely.
    float gy = mm.height + 40 * k;

    int cols = 2, cmdIdx = 0;
    for (size_t i = 0; i < btns.size(); ++i) {
        int col = (int)i % cols, row = (int)i / cols;
        Rectangle btn{ sideX + 6 * k + col * (cellW + gap), gy + row * (cellH + gap), cellW, cellH };
        if (btn.y + cellH > SH - 2) break;
        PBtn& b = btns[i];
        bool hover = CheckCollisionPointRec(mouse, btn);
        // SelectUnitTypeAction.getCostAmount() is 0 and getCost() is true: this button is
        // always available regardless of the player's credits.
        bool afford = b.selType || b.info > 0 || (me && me->credits >= b.price);
        DrawRectangleRec(btn, hover ? Color{ 58, 58, 62, 255 } : Color{ 34, 34, 38, 255 });
        DrawRectangleLinesEx(btn, 1, afford ? Color{ 90, 90, 95, 255 } : Color{ 60, 50, 50, 255 });

        Texture2D icon{};
        bool drew = false;
        // The info block is multi-line, so the hull preview moves to the left edge and the
        // numbers get the space beside it -- otherwise the text would bury the art the user
        // asked this button to show.
        bool infoLayout = (b.info == 1);
        float textL = btn.x + 3 * k, textR = btn.x + btn.width - 3 * k;
        // A unit-representing button (type button, build button, UnitInfo button)
        // draws that unit's own preview via UnitTypeEnum.drawUnit (GIR:1585-1604).
        if (!b.upgrade && !b.reclaim && (b.selType || b.info == 0 || b.info == 1)) {
            const UnitTemplate* t = g_templates.find(ToLower(b.key));
            if (t && g_render.unitIcon(t->name, icon)) {
                Rectangle ir{ btn.x + 5 * k, btn.y + 3 * k, btn.width - 10 * k, btn.height - 20 * k };
                if (infoLayout) {
                    float isz = btn.height - 10 * k;
                    ir = Rectangle{ btn.x + 4 * k, btn.y + 5 * k, isz, isz };
                    textL = ir.x + ir.width + 2 * k;
                    textR = btn.x + btn.width - 3 * k;
                }
                drawHudIcon(icon, ir, WHITE, true);
                drew = true;
            }
        }
        // AbstractUnitAction.getIconTexture():429-434 only supplies an icon for upgrade
        // actions; UnitInfoAction and SelectUnitTypeAction have none. A type button draws
        // the unit itself (GIR:1585-1604), so a failed unitIcon must not fall back to the
        // upgrade arrow.
        if (!drew && (b.upgrade || b.reclaim))
            drawHudIcon(b.reclaim ? g_hudArt.buildIcon : g_hudArt.upgrade,
                        Rectangle{ btn.x + 10 * k, btn.y + 5 * k, btn.width - 20 * k, btn.height - 22 * k },
                        Color{ 200, 230, 200, 255 });

        // Info-unit buttons have no key binding (GameInterfaceRenderer:815-821 returns null
        // for infoOnly actions), so the 1-9 hints belong only to real commands.
        if (!b.selType && b.info == 0) {
            if (cmdIdx < 9) DrawText(TextFormat("%d", cmdIdx + 1), (int)(btn.x + 4 * k), (int)(btn.y + 3 * k),
                                     px(15), Color{ 200, 200, 200, 220 });
            ++cmdIdx;
        }
        if (hover && b.info == 1) ui.hudTip = detailTip;
        // GameInterfaceRenderer:1658-1718 -- an infoOnly action prints its whole display text,
        // and for UnitInfoAction that is the multi-line "hp\n(shield)\n+$N" block. Multi-line
        // text is split on '\n' and stacked, centred vertically in the button (:1710-1715).
        std::vector<std::string> lines;
        {
            std::string cur;
            for (char ch : b.label) {
                if (ch == '\n') { lines.push_back(cur); cur.clear(); }
                else cur.push_back(ch);
            }
            lines.push_back(cur);
        }
        int lsize = px(15);
        float lineH = 17 * k;
        float ly = btn.y + btn.height - lineH * (float)lines.size();
        if (lines.size() > 1 && !drew) {
            // No hull preview to sit on, but keep the same dark plate so the block stays legible
            // over the button fill.
            DrawRectangle((int)(btn.x + 2 * k), (int)(ly - 1 * k), (int)(btn.width - 4 * k),
                          (int)(lineH * (float)lines.size() + 1 * k), Color{ 0, 0, 0, 110 });
        }
        // Fit each line into the button: shrink the font first and only cut characters when even the
        // smallest step overflows. A blanket 8-character cap used to swallow the count on a type
        // button's "Name xN" label, which is the one number that button exists to show.
        float avail = textR - textL;
        for (size_t li = 0; li < lines.size(); ++li) {
            std::string label = lines[li];
            int fs = lsize;
            while (fs > px(11) && MeasureText(label.c_str(), fs) > avail) --fs;
            while (label.size() > 1 && MeasureText(label.c_str(), fs) > avail) label.pop_back();
            int lw = MeasureText(label.c_str(), fs);
            DrawText(label.c_str(), (int)(textL + (avail - std::min<float>(lw, avail)) / 2),
                     (int)(ly + li * lineH), fs,
                     afford ? Color{ 150, 220, 150, 255 } : Color{ 150, 110, 110, 255 });
        }
        if (!b.reclaim && b.price > 0) {
            char pc[24]; snprintf(pc, sizeof(pc), "%d", (int)b.price);
            DrawText(pc, (int)(btn.x + 4 * k), (int)(btn.y + 3 * k), px(15),
                     afford ? Color{ 240, 220, 90, 255 } : Color{ 160, 120, 120, 255 });
        }
        // GameInterfaceRenderer:1606-1636 -- a queued unit's own buildProgress rides on
        // the producing factory's button as a 3px blue bar.
        if (b.info == 0 && !b.upgrade && !b.reclaim) {
            for (auto* s : sel) {
                if (!s->tpl || !s->tpl->isFactory || s->prodQueue.empty()) continue;
                if (ToLower(s->prodQueue.front()) != ToLower(b.key)) continue;
                float pr = std::max(0.0f, std::min(1.0f, s->prodProgress));
                DrawRectangle(btn.x + 4 * k, btn.y + btn.height - 4 * k,
                              (btn.width - 8 * k) * pr, 2 * k, Color{ 0, 90, 220, 255 });
                break;
            }
        }

        if (hover && b.selType && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            // SelectUnitTypeAction.onClicked:90-95 -- right click drops this type.
            for (auto& u : g.units)
                if (u.selected && u.tpl && ToLower(u.tpl->name) == ToLower(b.key)) u.selected = false;
            continue;
        }
        // QueueUnitAction.isHighPriority():101-103 -> GIR:1787-1795 maps left click to
        // 1 queue entry, Shift to 5, Ctrl to 10; right click sets stopCurrentAction
        // (GIR:1753/1802-1804) which removes one queued hull and refunds its price.
        bool queueBtn = b.info == 0 && !b.selType && !b.upgrade && !b.reclaim;
        if (hover && queueBtn && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            for (auto* s : sel) {
                if (!s->tpl || !s->tpl->isFactory || s->prodQueue.empty()) continue;
                for (int qi = (int)s->prodQueue.size() - 1; qi >= 0; --qi) {
                    if (ToLower(s->prodQueue[qi]) != ToLower(b.key)) continue;
                    const UnitTemplate* u = g_templates.find(s->prodQueue[qi]);
                    s->prodQueue.erase(s->prodQueue.begin() + qi);
                    if (u) g.reward(s->team, u->price);
                    break;
                }
            }
            continue;
        }
        if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && afford) {
            if (b.info > 0) continue; // UnitInfo buttons only show a tooltip
            if (b.selType) {
                // :72-89 -- left click only means something on a mixed selection: it
                // deselects every other type, leaving this one exclusively selected.
                bool changed = false;
                for (auto& u : g.units) {
                    if (!u.selected || !u.tpl || ToLower(u.tpl->name) == ToLower(b.key)) continue;
                    u.selected = false; changed = true;
                }
                if (!changed) continue;
            }
            else if (b.reclaim) { ui.reclaimPick = true; ui.placing = false; }
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
                int reps = 1;
                if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) reps = 5;
                else if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) reps = 10;
                for (int n = 0; n < reps; ++n)
                    for (auto* u : sel) if (u->tpl && u->tpl->isFactory) g.orderProduce(u->id, b.key);
            }
        }
    }

    // UnitInfoAction.getDescription() is a hover tooltip (GameUI:4109-4193).
    if (!ui.hudTip.empty()) {
        int tfs = px(15), maxw = 0;
        std::vector<std::string> lines;
        size_t pos = 0;
        while (true) {
            size_t nl = ui.hudTip.find('\n', pos);
            std::string ln = ui.hudTip.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            lines.push_back(ln);
            maxw = std::max(maxw, MeasureText(ln.c_str(), tfs));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        float tw = std::min((float)(maxw + px(20)), (float)(SW - 20));
        float th = (float)(lines.size() * px(20) + px(12));
        Rectangle tip{ std::min(mouse.x + 16, (float)(SW - tw - 8)),
                       std::min(mouse.y + 16, (float)(SH - th - 8)), tw, th };
        DrawRectangleRec(tip, Color{ 18, 20, 22, 245 });
        DrawRectangleLinesEx(tip, 1, Color{ 110, 130, 110, 255 });
        for (size_t i = 0; i < lines.size(); ++i)
            DrawText(lines[i].c_str(), (int)(tip.x + px(8)), (int)(tip.y + px(6) + i * px(20)),
                     tfs, Color{ 160, 225, 160, 255 });
    }

    // PC (GameUI.bQ=true) skips the compact info text block at
    // GameInterfaceRenderer:2030-2051; the selected-unit UI is the Name xN buttons
    // prepended to the grid above.

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
    std::string shotPath, mapOverride, testUpgrade, testWeapon, testBuild, testDamage, testRepair, selectTpl, testMods;
    int shotFrame = 3;
    bool startInGame = false, startInLevels = false, startInMods = false;
    bool checkMaps = false;
    bool checkBuiltins = false;
    bool unusedKeys = false;
    bool listMods = false;
    bool noFog = false;
    bool testFog = false;
    bool fogDemo = false;
    // RWX's HUD metrics are authored against 1920x1080, so that is the default surface; `--size WxH`
    // exists for the screenshot harness and for testing smaller windows.
    int winW = 1920, winH = 1080;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) { shotPath = argv[++i]; }
        else if (a == "--frames" && i + 1 < argc) { shotFrame = atoi(argv[++i]); }
        else if (a == "--game") startInGame = true;
        else if (a == "--levels") startInLevels = true;
        else if (a == "--mods") startInMods = true;
        else if (a == "--checkmaps") checkMaps = true;
        else if (a == "--checkbuiltins") checkBuiltins = true;
        else if (a == "--listmods") listMods = true;
        else if (a == "--nofog") noFog = true;
        else if (a == "--testfog") testFog = true;
        else if (a == "--fogdemo") fogDemo = true;
        else if (a == "--unusedkeys") unusedKeys = true;
        else if (a == "--testupgrade" && i + 1 < argc) { testUpgrade = argv[++i]; }
        else if (a == "--testbuild" && i + 1 < argc) { testBuild = argv[++i]; }
        else if (a == "--testdamage" && i + 1 < argc) { testDamage = argv[++i]; }
        else if (a == "--testrepair" && i + 1 < argc) { testRepair = argv[++i]; }
        else if (a == "--testmods" && i + 1 < argc) { testMods = argv[++i]; }
        else if (a == "--testweapon" && i + 1 < argc) { testWeapon = argv[++i]; }
        else if (a == "--map" && i + 1 < argc) { mapOverride = argv[++i]; }
        else if (a == "--select" && i + 1 < argc) { selectTpl = argv[++i]; }
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
    g_iniUnusedKeyReport = unusedKeys;
    g_templates.loadAll(g_assetsRoot);
    TraceLog(LOG_INFO, TextFormat("Loaded %zu unit templates", g_templates.order.size()));

    if (listMods) {
        // `--listmods` prints what the ModRegistry found and what each root contributed. It is the
        // machine gate for the mod layer: a mod that is enabled but contributes no template, or an
        // entry whose .ini count is 0, shows up here instead of as an empty build menu in game.
        for (const ModEntry& e : g_mods.all()) {
            TraceLog(LOG_INFO, "MOD %-14s %-28s %-8s ini=%-3d %s",
                     e.enabled ? "enabled" : "disabled", e.dirName.c_str(), e.kindLabel(), e.iniCount,
                     e.error.empty() ? e.relPath.c_str() : e.error.c_str());
        }
        TraceLog(LOG_INFO, "listmods: %d of %d toggleable mod(s) enabled, core ini=%d, %zu templates, selection=%s",
                 g_mods.enabledCount() - 1, g_mods.toggleableCount(),
                 g_mods.all().empty() ? 0 : g_mods.all()[0].iniCount,
                 g_templates.order.size(), g_mods.selectionPath().c_str());
        CloseWindow();
        return 0;
    }

    if (!testMods.empty()) {
        // `--testmods <folder>` proves the whole mod loop end to end: force the entry off to get a
        // baseline, toggle it on, persist the selection, reload every custom unit, confirm the store
        // actually changed, then put the selection back the way it was. mega_builders is the shipped
        // example and it overrides `builder` via overrideAndReplace, so both the template count and
        // that hull's hp move. Every lookup re-finds the entry by name, because loadAll rescans.
        auto findIdx = [&]() -> int {
            for (size_t i = 0; i < g_mods.all().size(); ++i)
                if (ToLower(g_mods.all()[i].dirName) == ToLower(testMods)) return (int)i;
            return -1;
        };
        if (findIdx() < 0) {
            TraceLog(LOG_WARNING, "testmods: no mod named '%s'", testMods.c_str());
            CloseWindow();
            return 1;
        }
        bool orig = g_mods.all()[findIdx()].enabled;
        g_mods.setEnabled((size_t)findIdx(), false);
        g_mods.saveSelection();
        g_templates.loadAll(g_assetsRoot);
        size_t baseN = g_templates.order.size();
        const UnitTemplate* b0 = g_templates.find("builder");
        int baseHp = b0 ? (int)b0->maxHp : -1;

        g_mods.setEnabled((size_t)findIdx(), true);
        g_mods.saveSelection();
        g_templates.loadAll(g_assetsRoot);
        size_t onN = g_templates.order.size();
        const UnitTemplate* b1 = g_templates.find("builder");
        int onHp = b1 ? (int)b1->maxHp : -1;
        bool changed = (onN != baseN) || (onHp != baseHp);

        g_mods.setEnabled((size_t)findIdx(), orig);
        int ri = findIdx();
        g_mods.saveSelection();
        g_templates.loadAll(g_assetsRoot);
        size_t backN = g_templates.order.size();
        // The restored count has to match what the test started with, which is onN when the mod was
        // already on before the run and baseN when it was off -- comparing against baseN alone made
        // the gate fail whenever the selection file happened to have the mod enabled.
        bool restored = (backN == (orig ? onN : baseN)) &&
                        (ri >= 0 && g_mods.all()[ri].enabled == orig);
        TraceLog(LOG_INFO, "testmods: %s off=%zu/%d on=%zu/%d restored=%zu(want %zu, orig=%d) changed=%d",
                 testMods.c_str(), baseN, baseHp, onN, onHp, backN, orig ? onN : baseN, (int)orig,
                 (int)changed);
        CloseWindow();
        return (changed && restored) ? 0 : 1;
    }

    if (checkBuiltins) {
        // `--checkbuiltins` dumps the weapon wiring built from the hard-coded templates, after the ini
        // scan has had its say: which projectile each barrel fires, the two target-domain alternates,
        // and the tier-2 upgrade rows. An index that does not resolve, or an upgrade that converts to a
        // template nobody registered, counts as a failure -- that is the machine gate for a port batch.
        int bad = 0, rows = 0;
        for (const std::string& key : g_templates.order) {
            auto it = g_templates.byName.find(key);
            if (it == g_templates.byName.end()) continue;
            const UnitTemplate& t = it->second;
            ++rows;
            std::string cbList;
            for (const BuildOption& b : t.canBuild) {
                if (!cbList.empty()) cbList += ',';
                cbList += b.unitName + ":" + std::to_string((int)b.pos);
            }
            // One canonical line per template: everything the ini loader and the built-in registration
            // both have to agree on. Diffing this before/after a loader change is the equivalence gate.
            TraceLog(LOG_INFO, "CORE %s|%d|%.1f|%.1f|%.2f|%.2f|%d|%.3f|%.3f|%.3f|%.3f|%.3f|%d%d%d%d|%.1f|%.1f|%.1f|%.6f|%d|%d|%d|%d,%d,%d,%d|%.1f|%.4f|%.4f|%d|%.3f|%.1f|%.5f|%d|%d|%d|%d%d%d|%d|%d|%.1f|%s|%s|%s|%s|%s|%d|%.1f|%d%d%d|%.1f|%.1f|%s",
                     key.c_str(), (int)t.price, t.maxHp, t.mass, t.radius, t.displayRadius,
                     (int)t.moveType, t.moveSpeed, t.accel, t.decel, t.maxTurnSpeed, t.turnAccel,
                     (int)t.canAttack, (int)t.canAttackFlying, (int)t.canAttackLand, (int)t.canAttackUnderwater,
                     t.attackRange, t.shootDelay, t.turretSize, t.buildSpeed, t.techLevel,
                     (int)t.isBuilding, (int)t.placeOnlyOnWater, t.footL, t.footU, t.footR, t.footD,
                     t.fogSight, t.resourceRate, t.creditIncomeRate, t.maxShield, t.shieldRegen,
                     t.maxEnergy, t.energyRegen, (int)t.showEnergyBar, t.maxTransportingUnits,
                     t.transportSlotsNeeded, (int)t.unselectable, (int)t.untargetable,
                     (int)t.cannotBeAttacked, (int)t.isBuilder, (int)t.isFactory, t.meleeRange,
                     t.image.c_str(), t.imageWreak.c_str(), t.imageTurret.c_str(), t.imageShadow.c_str(),
                     t.imageShield.c_str(), t.totalFrames, t.shieldRenderRadius,
                     (int)t.isMelee, (int)t.isBuilder, (int)t.isFixedFiring, t.targetHeight, t.targetHeightDrift,
                     cbList.c_str());
            // What the HUD would actually list for this hull, using the same rule the UI does. An empty
            // menu on a unit that owns canBuild entries is the symptom of the flag/tech gate going wrong.
            if (t.isBuilder || t.isFactory) {
                std::string vis;
                int resolvable = 0;
                for (const BuildOption& b : t.canBuild) {
                    const UnitTemplate* it = g_templates.find(ToLower(b.unitName));
                    if (!it) {
                        // Built-in hull actions rather than templates: repair / reclaim are run by
                        // the builder itself and setRally moves a factory's waypoint. Anything else
                        // that does not resolve is an option the menu silently loses.
                        std::string lo = ToLower(b.unitName);
                        if (lo != "repair" && lo != "reclaim" && lo != "setrally" && lo != "none") {
                            TraceLog(LOG_WARNING, "BADOPT %s -> %s (no such template)", key.c_str(), b.unitName.c_str());
                            ++bad;
                        }
                        continue;
                    }
                    ++resolvable;
                    if (!buildOptionAllowed(t, *it, b.tech)) continue;
                    if (!vis.empty()) vis += ',';
                    vis += it->name;
                    if (b.tech > 1) vis += "@" + std::to_string(b.tech);
                }
                TraceLog(LOG_INFO, "MENU %s tech=%d -> %s", key.c_str(), t.techLevel,
                         vis.empty() ? "(empty)" : vis.c_str());
                if (vis.empty() && resolvable > 0) {
                    TraceLog(LOG_WARNING, "BADMENU %s has %d resolvable option(s) but none usable", key.c_str(), resolvable);
                    ++bad;
                }
            }
            for (size_t i = 0; i < t.projectiles.size(); ++i) {
                const ProjectileTpl& p = t.projectiles[i];
                TraceLog(LOG_INFO, "  proj[%zu] dmg=%.0f speed=%.2f tgt=%.2f acc=%.2f life=%d frame=%d draw=%d size=%.0f color=%08X turn=%.2f area=%.0f/%.0f flags=%d%d%d reuse=%d mwp=%d",
                         i, p.directDamage, p.speed, p.targetSpeed, p.targetSpeedAccel, p.life, p.frame,
                         p.drawType, p.drawSize, p.color, p.turnSpeed, p.areaDamage, p.areaRadius,
                         (int)p.instant, (int)p.laserEffect, (int)p.trail,
                         (int)p.instantReuseLast, (int)p.moveWithParent);
            }
            for (size_t i = 0; i < t.turrets.size(); ++i) {
                const TurretTpl& tt = t.turrets[i];
                TraceLog(LOG_INFO, "  tur[%zu] proj=%d air=%d sea=%d range=%.0f delay=%.0f laserUse=%.3f shoot=%d x=%.1f y=%.1f by=%.1f turn=%.2f inv=%d img=%s cf=%d cl=%d cu=%d",
                         i, tt.projectile, tt.projectileAir, tt.projectileUnderwater, tt.range,
                         tt.shootDelay, tt.laserDefenceEnergyUse, (int)tt.canShoot,
                         tt.x, tt.y, tt.barrelY, tt.turnSpeed, (int)tt.invisible, tt.image.c_str(),
                         (int)tt.canAttackFlying, (int)tt.canAttackLand, (int)tt.canAttackUnderwater);
                for (int idx : { tt.projectile, tt.projectileAir, tt.projectileUnderwater })
                    if (idx >= (int)t.projectiles.size()) {
                        TraceLog(LOG_WARNING, "BADREF %s tur[%zu] idx=%d proj=%zu", key.c_str(), i, idx,
                                 t.projectiles.size());
                        ++bad;
                    }
            }
            for (size_t i = 0; i < t.actions.size(); ++i) {
                const UnitAction& a = t.actions[i];
                TraceLog(LOG_INFO, "  act[%zu] %s -> %s price=%.0f speed=%.6f",
                         i, a.name.c_str(), a.convertTo.c_str(), a.price, a.buildSpeed);
                if (!a.convertTo.empty() && !g_templates.find(ToLower(a.convertTo))) {
                    TraceLog(LOG_WARNING, "BADACT %s -> %s", key.c_str(), a.convertTo.c_str());
                    ++bad;
                }
            }
        }
        TraceLog(LOG_INFO, "checkbuiltins: %d row(s), %d bad reference(s)", rows, bad);
        CloseWindow();
        return 0;
    }

    if (unusedKeys) {
        // The UNUSED rows were already emitted while each .ini was parsed; nothing more to do.
        TraceLog(LOG_INFO, "unusedkeys: done");
        CloseWindow();
        return 0;
    }

    Game game;
    if (!loadMap(game, currentMapRel)) {
        TraceLog(LOG_WARNING, "Using fallback terrain");
        buildFallback(game);
        TraceLog(LOG_INFO, TextFormat("Map: %dx%d, spawns=%zu, units=%zu",
                 game.map.w, game.map.h, game.map.spawns.size(), game.units.size()));
    }
    if (noFog) game.fogEnabled = false;
    if (fogDemo) {
        // Screenshot aid for the fog layer: a fresh map only ever shows "in sight" and "never seen",
        // so the player's hulls are shoved sideways first to leave an explored band behind them.
        for (auto& u : game.units) if (u.team == game.playerTeam() && !u.dead) u.pos.x += TILE_SIZE * 12.0f;
        for (int i = 0; i < 3; ++i) game.updateFog();
    }

    if (testFog) {
        // `--testfog` walks the three fog states without depending on what the map happens to hold:
        // a fresh grid is entirely unseen, a sight circle lights only its own tiles, leaving a patch
        // dims it to "explored" instead of erasing it, and the visibility predicate hides an enemy
        // hull until its own tile is lit (BaseUnit.d(PlayerTeam):1431-1444).
        int bad = 0;
        game.fogEnabled = true;
        game.initFog();
        uint8_t fresh = game.fogAt(0, 0);
        if (fresh != Game::FOG_UNSEEN) { TraceLog(LOG_WARNING, "testfog: fresh grid is %d, want 10", fresh); ++bad; }

        Vec2 c{ game.map.worldW() * 0.5f, game.map.worldH() * 0.5f };
        game.revealFogAt(c, 6.0f, Game::FOG_VISIBLE);
        uint8_t centre = game.fogAtWorld(c);
        uint8_t outside = game.fogAtWorld(c + Vec2{ TILE_SIZE * 20.0f, 0.0f });
        if (centre >= Game::FOG_EXPLORED) { TraceLog(LOG_WARNING, "testfog: circle centre is %d", centre); ++bad; }
        if (outside < Game::FOG_UNSEEN) { TraceLog(LOG_WARNING, "testfog: outside circle is %d", outside); ++bad; }

        Unit* mine = nullptr;
        for (auto& u : game.units) if (u.team == game.playerTeam() && u.tpl && !u.dead) { mine = &u; break; }
        if (!mine) { TraceLog(LOG_WARNING, "testfog: no player hull on this map"); ++bad; }
        else {
            mine->pos = c;
            game.updateFog();
            uint8_t lit = game.fogAtWorld(c);
            float step = std::min(TILE_SIZE * 40.0f, game.map.worldW() * 0.4f);
            mine->pos = Vec2{ std::min(c.x + step, game.map.worldW() - TILE_SIZE * 2.0f), c.y };
            game.updateFog();
            uint8_t left = game.fogAtWorld(c), arrived = game.fogAtWorld(mine->pos);
            TraceLog(LOG_INFO, "testfog: lit=%d left=%d arrived=%d", lit, left, arrived);
            if (lit >= Game::FOG_EXPLORED) { TraceLog(LOG_WARNING, "testfog: sighted tile not lit"); ++bad; }
            if (left != Game::FOG_EXPLORED) { TraceLog(LOG_WARNING, "testfog: left tile is %d, want 5", left); ++bad; }
            if (arrived >= Game::FOG_EXPLORED) { TraceLog(LOG_WARNING, "testfog: arrived tile is %d", arrived); ++bad; }
        }

        Unit probe;
        probe.team = 1;
        probe.pos = Vec2{ TILE_SIZE * 2.5f, TILE_SIZE * 2.5f };
        bool hidden = !game.visibleToPlayer(probe);
        game.revealFogAt(probe.pos, 3.0f, Game::FOG_VISIBLE);
        bool shown = game.visibleToPlayer(probe);
        TraceLog(LOG_INFO, "testfog: probe hidden=%d shown=%d", (int)hidden, (int)shown);
        if (!hidden || !shown) ++bad;

        TraceLog(LOG_INFO, "testfog: %d failure(s)", bad);
        CloseWindow();
        return bad ? 1 : 0;
    }

    if (!testUpgrade.empty()) {
        // `--testupgrade <name>` is the functional gate for the tier-2 chain: spawn the hull, pay for its
        // convertTo action, run the build speed out tick by tick, and report what it turned into.
        // `--checkbuiltins` only proves the rows exist; this proves the conversion actually runs.
        const UnitTemplate* t0 = g_templates.find(ToLower(testUpgrade));
        if (!t0) {
            TraceLog(LOG_WARNING, "testupgrade: no template %s", testUpgrade.c_str());
            CloseWindow();
            return 1;
        }
        int act = -1;
        for (size_t i = 0; i < t0->actions.size(); ++i)
            if (!t0->actions[i].convertTo.empty()) { act = (int)i; break; }
        if (act < 0) {
            TraceLog(LOG_WARNING, "testupgrade: %s carries no upgrade action", testUpgrade.c_str());
            CloseWindow();
            return 1;
        }
        game.reward(0, 100000.0f);
        int id = game.spawnUnit(t0->name, 0, Vec2{ game.map.worldW() * 0.5f, game.map.worldH() * 0.5f });
        Unit* u = game.get(id);
        bool ok = u && game.orderUpgrade(id, act);
        int ticks = 0;
        while (ok && u && u->upgradeIdx >= 0 && ticks < 200000) { game.update(); ++ticks; }
        const UnitTemplate* t1 = u ? u->tpl : nullptr;
        TraceLog(LOG_INFO, "testupgrade: %s -> %s img=%s hp=%.0f/%.0f ticks=%d ok=%d",
                 testUpgrade.c_str(), t1 ? t1->name.c_str() : "?", t1 ? t1->image.c_str() : "-",
                 u ? u->hp : 0.0f, u ? u->maxHp : 0.0f, ticks, (int)ok);
        CloseWindow();
        return (ok && t1 && t1 != t0) ? 0 : 1;
    }

    if (!testWeapon.empty()) {
        // `--testweapon <shooter>,<target>` fires one real shot and reports the round that came out. A
        // per-turret alternate (projectileAir / projectileUnderwater) must show up here as fromProj 1;
        // the plain ground round stays at 0. This is the runtime proof the domain switch really fires.
        std::string shooter = testWeapon, targetName = "gunShip";
        size_t comma = testWeapon.find(',');
        if (comma != std::string::npos) { shooter = testWeapon.substr(0, comma); targetName = testWeapon.substr(comma + 1); }
        const UnitTemplate* ts = g_templates.find(ToLower(shooter));
        const UnitTemplate* tt = g_templates.find(ToLower(targetName));
        if (!ts || !tt) {
            TraceLog(LOG_WARNING, "testweapon: unknown template (shooter=%s target=%s)",
                     shooter.c_str(), targetName.c_str());
            CloseWindow();
            return 1;
        }
        Vec2 mid{ game.map.worldW() * 0.5f, game.map.worldH() * 0.5f };
        int sid = game.spawnUnit(ts->name, 0, mid);
        int tid = game.spawnUnit(tt->name, 1, mid + Vec2{ 70, 0 });
        Unit* su = game.get(sid);
        Unit* tu = game.get(tid);
        if (su && ts->moveType == MoveType::AIR) su->posZ = 40.0f;
        if (tu) {
            if (tt->moveType == MoveType::AIR) tu->posZ = 40.0f;        // airborne() needs posZ >= 4
            else if (targetName == "attackSubmarine") tu->posZ = -8.0f; // submerged() needs posZ <= -1
        }
        int fromProj = -1, atTick = -1;
        float dmg = -1, spd = -1;
        for (int i = 0; i < 1200 && atTick < 0; ++i) {
            game.update();
            for (const Projectile& p : game.projectiles)
                if (p.sourceId == sid) { fromProj = p.fromProj; dmg = p.damage; spd = p.speed; atTick = i; break; }
        }
        TraceLog(LOG_INFO, "testweapon: %s vs %s dmg=%.0f speed=%.2f fromProj=%d tick=%d",
                 shooter.c_str(), targetName.c_str(), dmg, spd, fromProj, atTick);
        CloseWindow();
        return atTick >= 0 ? 0 : 1;
    }

    if (!testBuild.empty()) {
        // `--testbuild <builder>,<target>` runs the player's build path end to end: pick a hull that
        // offers the option, scan the map for a legal tile, and call orderBuild exactly as tryPlace
        // does. This is the runtime proof that the menu gate and placement both let a player build.
        std::string owner = testBuild, targetName = "landFactory";
        size_t comma = testBuild.find(',');
        if (comma != std::string::npos) { owner = testBuild.substr(0, comma); targetName = testBuild.substr(comma + 1); }
        const UnitTemplate* to = g_templates.find(ToLower(owner));
        const UnitTemplate* tt = g_templates.find(ToLower(targetName));
        if (!to || !tt) {
            TraceLog(LOG_WARNING, "testbuild: unknown template (owner=%s target=%s)", owner.c_str(), targetName.c_str());
            CloseWindow();
            return 1;
        }
        // does the menu gate even offer it?
        bool offered = false;
        for (const BuildOption& b : to->canBuild) {
            const UnitTemplate* it = g_templates.find(ToLower(b.unitName));
            if (it && ToLower(it->name) == ToLower(tt->name) && buildOptionAllowed(*to, *it, b.tech)) offered = true;
        }
        Vec2 mid{ game.map.worldW() * 0.5f, game.map.worldH() * 0.5f };
        int bid = game.spawnUnit(to->name, 0, mid);
        game.reward(0, 100000);
        if (!tt->isBuilding) {
            // Production path: queue it like the HUD's produce button does, then tick until the hull
            // pops out. Buildings go through orderBuild below instead.
            bool queued = game.orderProduce(bid, tt->name);
            int baseCount = 0;
            for (auto& u : game.units) if (!u.dead && u.tpl && ToLower(u.tpl->name) == ToLower(tt->name)) ++baseCount;
            int producedTick = -1;
            for (int i = 0; i < 6000 && queued && producedTick < 0; ++i) {
                game.update();
                int now = 0;
                for (auto& u : game.units) if (!u.dead && u.tpl && ToLower(u.tpl->name) == ToLower(tt->name)) ++now;
                if (now > baseCount) producedTick = i;
            }
            TraceLog(LOG_INFO, "testbuild: %s -> %s offered=%d queued=%d produced=%d tick=%d",
                     owner.c_str(), tt->name.c_str(), (int)offered, (int)queued,
                     producedTick >= 0 ? 1 : 0, producedTick);
            CloseWindow();
            return (offered && producedTick >= 0) ? 0 : 1;
        }
        // The building path used to stop at "a foundation appeared": the nano gate could drop the
        // order on the next tick and the site would sit at 0% forever. Tick the game afterwards and
        // assert buildProgress actually advances, with the builder standing next to the site so the
        // result isolates the nano gate from pathfinding.
        int maxIdBefore = -1;
        for (auto& u : game.units) maxIdBefore = std::max(maxIdBefore, u.id);
        int builtAtX = -1, builtAtY = -1;
        for (int y = 2; y < game.map.h - 2 && builtAtX < 0; ++y)
            for (int x = 2; x < game.map.w - 2 && builtAtX < 0; ++x)
                if (game.orderBuild(bid, tt->name, x, y)) { builtAtX = x; builtAtY = y; }
        int siteId = -1;
        for (auto& u : game.units)
            if (u.id > maxIdBefore && u.tpl && ToLower(u.tpl->name) == ToLower(tt->name)) { siteId = u.id; break; }
        float prog1 = -1.0f, prog60 = -1.0f;
        int nanoAt1 = -1, doneTick = -1;
        if (siteId >= 0) {
            Unit* b = game.get(bid);
            if (b) b->pos = game.get(siteId)->pos + Vec2{ TILE_SIZE * 2.0f, 0 };
            for (int i = 0; i < 3000; ++i) {
                game.update();
                Unit* s = game.get(siteId);
                if (!s || s->dead) break;
                if (i == 0) {
                    prog1 = s->buildProgress;
                    Unit* bb = game.get(bid);
                    nanoAt1 = bb ? bb->nanoTargetId : -2;
                }
                if (i == 59) prog60 = s->buildProgress;
                if (!s->underConstruction || s->buildProgress >= 1.0f) { doneTick = i; break; }
            }
        }
        TraceLog(LOG_INFO, "testbuild: %s -> %s offered=%d placed=%d at=(%d,%d) orderBuild=%d",
                 owner.c_str(), tt->name.c_str(), (int)offered, builtAtX >= 0 ? 1 : 0,
                 builtAtX, builtAtY, builtAtX >= 0 ? 1 : 0);
        TraceLog(LOG_INFO, "testbuild progress: site=%d prog1=%.4f prog60=%.4f nanoAt1=%d doneTick=%d",
                 siteId, prog1, prog60, nanoAt1, doneTick);
        bool built = (doneTick >= 0) || (prog60 > prog1 && prog1 >= 0.0f);
        CloseWindow();
        return (offered && builtAtX >= 0 && built) ? 0 : 1;
    }

    if (!testDamage.empty()) {
        // `--testdamage <template>,<amount>` applies one real hit through Game::damage and reports the
        // health delta. A hull with core.canNotBeDamaged must not move at all (CustomUnit:4744), while
        // an ordinary one has to lose health -- that pair is the check.
        std::string name = testDamage;
        float amount = 100.0f;
        size_t comma = testDamage.find(',');
        if (comma != std::string::npos) { name = testDamage.substr(0, comma); amount = (float)atof(testDamage.c_str() + comma + 1); }
        const UnitTemplate* tpl = g_templates.find(ToLower(name));
        if (!tpl) {
            TraceLog(LOG_WARNING, "testdamage: unknown template (%s)", name.c_str());
            CloseWindow();
            return 1;
        }
        Vec2 mid{ game.map.worldW() * 0.5f, game.map.worldH() * 0.5f };
        int id = game.spawnUnit(tpl->name, 1, mid);
        Unit* u = game.get(id);
        if (!u) { CloseWindow(); return 1; }
        float before = u->hp;
        game.damage(*u, amount, nullptr);
        float after = u->hp;
        TraceLog(LOG_INFO, "testdamage: %s dmg=%.0f hp=%.1f -> %.1f blocked=%d cannotBeDamaged=%d",
                 tpl->name.c_str(), amount, before, after, (after == before) ? 1 : 0,
                 (int)tpl->cannotBeDamaged);
        CloseWindow();
        return 0;
    }

    if (!testRepair.empty()) {
        // `--testrepair <healer>,<target>` proves both halves of a RepairBay: an explicit repair
        // order, and the AutoRepairCallback that fires after 40 idle ticks (RepairBay.java:175-184).
        std::string healerName = testRepair, targetName = "tank";
        size_t comma = testRepair.find(',');
        if (comma != std::string::npos) {
            healerName = testRepair.substr(0, comma);
            targetName = testRepair.substr(comma + 1);
        }
        const UnitTemplate* ht = g_templates.find(ToLower(healerName));
        const UnitTemplate* tt = g_templates.find(ToLower(targetName));
        if (!ht || !tt) {
            TraceLog(LOG_WARNING, "testrepair: unknown template (healer=%s target=%s)",
                     healerName.c_str(), targetName.c_str());
            CloseWindow();
            return 1;
        }
        Vec2 mid{ game.map.worldW() * 0.5f, game.map.worldH() * 0.5f };
        int hid = game.spawnUnit(ht->name, 0, mid);
        int tid = game.spawnUnit(tt->name, 0, mid + Vec2{ TILE_SIZE * 2.0f, 0 });
        Unit* h = game.get(hid);
        Unit* t = game.get(tid);
        if (!h || !t) { CloseWindow(); return 1; }
        game.damage(*t, t->maxHp * 0.5f, nullptr);
        float hp0 = t->hp;
        h->nanoTargetId = tid; h->nanoReclaim = false; h->nanoOrderAge = 0;
        int explicitTick = -1;
        for (int i = 0; i < 300; ++i) {
            game.update();
            t = game.get(tid);
            if (!t || t->dead) break;
            if (t->hp > hp0 + 1.0f) { explicitTick = i; break; }
        }
        int autoTick = -1;
        if (t && !t->dead) {
            t->hp = hp0;
            h = game.get(hid);
            if (h) h->nanoTargetId = -1;
            for (int i = 0; i < 200; ++i) {
                game.update();
                t = game.get(tid);
                if (!t || t->dead) break;
                if (t->hp > hp0 + 1.0f) { autoTick = i; break; }
            }
        }
        TraceLog(LOG_INFO, "testrepair: %s -> %s hp=%.1f explicitTick=%d autoTick=%d",
                 ht->name.c_str(), tt->name.c_str(), hp0, explicitTick, autoTick);
        CloseWindow();
        return (explicitTick >= 0 && autoTick >= 0) ? 0 : 1;
    }

    g_render.init();

    // `--select <tplKey>` preselects every player hull of that template, so `--shot` can capture the
    // right-hand command panel without a mouse. Debug harness only; normal play never sets it.
    if (!selectTpl.empty()) {
        int picked = 0;
        for (auto& u : game.units) {
            if (u.dead || !u.tpl || u.team != game.playerTeam()) continue;
            if (ToLower(u.tpl->name) != ToLower(selectTpl) && ToLower(u.tplKey) != ToLower(selectTpl)) continue;
            u.selected = true;
            if (++picked >= 8) break;
        }
        TraceLog(LOG_INFO, "select: %d hull(s) of '%s'", picked, selectTpl.c_str());
    }

    Camera2D cam{};
    resetCamera(cam, game, SW, SH);

    double acc = 0; double last = GetTime();
    int renderedFrames = 0;
    enum class Screen { Menu, Levels, Mods, Game };
    Screen screen = startInGame ? Screen::Game
                  : (startInLevels ? Screen::Levels : (startInMods ? Screen::Mods : Screen::Menu));

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
                g_menu.close(); g_levels.close(); g_modScene.close(); g_hudArt.close(); g_render.close(); CloseWindow(); return 0;
            }
            if (act == MenuAction::SinglePlayer) {
                screen = Screen::Levels;
                TraceLog(LOG_INFO, "Main menu: opening level select");
            } else if (act == MenuAction::Mods) {
                g_modScene.init();
                screen = Screen::Mods;
                TraceLog(LOG_INFO, "Main menu: opening mods");
            } else if (act == MenuAction::Continue) {
                screen = Screen::Game;
                TraceLog(LOG_INFO, "Main menu: entering game");
            } else if (act == MenuAction::Exit) {
                g_menu.close(); g_levels.close(); g_modScene.close(); g_hudArt.close(); g_render.close(); CloseWindow(); return 0;
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
                g_menu.close(); g_levels.close(); g_modScene.close(); g_hudArt.close(); g_render.close(); CloseWindow(); return 0;
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

        // ---- mods: RWX ModsSceneHost ----
        if (screen == Screen::Mods) {
            BeginDrawing();
            ClearBackground(Color{ 10, 12, 16, 255 });
            ModUI::ModScene::Result res = g_modScene.drawAndInput();
            EndDrawing();
            if (!shotPath.empty() && ++renderedFrames >= shotFrame) {
                Image img = LoadImageFromScreen(); ExportImage(img, shotPath.c_str()); UnloadImage(img);
                TraceLog(LOG_INFO, TextFormat("shot written: %s", shotPath.c_str()));
                g_menu.close(); g_levels.close(); g_modScene.close(); g_hudArt.close(); g_render.close();
                CloseWindow(); return 0;
            }
            if (res.applied) {
                // ModManager.applyAndSaveMods():510-520 -- the selection is already on disk, now
                // rebuild every custom unit. The world is rebuilt too, because the units standing on
                // the map hold raw UnitTemplate pointers into the store loadAll is about to replace.
                g_modScene.close();
                g_templates.loadAll(g_assetsRoot);
                if (!loadMap(game, currentMapRel)) buildFallback(game);
                resetCamera(cam, game, SW, SH);
                ui = UI{};
                g_modScene.init();
                g_modScene.showToast("Mods applied");
                screen = Screen::Menu;
                TraceLog(LOG_INFO, TextFormat("Mods applied: %zu templates", g_templates.order.size()));
            } else if (res.back || IsKeyPressed(KEY_ESCAPE)) {
                // Leaving without Apply throws the pending toggles away, so the screen always shows
                // what the game is actually running.
                if (g_modScene.dirty) {
                    g_mods.loadSelection();
                    g_modScene.dirty = false;
                    g_modScene.rebuild();
                }
                screen = Screen::Menu;
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
        // Fog of war toggle. RWX exposes it as a room setting (TileMap.fogEnabled); RP has no settings
        // screen yet, so it is a key plus `--nofog`.
        if (IsKeyPressed(KEY_F)) {
            game.fogEnabled = !game.fogEnabled;
            TraceLog(LOG_INFO, "fog of war: %s", game.fogEnabled ? "on" : "off");
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
    g_modScene.close();
    g_hudArt.close(); g_render.close();
    CloseWindow();
    return 0;
}
