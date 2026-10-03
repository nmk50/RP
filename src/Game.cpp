#include "Game.h"
#include "Pathfinding.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

Rng g_rng;
std::string g_assetsRoot = "assets";

static float lerpAngle(float a, float b, float maxStep) {
    float d = b - a;
    while (d > 3.14159265f) d -= 2 * 3.14159265f;
    while (d < -3.14159265f) d += 2 * 3.14159265f;
    if (std::abs(d) <= maxStep) return b;
    return a + (d > 0 ? maxStep : -maxStep);
}

// shortest signed delta from a to b, in radians (RWX Utility.endsWith)
static float angleDiff(float a, float b) {
    float d = b - a;
    while (d > 3.14159265f) d -= 2 * 3.14159265f;
    while (d < -3.14159265f) d += 2 * 3.14159265f;
    return d;
}

// RWX keeps every angle in (-180, 180] (addRotation:1319-1335); without this the hull and barrel
// angles drift upward all game and lose precision.
static float wrapAngle(float a) {
    while (a > 3.14159265f) a -= 2 * 3.14159265f;
    while (a <= -3.14159265f) a += 2 * 3.14159265f;
    return a;
}

static Team* teamOf(Game& g, int id) { for (auto& t : g.teams) if (t.id == id) return &t; return nullptr; }

// RWX OrderableUnit.isOverLiquid / ScorchMark skip: no ground decal over water.
static bool overWater(const Game& g, Vec2 p) {
    int x = (int)(p.x / TILE_SIZE), y = (int)(p.y / TILE_SIZE);
    return g.map.inBounds(x, y) && g.map.at(x, y).water;
}

// RWX Utility.distanceSq(current, target, maxDelta): ramp a scalar toward a target by at most a step.
static float moveTowards(float v, float to, float step) {
    float d = to - v;
    return std::fabs(d) <= step ? to : v + (d > 0 ? step : -step);
}

// RWX Projectile.a:228 - [projectile_N] buildingDamageMultiplier only scales damage dealt to buildings.
static float damageScale(const Projectile& p, const Unit& u) {
    return (p.buildingMult != 1.0f && u.tpl && u.tpl->isBuilding) ? p.buildingMult : 1.0f;
}

// Sprite px -> world px, the same factor the renderer draws the body with.
static float texScale(const UnitTemplate& tpl) {
    return tpl.texFrameWpx > 0 ? (tpl.displayRadius * 2.0f * tpl.imageScale) / tpl.texFrameWpx : 1.0f;
}

// RWX OrderableUnit.a(f, angle, i):1351 - the barrel turns at [turret] turnSpeed deg/tick, or ramps
// to it through turnSpeedAcceleration and brakes with turnSpeedDeceleration, which is what makes a
// heavy turret lag behind a fast target. RWX does that arithmetic in degrees, so the angular velocity
// is kept in deg/tick and only the applied step becomes radians. Returns the angle still to cover.
static float rotateTurret(TurretState& s, const TurretTpl& tur, float want) {
    float err = angleDiff(s.angle, want);
    if (err == 0.0f) return 0.0f;
    float errDeg = err * 57.29578f;
    float applied;
    if (tur.turnAccel <= 0.0f) {                            // w(i) <= 0: no inertia, snap at turnSpeed
        applied = std::fabs(errDeg) > tur.turnSpeed ? (errDeg > 0.0f ? tur.turnSpeed : -tur.turnSpeed) : errDeg;
        s.angVel = 0.0f;
    } else {
        float decel = tur.turnDecel > 0.0f ? tur.turnDecel : tur.turnAccel;
        float dir = errDeg > 0.0f ? 1.0f : -1.0f;
        // :1366-1369 within the distance needed to stop, and still moving that way: bleed speed off
        if (decel > 0.0f && std::fabs(errDeg) < std::fabs(s.angVel) / decel && (s.angVel > 0.0f) == (errDeg > 0.0f))
            s.angVel = moveTowards(s.angVel, dir * decel, decel);
        else
            s.angVel = moveTowards(s.angVel, dir * tur.turnSpeed, tur.turnAccel);
        applied = s.angVel;
        if (std::fabs(applied) > std::fabs(errDeg)) { s.angVel = 0.0f; applied = errDeg; }
    }
    s.angle = wrapAngle(s.angle + applied * 0.0174533f);
    return err - applied * 0.0174533f;
}

// Can this barrel engage that unit at all: RWX checks the target's domain against the turret's own
// canAttack* flags and, when the section sets limitingRange / limitingMinRange, the distance
// (CustomUnit.a(TurretConfig, i, baseUnit, ..):1266, TurretConfig.java:484-501).
static bool turretSees(const Game& g, const Unit& u, int i, const Unit& t) {
    if (!t.tpl || t.dead || !t.operational() || !g.isEnemy(u.team, t.team)) return false;
    const TurretTpl& tur = u.tpl->turrets[i];
    bool fly = t.tpl->isAirUnit(), sea = t.tpl->isWaterUnit(), land = !fly && !sea;
    if (!((fly && tur.canAttackFlying) || (land && tur.canAttackLand) || (sea && tur.canAttackUnderwater))) return false;
    float d = u.pos.dist(t.pos);
    if (tur.range > 0 && d > tur.range) return false;
    if (tur.minRange > 0 && d < tur.minRange) return false;
    return true;
}

// RWX gives each [turret_N] its own UnitMovementData when the unit is built (CustomUnit:1033-1036),
// aimed along the hull with every timer zeroed. An upgrade that adds barrels only grows the array
// and keeps the ones already there (BaseUnit:975-986).
static void initTurrets(Unit& u) {
    if (!u.tpl) return;
    size_t n = u.tpl->turrets.size(), old = u.tur.size();
    if (old == n) return;
    u.tur.resize(n);
    for (size_t i = old; i < n; ++i) u.tur[i].angle = u.facing;
}

bool Game::init(const std::string& mapPath, std::string* err) {
    if (!map.loadTmx(mapPath, err)) return false;

    std::vector<bool> used(16, false);
    for (auto& s : map.spawns) if (s.team >= 0 && s.team < 16) used[s.team] = true;
    for (int i = 0; i < 16; ++i) if (used[i]) {
        Team t; t.id = i; t.color = i; t.isPlayer = (i == 0); t.credits = (i == 0) ? 4000 : 2500;
        teams.push_back(t);
    }
    if (!used[0]) { Team a; a.id = 0; a.isPlayer = true; a.credits = 4000; teams.push_back(a); }

    for (auto& s : map.spawns) {
        Vec2 pos{ (s.tx + 0.5f) * TILE_SIZE, (s.ty + 0.5f) * TILE_SIZE };
        spawnUnit(s.unit, s.team, pos);
    }

    auto ensure = [&](int teamId) {
        for (auto& u : units) if (u.team == teamId) return;
        int band = (teamId == 0) ? map.h * 3 / 4 : map.h / 4;
        for (int r = 0; r < map.h; ++r) {
            int ys[2] = { band + r, band - r };
            for (int sgn = 0; sgn < 2; ++sgn) {
                int y = ys[sgn]; if (y < 2 || y >= map.h - 2) continue;
                for (int x = map.w / 2 - 4; x < map.w / 2 + 4; ++x) {
                    if (!map.inBounds(x, y) || !map.canPlaceBuilding(x, y)) continue;
                    Vec2 p{ (x + 0.5f) * TILE_SIZE, (y + 0.5f) * TILE_SIZE };
                    auto done = [&](int id) { Unit* u = get(id); if (u) { u->underConstruction = false; u->buildProgress = 1; } };
                    done(spawnUnit("commandCenter", teamId, p));
                    done(spawnUnit("landFactory", teamId, p + Vec2{ TILE_SIZE * 3, 0 }));
                    spawnUnit("builder", teamId, p + Vec2{ TILE_SIZE, TILE_SIZE * 2 });
                    spawnUnit("builder", teamId, p + Vec2{ -TILE_SIZE, TILE_SIZE * 2 });
                    if (teamId != 0) for (int k = 0; k < 4; ++k) spawnUnit("tank", teamId, p + Vec2{ TILE_SIZE * (k - 2), -TILE_SIZE * 2 });
                    return;
                }
            }
        }
    };
    ensure(0);
    int enemy = -1; for (auto& t : teams) if (!t.isPlayer) { enemy = t.id; break; }
    if (enemy < 0) { Team e; e.id = 1; e.color = 1; e.credits = 3000; teams.push_back(e); enemy = 1; }
    ensure(enemy);

    // AI teams from map spawns may lack a factory; give them one near their base
    for (auto& t : teams) {
        if (t.isPlayer) continue;
        bool hasFactory = false;
        for (auto& u : units) {
            if (u.team != t.id || !u.tpl || !u.tpl->isFactory || u.dead) continue;
            if (u.tpl->name != "commandCenter") hasFactory = true;
        }
        if (hasFactory) continue;
        const UnitTemplate* lf = g_templates.find("landfactory");
        Vec2 anchor{ map.worldW() / 2.0f, map.worldH() / 4.0f };
        for (auto& u : units) if (u.team == t.id && u.tpl) {
            if (u.tpl->name == "commandCenter") { anchor = u.pos; break; }
            anchor = u.pos;
        }
        int cx = (int)(anchor.x / TILE_SIZE), cy = (int)(anchor.y / TILE_SIZE);
        for (int r = 2; r <= 8; ++r) {
            bool placed = false;
            for (int dy = -r; dy <= r && !placed; ++dy)
                for (int dx = -r; dx <= r && !placed; ++dx) {
                    int x = cx + dx, y = cy + dy;
                    if (!map.canPlaceBuilding(x, y)) continue;
                    Vec2 p{ (x + 0.5f) * TILE_SIZE, (y + 0.5f) * TILE_SIZE };
                    if (lf && !buildingSpotFree(p, *lf)) continue;
                    int id = spawnUnit("landFactory", t.id, p);
                    Unit* f = get(id);
                    if (f) { f->underConstruction = false; f->buildProgress = 1; placed = true; }
                }
            if (placed) break;
        }
    }
    return true;
}

Unit* Game::get(int id) { for (auto& u : units) if (u.id == id) return &u; return nullptr; }
const Unit* Game::get(int id) const { for (auto& u : units) if (u.id == id) return &u; return nullptr; }

// RW: building tiles get path cost -1; mark/unmark footprint tiles
static void markBuildingOcc(TileMap& map, const Unit& u, int delta) {
    if (!u.tpl || !u.tpl->isStatic()) return;
    int cx = (int)(u.pos.x / TILE_SIZE), cy = (int)(u.pos.y / TILE_SIZE);
    for (int y = cy + u.tpl->footU; y <= cy + u.tpl->footD; ++y)
        for (int x = cx + u.tpl->footL; x <= cx + u.tpl->footR; ++x)
            map.markOcc(x, y, delta);
}

int Game::spawnUnit(const std::string& tplKey, int team, Vec2 pos, bool underConstruction) {
    const UnitTemplate* tpl = g_templates.find(ToLower(tplKey));
    if (!tpl) return -1;
    Unit u;
    u.id = nextId++;
    u.tplKey = ToLower(tplKey);
    u.tpl = tpl;
    u.team = team;
    u.pos = pos;
    u.prevPos = pos;
    u.hp = u.maxHp = tpl->maxHp;
    // RWX CustomUnit.java:1074-1083: a shielded unit comes up with a full bubble, and only
    // [core] startShieldAtZero asks for it to fill up from empty instead.
    u.shieldMax = (float)tpl->maxShield;
    u.shield = tpl->startShieldAtZero ? 0.0f : u.shieldMax;
    u.footprint[0] = tpl->footL; u.footprint[1] = tpl->footU; u.footprint[2] = tpl->footR; u.footprint[3] = tpl->footD;
    // RWX BaseBuilding.java:108: a building's body angle is -90 deg, so its turret idles along the
    // unrotated sprite (north) plus [turret] idleDir.
    if (tpl->isStatic()) u.facing = -1.5708f;
    initTurrets(u);
    u.underConstruction = underConstruction;
    units.push_back(u);
    if (tpl->isStatic()) markBuildingOcc(map, units.back(), +1);
    return u.id;
}

bool Game::canAfford(int team, float cost) { Team* t = teamOf(*this, team); return t && t->credits >= cost; }
void Game::spend(int team, float cost) { Team* t = teamOf(*this, team); if (t) t->credits -= cost; }
void Game::reward(int team, float credits) { Team* t = teamOf(*this, team); if (t) t->credits += credits; }

static bool passableAt(Game& g, Vec2 p, MoveType mt) {
    int x = (int)(p.x / TILE_SIZE), y = (int)(p.y / TILE_SIZE);
    switch (mt) {
        case MoveType::AIR: case MoveType::HOVER: return g.map.inBounds(x, y);
        case MoveType::WATER: return g.map.walkableWater(x, y);
        default: return g.map.walkableLand(x, y);
    }
}

// Bresenham-style straight-line check; RW skips A* entirely when this passes
static bool losClear(Game& g, Vec2 a, Vec2 b, MoveType mt) {
    float d = a.dist(b);
    int n = (int)(d / (TILE_SIZE * 0.5f)) + 1;
    for (int i = 0; i <= n; ++i) {
        float t = (float)i / n;
        Vec2 p{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
        if (!passableAt(g, p, mt)) return false;
    }
    return true;
}

static void computePath(Game& g, Unit& u, Vec2 target) {
    MoveType mt = u.tpl->moveType;
    u.moveGoal = target; u.hasGoal = true; u.repathTimer = 30; u.stuckTimer = 0;
    u.path.clear(); u.pathIdx = 0;
    if (mt == MoveType::AIR || mt == MoveType::HOVER) { u.path.push_back(target); return; }
    if (losClear(g, u.pos, target, mt)) {
        Vec2 d = target - u.pos; float len = d.len();
        int n = (int)(len / TILE_SIZE);
        for (int i = 1; i <= n; ++i) {
            float k = (float)i * TILE_SIZE;
            if (k >= len) break;
            u.path.push_back(u.pos + d * (k / len));
        }
        u.path.push_back(target); // last waypoint = exact click point (RW :4216-4231)
        return;
    }
    int sx = (int)(u.pos.x / TILE_SIZE), sy = (int)(u.pos.y / TILE_SIZE);
    int gx = (int)(target.x / TILE_SIZE), gy = (int)(target.y / TILE_SIZE);
    u.path = findPath(g.map, sx, sy, gx, gy, mt);
    if (u.path.empty()) { u.hasGoal = false; return; }
    if (passableAt(g, target, mt)) u.path.back() = target;
}

void Game::orderMove(std::vector<int> ids, Vec2 target) {
    int k = 0;
    for (int id : ids) {
        Unit* u = get(id); if (!u || !u->tpl || u->isStatic()) continue;
        u->orderTargetId = -1; u->attackMove = false; u->nanoTargetId = -1;
        Vec2 t = target;
        if (k > 0) { // RW formation scatter: golden-angle offsets around the click point
            float ang = (float)k * 2.39996f;
            float rad = 6.0f + u->tpl->radius * 1.5f * std::sqrt((float)k);
            t = target + Vec2{ std::cos(ang) * rad, std::sin(ang) * rad };
        }
        computePath(*this, *u, t);
        k++;
    }
}

void Game::orderAttackMove(std::vector<int> ids, Vec2 target) {
    orderMove(ids, target);
    for (int id : ids) { Unit* u = get(id); if (u && u->tpl && !u->isStatic()) u->attackMove = true; }
}

int Game::orderAttackTarget(std::vector<int> ids, int targetId) {
    Unit* tgt = get(targetId); if (!tgt) return 0;
    int assigned = 0;
    for (int id : ids) {
        Unit* u = get(id); if (!u || !u->tpl || u->isStatic() || !u->tpl->canAttack) continue;
        u->orderTargetId = targetId; u->attackMove = false; u->nanoTargetId = -1;
        if (u->pos.dist(tgt->pos) > u->tpl->attackRange * 0.9f) {
            // RWX engagement: stop at range instead of walking into the target
            float stop = std::max(20.0f, u->tpl->attackRange * 0.8f - tgt->tpl->radius);
            Vec2 away = (u->pos - tgt->pos).norm();
            computePath(*this, *u, tgt->pos + away * stop);
        }
        assigned++;
    }
    return assigned;
}

// RWX CustomUnit.getTransportedUnitCount:3630-3644 through canRepairTarget:3648-3653. The vanilla
// builder class only ever fixes buildings (BuilderUnit.java:103-108), which RP's builder.ini says with
// canRepairBuildings:true.
bool canRepairTarget(const UnitTemplate& builder, const Unit& target) {
    if (target.dead || !target.tpl || target.tpl->isUnrepairableUnit) return false;
    return target.tpl->isBuilding ? builder.canRepairBuildings : builder.canRepairUnits;
}

// RWX has one command for both jobs: GameUI.java:2653 accepts a target that is damaged *or* unfinished
// and requires every selected unit to pass canRepairTarget, then issues UnitCommandType.repair
// (UnitCommand.java:236-240). OrderableUnit.java:2301 picks the branch each tick.
bool Game::orderRepair(std::vector<int> ids, int targetId) {
    Unit* t = get(targetId);
    if (!t || !t->tpl || t->dead) return false;
    if (t->operational() && t->hp >= t->maxHp) return false;
    if (ids.empty()) return false;
    for (int id : ids) {
        Unit* u = get(id);
        if (!u || !u->tpl || !u->tpl->isBuilder || u->team != t->team) return false;
        if (!canRepairTarget(*u->tpl, *t)) return false;
    }
    for (int id : ids) {
        Unit* u = get(id);
        u->nanoTargetId = targetId; u->orderTargetId = -1; u->attackMove = false;
        u->path.clear(); u->pathIdx = 0; u->hasGoal = false;
    }
    return true;
}

void Game::orderStop(std::vector<int> ids) {
    for (int id : ids) { Unit* u = get(id); if (!u) continue; u->path.clear(); u->pathIdx = 0; u->orderTargetId = -1; u->attackMove = false; u->nanoTargetId = -1; }
}

bool Game::buildingSpotFree(Vec2 center, const UnitTemplate& t) const {
    float w = t.footprintTilesW() * TILE_SIZE, h = t.footprintTilesH() * TILE_SIZE;
    for (auto& u : units) {
        if (u.dead || !u.tpl || !u.tpl->isStatic()) continue;
        float uw = u.tpl->footprintTilesW() * TILE_SIZE, uh = u.tpl->footprintTilesH() * TILE_SIZE;
        if (std::fabs(center.x - u.pos.x) < (w + uw) / 2 - 2 && std::fabs(center.y - u.pos.y) < (h + uh) / 2 - 2) return false;
    }
    return true;
}

bool Game::orderBuild(int builderId, const std::string& unitName, int tx, int ty) {
    Unit* b = get(builderId); if (!b || !b->tpl || !b->tpl->isBuilder) return false;
    const UnitTemplate* tpl = g_templates.find(ToLower(unitName)); if (!tpl || !tpl->isBuilding) return false;
    if (!canAfford(b->team, tpl->price)) return false;
    int fw = tpl->footprintTilesW(), fh = tpl->footprintTilesH();
    int minx = tx + tpl->footL, miny = ty + tpl->footU;
    for (int y = miny; y < miny + fh; ++y) for (int x = minx; x < minx + fw; ++x)
        if (!map.inBounds(x, y) || !(tpl->placeOnlyOnWater ? map.canPlaceWaterBuilding(x, y) : map.canPlaceBuilding(x, y))) return false;
    if (tpl->placeOnlyOnResPool) {
        // RWX ResourceExtractor.canPlaceAtCurrentPosition: only the center tile must be a resource pool
        if (!map.inBounds(tx, ty) || !map.at(tx, ty).resource) return false;
    }
    Vec2 center{ (tx + 0.5f) * TILE_SIZE, (ty + 0.5f) * TILE_SIZE };
    if (!buildingSpotFree(center, *tpl)) return false;
    spend(b->team, tpl->price);
    int bid = spawnUnit(unitName, b->team, center, true);
    if (bid < 0) { reward(b->team, tpl->price); return false; }
    if (b->tpl->dieOnConstruct) {
        // RWX CustomUnit.m():3668-3679: the builder is given to the foundation. It leaves play with no
        // death effect and the new building takes over its selection.
        bool wasSelected = b->selected;
        if (wasSelected) b->selected = false;
        removeUnit(*b);
        if (wasSelected) { Unit* s = get(bid); if (s) s->selected = true; }
    } else {
        b->nanoTargetId = bid;
    }
    return true;
}

// nearest water tile to `from` within `radiusPx` (ships must launch from water)
static Vec2 findWaterSpawn(Game& g, Vec2 from, float radiusPx) {
    int cx = (int)(from.x / TILE_SIZE), cy = (int)(from.y / TILE_SIZE);
    int r = (int)(radiusPx / TILE_SIZE) + 1;
    float best = 1e18f; Vec2 bp = from;
    for (int y = cy - r; y <= cy + r; ++y) for (int x = cx - r; x <= cx + r; ++x) {
        if (!g.map.inBounds(x, y) || !g.map.at(x, y).water) continue;
        Vec2 p{ (x + 0.5f) * TILE_SIZE, (y + 0.5f) * TILE_SIZE };
        float d = from.dist(p);
        if (d < best) { best = d; bp = p; }
    }
    return bp;
}

bool Game::orderProduce(int factoryId, const std::string& unitName) {
    Unit* f = get(factoryId); if (!f || !f->tpl || !f->tpl->isFactory || !f->operational()) return false;
    const UnitTemplate* tpl = g_templates.find(ToLower(unitName)); if (!tpl) return false;
    if (!canAfford(f->team, tpl->price)) return false;
    spend(f->team, tpl->price);
    f->prodQueue.push_back(ToLower(unitName));
    return true;
}

bool Game::orderUpgrade(int unitId, int actionIdx) {
    Unit* u = get(unitId);
    if (!u || !u->tpl || u->underConstruction || u->upgradeIdx >= 0) return false;
    const auto& acts = u->tpl->actions;
    if (actionIdx < 0 || actionIdx >= (int)acts.size()) return false;
    const UnitAction& a = acts[actionIdx];
    if (!g_templates.find(ToLower(a.convertTo))) return false;
    if (!canAfford(u->team, a.price)) return false;
    spend(u->team, a.price);
    u->upgradeIdx = actionIdx;
    u->upgradeProgress = 0;
    return true;
}

std::vector<Unit*> Game::selectedUnits() { std::vector<Unit*> r; for (auto& u : units) if (u.selected) r.push_back(&u); return r; }

std::vector<Unit*> Game::findUnitsInRect(int team, Vec2 a, Vec2 b) {
    Vec2 lo{ std::min(a.x, b.x), std::min(a.y, b.y) }, hi{ std::max(a.x, b.x), std::max(a.y, b.y) };
    std::vector<Unit*> r;
    for (auto& u : units) if (u.team == team && u.pos.x >= lo.x && u.pos.x <= hi.x && u.pos.y >= lo.y && u.pos.y <= hi.y) r.push_back(&u);
    return r;
}

std::vector<Unit*> Game::findEnemyUnits(int team, Vec2 center, float radius, bool flying, bool land, bool underwater) {
    std::vector<Unit*> r;
    for (auto& u : units) {
        if (!u.tpl || u.dead) continue;
        if (!isEnemy(team, u.team)) continue;
        bool isFly = u.tpl->isAirUnit(), isSea = u.tpl->isWaterUnit(), isLand = !isFly && !isSea;
        if (!((isFly && flying) || (isLand && land) || (isSea && underwater))) continue;
        if (center.dist(u.pos) <= radius + u.tpl->radius) r.push_back(&u);
    }
    return r;
}

std::vector<Unit*> Game::queryNearestEnemy(int team, Vec2 pos, float maxDist, const UnitTemplate* shooter) {
    return findEnemyUnits(team, pos, maxDist, shooter->canAttackFlying, shooter->canAttackLand, shooter->canAttackUnderwater);
}

Vec2 Game::nearestEnemyPos(int team) {
    float best = 1e18f; Vec2 r{ map.worldW() / 2, map.worldH() / 2 };
    for (auto& u : units) { if (!u.tpl || u.dead || !isEnemy(team, u.team)) continue; float d = r.dist(u.pos); if (d < best) { best = d; r = u.pos; } }
    return r;
}

void Game::update() {
    ++tick;
    for (size_t i = 0; i < units.size(); ++i) updateUnit(units[i]);
    for (auto& p : projectiles) updateProjectile(p);
    for (auto& a : units) {
        if (!a.tpl || a.isStatic() || a.dead) continue;
        for (auto& b : units) {
            if (&a == &b || b.dead || !b.tpl || b.isStatic()) continue;
            float rr = (a.tpl->radius + b.tpl->radius) * 0.9f;
            Vec2 d = a.pos - b.pos; float dist = d.len();
            if (dist < rr && dist > 0.0001f) a.pos = a.pos + d.norm() * (rr - dist) * 0.5f;
        }
    }
    for (auto& e : effects) { e.t += 1; if (e.t - e.delay >= 13.0f / e.frameSpeed) e.dead = true; }
    for (auto& p : particles) { p.t += 1; p.pos = p.pos + p.vel; if (p.t >= p.dur) p.dead = true; }
    units.erase(std::remove_if(units.begin(), units.end(), [](const Unit& u) { return u.dead; }), units.end());
    projectiles.erase(std::remove_if(projectiles.begin(), projectiles.end(), [](const Projectile& p) { return p.dead; }), projectiles.end());
    effects.erase(std::remove_if(effects.begin(), effects.end(), [](const Effect& e) { return e.dead; }), effects.end());
    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.dead; }), particles.end());
    if (tick % 30 == 0) runAI();
    checkWinLoss();
}

void Game::updateUnit(Unit& u) {
    if (u.dead || !u.tpl) return;
    const UnitTemplate& tpl = *u.tpl;
    if (u.underConstruction && u.buildProgress >= 1.0f) u.underConstruction = false;
    if (tpl.creditIncomeRate > 0 && u.operational() && tick % 10 == 0)
        reward(u.team, tpl.creditIncomeRate * (10.0f / 40.0f));

    // RWX CustomUnit.update:1848-1864: the bubble tops itself up by shieldRegen every tick with no
    // delay after being hit, and the deflection flash fades by 2% of itself plus a flat
    // shieldDeflectionDisplayRate per tick, which is what makes the bubble linger a moment.
    if (tpl.maxShield > 0) {
        u.shield = std::min(u.shield + tpl.shieldRegen, u.shieldMax);
        if (u.shield < 0.0f) u.shield = 0.0f;
        if (u.shieldFlash != 0.0f) {
            u.shieldFlash = std::max(0.0f, u.shieldFlash - u.shieldFlash * 0.02f - tpl.shieldFlashRate);
            if (u.shieldFlash > 50.0f) u.shieldFlash = 50.0f;
        }
    }

    if (tpl.isFactory && u.operational() && !u.prodQueue.empty()) {
        const UnitTemplate* item = g_templates.find(u.prodQueue.front());
        u.prodProgress += item ? item->buildSpeed : tpl.buildSpeed;
        if (u.prodProgress >= 1.0f) {
            u.prodProgress = 0; std::string name = u.prodQueue.front(); u.prodQueue.erase(u.prodQueue.begin());
            Vec2 sp = u.hasRally ? u.rallyPoint : u.pos + Vec2{ 0, (tpl.footprintTilesH() / 2 + 1.5f) * TILE_SIZE };
            if (item && item->isWaterUnit()) sp = findWaterSpawn(*this, sp, tpl.radius + TILE_SIZE * 2.0f);
            spawnUnit(name, u.team, sp);
        }
    }

    // [core] selfBuildRate: a foundation raises itself with no builder at all
    // (CustomUnit.update:1774-1782)
    if (u.underConstruction && tpl.selfBuildRate != 0.0f) {
        u.buildProgress = std::min(1.0f, u.buildProgress + tpl.selfBuildRate);
        if (u.buildProgress >= 1.0f) u.underConstruction = false;
    }

    if (tpl.isBuilder && u.nanoTargetId >= 0) {
        Unit* bs = get(u.nanoTargetId);
        if (!bs || bs->dead || !canRepairTarget(tpl, *bs)) u.nanoTargetId = -1;
        else if (bs->operational() && bs->hp >= bs->maxHp) u.nanoTargetId = -1;
        else {
            // [core] nanoRangeForRepair overrides the build range for repairs (:1760-1763)
            float range = tpl.nanoRangeForRepair > 0 ? tpl.nanoRangeForRepair : tpl.nanoRange;
            float d = u.pos.dist(bs->pos);
            if (d > range)
                computePath(*this, u, bs->pos + (bs->tpl->isBuilding ? Vec2{ 0, -TILE_SIZE * 2 } : Vec2{ 0, 0 }));
            else {
                if (bs->buildProgress < 1.0f) {
                    // RWX: nanoBuildSpeed multiplies with the target's buildSpeed (per-tick rate)
                    float target = bs->tpl->buildSpeed > 0 ? bs->tpl->buildSpeed : 0.001f;
                    float rate = (tpl.nanoBuildSpeed > 0 ? tpl.nanoBuildSpeed : 1.0f) * target;
                    bs->buildProgress = std::min(1.0f, bs->buildProgress + rate);
                    if (bs->buildProgress >= 1.0f) bs->underConstruction = false;
                } else {
                    // OrderableUnit.java:2352 - a flat nanoRepairSpeed per tick, no buildSpeed factor,
                    // and it writes currentHealth only: a drained shield bubble stays drained.
                    bs->hp = std::min(bs->maxHp, bs->hp + tpl.nanoRepairSpeed);
                }
                // both branches emit the same nano stream: OrderableUnit.java:5164-5185 counts down 5
                // ticks between particles
                if (tick % 5 == 0 && particles.size() < 100) {
                    Vec2 tp = bs->pos + Vec2{ g_rng.range(-8, 8), g_rng.range(-8, 8) };
                    Vec2 dd = tp - u.pos;
                    float spd = g_rng.range(2, 4);
                    Particle p;
                    p.pos = u.pos;
                    p.vel = dd * (spd / std::max(dd.len(), 0.001f));
                    p.dur = std::min(60.0f, dd.len() / spd);
                    particles.push_back(p);
                }
            }
        }
    }

    // RWX: idle builders auto-acquire nearby unfinished friendly buildings
    if (tpl.isBuilder && u.nanoTargetId < 0 && u.path.empty() && !u.hasGoal && u.orderTargetId < 0 && tick % 20 == 0) {
        Unit* best = nullptr; float bd = 260.0f;
        for (auto& o : units) {
            if (o.team != u.team || o.dead || !o.tpl || !o.tpl->isBuilding) continue;
            if (!o.underConstruction || o.buildProgress >= 1.0f) continue;
            if (!canRepairTarget(tpl, o)) continue;
            float d = u.pos.dist(o.pos);
            if (d < bd) { bd = d; best = &o; }
        }
        if (best) u.nanoTargetId = best->id;
    }

    // self upgrade action progress (RWX [action_upgrade*]: converts the unit in place)
    if (u.upgradeIdx >= 0) {
        if (u.upgradeIdx >= (int)tpl.actions.size()) u.upgradeIdx = -1;
        else {
            const UnitAction& a = tpl.actions[u.upgradeIdx];
            u.upgradeProgress += a.buildSpeed;
            if (u.upgradeProgress >= 1.0f) {
                const UnitTemplate* nt = g_templates.find(ToLower(a.convertTo));
                if (nt) {
                    float frac = u.maxHp > 0 ? u.hp / u.maxHp : 1.0f;
                    bool wasStatic = u.isStatic();
                    if (wasStatic) markBuildingOcc(map, u, -1);
                    u.tpl = nt;
                    u.tplKey = ToLower(nt->name);
                    u.maxHp = nt->maxHp;
                    u.hp = nt->maxHp * frac;
                    // the same proportionality RWX updateStats uses for the bubble (:1079-1083): a unit
                    // that had no shield gains a full one, one that had it keeps the same fill ratio.
                    float oldShieldMax = u.shieldMax;
                    u.shieldMax = (float)nt->maxShield;
                    if (nt->startShieldAtZero) u.shield = std::min(u.shield, u.shieldMax);
                    else if (oldShieldMax <= 0.0f) u.shield = u.shieldMax;
                    else u.shield = (u.shield / oldShieldMax) * u.shieldMax;
                    u.footprint[0] = nt->footL; u.footprint[1] = nt->footU; u.footprint[2] = nt->footR; u.footprint[3] = nt->footD;
                    if (nt->isStatic()) markBuildingOcc(map, u, +1);
                    explode(u.pos, false);
                }
                u.upgradeIdx = -1;
                u.upgradeProgress = 0;
            }
        }
    }

    int combatTarget = -1;
    if (tpl.canAttack && u.operational()) {
        Unit* ot = u.orderTargetId >= 0 ? get(u.orderTargetId) : nullptr;
        if (ot && !ot->dead && ot->operational()) combatTarget = ot->id;
        else {
            u.orderTargetId = -1;
            std::vector<Unit*> foes = queryNearestEnemy(u.team, u.pos, tpl.attackRange, &tpl);
            Unit* best = nullptr; float bd = 1e18f;
            for (auto* f : foes) { float d = u.pos.dist(f->pos); if (d < bd) { bd = d; best = f; } }
            if (best) combatTarget = best->id;
        }
    }
    Unit* tgt = combatTarget >= 0 ? get(combatTarget) : nullptr;

    if (!u.isStatic()) {
        if (u.repathTimer > 0) u.repathTimer--;
        // RWX air and hover units steer by velocity (moveIgnoringBody), so the hull never gates them
        bool ignoreBody = tpl.moveType == MoveType::AIR || tpl.moveType == MoveType::HOVER;
        bool pathing = u.pathIdx < (int)u.path.size();
        bool lastNode = pathing && u.pathIdx + 1 >= (int)u.path.size();
        int nodesLeft = pathing ? (int)u.path.size() - u.pathIdx : 0;
        Vec2 node = pathing ? u.path[u.pathIdx] : u.pos;
        float dist = pathing ? node.dist(u.pos) : 0.0f;
        float distSq = dist * dist;
        float bearing = pathing ? std::atan2(node.y - u.pos.y, node.x - u.pos.x) : u.facing;
        // OrderableUnit.java:2880-2884 vs 3175-3192: while pathing the hull follows the path node;
        // a fixed-firing unit faces its target only once it has stopped (or it ignores move orders).
        bool aimWithBody = tpl.isFixedFiring && tgt && (!pathing || ignoreBody);
        if (aimWithBody) bearing = Vec2::angleTo(u.pos, tgt->pos);
        float heading = bearing;
        // [movement] reverseSpeedPercentage decides whether a >100 deg swing becomes a flip-turn
        float revPct = tpl.reverseSpeed;
        bool flipOk = !ignoreBody && pathing && !aimWithBody &&
            (revPct > 0.95f || (revPct > 0.87f && nodesLeft <= 9 && distSq < 250000.0f)
             || (revPct > 0.7f && nodesLeft <= 4 && distSq < 40000.0f)
             || (revPct > 0.4f && nodesLeft <= 2 && distSq < 10000.0f));
        if (flipOk) {
            float errDeg = std::fabs(angleDiff(u.facing, bearing)) * 57.29578f;
            if (errDeg > 100.0f) { u.isRotating = true; heading = bearing + 3.14159265f; }
            else u.isRotating = false;
        } else {
            u.isRotating = false;
        }
        u.targetFacing = heading;
        u.hasFacingTarget = true;

        // rotateToward + addRotation: the hull keeps turning to its heading after the unit stops,
        // and the turret rides along with it.
        if (u.hasFacingTarget) {
            float diff = angleDiff(u.facing, u.targetFacing);
            float maxV = tpl.maxTurnSpeed * 0.0174533f;
            if (std::fabs(diff) < 1e-4f) {
                u.facing += diff; u.turnVel = 0; u.hasFacingTarget = false;
            } else {
                float accel = tpl.turnAccel * 0.0174533f;
                float wantV = diff > 0 ? maxV : -maxV;
                if (std::fabs(wantV - u.turnVel) <= accel) u.turnVel = wantV;
                else u.turnVel += (wantV > u.turnVel ? accel : -accel);
                float step = u.turnVel;
                if (std::fabs(step) > std::fabs(diff)) step = diff;
                u.facing = wrapAngle(u.facing + step);
                // RWX addRotation:1325-1337 - with [attack] turretRotateWithBody every barrel is
                // dragged around by the hull; without it the barrels keep their own world aim.
                if (tpl.turretRotateWithBody)
                    for (auto& ts : u.tur) ts.angle = wrapAngle(ts.angle + step);
            }
        }
        float aimErrDeg = std::fabs(angleDiff(u.facing, heading)) * 57.29578f;

        // RWX throttle: full speed only when the hull is aimed within the distance-scaled
        // tolerance (:2893-2916), otherwise the unit stops to turn, and it brakes on the way in.
        float tol = distSq > 3600.0f ? 89.0f : (distSq > 361.0f ? 46.0f : 20.0f);
        if (tpl.maxTurnSpeed < 1.4f) tol = distSq > 6400.0f ? tol * 0.5f : 17.0f;
        if (tpl.maxTurnSpeed < 1.1f) tol *= 0.7f;
        if (u.throttle > 0.4f && distSq > 16900.0f) tol = 180.0f;
        if (ignoreBody) tol = 181.0f;
        float want = 0;
        bool parked = lastNode && distSq < 16.0f;
        if (pathing && !parked && aimErrDeg <= tol) {
            want = 1.0f;
            if (lastNode) {
                if (distSq < 324.0f && tpl.decel < 0.13f && tpl.moveSpeed > 1.0f) want *= 0.5f;
                if (distSq < 169.0f && tpl.decel < 0.15f && tpl.moveSpeed > 0.9f) want *= 0.5f;
            }
            if (distSq < 9.0f) want = 0;
        }
        // :3003-3006 while flipping the unit backs up at reverseSpeedPercentage
        if (u.isRotating && !ignoreBody) want = -want * revPct;
        float rate = want > u.throttle ? tpl.accel : tpl.decel;
        if (std::fabs(want - u.throttle) <= rate) u.throttle = want;
        else u.throttle += (want > u.throttle ? rate : -rate);

        // position: land and water units drive along the hull, never sideways
        if (std::fabs(u.throttle) > 1e-4f) {
            float a = ignoreBody ? bearing : u.facing;
            Vec2 step{ std::cos(a), std::sin(a) };
            Vec2 np = u.pos + step * (tpl.moveSpeed * u.throttle);
            bool trapped = !passableAt(*this, u.pos, tpl.moveType);
            if (trapped || passableAt(*this, np, tpl.moveType)) u.pos = np;
            else {
                Vec2 nxp{ np.x, u.pos.y }, nyp{ u.pos.x, np.y };
                if (passableAt(*this, nxp, tpl.moveType)) u.pos = nxp;
                else if (passableAt(*this, nyp, tpl.moveType)) u.pos = nyp;
                else u.throttle *= 0.5f;
            }
        }
        if (pathing) {
            float arrive = lastNode ? 6.0f : 16.0f; // RW: distSq<256 mid, <16 final, cmd radius 7
            if (dist <= std::max(arrive, tpl.moveSpeed)) {
                u.pathIdx++;
                if (u.pathIdx >= (int)u.path.size()) { u.path.clear(); u.pathIdx = 0; u.hasGoal = false; }
            } else if (!lastNode) {
                // early switch when the next waypoint is already nearer (RW :2832-2839)
                Vec2 nx = u.path[u.pathIdx + 1];
                if (nx.dist(u.pos) < dist - 1.0f) u.pathIdx++;
            }
            // stuck recovery (RW blockedRecoveryTime / pathRetryTimer)
            if (u.pos.dist(u.prevPos) < 0.35f) u.stuckTimer++; else u.stuckTimer = 0;
            u.prevPos = u.pos;
            if (u.stuckTimer > 200 && dist < 60.0f) { u.stuckTimer = 0; if (u.pathIdx < (int)u.path.size() && !lastNode) u.pathIdx++; }
            else if (u.stuckTimer > 90 && u.repathTimer <= 0) { u.stuckTimer = 0; computePath(*this, u, u.hasGoal ? u.moveGoal : node); }
        }
    }
    if (!u.isStatic() && u.pathIdx >= (int)u.path.size() && u.hasGoal && u.repathTimer <= 0 && u.pos.dist(u.moveGoal) > 8.0f) {
        computePath(*this, u, u.moveGoal); // retry after blockage cleared
    }

    // OrderableUnit.update:865-890 - every barrel keeps its own cooldown, warmup and aim, so the
    // whole turret pass now runs per [turret_N] instead of on one shared barrel.
    if (u.operational() && !tpl.turrets.empty()) updateTurrets(u, tgt);

    if (u.attackMove && u.path.empty() && combatTarget < 0) u.attackMove = false;
    if (u.orderTargetId >= 0 && tgt && u.path.empty() && !u.isStatic() && u.repathTimer <= 0 && u.pos.dist(tgt->pos) > tpl.attackRange * 0.9f) {
        float stop = std::max(20.0f, tpl.attackRange * 0.8f - tgt->tpl->radius);
        Vec2 away = (u.pos - tgt->pos).norm();
        computePath(*this, u, tgt->pos + away * stop);
    }
}

// RWX turret pivot: [turret_N] x/y are sprite-space offsets (sprite faces up, y down);
// the pivot must rotate with the body so the turret stays glued to the hull.
Vec2 turretPivot(const Unit& u, const TurretTpl& tur, float k) {
    if (u.isStatic()) return u.pos + Vec2{ tur.x * k, tur.y * k };
    float cs = std::cos(u.facing), sn = std::sin(u.facing);
    return u.pos + Vec2{ -tur.x * sn - tur.y * cs, tur.x * cs - tur.y * sn } * k;
}

// RWX OrderableUnit.updateAttackTargeting(:3120) + advanceMovementLevelTowardTarget(:3239): each
// barrel holds its own target, aims with its own turn speed, charges its own warmup and counts its
// own delay down, and an attachedTo barrel simply rides along with the turret it is bolted to.
static constexpr int kMaxTur = 32;   // RWX sizes the per-turret arrays at 31 (MultiTurretPassiveTargetCallback.java:14-24)

void Game::updateTurrets(Unit& u, Unit* unitTarget) {
    const UnitTemplate& tpl = *u.tpl;
    if (tpl.turrets.empty()) return;
    initTurrets(u);            // an upgrade can swap the template and with it the barrel count
    // OrderableUnit.supportsIndependentTurretTargets:3067 - a single barrel just takes the unit's
    // target; with more than one, each picks its own and can shoot something else entirely.
    const bool independent = tpl.turrets.size() > 1;
    const int n = std::min((int)tpl.turrets.size(), kMaxTur);
    float k = texScale(tpl);
    float turn[kMaxTur] = {};    // each barrel's own turn this tick, applied to the barrels bolted to it
    Unit* target[kMaxTur] = {};
    bool anyReady = false;
    if (u.turretScanTimer > 0) u.turretScanTimer -= 1;        // :3138 moveTowardsZero(turretRotation, f)

    // --- pass 1: timers and aiming (:3151-3202). A linked barrel is not aimed on its own, and the
    // fire step is deliberately a second loop (:3203-3214) so every parent is settled first.
    for (int i = 0; i < n; ++i) {
        const TurretTpl& tur = tpl.turrets[i];
        TurretState& s = u.tur[i];
        // rotation counts down both ways, so the -10 "out of range" penalty clears on its own (:868)
        s.cooldown = moveTowards(s.cooldown, 0.0f, 1.0f);
        if (s.flameT > 0) s.flameT -= 1.0f;
        s.active = false;                                    // :3151-3153, re-decided by the aim below
        if (tur.attached >= 0) continue;                     // follows its parent in pass 2

        // RWX getTargetSearchRange() is 0 for a unit that cannot attack, so a builder's or
        // harvester's barrels never acquire a target and just rest along the hull.
        Unit* t = nullptr;
        if (tpl.canAttack) {
            if (!independent) {
                if (unitTarget && turretSees(*this, u, i, *unitTarget)) t = unitTarget;
            } else {
                Unit* own = get(s.targetId);
                if (own && turretSees(*this, u, i, *own)) t = own;
                else if (unitTarget && turretSees(*this, u, i, *unitTarget)) t = unitTarget;
                // MultiTurretPassiveTargetCallback:71, run at most once per sweep interval (:3140-3142)
                else if (u.turretScanTimer == 0) t = nearestInTurretRange(u, i);
            }
        }
        target[i] = t;
        s.targetId = t ? t->id : -1;

        float before = s.angle, residual;
        if (tpl.isFixedFiring) {
            residual = rotateTurret(s, tur, u.facing);       // OrderableUnit.a:3185-3197
        } else if (t) {
            Vec2 pv = turretPivot(u, i, k);
            residual = rotateTurret(s, tur, std::atan2(t->pos.y - pv.y, t->pos.x - pv.x));
        } else if (tur.shouldResetTurret) {
            // no target at all: walk back to C(i) = hull angle + idleDir (:846-857)
            float rest = u.facing + tur.idleDir * 0.0174533f;
            if (u.isRotating && tpl.reverseSpeed > 0.95f) rest += 3.14159265f;
            residual = rotateTurret(s, tur, rest);
        } else {
            residual = 0.0f;                                 // shouldResetTurret:false parks it here
        }
        turn[i] = angleDiff(before, s.angle);
        if (std::fabs(residual) * 57.29578f < tur.maxAngle) s.active = true;   // canAttackMaxAngle
    }
    // :3141 - after a sweep the unit arms the next one, staggered by its position so a whole army
    // does not re-query on the same tick.
    if (tpl.canAttack && independent && !unitTarget && u.turretScanTimer == 0)
        u.turretScanTimer = 20 + (int)std::fmod(u.pos.x, 5.0f) + (int)std::fmod(u.pos.y, 5.0f);

    // --- pass 2: warmup and firing (advanceMovementLevelTowardTarget:3239-3282)
    for (int i = 0; i < n; ++i) {
        const TurretTpl& tur = tpl.turrets[i];
        TurretState& s = u.tur[i];
        Unit* t = target[i];
        int pvt = tur.attached;
        if (pvt >= 0 && pvt < n) {
            // RWX CustomUnit.a(i, f):1341-1350 - the parent's turn this tick is added to the child,
            // so a barrel bolted to another aims and recoils with it instead of lagging behind.
            s.angle = wrapAngle(s.angle + turn[pvt]);
            s.angVel = u.tur[pvt].angVel;
            s.active = u.tur[pvt].active;
            t = target[pvt];
            s.targetId = t ? t->id : -1;
        }
        if (!s.active || !t || s.cooldown != 0.0f || !tur.canShoot) continue;
        anyReady = true;
        bool ready;
        if (tur.warmNoReset) {
            s.warm = std::min(s.warm + 1.0f, tur.warmup);
            ready = true;
        } else if (s.warm < tur.warmup) {
            s.warm += 1.0f;
            ready = false;
        } else {
            ready = true;
        }
        if (!ready) continue;
        s.cooldown = tur.shootDelay;
        if (!tur.warmNoReset) s.warm = 0;
        fire(u, *t, i);
        s.flip = !s.flip;
        // linkDelayWithTurret barrels never run their own timer; the shot that leads them restarts
        // theirs, but only once they are at least half a delay behind (:1323-1328).
        for (int j = 0; j < n; ++j) {
            if (j == i || tpl.turrets[j].linkDelay != i) continue;
            if (u.tur[j].cooldown < 9000.0f - (tur.shootDelay * 0.5f - tpl.turrets[j].warmup))
                u.tur[j].cooldown = 0.0f;
        }
        // a barrel bolted to a decoration turret gives that turret its recoil kick (:1329-1333)
        if (pvt >= 0 && !tpl.turrets[pvt].canShoot && tpl.turrets[pvt].recoil != 0.0f)
            u.tur[pvt].cooldown = tpl.turrets[pvt].shootDelay;
    }
    // warmupCallDownRate: an interrupted charge leaks away while nothing is lined up to fire (:3227)
    if (!anyReady)
        for (int i = 0; i < n; ++i)
            u.tur[i].warm = moveTowards(u.tur[i].warm, 0.0f, tpl.turrets[i].warmCallDown);
}

Unit* Game::nearestInTurretRange(const Unit& u, int i) const {
    Unit* best = nullptr; float bd = 1e18f;
    for (auto& o : units) {
        if (o.dead || &o == &u || !turretSees(*this, u, i, o)) continue;
        float d = u.pos.dist(o.pos);
        if (d < bd) { bd = d; best = const_cast<Unit*>(&o); }
    }
    return best;
}

// RWX CustomUnit.a(i, false):2870 - an attachedTo turret is bolted to its parent's pivot and turns
// with it, so the sprite x/y offsets are applied in the parent's frame instead of the hull's.
Vec2 turretPivot(const Unit& u, int i, float k) {
    const UnitTemplate& tpl = *u.tpl;
    if (i < 0 || i >= (int)tpl.turrets.size()) return u.pos;
    const TurretTpl& tur = tpl.turrets[i];
    if (tur.attached < 0 || tur.attached == i) return turretPivot(u, tur, k);
    Vec2 base = turretPivot(u, tur.attached, k);
    // turretPivot() reads the hull heading; rotate the offset into the parent barrel's frame instead
    float pa = (tur.attached < (int)u.tur.size()) ? u.tur[tur.attached].angle : 0.0f;
    float cs = std::cos(pa - u.facing), sn = std::sin(pa - u.facing);
    return base + Vec2{ cs * tur.y + sn * tur.x, -sn * tur.y + cs * tur.x } * k;
}

// RWX CustomUnit.a(i, false):2891-2902 - the barrel slides along its own aim by recoilOffset over
// recoilOutTime ticks and back over recoilReturnTime, timed from the shot that started it.
float turretRecoil(const Unit& u, int i) {
    if (i < 0 || i >= (int)u.tur.size() || !u.tpl) return 0.0f;
    const TurretTpl& tur = u.tpl->turrets[i];
    const TurretState& s = u.tur[i];
    if (tur.recoil == 0.0f || s.cooldown <= 0.0f) return 0.0f;
    float since = tur.shootDelay - s.cooldown;
    if (since < 0.0f) return 0.0f;
    if (tur.recoilOut <= 0.0f || since < tur.recoilOut)
        return tur.recoilOut > 0.0f ? (since / tur.recoilOut) * tur.recoil : tur.recoil;
    float back = since - tur.recoilOut;
    if (tur.recoilReturn <= 0.0f) return 0.0f;
    if (back < tur.recoilReturn) return tur.recoil - (back / tur.recoilReturn) * tur.recoil;
    return 0.0f;
}

void Game::fire(Unit& u, Unit& target, int ti) {
    const UnitTemplate& tpl = *u.tpl;
    if (ti < 0 || ti >= (int)tpl.turrets.size()) return;
    const TurretTpl& tur = tpl.turrets[ti];
    TurretState& s = u.tur[ti];
    int pi = (tur.projectile >= 0 && tur.projectile < (int)tpl.projectiles.size()) ? tur.projectile : -1;
    ProjectileTpl pt = pi >= 0 ? tpl.projectiles[pi] : ProjectileTpl{};
    if (pi < 0) pt.directDamage = 10;   // RWX's stand-in [projectile_1] for a unit with none at all
    Projectile p;
    Vec2 aim{ std::cos(s.angle), std::sin(s.angle) };
    Vec2 right{ -aim.y, aim.x };
    // RWX CustomUnit.D(i): muzzle = pivot + aim*barrelY + right*barrelX; barrelY defaults to [attack] turretSize.
    // Offsets are sprite px; scale with the drawn body (renderer uses k = displayRadius*2*imageScale / frameW).
    float barrelY = tur.barrelY >= 0 ? tur.barrelY : (tpl.turretSize > 0 ? tpl.turretSize : tpl.radius * 0.8f);
    float k = texScale(tpl);
    float bx = tur.barrelX;
    if (tur.barrelXOdd != 0.0f && s.flip) bx += tur.barrelXOdd;    // barrelOffsetX_onOddShots
    p.pos = turretPivot(u, ti, k) + (aim * barrelY + right * bx) * k;
    p.team = u.team; p.sourceId = u.id; p.targetId = target.id; p.dest = target.pos;
    p.damage = pt.directDamage; p.areaDamage = pt.areaDamage; p.areaRadius = pt.areaRadius;
    p.speed = pt.speed; p.life = pt.life;
    p.frame = pt.frame; p.drawType = pt.drawType; p.shadowFrame = pt.shadowFrame;
    p.drawSize = pt.drawSize; p.color = pt.color;
    p.lightColor = pt.lightColor; p.lightSize = pt.lightSize;
    p.largeHit = pt.largeHit;
    // RWX Projectile.az is set from the turret's aim line, not from the target's position
    // (CustomUnit.java:1371), and the shot then flies along that heading.
    p.heading = s.angle;
    p.turnSpeed = pt.turnSpeed;
    p.speedTarget = pt.targetSpeed; p.speedAccel = pt.targetSpeedAccel;
    if (pt.speedSpread > 0) p.speed += g_rng.range(-pt.speedSpread, pt.speedSpread);
    p.wobbleAmp = pt.wobbleAmp; p.wobbleFreq = pt.wobbleFreq;
    p.wobblePhase = g_rng.range(0.0f, 1.0f);   // RWX Projectile.I
    p.buildingMult = pt.buildingDamageMult;
    p.shieldMult = pt.shieldDamageMult;
    p.deflectMult = pt.shieldDeflectMult;
    p.hullMult = pt.hullDamageMult;
    p.armourIgnore = pt.armourIgnore;
    p.expandTotal = pt.areaExpandTime > 0 ? pt.areaExpandTime : 0.0f;
    p.vel = Vec2{ std::cos(p.heading), std::sin(p.heading) } * p.speed;
    if (tur.shootFlame) {
        // RWX EffectManager.createFlameEffect(2): small plays frames 0..3 at 0.5/tick (8 ticks),
        // large/medium frames 1..4 at 0.4/tick (10 ticks); the flame is attached to the shooter.
        s.flameType = tur.shootFlame;
        s.flameT = tur.shootFlame == 1 ? 8.0f : 10.0f;
        s.flameAng = s.angle;
        s.flameOff = p.pos - u.pos;
    }
    if (pt.instant) {
        // RWX update:841,979: an instant shot never moves, it lands on the firing tick.
        impact(p, &target, target.pos);
        if (p.expandTotal <= 0) return;   // nothing is left to animate, so no projectile is spawned
    }
    projectiles.push_back(p);
}

// RWX Projectile.update: the shot owns a heading and moves along it, so a shot with turnSpeed >= 0
// only curves at that rate and can miss; turnSpeed < 0 (the default) re-aims every tick. The step is
// clamped to the target distance, which also zeroes the gap so the following hit test lands it
// (update:857-866,975).
void Game::updateProjectile(Projectile& p) {
    if (p.dead) return;
    if (p.hit) {
        // The shot has landed: only the blast ring keeps growing, and it is no longer drawn.
        p.expandT -= 1.0f;
        splash(p, 1.0f - p.expandT / p.expandTotal);
        if (p.expandT <= 0) p.dead = true;
        return;
    }
    Unit* t = p.targetId >= 0 ? get(p.targetId) : nullptr;
    if (t && t->dead) t = nullptr;
    // RWX keeps chasing the corpse, so a shot whose target dies mid-flight still lands where it died.
    if (t) p.dest = t->pos;
    Vec2 aim = t ? t->pos : p.dest;
    float dist = p.pos.dist(aim);
    float want = std::atan2(aim.y - p.pos.y, aim.x - p.pos.x);
    p.heading = p.turnSpeed >= 0 ? lerpAngle(p.heading, want, p.turnSpeed * 0.0174533f) : want;
    if (p.speedTarget > 0) p.speed = moveTowards(p.speed, p.speedTarget, p.speedAccel);
    float step = p.speed;
    if (dist < step) { step = dist; dist = 0; }
    Vec2 dir{ std::cos(p.heading), std::sin(p.heading) };
    p.pos = p.pos + dir * step;
    if (p.wobbleAmp != 0) {
        float off = std::sin((p.ticks * 360.0f / p.wobbleFreq + p.wobblePhase * 360.0f) * 0.0174533f) * p.wobbleAmp;
        p.pos = p.pos + Vec2{ -dir.y, dir.x } * off;
    }
    p.vel = dir * p.speed;
    p.ticks++;
    // RWX lands within a flat 6px of a unit, or within 0.8 of a building's radius (update:961-967)
    float hitR = (t && t->tpl && t->tpl->isBuilding) ? std::max(6.0f, t->tpl->radius * 0.8f) : 6.0f;
    if (dist < hitR) {
        impact(p, t, p.pos);
        if (p.expandTotal <= 0) p.dead = true;
        return;
    }
    // RWX update:1411: a shot that runs out of life just vanishes; only explodeOnEndOfLife makes it
    // detonate there, which no shipped weapon sets.
    if (--p.life <= 0) p.dead = true;
}

// RWX update:1021-1140: the blast is centred on the impact point, the direct damage goes to the
// chased unit, and the splash either fills its radius at once or grows over areaExpandTime.
void Game::impact(Projectile& p, Unit* direct, Vec2 at) {
    p.hit = true;
    p.impactPos = at;
    p.airBurst = direct && direct->tpl && direct->tpl->moveType == MoveType::AIR;
    // largeHitEffect reads backwards but is literal in RWX: it means "this weapon brings its own
    // hit flash", which swaps the generic seven-flame blast for a small one, plus one flame scaled
    // by the blast radius when that radius exceeds 10px (update:1168-1185).
    if (p.largeHit) {
        if (p.areaRadius > 10) explode(at, false, p.areaRadius / 25.0f);
        explode(at, false);
    } else {
        explode(at, true);
    }
    if (direct && p.damage > 0) damage(*direct, p.damage * damageScale(p, *direct), &p);
    if (p.expandTotal > 0) p.expandT = p.expandTotal;
    else splash(p, 1.0f);
}

// RWX Projectile.b(f2):1443-1526: units inside the ring take areaDamage * (1 - dist/areaRadius + 0.1)
// capped at 1; the burst stays in the domain it detonated in, so a ground burst does not splash
// aircraft, and a growing ring catches each unit only once.
void Game::splash(Projectile& p, float frac) {
    if (p.areaDamage <= 0 || p.areaRadius <= 0) return;
    float ring = p.areaRadius * std::min(1.0f, frac);
    for (auto& u : units) {
        if (!u.tpl || u.dead || !isEnemy(p.team, u.team)) continue;
        if ((u.tpl->moveType == MoveType::AIR) != p.airBurst) continue;
        float d = u.pos.dist(p.impactPos);
        if (d > ring) continue;
        if (p.expandTotal > 0) {
            bool seen = false;
            for (int id : p.splashHits) if (id == u.id) { seen = true; break; }
            if (seen) continue;
            p.splashHits.push_back(u.id);
        }
        damage(u, p.areaDamage * std::min(1.0f, 1.0f - d / p.areaRadius + 0.1f) * damageScale(p, u), &p);
    }
}

// The whole RWX damage pipeline, which RP used to do in one subtraction:
// CustomUnit.setTarget:4187-4199 takes a flat armour off the hit (never below
// armourMinDamageToKeep, and armourIgnoreAmount buys that reduction back), then
// BaseUnit.setTarget:1559-1601 lets the bubble drink shieldMult * damage first - whatever runs past
// an empty bubble is cut by deflectMult, so a weapon with deflectMult 1 never touches the hull at all
// - and only the remainder, scaled by hullMult, is subtracted from health.
void Game::damage(Unit& target, float dmg, const Projectile* p) {
    if (dmg <= 0 || target.dead) return;
    const Projectile def;
    if (!p) p = &def;
    const UnitTemplate& tpl = *target.tpl;
    // a building still being built (or repaired back from rubble) takes 75% more (:1561)
    if (target.underConstruction && target.buildProgress < 1.0f) dmg *= 1.75f;
    if (tpl.armour > 0.0f && dmg > tpl.armourMinKeep) {
        float a = tpl.armour - p->armourIgnore;
        if (a > 0.0f) dmg -= a;
        if (dmg < tpl.armourMinKeep) dmg = tpl.armourMinKeep;
    }
    float toHull = dmg;
    // :1574 also asks for unit energy to be spent, and RWX never charges that field up for a shield
    if (target.shield > 0.0f) {
        float cost = dmg * p->shieldMult;
        if (target.shield < cost) {
            toHull = dmg - target.shield * p->deflectMult;
            target.shieldFlash += target.shield;
            target.shield = 0.0f;
        } else {
            target.shield -= cost;
            target.shieldFlash += cost;
            toHull = dmg - dmg * p->deflectMult;
        }
    }
    float hp = toHull * p->hullMult;
    if (hp <= 0.0f) return;
    target.hp -= hp;
    if (target.hp <= 0) killUnit(target);
}

// RWX EffectManager.createExplosionWithVelocity: a big explosion is 7 flames jittered +-20 px,
// each starting 0..20 ticks later and running at 0.3..0.6 frames/tick.
void Game::explode(Vec2 at, bool big, float scale) {
    for (int i = 0, n = big ? 7 : 1; i < n; ++i) {
        Effect e;
        if (big) e.pos = at + Vec2{ g_rng.range(-20.0f, 20.0f), g_rng.range(-20.0f, 20.0f) };
        else e.pos = at;
        e.rot = g_rng.range(-3.14159265f, 3.14159265f);
        e.scale = g_rng.range(0.8f, 1.0f) * scale;
        e.frameSpeed = big ? g_rng.range(0.3f, 0.6f) : 0.5f;
        e.delay = big ? g_rng.range(0.0f, 20.0f) : 0.0f;
        effects.push_back(e);
    }
}

// RWX ScorchMark.b(): a new mark is dropped when one already sits within 5px of it, or when three
// are within 25px. The variant picks one of the four 50x40 cells of blood_mark.png.
void Game::addScorch(Vec2 at) {
    int near25 = 0;
    for (auto& s : scorches) {
        if (std::abs(s.pos.x - at.x) >= 25.0f || std::abs(s.pos.y - at.y) >= 25.0f) continue;
        if (std::abs(s.pos.x - at.x) < 5.0f && std::abs(s.pos.y - at.y) < 5.0f) return;
        if (++near25 >= 3) return;
    }
    Scorch s; s.pos = at; s.variant = (int)g_rng.range(0.0f, 3.999f);
    scorches.push_back(s);
}

// RWX BaseUnit.removeFromGame: leave play and drop every order that pointed here. Used by death and by
// [core] dieOnConstruct, whose builder vanishes without any death effect (CustomUnit.m():3668-3679).
void Game::removeUnit(Unit& u) {
    if (u.dead) return;
    u.dead = true;
    if (u.tpl && u.tpl->isStatic()) markBuildingOcc(map, u, -1);
    for (auto& o : units) { if (o.orderTargetId == u.id) o.orderTargetId = -1; if (o.nanoTargetId == u.id) o.nanoTargetId = -1; }
    for (auto& p : projectiles) if (p.targetId == u.id) p.targetId = -1;
}

void Game::killUnit(Unit& u) {
    if (u.dead) return;
    bool big = u.tpl && u.tpl->radius > 20;
    removeUnit(u);
    explode(u.pos, big);
    // RWX CustomUnit.java:891-893: an exploding unit leaves a permanent mark on the ground.
    if (u.tpl && u.tpl->explodeOnDeath && !u.tpl->hideScorchMark && !overWater(*this, u.pos)) addScorch(u.pos);
    // RWX CustomUnit.java:946: with image_wreak the corpse stays behind instead of vanishing.
    if (u.tpl && !u.tpl->imageWreak.empty()) wrecks.push_back(Wreck{ u.pos, u.facing, u.tpl });
}

void Game::runAI() {
    for (auto& t : teams) {
        if (t.isPlayer || !t.alive) continue;
        int teamId = t.id;
        int extractors = 0, builders = 0;
        for (auto& u : units) if (u.team == teamId && !u.dead && u.tpl) {
            if (u.tpl->creditIncomeRate > 0 && u.operational()) extractors++;
            if (u.tpl->isBuilder) builders++;
        }
        for (auto& u : units) {
            if (u.team != teamId || !u.tpl || !u.tpl->isFactory || !u.operational() || u.dead) continue;
            if (u.prodQueue.empty())
                for (auto& bo : u.tpl->canBuild) {
                    const UnitTemplate* it = g_templates.find(ToLower(bo.unitName));
                    if (it && it->isBuilder && builders >= 2) continue;
                    // with no income yet, save up so the builder can afford an extractor (700)
                    if (it && !it->isBuilding && t.credits > it->price + 100 && (extractors > 0 || t.credits > it->price + 800)) { orderProduce(u.id, bo.unitName); break; }
                }
        }
        if (tick % 300 == 0)
            for (auto& u : units) {
                if (u.team != teamId || !u.tpl || !u.tpl->isBuilder || u.dead || u.nanoTargetId >= 0) continue;
                for (int y = 1; y < map.h - 1 && u.nanoTargetId < 0; ++y)
                    for (int x = 1; x < map.w - 1; ++x)
                        if (map.at(x, y).resource) {
                            Vec2 rp{ (x + 0.5f) * TILE_SIZE, (y + 0.5f) * TILE_SIZE };
                            if (u.pos.dist(rp) < 600 && orderBuild(u.id, "extractor", x, y)) { x = map.w; }
                        }
            }
        if (tick % 360 == 0) {
            std::vector<int> army;
            for (auto& u : units) if (u.team == teamId && u.tpl && !u.tpl->isBuilding && !u.tpl->isBuilder && u.tpl->canAttack && u.operational()) army.push_back(u.id);
            if (army.size() >= 3) orderAttackMove(army, nearestEnemyPos(teamId));
        }
    }
}

static bool teamHasUnits(Game& g, int teamId) { for (auto& u : g.units) if (u.team == teamId && !u.dead) return true; return false; }

void Game::checkWinLoss() {
    if (gameOver) return;
    bool playerAlive = teamHasUnits(*this, 0);
    bool anyEnemy = false; for (auto& t : teams) if (!t.isPlayer && teamHasUnits(*this, t.id)) anyEnemy = true;
    if (!playerAlive) { gameOver = true; gameOverText = "DEFEAT"; }
    else if (!anyEnemy) { gameOver = true; gameOverText = "VICTORY"; }
}
