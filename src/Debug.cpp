// Debug toolkit implementation. Everything here is read-only on the sim except the explicit
// cheats (F6-F9) and the spawner, which go through the same Game API the UI uses.
#include "Debug.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include "UnitTemplate.h"
#include "TeamColoring.h"

DebugState g_debug;

static const float kSpeeds[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
static const int kSpeedCount = (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0]));

static Color dbgTeamColor(int t) {
    uint32_t a = kTeamColors[teamColorIndex(t)];
    return Color{ (uint8_t)((a >> 16) & 0xFF), (uint8_t)((a >> 8) & 0xFF), (uint8_t)(a & 0xFF), 255 };
}

static void panel(float x, float y, float w, float h) {
    DrawRectangle((int)x, (int)y, (int)w, (int)h, Color{ 8, 10, 14, 215 });
    DrawRectangleLines((int)x, (int)y, (int)w, (int)h, Color{ 90, 110, 130, 255 });
}

// Cheats apply to the selection, or to every player unit when nothing is selected.
static std::vector<Unit*> cheatTargets(Game& g) {
    std::vector<Unit*> out = g.selectedUnits();
    if (out.empty())
        for (auto& u : g.units)
            if (u.team == g.playerTeam() && !u.dead && !u.riding()) out.push_back(&u);
    return out;
}

static Unit* unitAt(Game& g, Vec2 world) {
    Unit* best = nullptr; float bd = 1e9f;
    for (auto& u : g.units) {
        if (!u.tpl || u.dead || u.riding()) continue;
        float d = world.dist(u.pos);
        if (d < std::max(14.0f, u.tpl->radius) && d < bd) { bd = d; best = &u; }
    }
    return best;
}

static float maxRangeOf(const UnitTemplate& t) {
    float r = t.attackRange;
    for (auto& tur : t.turrets) r = std::max(r, tur.range);
    return r;
}

// ---------------------------------------------------------------- input

void debugHandleKeys(Game& g) {
    if (IsKeyPressed(KEY_F1)) g_debug.stats = !g_debug.stats;
    if (IsKeyPressed(KEY_F2)) {
        g_debug.inspector = !g_debug.inspector;
        if (!g_debug.inspector) { g_debug.inspRect = Rectangle{ 0, 0, 0, 0 }; g_debug.inspScroll = 0; }
    }
    if (IsKeyPressed(KEY_F3)) g_debug.world = !g_debug.world;
    if (IsKeyPressed(KEY_F4)) g_debug.grid = !g_debug.grid;

    if (IsKeyPressed(KEY_P)) g_debug.paused = !g_debug.paused;
    if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_LEFT_BRACKET)) {
        int i = 0;
        while (i < kSpeedCount - 1 && kSpeeds[i] < g_debug.speed - 1e-3f) ++i;
        i += IsKeyPressed(KEY_RIGHT_BRACKET) ? 1 : -1;
        i = std::max(0, std::min(kSpeedCount - 1, i));
        g_debug.speed = kSpeeds[i];
    }
    if (g_debug.paused && IsKeyPressed(KEY_PERIOD)) g_debug.step = true;

    if (IsKeyPressed(KEY_F6)) {
        for (auto& t : g.teams) if (t.id == g.playerTeam()) t.credits += 10000;
    }
    if (IsKeyPressed(KEY_F7)) {
        // Pointers into the units deque stay valid across the spawnUnit pushes below.
        for (auto* u : cheatTargets(g)) {
            if (u->underConstruction) { u->underConstruction = false; u->buildProgress = 1.0f; }
            if (!u->prodQueue.empty()) {
                int k = 0;
                for (auto& name : u->prodQueue)
                    g.spawnUnit(name, u->team, u->pos + Vec2{ (k++) * 12.0f, u->tpl ? u->tpl->radius + 16.0f : 28.0f });
                u->prodQueue.clear();
                u->prodProgress = 0;
            }
        }
    }
    if (IsKeyPressed(KEY_F8)) {
        for (auto* u : cheatTargets(g)) {
            u->hp = u->maxHp;
            u->shield = u->shieldMax;
            if (u->tpl) u->energy = u->tpl->maxEnergy;
            u->energyRecharging = false;
        }
    }
    if (IsKeyPressed(KEY_F9)) {
        for (auto* u : cheatTargets(g)) g.damage(*u, 1e9f);
    }

    if (IsKeyPressed(KEY_F10)) {
        g_debug.spawnOpen = !g_debug.spawnOpen;
        if (!g_debug.spawnOpen) g_debug.spawnArmed = false;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (g_debug.spawnArmed) g_debug.spawnArmed = false;
        else if (g_debug.spawnOpen) g_debug.spawnOpen = false;
    }
    if (g_debug.spawnOpen) {
        int ch;
        while ((ch = GetCharPressed()) != 0)
            if (ch >= 32 && ch < 127) { g_debug.spawnFilter += (char)ch; g_debug.spawnScroll = 0; }
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !g_debug.spawnFilter.empty()) {
            g_debug.spawnFilter.pop_back();
            g_debug.spawnScroll = 0;
        }
    }
}

static Rectangle pickerRect() {
    return Rectangle{ 8, 90, 280, (float)GetScreenHeight() - 110 };
}

static std::vector<const UnitTemplate*> pickerList() {
    std::vector<const UnitTemplate*> out;
    std::string f = ToLower(g_debug.spawnFilter);
    for (auto& name : g_templates.order) {
        const UnitTemplate* t = g_templates.find(name);
        if (!t) continue;
        if (!f.empty() && ToLower(t->name).find(f) == std::string::npos
            && ToLower(t->displayText).find(f) == std::string::npos) continue;
        out.push_back(t);
    }
    return out;
}

bool debugMouseLeft(Game& g, Vector2 mouse, Vec2 world, bool overUI) {
    if (g_debug.spawnOpen) {
        Rectangle pr = pickerRect();
        if (CheckCollisionPointRec(mouse, pr)) {
            auto list = pickerList();
            int visible = (int)((pr.height - 58) / 20);
            float wheel = GetMouseWheelMove();
            if (wheel != 0) {
                g_debug.spawnScroll -= (int)(wheel * 3);
                g_debug.spawnScroll = std::max(0, std::min(std::max(0, (int)list.size() - visible), g_debug.spawnScroll));
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                int row = (int)((mouse.y - (pr.y + 54)) / 20);
                int idx = g_debug.spawnScroll + row;
                if (row >= 0 && idx >= 0 && idx < (int)list.size()) {
                    g_debug.spawnName = list[idx]->name;
                    g_debug.spawnArmed = true;
                    g_debug.spawnOpen = false;
                }
            }
        }
        return true; // while the picker is up it owns every left click
    }
    if (g_debug.spawnArmed && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !overUI) {
        int team = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 1 : g.playerTeam();
        g.spawnUnit(g_debug.spawnName, team, world);
        return true;
    }
    return false;
}

bool debugMouseRight() {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) return false;
    if (g_debug.spawnArmed) { g_debug.spawnArmed = false; return true; }
    if (g_debug.spawnOpen) { g_debug.spawnOpen = false; return true; }
    return false;
}

// ---------------------------------------------------------------- world overlays

void debugDrawGrid(Game& g, const Camera2D& cam) {
    if (!g_debug.grid) return;
    Vector2 tl = GetScreenToWorld2D({ 0, 0 }, cam);
    Vector2 br = GetScreenToWorld2D({ (float)GetScreenWidth(), (float)GetScreenHeight() }, cam);
    int x0 = std::max(0, (int)(tl.x / TILE_SIZE)), y0 = std::max(0, (int)(tl.y / TILE_SIZE));
    int x1 = std::min(g.map.w - 1, (int)(br.x / TILE_SIZE)), y1 = std::min(g.map.h - 1, (int)(br.y / TILE_SIZE));
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        const MapTile& t = g.map.at(x, y);
        if (t.blocked) DrawRectangle(x * TILE_SIZE, y * TILE_SIZE, TILE_SIZE, TILE_SIZE, Color{ 220, 60, 60, 45 });
        else if (t.water) DrawRectangle(x * TILE_SIZE, y * TILE_SIZE, TILE_SIZE, TILE_SIZE, Color{ 60, 120, 220, 40 });
        if (t.resource) DrawRectangle(x * TILE_SIZE, y * TILE_SIZE, TILE_SIZE, TILE_SIZE, Color{ 240, 210, 60, 60 });
    }
    Color lc{ 255, 255, 255, 35 };
    for (int x = x0; x <= x1 + 1; ++x)
        DrawLine(x * TILE_SIZE, y0 * TILE_SIZE, x * TILE_SIZE, (y1 + 1) * TILE_SIZE, lc);
    for (int y = y0; y <= y1 + 1; ++y)
        DrawLine(x0 * TILE_SIZE, y * TILE_SIZE, (x1 + 1) * TILE_SIZE, y * TILE_SIZE, lc);
}

void debugDrawWorld(Game& g, const Camera2D& cam) {
    if (g_debug.spawnArmed) {
        Vector2 m = GetMousePosition();
        Vector2 w = GetScreenToWorld2D(m, cam);
        const UnitTemplate* t = g_templates.find(ToLower(g_debug.spawnName));
        float r = t ? t->radius : 12.0f;
        DrawCircleLines((int)w.x, (int)w.y, r, Color{ 120, 255, 120, 200 });
        DrawCircleLines((int)w.x, (int)w.y, 3.0f, Color{ 120, 255, 120, 200 });
    }
    if (!g_debug.world) return;

    for (auto& u : g.units) {
        if (!u.tpl || u.dead || u.riding()) continue;
        Vector2 p{ u.pos.x, u.pos.y };
        Color rc = u.selected ? Color{ 255, 255, 255, 200 } : Color{ 255, 255, 255, 70 };
        DrawCircleLines((int)p.x, (int)p.y, u.tpl->radius, rc);
        float range = maxRangeOf(*u.tpl);
        if (range > 0) {
            DrawCircleLines((int)p.x, (int)p.y, range, Color{ 255, 160, 40, (uint8_t)(u.selected ? 160 : 55) });
            float minR = 0;
            for (auto& tur : u.tpl->turrets) minR = std::max(minR, tur.minRange);
            if (minR > 0) DrawCircleLines((int)p.x, (int)p.y, minR, Color{ 255, 160, 40, 90 });
        }
        // path + goal
        if (u.pathIdx < (int)u.path.size()) {
            Vector2 prev = p;
            for (int i = u.pathIdx; i < (int)u.path.size(); ++i) {
                Vector2 n{ u.path[i].x, u.path[i].y };
                DrawLineV(prev, n, Color{ 60, 220, 220, 160 });
                DrawCircleV(n, 2.0f, Color{ 60, 220, 220, 200 });
                prev = n;
            }
        }
        if (u.hasGoal) {
            Vector2 gp{ u.moveGoal.x, u.moveGoal.y };
            DrawLineV({ gp.x - 5, gp.y - 5 }, { gp.x + 5, gp.y + 5 }, Color{ 60, 220, 220, 220 });
            DrawLineV({ gp.x - 5, gp.y + 5 }, { gp.x + 5, gp.y - 5 }, Color{ 60, 220, 220, 220 });
        }
        // order links
        auto link = [&](int id, Color c) {
            const Unit* t = g.get(id);
            if (t && !t->dead) DrawLineV(p, Vector2{ t->pos.x, t->pos.y }, c);
        };
        link(u.orderTargetId, Color{ 255, 60, 60, 200 });
        if (u.targetId != u.orderTargetId) link(u.targetId, Color{ 255, 160, 40, 140 });
        link(u.nanoTargetId, u.nanoReclaim ? Color{ 220, 60, 60, 180 } : Color{ 80, 230, 80, 180 });
        link(u.loadTargetId, Color{ 90, 150, 255, 180 });
        link(u.loadUpTargetId, Color{ 90, 150, 255, 180 });
        // turret aims
        for (size_t i = 0; i < u.tur.size() && i < u.tpl->turrets.size(); ++i) {
            Vec2 pv = turretPivot(u, (int)i);
            float a = u.tur[i].angle;
            Vec2 tip = pv + Vec2{ std::cos(a), std::sin(a) } * 26.0f;
            Color ac = u.tur[i].active ? Color{ 255, 230, 60, 230 } : Color{ 255, 230, 60, 110 };
            DrawLineV(Vector2{ pv.x, pv.y }, Vector2{ tip.x, tip.y }, ac);
        }
        // leg rig for the selected unit: attach pivot (cyan), rendered foot cross (magenta), limb line
        if (u.selected && !u.legs.empty()) {
            float dir = (u.tpl->lockLegRot && !u.tur.empty()) ? u.tur[0].angle : u.facing;
            Vec2 aim{ std::cos(dir), std::sin(dir) }, right{ -aim.y, aim.x };
            for (size_t i = 0; i < u.legs.size() && i < u.tpl->legs.size(); ++i) {
                const LegTpl& C = u.tpl->legs[i];
                const LegState& S = u.legs[i];
                Vec2 aw = u.pos + aim * C.attachY + right * C.attachX;
                Vector2 ap{ aw.x, aw.y - u.posZ };
                Vec2 fw = u.pos + S.foot;
                Vector2 fp{ fw.x, fw.y - S.z - u.posZ };
                DrawLineV(ap, fp, Color{ 255, 80, 255, 120 });
                DrawCircleV(ap, 2.0f, Color{ 80, 230, 230, 220 });
                DrawLineV({ fp.x - 4, fp.y }, { fp.x + 4, fp.y }, Color{ 255, 80, 255, 220 });
                DrawLineV({ fp.x, fp.y - 4 }, { fp.x, fp.y + 4 }, Color{ 255, 80, 255, 220 });
            }
        }
        if (u.selected) {
            char buf[48];
            snprintf(buf, sizeof(buf), "#%d %d/%d", u.id, (int)u.hp, (int)u.maxHp);
            DrawText(buf, (int)p.x - 20, (int)(p.y - u.tpl->radius - 14), 10, WHITE);
        }
    }
    for (auto& pr : g.projectiles) {
        Vector2 p{ pr.pos.x, pr.pos.y - pr.posZ };
        DrawLineV(p, { p.x + pr.vel.x * 5, p.y + pr.vel.y * 5 }, Color{ 255, 255, 255, 150 });
        const Unit* t = pr.targetId >= 0 ? g.get(pr.targetId) : nullptr;
        if (t && !t->dead)
            DrawLineV(p, Vector2{ t->pos.x, t->pos.y - t->posZ }, Color{ 255, 80, 255, 110 });
        else if (pr.unguided || pr.targetId < 0)
            DrawLineV(p, Vector2{ pr.dest.x, pr.dest.y }, Color{ 255, 80, 255, 70 });
        if (pr.hit && pr.areaRadius > 0)
            DrawCircleLines((int)pr.impactPos.x, (int)pr.impactPos.y, pr.areaRadius, Color{ 255, 80, 255, 140 });
    }
}

// ---------------------------------------------------------------- HUD panels

static void drawStats(Game& g) {
    float x = 6, y = 88, w = 360;
    int riding = 0, building = 0;
    for (auto& u : g.units) { if (u.riding()) ++riding; if (u.underConstruction) ++building; }
    std::vector<std::string> lines;
    char buf[160];
    snprintf(buf, sizeof(buf), "DEBUG  F1 stats  F2 unit  F3 world  F4 grid");
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "FPS %d  frame %.2fms  tick %ld (%ld:%02ld)",
             GetFPS(), GetFrameTime() * 1000.0, g.tick, g.tick / 3600, (g.tick / 60) % 60);
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "sim %s%gx   P pause  [ ] speed  . step",
             g_debug.paused ? "PAUSED  " : "", g_debug.speed);
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "units %zu (riding %d, building %d)  proj %zu",
             g.units.size(), riding, building, g.projectiles.size());
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "fx %zu  effects %zu  particles %zu  wrecks %zu  scorch %zu",
             g.fx.size(), g.effects.size(), g.particles.size(), g.wrecks.size(), g.scorches.size());
    lines.push_back(buf);
    for (auto& t : g.teams) {
        int n = 0;
        for (auto& u : g.units) if (u.team == t.id && !u.dead) ++n;
        snprintf(buf, sizeof(buf), "team %d (%s): $%d  units %d  %s",
                 t.id, t.isPlayer ? "player" : "ai", (int)t.credits, n, t.alive ? "alive" : "DEAD");
        lines.push_back(buf);
    }
    snprintf(buf, sizeof(buf), "templates %zu  map %dx%d", g_templates.order.size(), g.map.w, g.map.h);
    lines.push_back(buf);
    lines.push_back("F6 +$10k  F7 instant  F8 heal  F9 kill  F10 spawn");
    lines.push_back("cheats target selection, else all player units");
    float h = 12 + lines.size() * 16;
    panel(x, y, w, h);
    for (size_t i = 0; i < lines.size(); ++i)
        DrawText(lines[i].c_str(), (int)x + 8, (int)y + 6 + (int)i * 16, 13,
                 i == 0 ? Color{ 120, 220, 255, 255 } : LIGHTGRAY);
}

static void drawInspector(Game& g, const Camera2D& cam) {
    Vector2 mw = GetScreenToWorld2D(GetMousePosition(), cam);
    Unit* u = unitAt(g, Vec2{ mw.x, mw.y });
    if (!u) { auto sel = g.selectedUnits(); if (!sel.empty()) u = sel[0]; }
    float w = 292, x = GetScreenWidth() - w - 8, y = 176;
    if (!u) {
        g_debug.inspRect = Rectangle{ 0, 0, 0, 0 };
        panel(x, y, w, 30);
        DrawText("F2 inspector: hover or select a unit", (int)x + 8, (int)y + 8, 13, GRAY);
        return;
    }
    const UnitTemplate& t = *u->tpl;
    std::vector<std::string> lines;
    char buf[192];
    snprintf(buf, sizeof(buf), "#%d %s [%s]", u->id, t.displayText.c_str(), u->tplKey.c_str());
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "team %d  pos %.0f,%.0f  z %.1f", u->team, u->pos.x, u->pos.y, u->posZ);
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "hp %.0f/%.0f  shield %.0f/%.0f  energy %.0f/%.0f",
             u->hp, u->maxHp, u->shield, u->shieldMax, u->energy, t.maxEnergy);
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "%s%s%s%s%s",
             u->underConstruction ? "CONSTRUCTING " : "",
             u->driven ? "moving " : "idle ",
             u->isRotating ? "pivot " : "",
             u->deathFalling ? "FALLING " : "",
             u->riding() ? "RIDING" : "");
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "facing %.0fdeg  throttle %.2f  vel %.2f,%.2f",
             u->facing * 57.29578f, u->throttle, u->vel.x, u->vel.y);
    lines.push_back(buf);
    if (u->hasGoal) {
        snprintf(buf, sizeof(buf), "goal %.0f,%.0f  path %d/%zu  %s",
                 u->moveGoal.x, u->moveGoal.y, u->pathIdx, u->path.size(), u->attackMove ? "attack-move" : "move");
        lines.push_back(buf);
    }
    snprintf(buf, sizeof(buf), "repath %d  stuck %d  idle %.0f", u->repathTimer, u->stuckTimer, u->idleTime);
    lines.push_back(buf);
    auto idline = [&](const char* label, int id) {
        if (id < 0) return;
        const Unit* o = g.get(id);
        snprintf(buf, sizeof(buf), "%s #%d %s  d=%.0f", label, id,
                 o && o->tpl ? o->tplKey.c_str() : "(gone)", o ? u->pos.dist(o->pos) : 0.0f);
        lines.push_back(buf);
    };
    idline("orderTarget", u->orderTargetId);
    idline("target", u->targetId);
    idline(u->nanoReclaim ? "reclaim" : "nano", u->nanoTargetId);
    idline("chase", u->moveGoalId);
    idline("loadInto", u->loadTargetId);
    idline("loadUp", u->loadUpTargetId);
    idline("transport", u->transportId);
    if (!u->cargo.empty()) {
        snprintf(buf, sizeof(buf), "cargo %zu  %s  unloadT %.0f", u->cargo.size(),
                 u->unloading ? "unloading" : "held", u->unloadTimer);
        lines.push_back(buf);
    }
    if (u->underConstruction) {
        snprintf(buf, sizeof(buf), "build %.0f%%", u->buildProgress * 100.0f);
        lines.push_back(buf);
    }
    if (!u->prodQueue.empty()) {
        snprintf(buf, sizeof(buf), "queue %zu  first %.0f%%  [%s]", u->prodQueue.size(),
                 u->prodProgress * 100.0f, u->prodQueue[0].c_str());
        lines.push_back(buf);
    }
    if (u->upgradeIdx >= 0) {
        snprintf(buf, sizeof(buf), "upgrade %d  %.0f%%", u->upgradeIdx, u->upgradeProgress * 100.0f);
        lines.push_back(buf);
    }
    for (size_t i = 0; i < u->tur.size(); ++i) {
        const TurretState& s = u->tur[i];
        snprintf(buf, sizeof(buf), "tur%zu ang %.0f  cd %.0f  warm %.0f  tgt %d%s",
                 i, s.angle * 57.29578f, s.cooldown, s.warm, s.targetId, s.active ? " READY" : "");
        lines.push_back(buf);
    }
    if (u->animCur) {
        snprintf(buf, sizeof(buf), "anim t %.1f  frame %d  prio %d%s", u->animTime, u->animFrame,
                 u->animPrio, u->animLoop ? " loop" : "");
        lines.push_back(buf);
    }
    if (!u->legs.empty()) {
        snprintf(buf, sizeof(buf), "legs %zu  lockRot %d  scale %.2f",
                 u->legs.size(), t.lockLegRot ? 1 : 0, t.imageScale * u->animScale);
        lines.push_back(buf);
        for (size_t i = 0; i < u->legs.size() && i < t.legs.size(); ++i) {
            const LegTpl& C = t.legs[i];
            const LegState& S = u->legs[i];
            snprintf(buf, sizeof(buf), "leg%zu xy %.0f,%.0f att %.0f,%.0f L%dO%dU%dH%d",
                     i, C.x, C.y, C.attachX, C.attachY,
                     C.lockMovement ? 1 : 0, C.drawOverBody ? 1 : 0,
                     C.drawUnderAllUnits ? 1 : 0, C.hidden ? 1 : 0);
            lines.push_back(buf);
            snprintf(buf, sizeof(buf), "  foot %.1f,%.1f z %.1f a %.2f st%d",
                     S.foot.x, S.foot.y, S.z, S.alpha, S.stepping ? 1 : 0);
            lines.push_back(buf);
            snprintf(buf, sizeof(buf), "  ang %.0f sp %.0f tgt %.1f,%.1f i%d%d%d",
                     S.angle * 57.29578f, S.spin * 57.29578f, S.target.x, S.target.y,
                     C.imageLeg.empty() ? 0 : 1, C.imageFoot.empty() ? 0 : 1,
                     C.imageShadow.empty() ? 0 : 1);
            lines.push_back(buf);
        }
    }
    // Clamp the panel to the screen and wheel-scroll the overflow while hovering it.
    float maxH = (float)GetScreenHeight() - y - 8;
    float h = std::min(12 + lines.size() * 15.0f, maxH);
    g_debug.inspRect = Rectangle{ x, y, w, h };
    if (CheckCollisionPointRec(GetMousePosition(), g_debug.inspRect)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0) g_debug.inspScroll -= (int)(wheel * 3);
    }
    int visible = std::max(1, (int)((h - 12) / 15));
    g_debug.inspScroll = std::max(0, std::min(std::max(0, (int)lines.size() - visible), g_debug.inspScroll));
    panel(x, y, w, h);
    for (int i = 0; i < visible && g_debug.inspScroll + i < (int)lines.size(); ++i) {
        size_t li = (size_t)(g_debug.inspScroll + i);
        DrawText(lines[li].c_str(), (int)x + 8, (int)y + 6 + i * 15, 12,
                 li == 0 ? dbgTeamColor(u->team) : LIGHTGRAY);
    }
    if ((int)lines.size() > visible) {
        snprintf(buf, sizeof(buf), "%d-%d/%zu", g_debug.inspScroll + 1,
                 g_debug.inspScroll + visible, lines.size());
        DrawText(buf, (int)(x + w) - 70, (int)(y + h) - 14, 11, GRAY);
    }
}

static void drawPicker() {
    Rectangle pr = pickerRect();
    panel(pr.x, pr.y, pr.width, pr.height);
    DrawText("SPAWN UNIT (F10)", (int)pr.x + 8, (int)pr.y + 6, 15, Color{ 120, 220, 255, 255 });
    DrawRectangle((int)pr.x + 8, (int)pr.y + 28, (int)pr.width - 16, 20, Color{ 25, 32, 42, 255 });
    std::string f = g_debug.spawnFilter.empty() ? "type to filter..." : g_debug.spawnFilter + "_";
    DrawText(f.c_str(), (int)pr.x + 12, (int)pr.y + 32, 13,
             g_debug.spawnFilter.empty() ? GRAY : WHITE);
    auto list = pickerList();
    int visible = (int)((pr.height - 58) / 20);
    g_debug.spawnScroll = std::max(0, std::min(std::max(0, (int)list.size() - visible), g_debug.spawnScroll));
    Vector2 m = GetMousePosition();
    char buf[160];
    for (int i = 0; i < visible && g_debug.spawnScroll + i < (int)list.size(); ++i) {
        const UnitTemplate* t = list[g_debug.spawnScroll + i];
        Rectangle row{ pr.x + 8, pr.y + 54 + i * 20.0f, pr.width - 16, 19 };
        bool hover = CheckCollisionPointRec(m, row);
        if (hover) DrawRectangleRec(row, Color{ 50, 75, 105, 255 });
        snprintf(buf, sizeof(buf), "%s%s", t->displayText.c_str(), t->isBuilding ? " [B]" : "");
        DrawText(buf, (int)row.x + 4, (int)row.y + 3, 13, hover ? WHITE : LIGHTGRAY);
        snprintf(buf, sizeof(buf), "$%d", (int)t->price);
        DrawText(buf, (int)(row.x + row.width) - 52, (int)row.y + 3, 13, Color{ 240, 220, 90, 255 });
    }
    snprintf(buf, sizeof(buf), "%zu / %zu", list.size(), g_templates.order.size());
    DrawText(buf, (int)pr.x + 8, (int)(pr.y + pr.height) - 16, 12, GRAY);
}

void debugDrawHUD(Game& g, const Camera2D& cam) {
    if (g_debug.paused || g_debug.speed != 1.0f) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s%gx%s", g_debug.paused ? "PAUSED" : "",
                 g_debug.paused && g_debug.speed != 1.0f ? "  " : "",
                 g_debug.speed, g_debug.paused ? "  (. step)" : "");
        int tw = MeasureText(buf, 20);
        DrawText(buf, GetScreenWidth() / 2 - tw / 2, 10, 20, Color{ 255, 120, 90, 255 });
    }
    if (g_debug.stats) drawStats(g);
    if (g_debug.inspector) drawInspector(g, cam);
    if (g_debug.spawnOpen) drawPicker();
    if (g_debug.spawnArmed) {
        Vector2 m = GetMousePosition();
        std::string s = "Spawn: " + g_debug.spawnName + "  (LMB place, Shift=enemy, RMB cancel)";
        DrawText(s.c_str(), (int)m.x + 14, (int)m.y + 16, 15, Color{ 120, 255, 120, 255 });
    }
}