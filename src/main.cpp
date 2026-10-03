// RP main - window, game loop, camera, input, HUD, minimap.
#include <raylib.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Common.h"
#include "UnitTemplate.h"
#include "TileMap.h"
#include "Game.h"
#include "Renderer.h"

static const char* MAP_PATH_DEFAULT = "../assets/maps/l030;[demo]mission_3__-__Crossfire.tmx";

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
    // command marker anim
    bool markerOn = false; Vector2 markerPos; float markerT = 0;
};
static UI ui;

static Color TC(int t) {
    static Color pal[8] = { {90,140,240,255},{220,70,70,255},{80,190,90,255},{230,200,70,255},
        {200,100,220,255},{240,150,60,255},{70,200,210,255},{220,120,130,255} };
    return pal[((t % 8) + 8) % 8];
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
        if (u.team != player || !u.tpl) continue;
        if (u.tpl->isStatic() && (hi.x - lo.x) < 4 && (hi.y - lo.y) < 4) {} // allow buildings single select
        if (u.pos.x >= lo.x && u.pos.x <= hi.x && u.pos.y >= lo.y && u.pos.y <= hi.y) { u.selected = true; if (!firstMatch) firstMatch = &u; }
    }
    // if nothing of ours, try enemy single click for info (not selected)
    (void)firstMatch;
}

static void renderHUD(Game& g, Camera2D& cam) {
    int SW = GetScreenWidth(), SH = GetScreenHeight();
    ui.overPanel = false; ui.overMinimap = false;

    // ---- top-left resources ----
    Team* me = nullptr; for (auto& t : g.teams) if (t.id == g.playerTeam()) me = &t;
    std::string credits = "Credits: " + (me ? std::to_string((int)me->credits) : "0");
    DrawRectangle(6, 6, 200, 30, Color{ 0, 0, 0, 130 });
    DrawText(credits.c_str(), 14, 12, 20, Color{ 240, 220, 90, 255 });
    char buf[64]; snprintf(buf, 64, "Units: %zu  FPS: %d", g.units.size(), (int)GetFPS());
    DrawText(buf, 14, 40, 14, LIGHTGRAY);
    if (g.gameOver) DrawText(("*** " + g.gameOverText + " ***  (R to restart)").c_str(), 220, 10, 26, g.gameOverText == "VICTORY" ? GREEN : RED);

    // ---- minimap (top-right) ----
    float mmW = 180, mmH = 160;
    Rectangle mm{ SW - mmW - 8, 8, mmW, mmH };
    DrawRectangleRec(mm, Color{ 10, 14, 18, 220 });
    DrawRectangleLinesEx(mm, 1.5f, LIGHTGRAY);
    if (CheckCollisionPointRec(GetMousePosition(), mm)) ui.overMinimap = true;
    float sx = g.map.worldW() > 0 ? mmW / g.map.worldW() : 1;
    float sy = g.map.worldH() > 0 ? mmH / g.map.worldH() : 1;
    // terrain downsample
    for (int y = 0; y < g.map.h; y += 2) for (int x = 0; x < g.map.w; x += 2) {
        const MapTile& t = g.map.at(x, y);
        Color c = t.water ? Color{ 40, 96, 160, 255 } : (t.blocked ? Color{ 80, 80, 80, 255 } : Color{ 110, 120, 80, 255 });
        DrawRectangle(mm.x + x * sx, mm.y + y * sy, 2 * sx, 2 * sy, c);
    }
    for (auto& u : g.units) if (u.tpl) DrawRectangle(mm.x + u.pos.x * sx - 1, mm.y + u.pos.y * sy - 1, 3, 3, TC(u.team));
    // view rect
    Vector2 tl = GetScreenToWorld2D({ 0.0f, 0.0f }, cam), br = GetScreenToWorld2D({ (float)SW, (float)SH }, cam);
    DrawRectangleLines(mm.x + tl.x * sx, mm.y + tl.y * sy, (br.x - tl.x) * sx, (br.y - tl.y) * sy, WHITE);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && ui.overMinimap) {
        Vector2 m = GetMousePosition();
        cam.target = { (m.x - mm.x) / sx, (m.y - mm.y) / sy };
    }

    // ---- build / produce / upgrade panel (bottom) ----
    struct PBtn { bool upgrade; std::string label; float price; std::string key; };
    std::vector<PBtn> btns;
    for (auto* it : buildableOptions(g)) btns.push_back({ false, it->displayText, it->price, it->name });
    for (auto* u : g.selectedUnits()) {
        if (u->team != g.playerTeam() || !u->tpl) continue;
        for (auto& a : u->tpl->actions) {
            bool dup = false;
            for (auto& o : btns) if (o.upgrade && o.key == a.convertTo) dup = true;
            if (!dup) btns.push_back({ true, a.text, a.price, a.convertTo });
        }
    }
    if (!btns.empty()) {
        Rectangle panel{ 8, SH - ui.panelH - 8, SW - 16 - mmW, ui.panelH };
        DrawRectangleRec(panel, Color{ 12, 16, 20, 210 });
        DrawRectangleLinesEx(panel, 1, GRAY);
        if (CheckCollisionPointRec(GetMousePosition(), panel)) ui.overPanel = true;
        float bw = 96, gap = 8;
        int cols = (int)((panel.width - 16) / (bw + gap));
        for (size_t i = 0; i < btns.size(); ++i) {
            PBtn& b = btns[i];
            int cx = (int)i % cols, cy = (int)i / cols;
            Rectangle btn{ panel.x + 12 + cx * (bw + gap), panel.y + 34 + cy * 46, bw, 42 };
            bool hover = CheckCollisionPointRec(GetMousePosition(), btn) && !ui.overMinimap;
            bool afford = me && me->credits >= b.price;
            Color base = b.upgrade ? Color{ 19, 101, 94, 255 } : Color{ 40, 60, 85, 255 };
            Color hot = b.upgrade ? Color{ 39, 202, 189, 255 } : Color{ 60, 90, 120, 255 };
            DrawRectangleRec(btn, afford ? (hover ? hot : base) : Color{ 50, 40, 40, 255 });
            DrawRectangleLinesEx(btn, 1, afford ? LIGHTGRAY : DARKGRAY);
            std::string label = b.label.size() > 12 ? b.label.substr(0, 11) + "." : b.label;
            DrawText(label.c_str(), btn.x + 4, btn.y + 3, 14, afford ? WHITE : GRAY);
            DrawText((std::to_string((int)b.price) + "$").c_str(), btn.x + 4, btn.y + 24, 14, afford ? Color{ 240, 220, 90, 255 } : GRAY);
            if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && afford) {
                if (b.upgrade) {
                    for (auto* u : g.selectedUnits()) {
                        if (!u->tpl) continue;
                        for (size_t k = 0; k < u->tpl->actions.size(); ++k)
                            if (ToLower(u->tpl->actions[k].convertTo) == ToLower(b.key)) { g.orderUpgrade(u->id, (int)k); break; }
                    }
                } else if (g_templates.find(ToLower(b.key)) && g_templates.find(ToLower(b.key))->isBuilding) {
                    ui.placing = true; ui.placingName = b.key;
                } else {
                    for (auto* u : g.selectedUnits()) if (u->tpl && u->tpl->isFactory) g.orderProduce(u->id, b.key);
                }
            }
        }
        DrawText("Build / Production", panel.x + 8, panel.y + 8, 18, LIGHTGRAY);
        // production queue readout (RWX shows queued units with progress)
        for (auto* u : g.selectedUnits()) {
            if (!u->tpl || !u->tpl->isFactory || u->prodQueue.empty()) continue;
            std::string q = "Queue:";
            int shown = 0;
            for (auto& name : u->prodQueue) {
                if (shown >= 4) { q += " +"; q += std::to_string((int)u->prodQueue.size() - shown); break; }
                const UnitTemplate* it = g_templates.find(name);
                q += " ";
                q += it ? it->displayText : name;
                if (shown == 0) q += " " + std::to_string((int)(u->prodProgress * 100)) + "%";
                shown++;
            }
            DrawText(q.c_str(), panel.x + 200, panel.y + 8, 18, Color{ 250, 190, 40, 255 });
            break;
        }
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
        if (ok && !g.buildingSpotFree(Vec2{ (tx + 0.5f) * TILE_SIZE, (ty + 0.5f) * TILE_SIZE }, *t)) ok = false;
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

int main() {
    const int SW = 1280, SH = 720;
    SetTraceLogCallback(LogToFile);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(SW, SH, "RP - Rusted Warfare C++ remake");
    SetExitKey(KEY_NULL); // ESC cancels placement, must not close the window
    SetTargetFPS(120);

    g_assetsRoot = "../assets";
    TraceLog(LOG_INFO, "Loading unit templates...");
    g_templates.loadAll(g_assetsRoot);
    TraceLog(LOG_INFO, TextFormat("Loaded %zu unit templates", g_templates.order.size()));

    Game game;
    std::string err;
    if (!game.init(MAP_PATH_DEFAULT, &err)) {
        TraceLog(LOG_WARNING, TextFormat("Map load failed: %s - using fallback terrain", err.c_str()));
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
    TraceLog(LOG_INFO, TextFormat("Map: %dx%d, spawns=%zu, units=%zu", game.map.w, game.map.h, game.map.spawns.size(), game.units.size()));

    g_render.init();

    // camera
    Camera2D cam{};
    cam.rotation = 0; cam.zoom = 1.0f;
    cam.offset = { SW / 2.0f, SH / 2.0f };
    Vector2 cc{ game.map.worldW() / 2, game.map.worldH() / 2 };
    for (auto& u : game.units) if (u.team == 0 && u.tpl && u.tpl->name == "commandCenter") { cc = Vector2{ u.pos.x, u.pos.y }; break; }
    cam.target = cc;

    double acc = 0; double last = GetTime();

    while (!WindowShouldClose()) {
        double now = GetTime();
        acc += now - last; last = now;
        while (acc >= 1.0 / TICK_RATE) { if (!game.gameOver) game.update(); acc -= 1.0 / TICK_RATE; }

        // camera pan
        float pan = 600.0f / cam.zoom * GetFrameTime();
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) cam.target.y -= pan;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) cam.target.y += pan;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) cam.target.x -= pan;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) cam.target.x += pan;
        if (IsKeyDown(KEY_MINUS) || GetMouseWheelMove() < 0) cam.zoom = std::max(0.3f, cam.zoom - 0.1f);
        if (IsKeyDown(KEY_EQUAL) || GetMouseWheelMove() > 0) cam.zoom = std::min(3.0f, cam.zoom + 0.1f);
        // clamp target to world
        cam.target.x = std::max(0.0f, std::min(game.map.worldW(), cam.target.x));
        cam.target.y = std::max(0.0f, std::min(game.map.worldH(), cam.target.y));

        Vector2 mouse = GetMousePosition();
        Vector2 worldV = GetScreenToWorld2D(mouse, cam);
        Vec2 world{ worldV.x, worldV.y };
        bool overUI = ui.overPanel || ui.overMinimap;

        if (ui.markerOn) { ui.markerT += (float)GetFrameTime(); if (ui.markerT > 0.6f) ui.markerOn = false; }
        handlePlacement(game, world);

        // ---- selection drag (left mouse) ----
        if (!overUI && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (ui.placing) tryPlace(game);
            else { ui.dragging = true; ui.dragA = mouse; ui.dragB = mouse; }
        }
        if (ui.dragging) {
            ui.dragB = mouse;
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Vector2 a = GetScreenToWorld2D(ui.dragA, cam), b = GetScreenToWorld2D(ui.dragB, cam);
                float w = std::abs(b.x - a.x), h = std::abs(b.y - a.y);
                if (w * cam.zoom < 6 && h * cam.zoom < 6) {
                    // single click select
                    clearSelection(game);
                    Unit* best = nullptr; float bd = 1e9f;
                    for (auto& u : game.units) if (u.tpl) { float d = Vec2{ a.x, a.y }.dist(u.pos); if (d < std::max(12.0f, u.tpl->radius) && d < bd) { bd = d; best = &u; } }
                    if (best && best->team == game.playerTeam()) best->selected = true;
                } else selectBox(game, a, b);
                ui.dragging = false;
            }
        }

        // ---- right click orders ----
        if (!overUI && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            if (ui.placing) { ui.placing = false; }
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
                    if (enemy && game.orderAttackTarget(ids, enemy->id) > 0) {} // attack order taken
                    else if (site && game.orderRepair(ids, site->id)) {}
                    else if (IsKeyDown(KEY_LEFT_CONTROL)) game.orderAttackMove(ids, world);
                    else game.orderMove(ids, world);
                    ui.markerOn = true; ui.markerPos = worldV; ui.markerT = 0;
                }
            }
        }
        if (IsKeyPressed(KEY_ESCAPE)) { ui.placing = false; clearSelection(game); }
        if (IsKeyPressed(KEY_X)) { std::vector<int> ids; for (auto* s : game.selectedUnits()) ids.push_back(s->id); game.orderStop(ids); }
        if (IsKeyPressed(KEY_R) && game.gameOver) {
            Game fresh; std::string e2;
            if (fresh.init(MAP_PATH_DEFAULT, &e2)) { game = std::move(fresh); }
            for (auto& u : game.units) u.selected = false;
            game.gameOver = false;
        }

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
        EndDrawing();
    }

    g_render.close();
    CloseWindow();
    return 0;
}
