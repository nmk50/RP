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

// RWX BaseUnit.isOverLiquid -> GameViewUtils.d:265-280: water OR lava tile (the pathfinding fallback
// collapses to the same water flag on shipped maps).
static bool overLiquid(const Game& g, Vec2 p) {
    int x = (int)(p.x / TILE_SIZE), y = (int)(p.y / TILE_SIZE);
    return g.map.inBounds(x, y) && (g.map.at(x, y).water || g.map.at(x, y).lava);
}

// Shared predicate-list evaluator for unload / idleSweep / autoTrigger conditions. An unknown atom was
// dropped at parse time, so it cannot fail here either.
static bool condTestsOk(const Game& g, const Unit& u, const UnloadCond& c) {
    if (c.never) return false;
    for (auto& pr : c.tests) {
        bool v = pr.first == CondPred::overLiquid ? overLiquid(g, u.pos)
               : pr.first == CondPred::overWater ? overWater(g, u.pos)
               : u.driven;
        if (pr.second ? v : !v) return false;
    }
    return true;
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

// RWX OrderableUnit.a(f, angle, i):1351 - the barrel turns at [turret] turnSpeed deg/tick, or ramps
// to it through turnSpeedAcceleration and brakes with turnSpeedDeceleration, which is what makes a
// heavy turret lag behind a fast target. RWX does that arithmetic in degrees, so the angular velocity
// is kept in deg/tick and only the applied step becomes radians. Returns the angle still to cover.
static float rotateTurret(TurretState& s, const TurretTpl& tur, float want, float scale = 1.0f) {
    float err = angleDiff(s.angle, want);
    if (err == 0.0f) return 0.0f;
    float errDeg = err * 57.29578f;
    float applied;
    if (tur.turnAccel <= 0.0f) {                            // w(i) <= 0: no inertia, snap at turnSpeed
        float cap = tur.turnSpeed * scale;
        applied = std::fabs(errDeg) > cap ? (errDeg > 0.0f ? cap : -cap) : errDeg;
        s.angVel = 0.0f;
    } else {
        float decel = tur.turnDecel > 0.0f ? tur.turnDecel : tur.turnAccel;
        float dir = errDeg > 0.0f ? 1.0f : -1.0f;
        // :1366-1369 within the distance needed to stop, and still moving that way: bleed speed off.
        // The idle sweep calls this with f = idleSweepSpeed (:4011-4013), which scales the rate the
        // velocity approaches its target and the step it applies, but not the target or the stop test.
        if (decel > 0.0f && std::fabs(errDeg) < std::fabs(s.angVel) / decel && (s.angVel > 0.0f) == (errDeg > 0.0f))
            s.angVel = moveTowards(s.angVel, dir * decel, decel * scale);
        else
            s.angVel = moveTowards(s.angVel, dir * tur.turnSpeed, tur.turnAccel * scale);
        applied = s.angVel * scale;
        if (std::fabs(applied) > std::fabs(errDeg)) { s.angVel = 0.0f; applied = errDeg; }
    }
    s.angle = wrapAngle(s.angle + applied * 0.0174533f);
    return err - applied * 0.0174533f;
}

// CustomUnit.b(i,f):3996-4029 -- the idle sweep, run in place of the rest-angle reset when
// idleSweepAngle != 0 and the condition passes. The barrel walks out to centre+angle, parks there
// (l = +inf), and once the delay elapses flips to centre-angle and back. centre is C(i) for a
// shouldResetTurret barrel, else the angle it last aimed at (targetY, :4005-4006). The rotate helper is
// handed idleSweepSpeed as its tick scale (:4011-4013), so the whole turn dynamics run that fraction as
// fast. Returns the angle still to cover, like rotateTurret.
static float idleSweep(TurretState& s, const TurretTpl& tur, const Unit& u, const UnitTemplate& tpl) {
    // :4003-4006 centre precedence: a spinning barrel sweeps about its own current angle (targetX),
    // then a shouldResetTurret:false one about its last aim (targetY), else C(i).
    float centre;
    if (tur.idleSpin != 0.0f) centre = s.angle;
    else if (!tur.shouldResetTurret) centre = s.lastAim;
    else {
        centre = u.facing + tur.idleDir * 0.0174533f;
        if (u.isRotating && tpl.reverseSpeed > 0.95f) centre += 3.14159265f;
    }
    s.sweepTimer += 1.0f;
    float residual = 0.0f;
    if (s.sweepOffset > 0.0f) {
        if (s.sweepOffset < INFINITY) {
            residual = rotateTurret(s, tur, centre + s.sweepOffset * 0.0174533f, tur.idleSweepSpeed);
            if (residual == 0.0f) s.sweepOffset = INFINITY;
        }
    } else if (s.sweepOffset > -INFINITY) {
        residual = rotateTurret(s, tur, centre + s.sweepOffset * 0.0174533f, tur.idleSweepSpeed);
        if (residual == 0.0f) s.sweepOffset = -INFINITY;
    }
    if (s.sweepTimer > tur.idleSweepDelay) {
        // :4020 k = -randomInt(0, addRandomDelay), a whole tick count in [0, n)
        s.sweepTimer = -(float)(int)g_rng.range(0.0f, tur.idleSweepAddRandomDelay);
        float clamp = tur.idleSweepAngle;
        if (tur.idleSweepAddRandomAngle > 0.0f) clamp += g_rng.range(0.0f, tur.idleSweepAddRandomAngle);
        s.sweepOffset = s.sweepOffset > 0.0f ? -clamp : clamp;
    }
    return residual;
}

// OrderableUnit.getEngagementRange:4482-4487 -- the hull's [attack] maxAttackRange, plus the two hull
// radii when the shooter is melee (useVelocityExtendedRange, CustomUnit.java:3612-3613). That sum is the
// only distance test a shot has to pass: isWithinEngagementRange:3289-3296 squares it against the centre
// distance and updateAttackTargeting:3154-3157 runs the whole barrel loop behind it, so a hull outside
// it aims at nothing and a melee chase-attacker instead walks on (:3215-3221). Adding the radii is what
// lets a 9px bite reach a hull that soft collision never lets overlap it.
float engagementRangeOf(const Unit& u, const Unit& t) {
    float r = u.tpl->attackRange;
    if (u.tpl->isMelee && t.tpl) r += u.tpl->radius + t.tpl->radius;
    return r;
}

// Can this barrel engage that unit at all: RWX checks the target's domain against the turret's own
// canAttack* flags and, when the section sets limitingRange / limitingMinRange, the distance
// (CustomUnit.a(TurretConfig, i, baseUnit, ..):1266, TurretConfig.java:484-501).
//
// BaseUnit.isTouchingWater:2082-2087 -- a water tile under the hull, and the hull no higher than 2. That
// is the whole of the test behind [attack]/[turret_N] canAttackNotTouchingWaterUnits:false: a submarine
// can shoot a surface ship (which stands in water) or a beached bomber (landOnGround parks it at exactly
// posZ 2), but not a tank on the shore.
static bool touchingWater(const Game& g, const Unit& u) {
    return overWater(g, u.pos) && u.posZ <= 2.0f;
}

// The world position a hull of this template takes when its anchor tile is (tx,ty). A building's position
// is the footprint's top-left tile corner plus [core] buildingToFootprintOffsetX/Y, which is exactly how
// BaseBuilding.canBuildingBePlacedAt:72-74 and the onNewMapSpawn placements (CustomUnitConfig:1300-1301)
// set it; everything else stands on the tile centre, the same 10 px the offset defaults to.
static Vec2 tileSpawnPos(const UnitTemplate* tpl, int tx, int ty) {
    if (tpl && tpl->isBuilding) return tpl->footprintPos(tx, ty);
    return { (tx + 0.5f) * TILE_SIZE, (ty + 0.5f) * TILE_SIZE };
}

// RWX CustomUnit.a(TurretConfig,...):1277-1285 -- the limitingAngle firing cone. The reference direction is
// the attached parent's aim (movementLevels[linkedTurretIndex].targetX, which RP stores as TurretState.angle)
// or the hull facing when unattached, plus idleDir; the target's bearing from the hull centre must lie within
// limitingAngle degrees of it. -1 (the default) disables the test entirely.
static bool limitingAngleOk(const Unit& u, int i, const Unit& t) {
    const TurretTpl& tur = u.tpl->turrets[i];
    if (tur.limitingAngle < 0) return true;
    float ref = (tur.attached >= 0 ? u.tur[tur.attached].angle : u.facing) + tur.idleDir * 0.0174533f;
    float bearing = std::atan2(t.pos.y - u.pos.y, t.pos.x - u.pos.x);
    return std::fabs(angleDiff(ref, bearing)) * 57.29578f <= tur.limitingAngle;
}

// UnitPrice.b(unit):285-305 -- canPay. Only a positive requirement gates; flags use the masks built at parse
// time (hasFlag needs every bit set, hasMissingFlag needs none of them). ammo lives in u.resources["ammo"].
static bool canPayResourceCost(const Game& g, const Unit& u, const ResourceCost& rc) {
    if (rc.credits > 0) { const Team* tm = nullptr; for (auto& t : g.teams) if (t.id == u.team) { tm = &t; break; } if (!tm || tm->credits < rc.credits) return false; }
    if (rc.energy > 0 && u.energy < rc.energy) return false;
    if (rc.hp > 0 && u.hp < rc.hp) return false;
    if (rc.shield > 0 && u.shield < rc.shield) return false;
    if (rc.ammo > 0) { auto it = u.resources.find("ammo"); if (it == u.resources.end() || it->second < rc.ammo) return false; }
    if (rc.hasFlag && (u.flags & rc.hasFlag) != rc.hasFlag) return false;
    if (rc.hasMissingFlag && (u.flags & rc.hasMissingFlag) != 0) return false;
    for (auto& kv : rc.custom) {
        if (kv.second <= 0) continue;
        auto it = u.resources.find(kv.first);
        if (it == u.resources.end() || it->second < kv.second) return false;
    }
    return true;
}

// UnitPrice.a(unit):378-389 then d():315-334 -- pay unconditionally (no precheck) and clamp the pools.
static void payResourceCost(Game& g, Unit& u, const ResourceCost& rc) {
    if (rc.credits != 0) g.reward(u.team, -rc.credits);
    u.energy -= rc.energy;
    u.hp -= rc.hp;
    u.shield -= rc.shield;
    if (rc.ammo != 0) u.resources["ammo"] -= rc.ammo;
    if (rc.unsetFlag) u.flags &= ~rc.unsetFlag;
    if (rc.setFlag) u.flags |= rc.setFlag;
    for (auto& kv : rc.custom) u.resources[kv.first] -= kv.second;
    if (u.energy < 0) u.energy = 0;
    if (u.shield < 0) u.shield = 0;
    if (u.shield > u.shieldMax) u.shield = u.shieldMax;
    if (u.hp > u.maxHp) u.hp = u.maxHp;
    auto ammo = u.resources.find("ammo");
    if (ammo != u.resources.end() && ammo->second < 0) ammo->second = 0;
}

static bool turretSees(const Game& g, const Unit& u, int i, const Unit& t) {
    if (!t.tpl || t.dead || t.riding() || !t.operational() || !g.isEnemy(u.team, t.team)) return false;
    const TurretTpl& tur = u.tpl->turrets[i];
    // RWX CustomUnit.canAttackUnitType-style domain test (:1296-1314) reads the target's *height*, not
    // its movement type: airborne means canAttackFlyingUnits, submerged means canAttackUnderwaterUnits,
    // and everything else -- surface ships included -- is the canAttackLandUnits domain.
    bool fly = t.airborne(), sea = t.submerged(), land = !fly && !sea;
    if (!((fly && tur.canAttackFlying) || (land && tur.canAttackLand) || (sea && tur.canAttackUnderwater))) return false;
    // :1308, and it comes before canAttackLandUnits but after the flying/underwater early returns, so a
    // barrel that refuses dry targets still snipes a plane over the same patch of ground.
    if (land && !tur.canAttackNotTouchingWater && !touchingWater(g, t)) return false;
    float d = u.pos.dist(t.pos);
    if (d > engagementRangeOf(u, t)) return false;
    if (tur.range > 0 && d > tur.range) return false;
    if (tur.minRange > 0 && d < tur.minRange) return false;
    if (!limitingAngleOk(u, i, t)) return false;
    return true;
}

// RWX gives each [turret_N] its own UnitMovementData when the unit is built (CustomUnit:1033-1044),
// aimed along the hull with every timer zeroed. UnitMovementData.a(f):37-38 sets targetY = targetX = f,
// so the last-aim angle the idle sweep centres on when shouldResetTurret:false starts at the spawn angle
// too, not at zero. An upgrade that adds barrels only grows the array and keeps the ones already there
// (BaseUnit:975-986).
static void initTurrets(Unit& u) {
    if (!u.tpl) return;
    size_t n = u.tpl->turrets.size(), old = u.tur.size();
    if (old == n) return;
    u.tur.resize(n);
    for (size_t i = old; i < n; ++i) { u.tur[i].angle = u.facing; u.tur[i].lastAim = u.facing; }
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
        Vec2 pos = tileSpawnPos(g_templates.find(ToLower(s.unit)), s.tx, s.ty);
        int id = spawnUnit(s.unit, s.team, pos);
        // RWX Tree.a_(String):"<atlas>.<subType>" from the tileset's `type` property
        if (id >= 0 && !s.type.empty() && ToLower(s.unit) == "tree") {
            Unit* u = get(id);
            if (u) {
                const char* dot = strchr(s.type.c_str(), '.');
                u->treeType = atoi(s.type.c_str());
                u->treeSub = dot ? atoi(dot + 1) : -1;
            }
        }
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

Projectile* Game::findProjectile(int id) {
    if (id < 0) return nullptr;
    for (auto& p : projectiles) if (p.id == id && !p.dead) return &p;
    return nullptr;
}

// RW: building tiles get path cost -1; mark/unmark footprint tiles
static void markBuildingOcc(TileMap& map, const Unit& u, int delta) {
    if (!u.tpl || !u.tpl->isStatic()) return;
    // The anchor tile is what placement added the footprint offset to, so the same offset comes back off
    // the position to recover it (BaseUnit.a(TileMap,Point):2355-2358). A turret that asks for offset 20
    // lands a whole tile further into its footprint than the default, and would block the wrong tiles if
    // this divided the bare position.
    int cx = (int)((u.pos.x - u.tpl->buildingToFootprintOffsetX) / TILE_SIZE);
    int cy = (int)((u.pos.y - u.tpl->buildingToFootprintOffsetY) / TILE_SIZE);
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
    // [movement] startingHeightOffset is added once, when the config is first applied
    // (CustomUnit.java:1063-1065), so an air unit without it lifts off from the ground.
    u.posZ = tpl->startingHeightOffset;
    // RWX CustomUnit.java:1074-1083: a shielded unit comes up with a full bubble, and only
    // [core] startShieldAtZero asks for it to fill up from empty instead.
    u.shieldMax = (float)tpl->maxShield;
    u.shield = tpl->startShieldAtZero ? 0.0f : u.shieldMax;
    // :1082-1089 the same rule for the energy pool: a fresh unit comes up at
    // maxEnergy * energyStartingPercentage, and startEnergyAtZero leaves it on the 0 it is born with.
    u.energy = tpl->startEnergyAtZero ? 0.0f : tpl->maxEnergy * tpl->energyStartingPercentage;
    u.footprint[0] = tpl->footL; u.footprint[1] = tpl->footU; u.footprint[2] = tpl->footR; u.footprint[3] = tpl->footD;
    u.animFrame = tpl->defaultFrame;      // CustomUnit.setUnitType:1060
    // RWX BaseBuilding.java:108: a building's body angle is -90 deg, so its turret idles along the
    // unrotated sprite (north) plus [turret] idleDir.
    if (tpl->isStatic()) {
        u.facing = -1.5708f;
        u.targetFacing = u.facing;   // a hull that never turns is always aimed where it points
    }
    initTurrets(u);
    initLegs(u);
    u.underConstruction = underConstruction;
    units.push_back(u);
    if (tpl->isStatic()) markBuildingOcc(map, units.back(), +1);
    if (attachmentSpawnDepth < 4 && !tpl->attachments.empty()) createAttachments(units.back());
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
        case MoveType::OVER_CLIFF_WATER: return g.map.walkableAmphibious(x, y);
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
        // A submarine handed a dry target refuses the order outright: this is the same hull-level
        // canAttackUnitType:3356 test the passive pass gets through queryNearestEnemy, and RWX's "cannot
        // attack that" path runs it before committing orderTargetId.
        if (!u->tpl->canAttackNotTouchingWater && !tgt->airborne() && !tgt->submerged() &&
            !touchingWater(*this, *tgt)) continue;
        u->orderTargetId = targetId; u->attackMove = false; u->nanoTargetId = -1;
        if (u->pos.dist(tgt->pos) > engagementRangeOf(*u, *tgt)) {
            // RWX engagement: stop at range instead of walking into the target -- but a melee hull is a
            // chase-attacker (updateAttackTargeting:3215-3220 sets pathTargetRadius 0 for one), so it
            // walks onto the target itself and lets the soft collision settle the overlap.
            float stop = u->tpl->isMelee ? 0.0f
                       : std::max(20.0f, u->tpl->attackRange * 0.8f - tgt->tpl->radius);
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
    // :2192-2201 cancels a repair the moment it lands on a [core] resourceRate target -- a crate or crystal
    // is meant to be reclaimed, not mended.
    if (t->tpl->resourceRate != 0.0f) return false;
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
        u->nanoReclaim = false; u->nanoOrderAge = 0;   // a fresh waypoint restarts RWX's wayPointTimer
        u->path.clear(); u->pathIdx = 0; u->hasGoal = false;
    }
    return true;
}

// GameUI.handleUnitTargetClick:2700-2726 / issueReclaimCommand:3272-3280: the reclaim verb needs nothing
// but [core] canReclaimResources on the hull that is ordered. It is not a builder command -- the bug mod's
// robotCrab reclaims without being one -- and it does not ask canRepairTarget either.
bool Game::orderReclaim(int builderId, int targetId) {
    Unit* b = get(builderId);
    Unit* t = get(targetId);
    if (!b || !b->tpl || !t || !t->tpl || t->dead || t->riding()) return false;
    if (!b->tpl->canReclaimResources) return false;
    // OrderableUnit:2202-2211 drops a cross-team reclaim only when the target has no [core] resourceRate --
    // a neutral/enemy credit crate or crystal is fair game, an enemy extractor is not -- and RP keeps no
    // alliances (the :2204 single-player escape hatch) to fall back on.
    if (b->team != t->team && t->tpl->resourceRate == 0.0f) return false;
    b->nanoTargetId = targetId; b->nanoReclaim = true; b->nanoOrderAge = 0;
    b->orderTargetId = -1; b->attackMove = false;
    b->path.clear(); b->pathIdx = 0; b->hasGoal = false;
    return true;
}

void Game::orderStop(std::vector<int> ids) {
    // OrderableUnit.clearAllWaypoints:3790-3800 drops the waypoints and calls clearPathData, which
    // zeroes activePathCount and pathRetryTimer -- RWX keeps no remembered goal to walk back to, so a
    // stopped hull must not re-path once its blockage timer runs out.
    for (int id : ids) {
        Unit* u = get(id); if (!u) continue;
        u->path.clear(); u->pathIdx = 0; u->hasGoal = false; u->repathTimer = 0;
        u->orderTargetId = -1; u->attackMove = false; u->nanoTargetId = -1;
        // clearAllWaypoints drops the transport commands along with the rest of them
        u->loadTargetId = -1; u->loadUpTargetId = -1;
    }
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
    Vec2 center = tpl->footprintPos(tx, ty);
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
        b->nanoTargetId = bid; b->nanoReclaim = false; b->nanoOrderAge = 0;
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
    if (!u || !u->tpl || u->dead) return false;
    const auto& acts = u->tpl->actions;
    if (actionIdx < 0 || actionIdx >= (int)acts.size()) return false;
    const UnitAction& a = acts[actionIdx];
    if (a.hidden || !actionAvailable(*u, actionIdx)) return false;
    if (!a.convertTo.empty() && !g_templates.find(ToLower(a.convertTo))) return false;
    bool instant = a.buildSpeed >= 1e8f;
    if (!instant && u->upgradeIdx >= 0) return false;
    if (!canAfford(u->team, a.price)) return false;
    spend(u->team, a.price);
    if (instant) {
        UnitAction copy = a;
        actionEvent(*u, "queueitemadded", copy.tags);
        Unit* again = get(unitId);
        if (again && !again->dead && again->tpl == u->tpl) runAction(*again, copy);
        return true;
    }
    u->upgradeIdx = actionIdx;
    u->upgradeProgress = 0;
    actionEvent(*u, "queueitemadded", a.tags);
    return true;
}

// ---------------------------------------------------------------------------
// Transport. RWX's rules live in CustomUnit.java: the hold is a list (transportedUnits), a carried hull
// points back at the carrier through BaseUnit.unitTransportTarget, and the two meet in d():335-362 (may
// this pair board?), loadTransportedUnit():374-383, g():422-429 (pop the last one) and b():718-809 (put it
// down at the exit point).
// ---------------------------------------------------------------------------

// CustomUnit.dI():4826-4837 -- the slots taken, not the hulls. transportUnitsEachUnitAlwaysUsesSingleSlot
// (:1830) flattens every passenger to one slot.
int Game::transportSlotsUsed(const Unit& t) const {
    if (!t.tpl || !t.tpl->canTransportUnits()) return 0;
    int used = 0;
    for (int id : t.cargo) {
        const Unit* c = get(id);
        if (!c || !c->tpl) continue;
        used += t.tpl->transportEachUnitSingleSlot ? 1 : c->tpl->transportSlotsNeeded;
    }
    return used;
}

// CustomUnit.d(BaseUnit,boolean):335-362 plus GameViewUtils.a():119-130. RWX's second argument lets an AI
// or a hook board a hostile hull (whileNeutralTransportAnyTeam, :339); the player command never does, so RP
// only ever models the same-team case.
bool Game::canLoadInto(const Unit& t, const Unit& ride) const {
    if (!t.tpl || !ride.tpl || !t.tpl->canTransportUnits()) return false;
    const UnitTemplate& tpl = *t.tpl;
    if (t.unloading || !t.operational() || t.id == ride.id) return false;
    int need = tpl.transportEachUnitSingleSlot ? 1 : ride.tpl->transportSlotsNeeded;
    if (transportSlotsUsed(t) + need > tpl.maxTransportingUnits) return false;
    if (t.team != ride.team) return false;
    if (!tpl.transportRequireMovementType.empty()) {
        bool ok = false;
        for (MoveType mt : tpl.transportRequireMovementType) if (mt == ride.tpl->moveType) ok = true;
        if (!ok) return false;
    }
    if (ride.tpl->canTransportUnits() && tpl.transportBlockOtherTransports) return false;
    if (tpl.transportBlockAirAndWater && (ride.tpl->isAirOrWaterHull() || ride.airborne())) return false;
    if (ride.tpl->isStatic()) return false;              // BaseUnit.bI(): a building never boards
    if (ride.riding() || t.riding()) return false;       // unitTransportTarget / parentEntity, :126-127
    return true;
}

// CustomUnit.loadTransportedUnit():374-383. RWX also fires the transportingNewUnit / enteredTransport
// listeners and deselects the passenger; the position is deliberately left where it was, since a hull in
// the hold is invisible, intangible and out of the collision world until b() places it.
void Game::loadUnit(Unit& t, Unit& ride) {
    ride.transportId = t.id;
    ride.loadTargetId = -1;
    ride.selected = false;
    t.cargo.push_back(ride.id);
}

// CustomUnit.b(BaseUnit,boolean,boolean):718-809 -- place one hull at the carrier's exit point. `evenSide`
// is the parity the hold had before the pop (g():422-429 passes size%2) and picks which side of the exit
// line the hull lands on, so a pair of loads steps out left, right, left, right instead of stacking.
bool Game::placeCargo(Unit& t, Unit& ride, bool evenSide) {
    const UnitTemplate& tpl = *t.tpl;
    float offDeg = tpl.exitDirSet ? tpl.exitDirOffset : 180.0f;
    float a = t.facing + offDeg * 0.0174533f;
    float side = evenSide ? -7.0f : 7.0f;
    // :730-733, written out as RWX has it: exit_y runs along the rotated axis and exit_x across it, then
    // the +-7 px sidestep goes along a + 90 degrees.
    Vec2 at{ t.pos.x + std::cos(a) * tpl.exitY - std::sin(a) * tpl.exitX,
             t.pos.y + std::sin(a) * tpl.exitY + std::cos(a) * tpl.exitX };
    at = at + Vec2{ std::cos(a + 1.5707963f) * side, std::sin(a + 1.5707963f) * side };
    // :734-749 -- a mobile carrier tries four nudges and then refuses to open its hold at all; a building
    // carrier sets its cargo down wherever the exit point falls.
    if (!tpl.isBuilding && !t.tpl->isStatic()) {
        MoveType mt = ride.tpl->moveType;
        if (!passableAt(*this, at, mt)) at.x += 10.0f;
        if (!passableAt(*this, at, mt)) at.x -= 20.0f;
        if (!passableAt(*this, at, mt)) { at.x -= 10.0f; at.y += 10.0f; }
        if (!passableAt(*this, at, mt)) at.y -= 20.0f;
        if (!passableAt(*this, at, mt)) return false;
    }
    // :769-776 -- the hull is teleported, its heading and momentum reset, and it looks back at the carrier
    ride.pos = at;
    ride.facing = a;
    ride.targetFacing = a;
    ride.turnVel = 0;
    ride.vel = Vec2{};
    ride.push = Vec2{};
    // :799-802 height follows the carrier unless exitHeightIgnoreParent, then gains exit_heightOffset
    if (!tpl.exitHeightIgnoreParent) ride.posZ = t.posZ;
    ride.posZ += tpl.exitHeightOffset;
    // :788-796 -- unless transportUnitsKeepWaypoints, the old order is thrown away and replaced by a move
    // down the exit line for exit_moveAwayAmount px (70 by default). The 7 degree tilt on that line is the
    // same evenSide sign as the sidestep, in *degrees* here, not px.
    if (!tpl.transportKeepWaypoints) {
        ride.orderTargetId = -1;
        ride.attackMove = false;
        ride.nanoTargetId = -1;
        ride.loadTargetId = -1;
        ride.loadUpTargetId = -1;
        ride.path.clear();
        ride.pathIdx = 0;
        ride.hasGoal = false;
        if (tpl.exitMoveAway != 0.0f) {
            float wa = a + side * 0.0174533f;
            computePath(*this, ride, Vec2{ ride.pos.x + std::cos(wa) * tpl.exitMoveAway,
                                           ride.pos.y + std::sin(wa) * tpl.exitMoveAway });
        }
    } else {
        ride.path.clear();      // clearPathData(): the waypoints stay, the walk gets rebuilt
        ride.pathIdx = 0;
        ride.repathTimer = 0;
    }
    return true;
}

// CustomUnit.h(boolean):553-562 -- drop everything aboard, 9 px behind the carrier's nose, killing it if
// transportUnitsKillOnDeath says so. This is the only exit that ignores the exit point.
void Game::releaseCargo(Unit& t, bool kill) {
    // The list is taken out first: a passenger killed here dies through killUnit -> removeUnit, which
    // would otherwise find its own carrier still holding it.
    std::vector<int> ids;
    ids.swap(t.cargo);
    for (int id : ids) {
        Unit* c = get(id);
        if (!c) continue;
        c->transportId = -1;
        c->pos = Vec2{ t.pos.x + std::cos(t.facing) * -9.0f, t.pos.y + std::sin(t.facing) * -9.0f };
        if (kill) killUnit(*c);
    }
}

// CustomUnit.update:1966 -- transportUnitsCanUnloadUnits.read(this), the logic boolean that gates the
// countdown. isOverLiquid / overWater are the tile predicates and isMoving is `driven`, the order-driven
// flag the movement pass raises (OrderableUnit.java:2841-2850).
bool Game::unloadCondOk(const Unit& t) const {
    return condTestsOk(*this, t, t.tpl->transportCanUnload);
}

// CustomUnit.b(i,f):3998 -- the same predicate list, but an absent idleSweepCondition is null (always
// true) rather than the unload default, so an empty test list passes outright.
bool Game::sweepCondOk(const Unit& u, const TurretTpl& tur) const {
    return condTestsOk(*this, u, tur.idleSweepCond);
}

// GameUI.canSelectedUnitsLoadInto:3856-3869 + issueLoadIntoCommand:3588-3592, arriving at
// UnitCommandType.loadInto (UnitCommand.java:279-283). RWX hands the order to every selected hull that is
// allowed aboard, and each one drops it again on the tick it stops qualifying (:2039-2041).
int Game::orderLoadInto(std::vector<int> ids, int transportId) {
    Unit* t = get(transportId);
    if (!t || !t->tpl) return 0;
    int taken = 0;
    for (int id : ids) {
        Unit* u = get(id);
        if (!u || u->id == transportId || !canLoadInto(*t, *u)) continue;
        u->orderTargetId = -1;
        u->attackMove = false;
        u->nanoTargetId = -1;
        u->loadTargetId = transportId;
        computePath(*this, *u, t->pos);
        taken++;
    }
    return taken;
}

// GameUI.canSelectedUnitsLoadUp:3872-3885 + issueLoadUpCommand, arriving at UnitCommandType.loadUp: the
// selected carriers walk onto the chosen hull and take it aboard themselves.
int Game::orderLoadUp(std::vector<int> ids, int rideId) {
    Unit* ride = get(rideId);
    if (!ride || !ride->tpl) return 0;
    int taken = 0;
    for (int id : ids) {
        Unit* t = get(id);
        if (!t || t->id == rideId || !canLoadInto(*t, *ride)) continue;
        t->orderTargetId = -1;
        t->attackMove = false;
        t->nanoTargetId = -1;
        t->loadUpTargetId = rideId;
        computePath(*this, *t, ride->pos);
        taken++;
    }
    return taken;
}

// CustomUnit.getTransportedUnits():406-412 -- the unload order arms a 30 tick fuse, and getLoadedUnits()
// (:415-420) calls the whole thing off.
void Game::orderUnload(int transportId) {
    Unit* t = get(transportId);
    if (!t || !t->tpl || !t->tpl->canTransportUnits()) return;
    t->unloading = true;
    t->unloadTimer = 30.0f;
}

// CustomUnit.getLoadedUnits():415-420 -- "load up": the fuse is simply switched off, and whoever had
// started walking out of the hold stays in it.
void Game::orderLoadUpAgain(int transportId) {
    Unit* t = get(transportId);
    if (!t || !t->tpl || !t->tpl->canTransportUnits()) return;
    t->unloading = false;
}

std::vector<Unit*> Game::selectedUnits() { std::vector<Unit*> r; for (auto& u : units) if (u.selected) r.push_back(&u); return r; }

std::vector<Unit*> Game::findUnitsInRect(int team, Vec2 a, Vec2 b) {
    Vec2 lo{ std::min(a.x, b.x), std::min(a.y, b.y) }, hi{ std::max(a.x, b.x), std::max(a.y, b.y) };
    std::vector<Unit*> r;
    // CustomUnit.a(GameEngine):4656 -- a hull in the hold is not drawn, so a drag box cannot pick it up.
    for (auto& u : units) if (u.team == team && !u.dead && !u.riding() && selectable(u) && u.pos.x >= lo.x && u.pos.x <= hi.x && u.pos.y >= lo.y && u.pos.y <= hi.y) r.push_back(&u);
    return r;
}

std::vector<Unit*> Game::findEnemyUnits(int team, Vec2 center, float radius, bool flying, bool land, bool underwater,
                                        bool landMustTouchWater) {
    std::vector<Unit*> r;
    for (auto& u : units) {
        if (!u.tpl || u.dead || u.riding() || !targetable(u)) continue;
        if (!isEnemy(team, u.team)) continue;
        bool isFly = u.airborne(), isSea = u.submerged(), isLand = !isFly && !isSea;
        if (!((isFly && flying) || (isLand && land) || (isSea && underwater))) continue;
        // OrderableUnit.canAttackUnitType:3356 -- the same water gate as the barrel's, applied to the hull's
        // own flag so a submarine never even lists a dry target.
        if (landMustTouchWater && isLand && !touchingWater(*this, u)) continue;
        if (center.dist(u.pos) <= radius + u.tpl->radius) r.push_back(&u);
    }
    return r;
}

std::vector<Unit*> Game::queryNearestEnemy(int team, Vec2 pos, float maxDist, const UnitTemplate* shooter) {
    return findEnemyUnits(team, pos, maxDist, shooter->canAttackFlying, shooter->canAttackLand,
                          shooter->canAttackUnderwater, !shooter->canAttackNotTouchingWater);
}

Vec2 Game::nearestEnemyPos(int team) {
    float best = 1e18f; Vec2 r{ map.worldW() / 2, map.worldH() / 2 };
    for (auto& u : units) { if (!u.tpl || u.dead || u.riding() || !isEnemy(team, u.team)) continue; float d = r.dist(u.pos); if (d < best) { best = d; r = u.pos; } }
    return r;
}

void Game::update() {
    ++tick;
    for (size_t i = 0; i < units.size(); ++i) updateUnit(units[i]);
    flushMessages();
    // Indexed, and over a snapshot of the size: a spawnProjectilesOnEndOfLife list appends to this very
    // vector in the middle of the pass, which both reallocates it (so a range-for's iterators would die) and
    // would otherwise let a split round fly a tick it has not lived yet. RWX appends to its live list the
    // same way, and the rounds it hands off next are its own Projectile.a list, so waiting a tick matches.
    for (size_t i = 0, n = projectiles.size(); i < n; ++i) updateProjectile(projectiles[i]);
    resolveSoftCollisions();
    // A puff drifts by its own Effect.Q per tick (Effect.java:364-366); only the projectile trail sets
    // one, at 0.1 (Projectile:934).
    for (auto& e : effects) { e.t += 1; e.pos.y += e.sink; if (e.t - e.delay >= e.life) e.dead = true; }
    updateFx();
    for (auto& p : particles) { p.t += 1; p.pos = p.pos + p.vel; if (p.t >= p.dur) p.dead = true; }
    // A dead unit only leaves the list when its death fall is over -- CustomUnit keeps the falling hull
    // as a live game object until it touches down (:1672, and the object is dropped at :1731 / by the
    // isDeathFallComplete path at :1752).
    units.erase(std::remove_if(units.begin(), units.end(), [](const Unit& u) {
        return u.dead && !u.deathFalling; }), units.end());
    projectiles.erase(std::remove_if(projectiles.begin(), projectiles.end(), [](const Projectile& p) { return p.dead; }), projectiles.end());
    effects.erase(std::remove_if(effects.begin(), effects.end(), [](const Effect& e) { return e.dead; }), effects.end());
    fx.erase(std::remove_if(fx.begin(), fx.end(), [](const Fx& f) { return f.dead; }), fx.end());
    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.dead; }), particles.end());
    if (tick % 30 == 0) runAI();
    checkWinLoss();
}

// OrderableUnit.resolveSoftCollisionWithUnit:1106-1184 for one ordered pair, `a` being the mover that
// gathered `b` as a neighbour. The correction lands in both units' push buffers, weighted by
// [core] mass.
static void softPushPair(Unit& a, Unit& b) {
    float dx = b.pos.x - a.pos.x, dy = b.pos.y - a.pos.y;
    float d2 = dx * dx + dy * dy;
    float rr = a.tpl->radius + b.tpl->radius;
    if (d2 >= rr * rr) return;
    float dist = std::sqrt(d2);
    float overlap = (rr - dist) + 0.001f;
    int divisor = std::max(a.tpl->softCollisionOnAll, b.tpl->softCollisionOnAll);
    // :1115-1121, with f == 1 tick. A high softCollisionOnAll makes a huge hull ease out of an
    // intersection instead of popping out of it.
    if (divisor != 0) overlap = std::min(overlap / divisor, overlap);
    // :1122-1134 -- 5% off the top, then 30% off everything past 1 px, 3 px, 6 px and 10 px in turn,
    // so units buried deep in a crowd separate slowly rather than exploding apart.
    float t = overlap * 0.95f;
    if (t > 1.0f) t *= 0.7f;
    if (t > 3.0f) t = 3.0f + (t - 3.0f) * 0.7f;
    if (t > 6.0f) t = 6.0f + (t - 6.0f) * 0.7f;
    if (t > 10.0f) t = 10.0f + (t - 10.0f) * 0.7f;
    // :1175-1176, the direction from the mover toward the neighbour. Two units on the exact same pixel
    // push along +X, which is what fastCos(atan2(0, 0)) gives RWX.
    Vec2 dir{ 1.0f, 0.0f };
    if (dist > 1e-4f) dir = Vec2{ dx / dist, dy / dist };
    float mA = a.tpl->mass, mB = b.tpl->mass;
    // :1142-1170, friendly pairs only: whoever is parked counts as 1.7x heavier, so a moving hull
    // gives way around a standing one rather than ploughing through it -- 5x once either has been
    // retrying its path for 200 ticks, which is how RWX unjams a blocked column. RWX weights a unit
    // riding inside its transport the same way, which RP has no equivalent for yet.
    if (a.team == b.team) {
        float k = (a.stuckTimer > 200 || b.stuckTimer > 200) ? 5.0f : 1.7f;
        if (a.throttle == 0.0f && b.throttle != 0.0f) mA *= k;
        else if (b.throttle == 0.0f && a.throttle != 0.0f) mB *= k;
    }
    // :1171-1184 -- b's slice, by mass. A building takes its share into a buffer it then throws away,
    // because CustomUnit.I():598-603 stops a building applying any position change at all.
    float share = mA / (mA + mB);
    b.push = b.push + dir * (t * share);
    a.push = a.push - dir * (t * (1.0f - share));
}

// RWX OrderableUnit.updateAllUnitCollisions:451-509 with resolveSoftCollisionWithUnit:1043-1204, the
// rule that separates overlapping hulls. It replaces a symmetric half-overlap nudge, which ignored
// mass, terrain group and buildings alike, so a scout could bulldoze a tank across a factory floor.
void Game::resolveSoftCollisions() {
    // BaseUnit.bR():944-958 numbers the groups straight off the height: 3 airborne (posZ >= 4),
    // 2 submerged (posZ <= -1), 1 everything else, and -1 for a unit out of the collision world. A
    // surface ship is group 1 and shares it with the tanks, exactly as in RWX; only a plane low on its
    // approach is still down there with them (CustomUnit.i():636-638).
    for (auto& u : units) {
        int group = 1;
        // BaseUnit.bR():951 -- a hull in someone's hold is taken out of the collision world entirely,
        // as is a corpse; nothing pushes it and it pushes nothing until it is set down again.
        if (!u.tpl || u.dead || u.riding() || u.parentId >= 0) group = -1;
        else if (u.airborne()) group = 3;
        else if (u.submerged()) group = 2;
        u.collisionGroup = group;
    }
    // Last tick's buffered push is folded in here, before the new overlaps are measured, clamped to
    // +-9 px per axis (OrderableUnit.java:922-928, where clampTo255 is despite its name a plain clamp)
    // and put through the same passability test as a step -- a unit crushed by a whole crowd simply
    // loses the rest of that tick's correction.
    for (auto& u : units) {
        Vec2 d{ std::min(9.0f, std::max(-9.0f, u.push.x)), std::min(9.0f, std::max(-9.0f, u.push.y)) };
        u.push = Vec2{};
        if (d.len() < 1e-6f || u.collisionGroup < 0 || u.isStatic()) continue;
        Vec2 np = u.pos + d;
        if (passableAt(*this, np, u.tpl->moveType)) u.pos = np;
        else {
            Vec2 nx{ np.x, u.pos.y }, ny{ u.pos.x, np.y };
            if (passableAt(*this, nx, u.tpl->moveType)) u.pos = nx;
            else if (passableAt(*this, ny, u.tpl->moveType)) u.pos = ny;
        }
        u.moved = true;   // :929 -- being pushed counts as moving
    }
    // Gather, then resolve (the two loops at :463-494 and :497-508), so every pair in a tick is
    // measured against the positions at the start of it. A mover keeps its ten nearest neighbours
    // within its own radius plus 7 px (:470, list capped at :1089-1103); RWX stretches each unit's
    // query over 50-250 ms as a pure optimisation, which RP at this scale does not need.
    // The gate at :466 is `movementActiveThisFrame || collisionActive`, and that flag is only raised by
    // moving under one's own velocity (:920) or absorbing a push (:929) -- so a hull parked by an order
    // that has since completed is *not* queried, and two tanks that came to rest overlapping at the end
    // of a factory's exit line stay overlapping until something shoves them. RP matching that is the
    // faithful behaviour, not a gap.
    struct Near { Unit* other; float rank; };
    std::vector<std::vector<Near>> near(units.size());
    for (size_t i = 0; i < units.size(); i++) {
        Unit& a = units[i];
        if (!a.moved || a.collisionGroup < 0 || a.isStatic()) continue;
        float reach = a.tpl->radius + 7.0f, reachSq = reach * reach;
        for (size_t j = 0; j < units.size(); j++) {
            if (j == i) continue;
            Unit& b = units[j];
            // :1047 -- a unit only pushes its own group, and never the unit it is shooting at, so a
            // tank can sit against its target instead of being shoved out of range.
            if (b.collisionGroup != a.collisionGroup || a.targetId == b.id || b.targetId == a.id) continue;
            float dx = b.pos.x - a.pos.x, dy = b.pos.y - a.pos.y;
            float d2 = dx * dx + dy * dy;
            if (d2 > reachSq) continue;
            float rr = a.tpl->radius + b.tpl->radius;
            float rank = d2 < rr * rr ? 0.0f : d2;   // :1057-1060, overlapping units sort first
            auto& lst = near[i];
            int at = (int)lst.size();
            for (int k = 0; k < (int)lst.size(); k++) if (rank < lst[k].rank) { at = k; break; }
            if (at >= (int)lst.size() && lst.size() >= 10) continue;
            if ((int)lst.size() < 10) lst.push_back({ nullptr, 0 });
            for (int k = (int)lst.size() - 1; k > at; k--) lst[k] = lst[k - 1];
            lst[at] = { &b, rank };
        }
    }
    for (size_t i = 0; i < units.size(); i++)
        for (auto& n : near[i]) softPushPair(units[i], *n.other);
}

// RWX CustomUnit.update:1996-2029, the AIR height pass. The hull bobs on a sine around
// [movement] targetHeight and eases toward it: the wanted rate is |posZ-2|*0.02+0.2 (so a hull far
// from the ground climbs fast), but it is clamped to |posZ-desired|*0.015+0.1, which turns the last
// pixel of the climb into a smooth arrival instead of a jitter across the target. RWX folds these into
// Utility.distanceSq(:233-241), which despite the name is an approach-with-limit.
// `wasMoving` is the hull's isMoving from the pass below, which :1981-1984 clocks into `s` (`idleTime`)
// one line before this branch reads it.
static void updateHeight(Game& g, Unit& u, const UnitTemplate& tpl, bool wasMoving) {
    if (u.parentId >= 0) return;   // :1986/2039 -- an attached child's height belongs to its parent
    if (tpl.moveType != MoveType::AIR) {
        // :2031-2102, every other movement type: climb up to the bottom of the drift band at 0.2 px a
        // tick, ease toward targetHeight, and fall under fallingAcceleration once too far above it --
        // the dive/surface of a submarine converting between its configs.
        float drift = tpl.targetHeightDrift, low = tpl.targetHeight - drift;
        if (u.posZ < low) u.posZ = std::min(u.posZ + 0.2f, low);
        if (tpl.targetHeight == 0.0f && drift == 0.0f && u.posZ <= 0.0f) return;
        float want = tpl.targetHeight;
        if (drift != 0.0f) {
            u.heightPhase += 2.0f;
            if (u.heightPhase > 360.0f) u.heightPhase -= 360.0f;
            want += std::sin(u.heightPhase * 0.0174533f) * drift;
        }
        float raw = tpl.heightChangeRate < 0.0f ? std::fabs(u.posZ - 2.0f) * 0.05f * 0.4f + 0.2f : tpl.heightChangeRate;
        u.posZ = moveTowards(u.posZ, want, std::min(raw, std::fabs(u.posZ - want) * 0.05f * 0.3f + 0.1f));
        float top = tpl.targetHeight + drift;
        if (u.posZ > top + 1.0f) {
            u.fallVel += tpl.fallingAcceleration;
            if (u.posZ < 0.0f) u.fallVel = std::min(u.fallVel, 0.2f);
            u.posZ -= u.fallVel;
            if (u.posZ <= top + 1.0f) {
                if (u.posZ < top) u.posZ = top;
                u.fallVel = 0;
            }
        } else u.fallVel = 0;
        return;
    }
    u.idleTime = wasMoving ? 0.0f : u.idleTime + 1.0f;
    u.heightPhase += 2.0f;                       // :1996-2000, 2 degrees per tick, wrapping at 360
    if (u.heightPhase > 360.0f) u.heightPhase -= 360.0f;
    float raw = tpl.heightChangeRate >= 0.0f ? tpl.heightChangeRate
                                             : std::fabs(u.posZ - 2.0f) * 0.05f * 0.4f + 0.2f;
    // :2003-2007 -- [movement] landOnGround parks the hull on the ground at posZ 2 while it is still,
    // off open water and has been so for more than three ticks; onlyIdle adds "with no order left to
    // finish". Note the landing branch (:2011-2016) takes the rate unclamped, unlike the hover below.
    if (tpl.landOnGround && !wasMoving && u.idleTime > 3.0f && !overWater(g, u.pos) &&
        (!tpl.landOnGroundOnlyIdle || (u.path.empty() && !u.hasGoal))) {
        u.posZ = moveTowards(u.posZ, 2.0f, raw);
        return;
    }
    float want = tpl.targetHeight + std::sin(u.heightPhase * 0.0174533f) * tpl.targetHeightDrift;
    float limit = std::fabs(u.posZ - want) * 0.05f * 0.3f + 0.1f;
    u.posZ = moveTowards(u.posZ, want, std::min(raw, limit));
}

static float& legAnimValue(LegState& leg, LegAnimProperty property) {
    switch (property) {
        case LegAnimProperty::X: return leg.animOffset.x;
        case LegAnimProperty::Y: return leg.animOffset.y;
        case LegAnimProperty::DIRECTION: return leg.spin;
        case LegAnimProperty::HEIGHT: return leg.z;
        default: return leg.alpha;
    }
}

static void captureAnimationPose(Unit& u) {
    u.animBaseScale = u.animScale;
    u.animBaseLegs.clear();
    for (auto& track : u.animCur->legs) {
        float& value = legAnimValue(u.legs[track.index], track.property);
        if (track.property == LegAnimProperty::DIRECTION) value = std::fmod(value, 360.0f);
        u.animBaseLegs.push_back(value);
    }
}

static void applyAnimationPose(Unit& u, bool reset) {
    const BodyAnim& cfg = *u.animCur;
    float blend = std::max(0.0f, u.animBlend);
    if (!cfg.frame.empty()) u.animFrame = reset ? u.tpl->defaultFrame : (int)cfg.frameAt(u.animTime);
    if (!cfg.scale.empty())
        u.animScale = (reset ? 1.0f : cfg.scaleAt(u.animTime)) * (1 - blend) + u.animBaseScale * blend;
    for (size_t i = 0; i < cfg.legs.size(); ++i) {
        const LegAnimTrack& track = cfg.legs[i];
        LegState& leg = u.legs[track.index];
        float value = reset ? (track.property == LegAnimProperty::ALPHA ? u.tpl->legs[track.index].alpha : 0.0f)
                            : BodyAnim::valueAt(track.keys, u.animTime);
        legAnimValue(leg, track.property) = value * (1 - blend) + u.animBaseLegs[i] * blend;
        if (track.property == LegAnimProperty::X || track.property == LegAnimProperty::Y) {
            leg.stepping = true;
            leg.dirty = true;
        }
    }
}

static void requestAnimation(Unit& u, const BodyAnim* cfg, int prio, bool loop) {
    if (!cfg || !cfg->valid()) return;
    if ((u.animRequested || (u.animLoop && u.animPlaying)) && prio <= u.animPrio &&
        (!loop || cfg != u.animCur)) return;
    u.animRequested = true;
    if (cfg != u.animCur || loop || u.animEnding) {
        float oldRate = u.animCur && u.animPlaying ? u.animCur->blendOut : 0;
        u.animCur = cfg;
        captureAnimationPose(u);
        u.animPrio = prio;
        u.animLoop = loop;
        u.animTime = -1.0f;
        u.animSpeed = 1.0f;
        u.animEnding = false;
        u.animBlendRate = std::max(oldRate, cfg->blendIn);
        u.animBlend = u.animBlendRate > 0 ? 1.0f : 0.0f;
    }
    u.animPlaying = true;
}

static void stopAnimation(Unit& u, bool immediate = false) {
    if (!immediate && u.animCur && !u.animEnding && u.animCur->blendOut > 0) {
        captureAnimationPose(u);
        u.animEnding = true;
        u.animPrio = -1;
        u.animBlend = 1;
        u.animBlendRate = u.animCur->blendOut;
        return;
    }
    u.animBlend = 0;
    if (u.animCur) applyAnimationPose(u, true);
    u.animPlaying = false;
    u.animCur = nullptr;
    u.animPrio = -1;
    u.animLoop = false;
    u.animEnding = false;
    u.animRequested = false;
    u.animScale = 1.0f;
    u.animFrame = u.tpl ? u.tpl->defaultFrame : 0;
}

static void stepAnimation(Unit& u, float f) {
    if (!u.animPlaying) return;
    if (u.animTime < 0.0f) u.animTime = 0.0f;
    u.animTime += u.animSpeed * f;
    if (!u.animLoop && !u.animRequested && !u.animEnding) stopAnimation(u);
    u.animRequested = false;
    if (!u.animPlaying) return;
    if (u.animBlend > 0) u.animBlend = std::max(0.0f, u.animBlend - u.animBlendRate * f);
    else if (u.animEnding) { stopAnimation(u, true); return; }
    const BodyAnim& cfg = *u.animCur;
    if (!u.animEnding) {
        if (!cfg.pingPong) {
            if (u.animTime > cfg.maxTime) {
                if (u.animLoop) stopAnimation(u);
                else { u.animTime = 0; u.animSpeed = 1; }
            }
        } else {
            if (u.animTime > cfg.maxTime) { u.animTime = cfg.maxTime; u.animSpeed = -1; }
            else if (u.animTime < 0) {
                u.animTime = 0; u.animSpeed = 1;
                if (u.animLoop) stopAnimation(u);
            }
        }
    }
    if (u.animPlaying) applyAnimationPose(u, u.animEnding);
}

void Game::updateUnit(Unit& u) {
    if (!u.tpl) return;
    // A corpse that died above the ground does not update as a unit any more -- no height, no shield, no
    // orders -- it only falls (CustomUnit.update:1672-1768, and :1770-1772 returns for every other dead
    // unit before the live part of update starts).
    if (u.dead) { if (u.deathFalling) updateDeathFall(u); return; }
    {
        const UnitTemplate* before = u.tpl;
        updateActions(u);
        if (u.dead || u.tpl != before) return;
    }
    const UnitTemplate& tpl = *u.tpl;
    u.moved = false;
    // RWX runs the height pass after the movement one, so it reads this tick's isMoving; here the
    // movement pass is below, so carry last tick's over as the same signal.
    bool wasMoving = u.driven;
    u.driven = false;
    updateHeight(*this, u, tpl, wasMoving);
    if (u.underConstruction && u.buildProgress >= 1.0f) u.underConstruction = false;
    // CustomUnit.update:1869-1884 -- o += f runs whether or not the payout is currently allowed, and the whole
    // generation_credits lump is paid once o passes generationDelay - 0.1, wrapping by exactly generationDelay.
    // So a delay of 120 pays a third as often (and a third as much per 40 ticks) as the default 40; RWX only
    // divides the lump by 40/delay for the income readout at getCreditIncomeRate():1214-1219.
    if (tpl.creditIncomeRate > 0) {
        u.genAcc += 1.0f;
        if (u.genAcc > (float)tpl.generationDelay - 0.1f) {
            u.genAcc -= (float)tpl.generationDelay;
            if (u.operational()) reward(u.team, tpl.creditIncomeRate);
        }
    }

    // CustomUnit.update:1813-1818 dispatches ProjectileInterceptorHook.b() through the unit's own update, and
    // it does so ahead of the selfRegen (:1824-1831) and energy-refill (:1828-1848) blocks below -- so a beam
    // or a hunt spends the pool as it was left at the end of last tick, before this tick's top-up is added.
    // It does not ask whether the hull has a target either: an anti-nuke with nothing to shoot at is exactly
    // the interceptor's job. Built hulls only -- the not-yet-built branch of that update never reaches here.
    if (u.operational()) updateInterceptors(u);

    // CustomUnit.update:1826-1845: the pool refills by energyRegen per tick, or by
    // energyRegenWhenRecharging while the recharge-to-full latch is on; it only ever clamps at the top
    // (which also clears the latch), and an empty pool either kills the unit or parks it at 0.
    if (tpl.maxEnergy != 0.0f) {
        float rate = u.energyRecharging ? tpl.energyRegenWhenRecharging : tpl.energyRegen;
        if (u.energy < tpl.maxEnergy || rate < 0.0f) u.energy += rate;
        if (u.energy >= tpl.maxEnergy) {
            u.energy = tpl.maxEnergy;
            u.energyRecharging = false;
        }
        if (u.energy <= 0.0f) {
            if (tpl.dieOnZeroEnergy) { damage(u, 1e7f, nullptr); return; }
            u.energy = 0.0f;
        }
    }

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

    if (tpl.isFactory && u.operational() && !u.prodQueue.empty() && u.upgradeIdx < 0) {
        const UnitTemplate* item = g_templates.find(u.prodQueue.front());
        // FactoryQueueManager.a:249 -- `projectile.b * getNanoFactorySpeed() * f`: the queued unit's own
        // buildSpeed (QueueUnitAction.K() = the produced UnitType's [core] buildSpeed) times the
        // producer's nanoFactorySpeed, so a nautilus (2) pays for a lightSub in half the ticks a
        // seaFactory takes while both still cost the same credits.
        u.prodProgress += (item ? item->buildSpeed : tpl.buildSpeed) * tpl.nanoFactorySpeed;
        if (u.prodProgress >= 1.0f) {
            u.prodProgress = 0; std::string name = u.prodQueue.front(); u.prodQueue.erase(u.prodQueue.begin());
            // FactoryQueueManager.a:135-141 with CustomUnit.a:4300-4311: the newborn is placed 5 px south of
            // its producer, given the exit line as its body angle, and then *ordered* exit_moveAwayAmount
            // (70 by default) px down that line -- the exit step first, and the rally point only as a
            // second waypoint when one is set. Appearing parked on the spot it was wanted is what stacked
            // RP's whole production queue on one pixel, since the collision pass only spreads moving units.
            float dirDeg = 90.0f + (tpl.isBuilding && tpl.exitDirSet ? tpl.exitDirOffset : 0.0f);
            float rad = dirDeg * 0.0174533f;
            Vec2 exitAt{ u.pos.x, u.pos.y + 5.0f };
            Vec2 push{ u.pos.x + std::cos(rad) * tpl.exitMoveAway,
                       u.pos.y + std::sin(rad) * tpl.exitMoveAway };
            // :121-126 -- with no rally the single waypoint is the push point shifted one pixel sideways.
            Vec2 exitGoal = u.hasRally ? push : Vec2{ push.x - std::sin(rad), push.y + std::cos(rad) };
            Vec2 dir{ std::cos(rad), std::sin(rad) };
            Vec2 sp = exitAt;
            if (item && item->isWaterUnit()) sp = findWaterSpawn(*this, exitAt, tpl.radius + TILE_SIZE * 2.0f);
            else {
                // RP keeps a building's footprint unwalkable, so a unit born 5 px from the centre is
                // standing inside its own factory and can never path out; RWX drives straight at the
                // waypoint and lets collision sort it. Sliding the birth point down the exit line to the
                // first free ground gives the same exit without the deadlock.
                MoveType mt = item ? item->moveType : MoveType::LAND;
                for (int k = 1; k < 40 && !passableAt(*this, sp, mt); ++k)
                    sp = Vec2{ exitAt.x + dir.x * (float)(k * 6), exitAt.y + dir.y * (float)(k * 6) };
            }
            int newId = spawnUnit(name, u.team, sp);
            if (newId >= 0) {
                if (Unit* n = get(newId)) {
                    n->facing = rad;
                    orderMove({ newId }, exitGoal);
                    // :113-115 -- the rally is only reached as a second waypoint, one pixel off centre so a
                    // line of units does not all stop on the same tile.
                    if (u.hasRally && !n->path.empty()) n->path.push_back(u.rallyPoint + Vec2{ 1.0f, 0.0f });
                }
            }
        }
    }

    // [core] selfBuildRate: a foundation raises itself with no builder at all
    // (CustomUnit.update:1774-1782)
    if (u.underConstruction && tpl.selfBuildRate != 0.0f) {
        u.buildProgress = std::min(1.0f, u.buildProgress + tpl.selfBuildRate);
        if (u.buildProgress >= 1.0f) u.underConstruction = false;
    }

    // [core] selfRegenRate: RWX runs this for every unit that has passed the isDead early return and
    // isAlive() check, so a foundation heals too, and a negative rate is kept even at full health so
    // it can burn the unit down (CustomUnit.update:1821-1826). The bubble is untouched — that is
    // shieldRegen's job, not this one.
    if (tpl.selfRegenRate != 0.0f && (u.hp < u.maxHp || tpl.selfRegenRate < 0.0f)) {
        u.hp += tpl.selfRegenRate;
        if (u.hp > u.maxHp) u.hp = u.maxHp;
    }

    // CustomUnit.update:1953-1980, the cargo pass. transportUnitsHealBy (:1836) is a flat amount per tick
    // onto every hull aboard, applied even at full health when it is negative, and the unload countdown
    // only runs while transportUnitsCanUnloadUnits holds -- which for the nautilus is the literal `false`,
    // so its hold never opens. One hull leaves each time the fuse reaches 0, then the fuse is re-armed at
    // transportUnitsUnloadDelayBetweenEachUnit; an empty hold switches unloading off by itself.
    if (tpl.maxTransportingUnits > 0) {
        if (tpl.transportHealBy != 0.0f) {
            for (int id : u.cargo) {
                Unit* c = get(id);
                if (!c || (c->hp >= c->maxHp && tpl.transportHealBy > 0.0f)) continue;
                c->hp = std::min(c->maxHp, c->hp + tpl.transportHealBy);
            }
        }
        if (u.unloading && unloadCondOk(u)) {
            u.unloadTimer = std::max(0.0f, u.unloadTimer - 1.0f);
            if (u.unloadTimer == 0.0f) {
                u.unloadTimer = tpl.transportUnloadDelay;
                if (u.cargo.empty()) {
                    u.unloading = false;
                } else {
                    // CustomUnit.g(false):422-429 -- the last hull aboard is the first one out, and the
                    // parity of the hold at that moment decides which side of the exit line it takes.
                    bool evenSide = (u.cargo.size() % 2) == 0;
                    int id = u.cargo.back();
                    u.cargo.pop_back();
                    Unit* c = get(id);
                    if (c) {
                        if (placeCargo(u, *c, evenSide)) {
                            c->transportId = -1;
                        } else {
                            u.cargo.push_back(id);   // :808-810, no room to stand: straight back in the hold
                        }
                    }
                    if (u.cargo.empty()) u.unloading = false;
                }
            }
        }
    }

    // A hull in the hold is out of the world: it does not walk, aim, shoot or get hit, and RWX drops it from
    // the collision groups (BaseUnit.bR():951), the renderer (CustomUnit.a(GameEngine):4656), the picking
    // and the projectiles (Projectile:992, 1463). Its orders stay exactly as they were, since b():788-796
    // is what decides them on the way out.
    if (u.riding()) {
        for (auto& leg : u.legs) leg.reset = true;
        return;
    }

    // A hull with [core] autoRepair repairs too, even when it is no one's builder: the experimental
    // gunship is exactly that, and the hook hands it the same repair command a builder is given.
    // [core] canReclaimResources buys that same slot for the other command in the pair -- RP keeps one
    // target id and a flag, since UnitCommandType.repair and .reclaim differ only in what they do once
    // the hull is standing next to the target (OrderableUnit:2300).
    if ((tpl.isBuilder || tpl.autoRepair || tpl.canReclaimResources) && u.nanoTargetId >= 0) {
        Unit* bs = get(u.nanoTargetId);
        bool reclaiming = u.nanoReclaim;
        // The repair gates belong to repair: canRepairTarget (:1746-1747) says nothing about reclaiming,
        // and a hull at full health is precisely what a reclaim order is issued against. Reclaim does ask
        // for a team, except for the resourceRate targets :2202-2211 let anyone take over.
        if (!bs || bs->dead || bs->riding() ||
            (reclaiming ? (bs->team != u.team && bs->tpl->resourceRate == 0.0f)
                        : !canRepairTarget(tpl, *bs))) {
            u.nanoTargetId = -1; u.nanoReclaim = false;
        }
        else if (!reclaiming && bs->operational() && bs->hp >= bs->maxHp) { u.nanoTargetId = -1; u.nanoReclaim = false; }
        else {
            u.nanoOrderAge++;
            // [core] nanoRangeForRepair overrides the build range for repairs (:1760-1763); reclaim asks
            // y() + the target's own reach (:2226) and the shipped ini never writes nanoRangeForReclaim,
            // so both commands travel nanoRange here.
            float range = tpl.nanoRangeForRepair > 0 ? tpl.nanoRangeForRepair : tpl.nanoRange;
            float d = u.pos.dist(bs->pos);
            if (d > range)
                computePath(*this, u, bs->pos + (bs->tpl->isBuilding ? Vec2{ 0, -TILE_SIZE * 2 } : Vec2{ 0, 0 }));
            else if (reclaiming) {
                // OrderableUnit:2362-2454. A site is taken apart in buildProgress and a finished hull in hp
                // (:2381 splits them), and the two pay out differently: nothing per tick for a site, since
                // getRepairOrReclaimPrice (:2113, unitData2 ?? streamingCost) is null for every shipped hull
                // (:2390); for a finished hull the price drips back only if it is sellable (:2405), and the
                // lump on collapse lands only if it is not (:2431). Sellable is BaseUnit.y():1929-1935 --
                // [core] resourceRate, which no extractor writes -- not the generation payout.
                bool producer = bs->tpl->resourceRate > 0;   // zY (:2364) and z12 (:2366) are one test
                bool site = !bs->operational();              // :2381 buildProgress < 1, RP's isUnfinished
                // z13 (:2370-2379): during the first 100 ticks of the order a target that is not yet half
                // there -- a site under 50% built, otherwise a hull under 50% hp -- is left alone, it was on
                // its way out anyway. A sellable target is exempt (:2371 tests !z12 twice). RP stores 0 in
                // buildProgress on finished mobile hulls, so those read as RWX's 1.0f here.
                float prog = site ? bs->buildProgress : 1.0f;
                bool skip = false;
                if (!producer && u.nanoOrderAge < 100) {
                    if (prog < 0.5f) skip = true;            // :2373 -- getRepairOrReclaimPrice() is null
                    else if (bs->hp < bs->maxHp * 0.5f) skip = true;
                }
                bool collapse = false;
                if (!skip) {
                    if (site) {
                        bs->buildProgress = std::max(0.0f, bs->buildProgress - tpl.nanoUnbuildSpeed);
                        collapse = bs->buildProgress <= 0.0f;
                    } else {
                        // :2397 spends calculateUnitSpeed (CustomUnit:2252-2258): a sellable target loses its
                        // own resourceRate times [core] resourceReclaimMultiplier, anything else nanoReclaimSpeed.
                        float rate = producer ? bs->tpl->resourceRate * tpl.resourceReclaimMultiplier
                                              : tpl.nanoReclaimSpeed;
                        float taken = std::min(rate, bs->hp);
                        bs->hp -= taken;
                        collapse = bs->hp <= 0.0f;
                        if (producer) {
                            // :2406-2419: the fraction of the max health just drained, times the price the
                            // target asks for (reclaimPrice when the ini wrote one, build price otherwise),
                            // banked on the builder as `this.ab` so only whole credits reach the team.
                            float payout = bs->tpl->hasReclaimPrice ? bs->tpl->reclaimPrice : bs->tpl->price;
                            if (payout > 0.0f) {
                                u.creditFrac += (taken / std::max(bs->maxHp, 1.0f)) * payout;
                                if (u.creditFrac > 1.0f) {
                                    int whole = (int)u.creditFrac;
                                    reward(u.team, (float)whole);
                                    u.creditFrac -= (float)whole;
                                }
                            }
                        }
                    }
                }
                if (collapse && !bs->dead) {
                    // :2431-2446: only a hull that never paid out as it died is refunded here, the whole
                    // reclaimPrice if it wrote one -- which robotCrab does, as 0 -- else 80% of its price.
                    if (!producer) {
                        reward(u.team, bs->tpl->hasReclaimPrice ? bs->tpl->reclaimPrice
                                                                : bs->tpl->price * 0.8f);
                    }
                    // :2448-2450 isDead + removeFromGame: the hull leaves play without a death effect.
                    removeUnit(*bs);
                    u.nanoTargetId = -1; u.nanoReclaim = false;
                }
            }
            else {
                // OrderableUnit.java:2301 splits the two jobs on buildProgress, but in RP only a site
                // carries one: a finished unit sits at the field default 0 while being perfectly
                // operational, so the branch has to ask underConstruction as RWX's isUnfinished does --
                // otherwise a wounded tank gets "built" for a few ticks before it starts taking hp.
                if (bs->underConstruction && bs->buildProgress < 1.0f) {
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
            }
            // The nano stream. Repair throws its flecks from the builder at the target (a():5164-5185,
            // effects.png frame 6); reclaim throws them the other way, from the target back at the hull
            // that is taking it apart (b():5187-5208, frame 5). Both fire once per 5 ticks.
            if (tick % 5 == 0 && particles.size() < 100 && !bs->dead) {
                Unit& from = reclaiming ? *bs : u;
                Unit& to = reclaiming ? u : *bs;
                Vec2 tp = to.pos + Vec2{ g_rng.range(-8, 8), g_rng.range(-8, 8) };
                Vec2 dd = tp - from.pos;
                float spd = g_rng.range(2, 4);
                Particle p;
                p.pos = from.pos;
                p.vel = dd * (spd / std::max(dd.len(), 0.001f));
                p.dur = std::min(60.0f, dd.len() / spd);
                p.reclaim = reclaiming;
                particles.push_back(p);
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
        if (best) { u.nanoTargetId = best->id; u.nanoReclaim = false; u.nanoOrderAge = 0; }
    }

    // AutoRepairRenderHook.b:14-20 -- the ini flag buys no rate and no self-heal, just a timer: `u` counts
    // up every tick, busy or not, and on the first tick past 40 that the hull has nowhere to go
    // (hasNoCurrentWaypoint) it spends the repair bay's own search and zeroes itself. RepairBay.a(...,0f,
    // false) reaches y() (CustomUnit.y():1641-1646 -- [core] nanoRange, else BaseUnit.y():1895's 85) and
    // AutoRepairCallback:71-95 keeps the *lowest health* friendly it may repair, not the nearest, so an
    // idle mech engineer flits to whoever is hurt worst. A site that still needs paying for is skipped
    // (:78-80) -- that is a builder's job, not a repair -- and so is anything with a resource rate (:89).
    if (tpl.autoRepair) {
        u.autoRepairTick += 1.0f;
        if (u.autoRepairTick > 40.0f && u.nanoTargetId < 0 && u.orderTargetId < 0 && u.path.empty() &&
            !u.hasGoal) {
            u.autoRepairTick = 0.0f;
            float range = tpl.nanoRange > 0 ? tpl.nanoRange : 85.0f;
            Unit* best = nullptr; float bestHp = -1;
            for (auto& o : units) {
                if (&o == &u || o.dead || !o.tpl || o.team != u.team) continue; // searcherTeam.d(): same team
                if (o.riding()) continue;                                       // unitTransportTarget == null
                if (o.tpl->resourceRate != 0.0f) continue;                       // getResourceRate() != 0
                // :71 admits anything hurt *or* unfinished; RP stores buildProgress 0 on every finished
                // mobile hull, so the unfinished half of that test is !operational() here.
                if (!o.operational() || o.hp >= o.maxHp) continue;
                if (!canRepairTarget(tpl, o)) continue;
                if (u.pos.dist(o.pos) > range) continue;
                if (bestHp < 0 || o.hp < bestHp) { bestHp = o.hp; best = &o; }
            }
            if (best) { u.nanoTargetId = best->id; u.nanoReclaim = false; u.nanoOrderAge = 0; }
        }
    }

    // self action progress (RWX [action_*] queued on the unit itself: upgrades, dive/surface, ...)
    if (u.upgradeIdx >= 0) {
        if (u.upgradeIdx >= (int)tpl.actions.size()) u.upgradeIdx = -1;
        else {
            const UnitAction& a = tpl.actions[u.upgradeIdx];
            u.upgradeProgress += a.buildSpeed * tpl.nanoFactorySpeed;
            if (u.upgradeProgress >= 1.0f) {
                UnitAction done = a;
                u.upgradeIdx = -1;
                u.upgradeProgress = 0;
                const UnitTemplate* before = u.tpl;
                runAction(u, done);
                if (u.tpl != before || u.dead) return;
            }
        }
    }

    int combatTarget = -1;
    if (tpl.canAttack && u.operational()) {
        Unit* ot = u.orderTargetId >= 0 ? get(u.orderTargetId) : nullptr;
        if (ot && !ot->dead && ot->operational()) combatTarget = ot->id;
        else {
            u.orderTargetId = -1;
            // OrderableUnit.getTargetSearchRange:3498 -- the passive sweep looks as far as the weapon
            // reaches plus the hull's own bonus, which is 250px for a melee unit and nothing else.
            std::vector<Unit*> foes = queryNearestEnemy(u.team, u.pos, tpl.attackRange + tpl.meleeRange, &tpl);
            Unit* best = nullptr; float bd = 1e18f;
            for (auto* f : foes) { float d = u.pos.dist(f->pos); if (d < bd) { bd = d; best = f; } }
            if (best) combatTarget = best->id;
        }
    }
    Unit* tgt = combatTarget >= 0 ? get(combatTarget) : nullptr;
    u.targetId = combatTarget;

    // OrderableUnit.updateLoadIntoCommand:2031-2093 -- walk to the carrier and board it once within
    // getTransportInteractionRange(), which is a flat 21 px on BaseUnit.java:1867 and not a config key. The
    // order is dropped the tick the pairing stops qualifying (:2039-2041), which is what cancels the fifth
    // tank when a five-slot hold filled up while it was walking.
    if (u.loadTargetId >= 0) {
        Unit* tr = get(u.loadTargetId);
        if (!tr || tr->dead || !canLoadInto(*tr, u)) {
            u.loadTargetId = -1;
        } else if (u.pos.dist(tr->pos) <= 21.0f) {
            loadUnit(*tr, u);
        } else if (u.path.empty() || u.moveGoal.dist(tr->pos) > 18.0f) {
            computePath(*this, u, tr->pos);      // :2077 repathDistanceThreshold = 18
        }
    }

    // OrderableUnit.updateLoadUpCommand:1645-1681 -- the other way round: the carrier is ordered onto a
    // friendly hull and boards it itself (GameUI:2732, PC right-click). Same 21 px interaction range, same
    // 18 px repath threshold, and the order dies as soon as the pairing stops qualifying.
    if (u.loadUpTargetId >= 0) {
        Unit* ride = get(u.loadUpTargetId);
        if (!ride || ride->dead || !canLoadInto(u, *ride)) {
            u.loadUpTargetId = -1;
        } else if (u.pos.dist(ride->pos) <= 21.0f) {
            loadUnit(u, *ride);
        } else if (u.path.empty() || u.moveGoal.dist(ride->pos) > 18.0f) {
            computePath(*this, u, ride->pos);
        }
    }

    // whenBuilding_cannotMove: a queued self action that roots the hull while it charges (dive, blink)
    bool frozen = u.upgradeIdx >= 0 && u.upgradeIdx < (int)tpl.actions.size() && tpl.actions[u.upgradeIdx].cannotMove;
    float moveSpeed = u.moveSpeedOverride >= 0 ? u.moveSpeedOverride : tpl.moveSpeed;
    if (!u.isStatic() && u.parentId < 0) {
        if (u.repathTimer > 0) u.repathTimer--;
        // [movement] moveIgnoringBody is the key that lets a hull steer apart from its facing
        // (OrderableUnit.java:876-878). RWX gives air and hover units that behaviour through their
        // movement type, so those keep the heuristic on top of whatever a mod declares.
        bool ignoreBody = tpl.moveIgnoringBody || tpl.moveType == MoveType::AIR || tpl.moveType == MoveType::HOVER;
        // [movement] moveSlidingMode turns the position into an integrated velocity (:893-918)
        bool sliding = tpl.moveSlidingMode;
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
                if (distSq < 324.0f && tpl.decel < 0.13f && moveSpeed > 1.0f) want *= 0.5f;
                if (distSq < 169.0f && tpl.decel < 0.15f && moveSpeed > 0.9f) want *= 0.5f;
            }
            if (distSq < 9.0f) want = 0;
        }
        if (frozen) want = 0;
        // :3003-3006 while flipping the unit backs up at reverseSpeedPercentage
        if (u.isRotating && !ignoreBody) want = -want * revPct;
        if (sliding) {
            u.throttle = want;   // :3009-3010 the accel/decel ramp is skipped altogether
        } else {
            float rate = want > u.throttle ? tpl.accel : tpl.decel;
            if (std::fabs(want - u.throttle) <= rate) u.throttle = want;
            else u.throttle += (want > u.throttle ? rate : -rate);
        }

        // position: a normal hull drives along its facing and never sideways (:879-892)
        float a = ignoreBody ? bearing : u.facing;
        auto stepTo = [&](Vec2 d) {
            Vec2 np = u.pos + d;
            bool trapped = !passableAt(*this, u.pos, tpl.moveType);
            if (trapped || passableAt(*this, np, tpl.moveType)) u.pos = np;
            else {
                Vec2 nxp{ np.x, u.pos.y }, nyp{ u.pos.x, np.y };
                if (passableAt(*this, nxp, tpl.moveType)) u.pos = nxp;
                else if (passableAt(*this, nyp, tpl.moveType)) u.pos = nyp;
                else u.throttle *= 0.5f;
            }
        };
        if (sliding) {
            // OrderableUnit.java:893-918: the wanted velocity is dir*moveSpeed*throttle; the gap to it
            // closes at (accel while throttling, decel otherwise) * 1.41 per tick and snaps once that gap
            // is inside one step, and a gap wider than the unit's own top speed sheds 5% of the current
            // velocity first. The position is then just the velocity.
            Vec2 wantV{ 0, 0 };
            if (u.throttle != 0.0f) wantV = Vec2{ std::cos(a), std::sin(a) } * (moveSpeed * u.throttle);
            float gap = u.vel.dist(wantV);
            float vRate = (u.throttle != 0.0f ? tpl.accel : tpl.decel) * 1.41f;
            if (gap > moveSpeed) u.vel = u.vel * 0.95f;
            if (gap < vRate) u.vel = wantV;
            else {
                float ang = std::atan2(wantV.y - u.vel.y, wantV.x - u.vel.x);
                u.vel = u.vel + Vec2{ std::cos(ang), std::sin(ang) } * vRate;
            }
            if (frozen) u.vel = Vec2{};
            if (u.vel.len() > 1e-4f) stepTo(u.vel);
        } else if (std::fabs(u.throttle) > 1e-4f) {
            stepTo(Vec2{ std::cos(a), std::sin(a) } * (moveSpeed * u.throttle));
        }
        // OrderableUnit.java:873, 920 -- a unit only takes part in the collision pass when it had
        // throttle or carried velocity; whether the step above actually moved it is not asked.
        u.moved = u.throttle != 0.0f || u.vel.len() > 1e-6f;
        // :2841-2850 stateFlag2 is raised by the order, not by the momentum: a path node still to be
        // reached, or else a live attack target this unit is holding position against.
        u.driven = pathing || (u.orderTargetId >= 0 && tgt && !tgt->dead);
        if (pathing) {
            float arrive = lastNode ? 6.0f : 16.0f; // RW: distSq<256 mid, <16 final, cmd radius 7
            if (dist <= std::max(arrive, moveSpeed)) {
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
            else if (u.stuckTimer > 90 && u.repathTimer <= 0 && !frozen) { u.stuckTimer = 0; computePath(*this, u, u.hasGoal ? u.moveGoal : node); }
        }
    }
    if (!u.attachments.empty()) updateAttachments(u);
    if (!u.isStatic() && u.parentId < 0 && u.pathIdx >= (int)u.path.size() && u.hasGoal && u.repathTimer <= 0 && u.pos.dist(u.moveGoal) > 8.0f) {
        computePath(*this, u, u.moveGoal); // retry after blockage cleared
    }

    // OrderableUnit.update:865-890 - every barrel keeps its own cooldown, warmup and aim, so the
    // whole turret pass now runs per [turret_N] instead of on one shared barrel.
    if (u.operational() && !tpl.turrets.empty()) updateTurrets(u, tgt);
    // The [turret] interceptProjectiles / laserDefence hook is not part of this pass -- see the dispatch at
    // the head of updateUnit, which mirrors RWX running it before the energy refill and the guns.

    if (u.attackMove && u.path.empty() && combatTarget < 0) u.attackMove = false;
    // OrderableUnit.updateAttackTargeting:3215-3221 -- a hull whose target has walked back outside the
    // engagement range does not shrug it off: when nothing else is being followed (:3215 demands
    // !isPathingActive) a chase-attacker raises the path onto the target with pathTargetRadius 0 and
    // canAttack true. [attack] isMelee is the only thing that makes a hull a chase-attacker in RP, since
    // the other three cases are the out-of-range / guard-area / aggressive attack modes (:3470-3472) and
    // those are per-player orders RP has no UI for; an ordered hull chasing its order target is RP's
    // stand-in for the same code path.
    if (tgt && u.path.empty() && !u.isStatic() && u.parentId < 0 && u.repathTimer <= 0 && (tpl.isMelee || u.orderTargetId >= 0) &&
        u.pos.dist(tgt->pos) > engagementRangeOf(u, *tgt)) {
        // the same engagement stop as the order: a melee hull is chased onto the target, pathTargetRadius 0
        float stop = tpl.isMelee ? 0.0f : std::max(20.0f, tpl.attackRange * 0.8f - tgt->tpl->radius);
        Vec2 away = (u.pos - tgt->pos).norm();
        computePath(*this, u, tgt->pos + away * stop);
    }

    // CustomUnit.update:1788-1795 runs the construction animation instead of the normal pick, and the
    // linked variant is not clocked by ticks at all: currentTime is pinned to buildProgress with
    // speedMultiplier 0, so [animation_build]'s body_0 {scale:0} .. body_1 {scale:1.2} is one long
    // frame the foundation grows through as it is paid for. With neither section the update requests
    // nothing at all, and an unfinished building holds its default frame. Otherwise :1893-1897 lets
    // isMoving pick the tread strip, and the idle strip is only requested when nothing else owns the
    // clock, so the priority-11 shot recoil of :1335 gets to finish.
    if (u.underConstruction && u.buildProgress < 1.0f) {
        if (tpl.animBuild.valid()) {
            requestAnimation(u, &tpl.animBuild, 99, false);
            u.animTime = u.buildProgress;
            u.animSpeed = 0.0f;
        } else if (tpl.animUnderConstruction.valid())
            requestAnimation(u, &tpl.animUnderConstruction, 8, false);
    } else if (u.driven) {
        requestAnimation(u, tpl.animMoving.valid() ? &tpl.animMoving : nullptr, 3, false);
    } else if (!u.animPlaying || u.animCur == &tpl.animIdle) {
        requestAnimation(u, tpl.animIdle.valid() ? &tpl.animIdle : nullptr, 2, false);
    }
    if (u.operational()) {
        if (!u.prodQueue.empty() && u.prodProgress >= tpl.animQueued.queuedUnitPlayAt)
            requestAnimation(u, &tpl.animQueued, 5, false);
        if (u.upgradeIdx >= 0 && u.upgradeIdx < (int)tpl.actions.size()) {
            auto it = tpl.animations.find(tpl.actions[u.upgradeIdx].buildAnimation);
            if (it != tpl.animations.end()) requestAnimation(u, &it->second, 10, false);
        }
    }
    updateMovementFx(u);
    if (!u.legs.empty()) updateLegs(u, 1.0f);
    stepAnimation(u, 1.0f);
    // CommandCenter.update:203-220 -- the radar blink is the class's own frame clock: frameIndex walks
    // 0..6 and folds back to 0..3 (0,1,2,3,2,1,0), 5 ticks a step, then a 70-tick park at frame 0 when
    // the index wraps. It writes currentAnimationFrame straight, outside the animation controller.
    if (tpl.name == "commandCenter" && u.operational()) {
        u.ccFrameTimer = moveTowards(u.ccFrameTimer, 0.0f, 1.0f);
        if (u.ccFrameTimer == 0.0f) {
            u.ccFrameTimer = 5.0f;
            u.ccFrameIdx++;
            if (u.ccFrameIdx > 6) { u.ccFrameIdx = 0; u.ccFrameTimer = 70.0f; }
            u.animFrame = u.ccFrameIdx <= 3 ? u.ccFrameIdx : 6 - u.ccFrameIdx;
        }
    }
}

void Game::updateMovementFx(Unit& u) {
    if (u.dead || u.riding() || !u.operational() || u.throttle == 0.0f) return;
    const UnitTemplate& t = *u.tpl;
    if (!t.splashEffect && !t.dustEffect && t.movementEffect.empty() && t.movementEffectReverse.empty()) return;

    bool reverse = u.throttle < 0.0f;
    bool water = overWater(*this, u.pos);
    float rear = u.facing + 3.14159265f + (reverse ? 3.14159265f : 0.0f);
    if (t.splashEffect && (!reverse || t.splashEffectReverse)) {
        u.splashEffectTimer += 1.0f;
        // RWX clocks water wakes independently of movementEffectRate, including travel over land.
        if (u.splashEffectTimer > 10.0f) {
            u.splashEffectTimer = 0.0f;
            if (water) {
                Vec2 heading{ std::cos(rear), std::sin(rear) };
                Vec2 at = u.pos + heading * std::max(4.0f, t.radius - 6.0f);
                EffectTpl wake;
                wake.strip = 0;
                wake.frame = 13;
                wake.layer = 1;
                wake.life = 80.0f;
                wake.alpha = 0.8f;
                wake.scaleFrom = g_rng.range(0.6f, 0.8f);
                wake.scaleTo = 1.5f;
                wake.startDirAtZero = true;
                wake.dirOffsetRandom = 180.0f;
                wake.xAbs = heading.x * 0.13f * g_rng.range(1.0f, 1.5f);
                wake.yAbs = heading.y * 0.13f * g_rng.range(1.0f, 1.5f);
                wake.xAbsRnd = wake.yAbsRnd = 0.01f;
                wake.attached = false;
                spawnFx(wake, u.tpl, at, 0.0f, rear * 57.29578f, nullptr);
            }
        }
    }

    u.movementEffectTimer += 1.0f;
    if (u.movementEffectTimer <= t.movementEffectRate) return;
    u.movementEffectTimer = 0.0f;
    float dirDeg = u.facing * 57.29578f;
    if (reverse && t.movementEffectReverseFlipEffects) dirDeg += 180.0f;
    FxOwner owner;
    owner.unitId = u.id; owner.pos = u.pos; owner.posZ = u.posZ;
    const auto& list = reverse ? t.movementEffectReverse : t.movementEffect;
    for (const FlameSpec& fs : list) spawnFxEntry(fs, u.tpl, u.pos, u.posZ, dirDeg, &owner);

    if (t.dustEffect && (!reverse || t.dustEffectReverse) && !water) {
        EffectTpl dust = muzzleEffect(4);
        dust.layer = 1;
        dust.attached = false;
        dust.pivotOffsetRandom = 7.0f;
        dust.xAbsRnd = dust.yAbsRnd = 0.15f;
        for (float side : { -20.0f, 20.0f }) {
            float angle = rear + side * 0.0174532924f;
            Vec2 at = u.pos + Vec2{ std::cos(angle), std::sin(angle) } * (t.radius - 1.0f);
            spawnFx(dust, u.tpl, at, u.posZ, rear * 57.29578f, nullptr);
        }
    }
}

void Game::initLegs(Unit& u) {
    if (u.legs.size() == u.tpl->legs.size()) return;
    u.legs.assign(u.tpl->legs.size(), LegState{});
    for (size_t i = 0; i < u.legs.size(); ++i) u.legs[i].alpha = u.tpl->legs[i].alpha;
    u.legPrevPos = u.pos;
    u.legPrevFacing = legFacing(u);
    u.legPrevZ = u.posZ;
    updateLegs(u, 0.0f);
    for (auto& leg : u.legs) leg.reset = true;
}

float Game::legFacing(const Unit& u) const {
    if (u.tpl->lockLegRot && !u.tur.empty()) return u.tur[0].angle;
    return u.facing;
}

static Vec2 legRotVec(Vec2 v, float a) {
    float c = std::cos(a), s = std::sin(a);
    return { c * v.x - s * v.y, s * v.x + c * v.y };
}

void Game::updateLegs(Unit& u, float f) {
    const UnitTemplate& t = *u.tpl;
    if (u.riding()) { for (auto& S : u.legs) S.reset = true; return; }
    float dir = legFacing(u);
    Vec2 dPos = u.pos - u.legPrevPos;
    float dDir = wrapAngle(dir - u.legPrevFacing);
    float dZ = u.posZ - u.legPrevZ;
    bool moved = dPos.x != 0.0f || dPos.y != 0.0f || dDir != 0.0f;
    u.legPrevPos = u.pos; u.legPrevFacing = dir; u.legPrevZ = u.posZ;
    // OrderableUnit.getMovementDeltaOffset(15):5132-5148, then the controller's own clamp (:165-174):
    // length = min(|look|^2 * 240, 15) along the look direction.
    Vec2 look;
    if (!u.isStatic() && u.operational()) {
        float heading = t.moveIgnoringBody ? u.targetFacing : u.facing;
        if (t.moveSlidingMode) look = u.vel * 15.0f;
        else if (u.throttle != 0.0f)
            look = Vec2{ std::cos(heading), std::sin(heading) } * (t.moveSpeed * u.throttle * 15.0f);
        float sq = look.x * look.x + look.y * look.y;
        if (sq > 0.0f) look = look.norm() * std::min(sq * 240.0f, 15.0f);
    }
    Vec2 aim{ std::cos(dir), std::sin(dir) }, right{ -aim.y, aim.x };
    int moving = 0, farIdx = 0; float farSq = 0;
    for (size_t i = 0; i < u.legs.size(); ++i) {
        LegState& S = u.legs[i];
        const LegTpl& C = t.legs[i];
        bool wasReset = false, shrinkNow = false;
        if (S.reset) {
            wasReset = true; S.reset = false; S.dirty = true;
            if (S.shrink) shrinkNow = true;
            S.shrink = false;
        }
        if (!C.targetHeightRelative) S.z -= dZ;
        if (!C.lockMovement) {
            if (moved) { S.foot = S.foot - dPos; S.dirty = true; }
        } else if (moved && dDir != 0.0f) {
            S.foot = legRotVec(S.foot, dDir);
            S.angle += dDir;
            S.dirty = true;
        }
        if (C.hidden) continue;
        if (C.spinRate != 0.0f) S.spin = std::fmod(S.spin + C.spinRate * f, 360.0f);
        if (S.dirty) {
            // body frame: x right of the nose, y forward -- same convention as the turret pivots
            Vec2 root = aim * (C.y + S.animOffset.y) + right * (C.x + S.animOffset.x);
            S.target = root;
            if (wasReset) {
                S.foot = root;
                S.angle = dir + C.endDirOffset * 0.0174533f;
                S.dirty = true;
                if (shrinkNow) { S.foot = S.foot * 0.6f; S.z = -3.0f; }
            }
            if (!C.lockMovement) S.target = S.target + look * C.estimatingMultiplier;
            Vec2 d = S.foot - S.target;
            S.distSq = d.x * d.x + d.y * d.y;
            if (S.distSq > farSq) { farSq = S.distSq; farIdx = (int)i; }
            if (S.stepping && !C.lockMovement) moving++;
        }
    }
    for (size_t i = 0; i < u.legs.size(); ++i) {
        LegState& S = u.legs[i];
        const LegTpl& C = t.legs[i];
        if (C.hidden) continue;
        float tz = C.targetHeight;
        if (!C.targetHeightRelative) tz -= u.posZ;
        float lz = tz + C.liftHeight;
        if (S.distSq > 90000.0f) S.foot = Vec2{ C.x, C.y };
        else if (S.distSq > C.hardLimit * C.hardLimit) {
            S.foot = S.target + (S.foot - S.target).norm() * C.hardLimit;
            S.stepping = true;
        }
        if (!S.stepping && S.z <= tz + 0.1f && moving < C.maxMovingLegs &&
            ((int)i == farIdx || !C.holdMoveOnlyIfFurthest)) {
            bool nb = false;
            if (C.checkNeighbours)
                for (int n : C.neighbours) if (u.legs[n].stepping) nb = true;
            float th = nb ? C.holdDisMax : C.holdDisMin;
            if (S.distSq > th * th) { S.stepping = true; moving++; }
        }
        if (C.lockMovement) S.stepping = true;
        if (!S.stepping) {
            S.warm = 0;
            if (S.z > tz) {
                S.z -= f * C.heightSpeed;
                if (S.z <= tz) {
                    S.z = tz;
                    Vec2 w = u.pos + S.foot;
                    int tx = (int)(w.x / TILE_SIZE), ty = (int)(w.y / TILE_SIZE);
                    S.onWater = map.inBounds(tx, ty) && map.at(tx, ty).water;
                    if (C.dust && !S.dustDone) {
                        S.dustDone = true;
                        EffectTpl e = muzzleEffect(S.onWater ? 5 : 4);
                        if (S.onWater) { e.life = 120; e.scaleFrom = 0.2f; e.scaleTo = 0.9f; e.alpha = 0.5f; }
                        else { e.scaleFrom = 1.6f; e.scaleTo = 2.8f; e.yRel = 0.0f; }
                        e.layer = 1;
                        e.attached = false;
                        spawnFx(e, u.tpl, w, S.z, S.angle * 57.29578f, nullptr);
                    }
                }
            } else if (S.onWater && S.z > tz - 3.0f) S.z -= f * 0.3f;
        } else if (S.z > lz || C.lockMovement) {
            float step = f * C.moveSpeed;
            if (S.distSq <= step * step) {
                S.foot = S.target; S.dirty = true; S.stepping = false;
            } else {
                S.foot = S.foot + (S.target - S.foot).norm() * step;
                S.dirty = true;
            }
            if (C.lockMovement && S.z > tz) {
                S.z -= f * C.heightSpeed;
                if (S.z <= tz) S.z = tz;
            }
            float want = dir + C.endDirOffset * 0.0174533f;
            float maxStep = C.rotateSpeed * 0.0174533f * f;
            S.angle += std::max(-maxStep, std::min(maxStep, angleDiff(S.angle, want)));
            S.dustDone = false;
        } else if (S.warm < C.moveWarmUp) S.warm += f;
        else S.z += f * C.heightSpeed;
    }
}

// RWX OrderableUnit.updateAttackTargeting(:3120) + advanceMovementLevelTowardTarget(:3239): each
// barrel holds its own target, aims with its own turn speed, charges its own warmup and counts its
// own delay down, and an attachedTo barrel simply rides along with the turret it is bolted to.
static constexpr int kMaxTur = 32;   // RWX sizes the per-turret arrays at 31 (MultiTurretPassiveTargetCallback.java:14-24)

// OrderableUnit.M(i):4812-4827, run straight after the shot leaves (:3279): the barrel's aim point is
// re-rolled to a whole number of pixels in [-r, r) with r = target.radius * aimOffsetSpread truncated,
// x and y drawn separately (RWX seeds them 1+i and 2+i apart). The roll always reads the *hull's*
// attackTarget, not the barrel's own lock, which is why a multi-target unit spreads its fire over the
// target it was ordered onto. A spread of 0, or a target small enough that radius*spread < 1 px, puts
// the barrel back on the centre.
static void rerollAimOffset(TurretState& s, const Unit* roll, float spread) {
    s.aimOff = Vec2{};
    if (!roll || !roll->tpl || spread <= 0.0f) return;
    const int r = (int)(roll->tpl->radius * spread);
    if (r <= 0) return;
    s.aimOff = Vec2{ (float)((int)g_rng.range(0.0f, (float)(2 * r)) - r),
                     (float)((int)g_rng.range(0.0f, (float)(2 * r)) - r) };
}

void Game::updateTurrets(Unit& u, Unit* unitTarget) {
    const UnitTemplate& tpl = *u.tpl;
    if (tpl.turrets.empty()) return;
    initTurrets(u);            // an upgrade can swap the template and with it the barrel count
    // CustomUnit.supportsIndependentTurretTargets:4216-4223 -- the hull has to both carry
    // [attack] turretMultiTargeting and have more than one barrel (super returns getTechLevel() > 1,
    // which for CustomUnit:2844-2849 is just the turret count). Without the flag every barrel follows
    // the unit's target however many guns it has, which is most of the mod set.
    const bool independent = tpl.turretMultiTargeting && tpl.turrets.size() > 1;
    const int n = std::min((int)tpl.turrets.size(), kMaxTur);
    float turn[kMaxTur] = {};    // each barrel's own turn this tick, applied to the barrels bolted to it
    Unit* target[kMaxTur] = {};
    bool anyReady = false, swept = false;
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
            // OrderableUnit.syncAttackTargetToMovementLevels:3081-3098 -- the unit's own order target
            // goes to every barrel that can engage it, in preference to a target that barrel had
            // picked for itself; only a barrel that cannot hit the order target keeps its own lock, and
            // it drops that lock when the lock *is* the order target, leaving the sweep to refill it.
            if (unitTarget && turretSees(*this, u, i, *unitTarget)) {
                t = unitTarget;
            } else if (independent) {
                Unit* own = get(s.targetId);
                if (own && (!unitTarget || own->id != unitTarget->id) && turretSees(*this, u, i, *own)) t = own;
                // MultiTurretPassiveTargetCallback:71, run at most once per sweep interval (:3140-3142)
                else if (u.turretScanTimer == 0) { t = nearestInTurretRange(u, i); swept = true; }
            }
        }
        target[i] = t;
        s.targetId = t ? t->id : -1;

        float before = s.angle, residual;
        if (tpl.isFixedFiring) {
            // OrderableUnit.java:3185-3197 -- a fixed-firing unit never turns its barrel at the target. It
            // turns the HULL with faceTowardPosition and leaves targetX to addRotation (:1325-1337), which
            // shifts every barrel by exactly the hull's step, so targetX - rotationSpeed stays pinned at the
            // value the unit was reset to (CustomUnit:1044 and j():1343 both use rotationSpeed + idleDir).
            // Chasing u.facing at tur.turnSpeed instead left the mount a few degrees behind the hull it is
            // bolted to, and canAttackMaxAngle's 5 degrees is measured off that same hull turn (:3194).
            if (tpl.turretRotateWithBody)
                s.angle = wrapAngle(u.facing + tur.idleDir * 0.0174533f);
            residual = tpl.turretRotateWithBody ? 0.0f : angleDiff(u.facing, u.targetFacing);
        } else if (t) {
            Vec2 pv = turretPivot(u, i);
            // :3169-3173 -- the barrel points at the target's position plus this shot's aim jitter, so
            // with the default 0.6 spread a round is lined up somewhere on the hull rather than always
            // dead centre.
            Vec2 at{ t->pos.x + s.aimOff.x, t->pos.y + s.aimOff.y };
            s.lastAim = s.angle;                               // :3177 targetY = targetX, before the step
            residual = rotateTurret(s, tur, std::atan2(at.y - pv.y, at.x - pv.x));
            s.aimHold = 70.0f;                                 // :3176 m.a(70): idle steps wait while aiming
        } else {
            // OrderableUnit.update:846-861 -- the idle step only runs once velocityY has decayed to 0, so
            // a barrel keeps its last aim for ~70 ticks after the target is gone before it sweeps or resets.
            if (s.aimHold > 0.0f) {
                s.aimHold = moveTowards(s.aimHold, 0.0f, 1.0f);
                residual = 0.0f;
            } else if (tur.idleSweepAngle != 0.0f && sweepCondOk(u, tur)) {
                residual = idleSweep(s, tur, u, tpl);          // CustomUnit.b(i,f):3996-4029
            } else if (tur.idleSpin != 0.0f) {
                // :4030-4041 -- targetX is the drawn barrel angle itself, so the spin adds straight to
                // it at idleSpin deg/tick, no turnSpeed chase, and returns false to skip the rest reset.
                s.angle = wrapAngle(s.angle + tur.idleSpin * 0.0174533f);
                residual = 0.0f;
            } else if (tur.shouldResetTurret) {
                // no target at all: walk back to C(i) = hull angle + idleDir (:846-857)
                float rest = u.facing + tur.idleDir * 0.0174533f;
                if (u.isRotating && tpl.reverseSpeed > 0.95f) rest += 3.14159265f;
                residual = rotateTurret(s, tur, rest);
            } else {
                residual = 0.0f;                               // shouldResetTurret:false parks it here
            }
        }
        turn[i] = angleDiff(before, s.angle);
        if (std::fabs(residual) * 57.29578f < tur.maxAngle) s.active = true;   // canAttackMaxAngle
    }
    // :3141/:3148 - whichever sweep ran arms the next one, staggered by position so a whole army does
    // not re-query on the same tick. RWX has two timers for this (turretRotation with no order
    // target, targetTurretRotation with one) but both hold the same 20+x%5+y%5, so one is enough.
    if (swept && u.turretScanTimer == 0)
        u.turretScanTimer = 20 + (int)std::fmod(u.pos.x, 5.0f) + (int)std::fmod(u.pos.y, 5.0f);

    // --- pass 2: warmup and firing (advanceMovementLevelTowardTarget:3239-3282)
    for (int i = 0; i < n; ++i) {
        const TurretTpl& tur = tpl.turrets[i];
        TurretState& s = u.tur[i];
        Unit* t = target[i];
        int pvt = tur.attached;
        if (pvt >= 0 && pvt < n) {
            // RWX CustomUnit.a(i, f):1341-1350 - the parent's turn this tick is added to the child,
            // so a barrel bolted to another aims and recoils with it instead of lagging behind. A slave
            // (x != -1) instead HARD-copies the master's aim at advanceMovementLevelTowardTarget:3242-3243
            // (targetX = movementLevels[master].targetX), pointing exactly where the master points.
            if (tur.slaveMaster >= 0) s.angle = u.tur[pvt].angle;
            else s.angle = wrapAngle(s.angle + turn[pvt]);
            s.angVel = u.tur[pvt].angVel;
            s.active = u.tur[pvt].active;
            t = target[pvt];
            s.targetId = t ? t->id : -1;
        }
        // u(i) = isActive && a(TurretConfig) (CustomUnit:2748 -> :1340-1348, OrderableUnit:4597).
        // The energy gate sits here, before any charging: a barrel the pool cannot cover never has
        // advance called for it (OrderableUnit:3210), so it neither charges nor holds off the leak.
        if (!s.active || !t) continue;
        if (tur.energyUsage > 0.0f && (tur.energyUsage > u.energy || u.energyRecharging)) continue;
        // a(TurretConfig):1344 -- resourceUsage is a second affordability gate beside energy.
        if (tur.resourceUsage.any() && !canPayResourceCost(*this, u, tur.resourceUsage)) continue;
        // :3259 re-runs canAttack at the fire moment, which is the only limitingAngle test an attached
        // slave ever gets (it never acquires a target on its own in pass 1).
        if (!limitingAngleOk(u, i, *t)) continue;
        // z: any barrel aimed and off cooldown suppresses the charge leak (OrderableUnit:3207).
        // r(i)/canShoot is not part of this test -- only the fire branch below checks it (:3258).
        if (s.cooldown == 0.0f) anyReady = true;
        // warmupNoReset charges on every aimed tick, cooldown or not; only the fire itself waits for
        // rotation == 0 (OrderableUnit:3247-3257). This is what holds a flame turret's glow full
        // between shots instead of bleeding it down to the fire tick alone.
        if (tur.warmNoReset)
            s.warm = std::min(s.warm + 1.0f, tur.warmup);
        if (s.cooldown != 0.0f || !tur.canShoot) continue;
        bool ready;
        if (tur.warmNoReset) {
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
        fire(u, t, i);
        // :3279 -- the barrel gets a fresh aim point once the shot has taken the old one with it. M(i)
        // reads the hull's attackTarget, not this barrel's own lock, so a multi-targeting hull with no
        // order target fires dead centre; the unit target is what RP's `unitTarget` holds.
        rerollAimOffset(s, unitTarget, tur.aimOffsetSpread);
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
    // MultiTurretPassiveTargetCallback.prepareForUnit:44-59 sizes each barrel's hunt by
    // min([turret_N] limitingRange, getTargetSearchRange), and CustomUnit.m():1636-1638 makes that
    // second term the hull's [attack] maxAttackRange -- so a barrel with no limitingRange of its own
    // still stops at the hull's range instead of acquiring across the map.
    const UnitTemplate& tpl = *u.tpl;
    float maxD = tpl.attackRange;
    if (tpl.turrets[i].range > 0 && tpl.turrets[i].range < maxD) maxD = tpl.turrets[i].range;
    Unit* best = nullptr; float bd = 1e18f;
    for (auto& o : units) {
        if (o.dead || &o == &u || !turretSees(*this, u, i, o)) continue;
        float d = u.pos.dist(o.pos);
        if (d > maxD) continue;
        if (d < bd) { bd = d; best = const_cast<Unit*>(&o); }
    }
    return best;
}

// RWX CustomUnit.a(i, false):2870-2916, in its own order: an attachedTo barrel is bolted to its
// parent's pivot and carries its x/y offsets rotated by the *parent barrel's* aim (:2889, which is an
// absolute angle, not a delta off the hull), then this barrel's recoil slides it along its own aim
// (:2891-2902, only while the cooldown still counts), and only then the barrel's own x/y offsets.
// This is the point both the muzzle and the turret sprite are drawn from (GameViewUtils:183-192), so
// the shoot_flame that CustomUnit.java:1594 spawns off D(i) rides it too. Every offset here is already
// world px: :2907-2911 adds them raw, with no imageScale and no body-size factor anywhere in the chain.
Vec2 turretPivot(const Unit& u, int i) {
    const UnitTemplate& tpl = *u.tpl;
    if (i < 0 || i >= (int)tpl.turrets.size()) return u.pos;
    const TurretTpl& tur = tpl.turrets[i];
    Vec2 base = u.pos;
    float f2 = u.facing;
    if (tur.attached >= 0 && tur.attached != i) {
        base = turretPivot(u, tur.attached);
        f2 = (tur.attached < (int)u.tur.size()) ? u.tur[tur.attached].angle : u.facing;
    }
    float rc = turretRecoil(u, i);
    if (rc != 0.0f && i < (int)u.tur.size()) {
        float a = u.tur[i].angle;
        base = base + Vec2{ std::cos(a) * rc, std::sin(a) * rc };
    }
    float cs = std::cos(f2), sn = std::sin(f2);
    return base + Vec2{ cs * tur.y - sn * tur.x, sn * tur.y + cs * tur.x };
}

// RWX CustomUnit.D(i):4465-4480 -- the muzzle point: pivot F(i) plus barrelY along the muzzle direction
// and barrelX across it. The direction is `E() ? rotationSpeed : targetX` (:4474): with [attack]
// isFixedFiring the mouth swings with the HULL while the turret sprite and the flame's own angle still
// ride movementLevels[i].targetX (GameViewUtils:191 and CustomUnit:1594). barrelY defaults to [attack]
// turretSize, barrelOffsetX_onOddShots jumps sideways on every other shot, and no offset in the chain
// is multiplied by imageScale. The chargeEffectImage is drawn centred on this same point.
Vec2 muzzlePoint(const Unit& u, int i) {
    const UnitTemplate& tpl = *u.tpl;
    if (i < 0 || i >= (int)tpl.turrets.size() || i >= (int)u.tur.size()) return u.pos;
    const TurretTpl& tur = tpl.turrets[i];
    const TurretState& s = u.tur[i];
    float muzzleAng = tpl.isFixedFiring ? u.facing : s.angle;
    Vec2 aim{ std::cos(muzzleAng), std::sin(muzzleAng) };
    Vec2 right{ -aim.y, aim.x };
    float barrelY = tur.barrelY >= 0 ? tur.barrelY : tpl.turretSize;
    float bx = tur.barrelX;
    if (tur.barrelXOdd != 0.0f && s.flip) bx += tur.barrelXOdd;
    return turretPivot(u, i) + (aim * barrelY + right * bx);
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

// RWX CustomProjectileTemplate.a():377-405 with BaseUnit.a():2122-2142 -- a targetGround weapon is never
// guided onto the unit; the shell is handed a point instead. The target's own velocity carries that point
// up to `life` ticks of flight into it (three fixed-point passes over the flight time at leadSpeed px per
// tick), it is pulled back onto the shooter's [attack] maxAttackRange circle when the lead would put it
// out of reach, and targetGroundSpread then scatters it. RWX leaves Projectile.l null here, so a direct
// hit is impossible for the whole flight and only the splash bites.
// `clampToRange` is BaseUnit.a()'s fifth argument: the barrel that fires hands it m(), the unit's own attack
// range (CustomUnit.java:1585), while a spawn list hands it a flat -1 (:383,395 read through
// UnitSpawnList.java:171) -- a hand-me-down round is therefore free to lead its target well past the reach
// of the barrel that launched the original. Which is also why `shooter` may be null here: only the clamped
// branch has any use for the hull.
static void aimAtGroundPoint(Projectile& p, const Unit* shooter, const Unit& target,
                             const ProjectileTpl& pt, float leadSpeed, bool clampToRange = true) {
    float time = 0.0f;
    if (leadSpeed > 0.1f && target.moved) {
        for (int i = 0; i < 3; i++)
            time = p.pos.dist(target.pos + target.vel * time) / leadSpeed;
    }
    if (time > (float)pt.life) time = (float)pt.life;
    Vec2 at = target.pos + target.vel * time;
    float range = (shooter && shooter->tpl) ? shooter->tpl->attackRange : 0.0f;   // BaseUnit.m()
    if (!clampToRange) range = -1.0f;   // BaseUnit.a()'s fifth argument; see the signature note above
    float d = p.pos.dist(at);
    if (range >= 0.0f && range * range < d * d) {
        float ang = std::atan2(at.y - p.pos.y, at.x - p.pos.x);
        at = p.pos + Vec2{ std::cos(ang), std::sin(ang) } * range;
    }
    if (pt.targetGroundSpread > 0.0f) {
        at.x += g_rng.range(-pt.targetGroundSpread, pt.targetGroundSpread);
        at.y += g_rng.range(-pt.targetGroundSpread, pt.targetGroundSpread);
    }
    p.unguided = true;
    p.targetId = -1;
    p.dest = at;
}

// CustomUnit.a(BaseUnit,int,CustomProjectileTemplate,float,float,float,float):1368-1518 is where a template
// becomes a shot, and RWX runs the whole of it twice over: for the round a barrel lets go (CustomUnit:1573-1585)
// and for every round a spawn list hands off (UnitSpawnList:169-171), the only difference being the turret
// index, which the second passes as -1. Everything below therefore comes off the template; what the caller
// owes is the position, the heading and the target or frozen point, all three of which have to be in place
// first -- :1417-1419 starts the drawn angle on az, and the shot's velocity is az and the speed together.
static void applyTemplate(Projectile& p, const ProjectileTpl& pt) {
    p.tags = pt.tags;                     // :1503, the names an interceptor hunt reads
    // :1462 copies the instant flag onto the shot (RWX's Projectile.A), and :1401-1404 reads deflectionPower
    // the one way it is ever read: under 0.5 buys the round immunity from a laser defence beam, anything else
    // is the health the beam has to burn through.
    p.instant = pt.instant;
    p.beamImmune = pt.deflectionPower < 0.5f;
    p.deflectHp = pt.deflectionPower;
    // CustomUnit.a:1398,1462-1464 -- the beam/nuke/teleport flags travel onto the shot.
    p.nuke = pt.nukeWeapon;
    p.teleportSource = pt.teleportSource;
    p.laserEffect = pt.laserEffect;
    p.lightingEffect = pt.lightingEffect;
    p.beamImage = pt.beamImage; p.beamImageStart = pt.beamImageStart; p.beamImageEnd = pt.beamImageEnd;
    p.beamStartRotated = pt.beamImageStartRotated; p.beamEndRotated = pt.beamImageEndRotated;
    p.beamImageOffsetRate = pt.beamImageOffsetRate;
    p.lightCastOnGround = pt.lightCastOnGround;
    p.isBeam = !pt.beamImage.empty() || pt.laserEffect || pt.lightingEffect;
    p.reuseLast = pt.instantReuseLast;
    p.reuseKeepAreaList = pt.instantReuseLastKeepAreaDamageList;
    p.moveWithParent = pt.moveWithParent;
    p.interceptLifeOnly = pt.interceptRemoveTargetLifeOnly;
    p.damage = pt.directDamage; p.areaDamage = pt.areaDamage; p.areaRadius = pt.areaRadius;
    p.speed = pt.speed; p.life = pt.life;
    p.frame = pt.frame; p.drawType = pt.drawType; p.shadowFrame = pt.shadowFrame;
    p.drawSize = pt.drawSize; p.color = pt.color;
    p.lightColor = pt.lightColor; p.lightSize = pt.lightSize;
    p.trail = pt.trail; p.trailRate = pt.trailRate;
    p.largeHit = pt.largeHit;
    p.flameWeapon = pt.flameWeapon;
    p.turnSpeed = pt.turnSpeed;
    p.speedTarget = pt.targetSpeed; p.speedAccel = pt.targetSpeedAccel;
    if (pt.speedSpread > 0) p.speed += g_rng.range(-pt.speedSpread, pt.speedSpread);
    p.wobbleAmp = pt.wobbleAmp; p.wobbleFreq = pt.wobbleFreq;
    p.wobblePhase = g_rng.range(0.0f, 1.0f);   // RWX Projectile.I
    // The vertical half of the shot's own motion (:844-856) and the arc it may fly (:857-888). A template
    // that leaves the two ballistic heights at -1 does not overwrite the shot's 40 / 60 defaults
    // (CustomUnit.java:1490-1495).
    p.velZ = pt.launchVelZ; p.gravity = pt.gravity; p.trueGravity = pt.trueGravity;
    p.ballistic = pt.ballistic;
    p.ballisticDelayHeight = pt.ballisticDelayHeight != -1.0f ? pt.ballisticDelayHeight : 40.0f;
    p.ballisticHeight = pt.ballisticHeight != -1.0f ? pt.ballisticHeight : 60.0f;
    p.autoRetarget = pt.autoRetarget;
    p.autoRetargetRange = pt.autoRetargetRange;
    p.autoRetargetLead = pt.autoRetargetLead;
    p.buildingMult = pt.buildingDamageMult;
    p.shieldMult = pt.shieldDamageMult;
    p.deflectMult = pt.shieldDeflectMult;
    p.hullMult = pt.hullDamageMult;
    p.armourIgnore = pt.armourIgnore;
    // CustomUnit.java:1387-1397, in the same order: aa is the enemy-only guard, so clearing it is what
    // lets the ring touch the firing team's own hulls, and l (only-ignoreEnemy) clears aa *and* sets ab,
    // which then excludes every other colour. m is only ever an override toward the wider burst -- an ini
    // that never mentions it keeps the constructor's ae gate (Projectile:196).
    if (pt.friendlyFireIgnoresEnemy) { p.damagesAllies = true; p.alliesOnly = true; }
    else { p.damagesAllies = pt.friendlyFire; p.alliesOnly = false; }
    if (pt.areaHitAirAndLandAtSameTime) p.hitsAirAndLand = true;
    p.hitsUnderwater = pt.areaHitUnderwaterAlways;
    p.noFalloff = pt.areaDamageNoFalloff;
    p.ignoreCloserThan = pt.areaIgnoreUnitsCloserThan;
    p.radiusFromEdge = pt.areaRadiusFromEdge;
    p.expandTotal = pt.areaExpandTime > 0 ? pt.areaExpandTime : 0.0f;
    // :1417-1419 -- aT starts on the firing angle before it starts chasing it.
    p.drawAngle = p.heading;
    p.vel = Vec2{ std::cos(p.heading), std::sin(p.heading) } * p.speed;
}

Projectile* Game::fire(Unit& u, Unit* target, int ti, Vec2 groundAt) {
    const UnitTemplate& tpl = *u.tpl;
    if (ti < 0 || ti >= (int)tpl.turrets.size()) return nullptr;
    const TurretTpl& tur = tpl.turrets[ti];
    TurretState& s = u.tur[ti];
    // CustomUnit.a(TurretConfig):1335 asks for the attack animation with loop=true at priority 11, so a
    // firing unit shows the recoil strip instead of its moving cycle, and the shot restarts it.
    requestAnimation(u, tpl.animAttack.valid() ? &tpl.animAttack : nullptr, 11, true);
    auto shotAnimation = tpl.animations.find(tur.shootAnimation);
    if (shotAnimation != tpl.animations.end()) requestAnimation(u, &shotAnimation->second, 6, true);
    // :1336 -> b(TurretConfig):1349-1359, right after the animation request: the shot pays its energy,
    // and paying it down to less than the next shot needs trips the recharge latch.
    if (tur.energyUsage > 0.0f) {
        u.energy -= tur.energyUsage;
        if (u.energy < tur.energyUsage && tpl.energyNeedsToRechargeToFull) u.energyRecharging = true;
    }
    // b(TurretConfig):1357-1359 -- the per-shot resourceUsage is paid right after the energy, unconditionally
    // (the affordability gate already ran in updateTurrets), then the pools are clamped.
    if (tur.resourceUsage.any()) payResourceCost(*this, u, tur.resourceUsage);
    int pi = (tur.projectile >= 0 && tur.projectile < (int)tpl.projectiles.size()) ? tur.projectile : -1;
    ProjectileTpl pt = pi >= 0 ? tpl.projectiles[pi] : ProjectileTpl{};
    if (pi < 0) pt.directDamage = 10;   // RWX's stand-in [projectile_1] for a unit with none at all
    Projectile p;
    p.pos = muzzlePoint(u, ti);
    // RWX Projectile.posZ (:889-900 chases the aim height from the muzzle) starts at the shooter's own
    // height, and aimZ is the chased unit's live posZ (:754-761). No shipped turret sets [turret]
    // height, so the muzzle adds nothing on top of that.
    // An interceptor round is the case CustomProjectileTemplate.a():362-375 builds from a null target: it
    // keeps the frozen point it was fired at (n/o), its aim height becomes the ground (p = 0) and no unit is
    // recorded, so the 600 directDamage it carries can never land on a hull -- the round exists only to
    // reach Projectile.q. CustomUnit.java:1503 is where the tags travel onto the shot.
    p.posZ = u.posZ;
    p.team = u.team; p.sourceId = u.id;
    p.targetId = target ? target->id : -1;
    p.dest = target ? target->pos : groundAt;
    p.aimZ = target ? target->posZ : 0.0f;
    p.interceptor = !target;
    if (!target) p.unguided = pt.targetGround;   // RWX takes m straight from the template at :1461
    p.id = nextProjectileId++;
    p.from = &tpl; p.fromProj = pi;
    // :1587-1588 -- the shot takes the barrel's current aim jitter with it, so it keeps flying to the
    // same off-centre point instead of being re-pulled onto the hull centre every tick.
    p.aimOff = s.aimOff;
    // RWX Projectile.az is set from the turret's aim line, not from the target's position
    // (CustomUnit.java:1371), and the shot then flies along that heading.
    p.heading = s.angle;
    // The barrel's domains are the one part of a shot that RWX reads from the turret rather than the
    // projectile section, so they stay out of applyTemplate -- and out of a spawn list's rounds, which are
    // built with turret index -1 and take RWX's constructor defaults instead.
    p.atkFly = tur.canAttackFlying; p.atkLand = tur.canAttackLand; p.atkSea = tur.canAttackUnderwater;
    applyTemplate(p, pt);
    // CustomUnit.java:1412-1425 -- moveWithParent caches the firing turret's muzzle world position so the
    // round can add the parent's per-tick muzzle delta to its own pos (Projectile.update:816-840).
    if (pt.moveWithParent) {
        p.parentUnitId = u.id; p.parentTurret = ti;
        p.lastMuzzle = p.pos; p.lastMuzzleZ = p.posZ;
    }
    if (pt.targetGround && target)
        aimAtGroundPoint(p, &u, *target, pt, pt.targetSpeed != -1.0f ? pt.targetSpeed : p.speed);
    if (tur.shootFlame) {
        // RWX EffectManager.createFlameEffect(2): small plays frames 0..3 at 0.5/tick (8 ticks),
        // large/medium frames 1..4 at 0.4/tick (10 ticks); the flame is attached to the shooter.
        s.flameType = tur.shootFlame;
        s.flameT = tur.shootFlame == 1 ? 8.0f : 10.0f;
        s.flameAng = s.angle;
        s.flameOff = p.pos - u.pos;
    }
    // CustomUnit.java:1590-1594 -- the glow and the rest of the shoot_flame list. The angle they take is
    // movementLevels[i].targetX, so it stays the turret's own aim even when isFixedFiring swung the muzzle
    // with the hull, and RWX keeps that field in degrees.
    spawnMuzzleFx(tur, p.pos, u.posZ, s.angle * 57.29578f, u);
    // :1468-1469 -- the shot's own effectOnCreate, run at the muzzle before the round is registered.
    spawnBirthFx(p);
    if (pt.instant && target) {
        // RWX update:841,979: an instant shot never moves, it lands on the firing tick. It also skips
        // the height gate, and its burst height aX becomes the target's own posZ (:1026-1036).
        p.impactZ = target->posZ;
        impact(p, target, { target->pos.x + p.aimOff.x, target->pos.y + p.aimOff.y });
        // RWX Projectile.java:1038-1040 -- a beam (beamImage/laserEffect/lightingEffect) keeps shouldDraw
        // after landing and is only removed once its life runs out (:1411), so it is not dropped here.
        if (p.expandTotal <= 0 && !p.isBeam) {
            // Nothing is left to animate, so no caller gets a handle -- but the round still existed this
            // tick, and an attached effectOnCreate entry has to find it before the compaction drops it.
            p.dead = true;
            projectiles.push_back(p);
            return nullptr;
        }
    }
    // CustomUnit.G[i] / instantReuseLast (CustomUnit.java:1556-1582): a turret keeps one cached round and
    // resets it on each shot instead of spawning a new one. RP copies the freshly-built state into the
    // cached instance, keeping its stable id (and, optionally, its area-hit list) so attached effects and
    // interceptors that hold the handle keep seeing the same beam.
    if (pt.instantReuseLast && ti >= 0) {
        Projectile* c = s.beamProjId >= 0 ? findProjectile(s.beamProjId) : nullptr;
        if (c && !c->dead && c->from == &tpl && c->fromProj == pi) {
            int keepId = c->id;
            std::vector<int> keepHits;
            if (pt.instantReuseLastKeepAreaDamageList) keepHits = c->splashHits;
            *c = p;
            c->id = keepId;
            if (pt.instantReuseLastKeepAreaDamageList) c->splashHits = std::move(keepHits);
            return c;
        }
    }
    projectiles.push_back(p);
    if (pt.instantReuseLast && ti >= 0) s.beamProjId = projectiles.back().id;
    return &projectiles.back();
}

// UnitSpawnList.a():132-178, the list a round runs at Projectile.update:1411-1415 when its life gives out
// before it reaches anything. The entries go in file order, each one `count` times, and only a round that
// really spawns spends the entry's per-call maxSpawnLimit. RWX's second counter (i3) exists purely to seed
// getDeterministicRandomFloat, which RP's single generator has no use for, so it is not carried over. The
// new rounds are fired by the *original* unit -- the list passes the dying round's own j (:169) -- so a split
// keeps its predecessor's sourceId and team and is credited to the same hull.
void Game::spawnFromList(Projectile src, const UnitTemplate& owner,
                         const std::vector<ProjectileTpl::SpawnEntry>& list, int depth) {
    int made = 0;
    for (const ProjectileTpl::SpawnEntry& e : list) {
        if (e.proj < 0 || e.proj >= (int)owner.projectiles.size()) continue;   // "projectileType==null"
        for (int i = 0; i < e.count; ++i) {
            // :160 -- three gates, and the chance roll is only drawn when the entry is not a certainty.
            if (e.chance < 1.0f && g_rng.uniform() > e.chance) continue;
            if (made >= e.maxPerCall || depth > e.recursionLimit) continue;
            made++;
            const ProjectileTpl& pt = owner.projectiles[e.proj];
            // :150-158 in RWX's order: the absolute offsets, the height, then the direction, then the three
            // random spreads. All of it is px and *degrees* off the dying round -- Utility works in degrees,
            // which is why the heading only becomes a radian on the shot itself.
            float inDeg = src.heading * 57.29578f;
            Vec2 at{ src.pos.x + e.offsetX, src.pos.y + e.offsetY };
            float z = src.posZ + e.offsetZ;
            float dirDeg = inDeg + e.offsetDir;
            if (e.rndDir != 0.0f) dirDeg += g_rng.range(-e.rndDir, e.rndDir);
            if (e.rndX != 0.0f) at.x += g_rng.range(-e.rndX, e.rndX);
            if (e.rndY != 0.0f) at.y += g_rng.range(-e.rndY, e.rndY);
            // :159-163 swings the relative pair around the direction the list was *handed*, before offsetDir:
            // relY runs along the old heading, relX across it.
            if (e.relX != 0.0f || e.relY != 0.0f) {
                float c = std::cos(inDeg * 0.0174533f), s = std::sin(inDeg * 0.0174533f);
                at.x += c * e.relY - s * e.relX;
                at.y += s * e.relY + c * e.relX;
            }
            Projectile q;
            q.id = nextProjectileId++;
            q.team = src.team; q.sourceId = src.sourceId;
            q.from = &owner; q.fromProj = e.proj; q.recursion = depth;
            q.pos = at; q.posZ = z;
            q.heading = dirDeg * 0.0174533f;
            // :170-171 hands the dying round's own target to the same aiming helper a barrel runs
            // (CustomProjectileTemplate.a():362-405). With nothing left to chase the new shot goes unguided
            // to the point the old one was flying to; a targetGround template leads the survivor to a frozen
            // point of its own, and, unlike a barrel, with no range clamp at all; only an ordinary guided
            // template takes over the chase.
            Unit* tgt = src.targetId >= 0 ? get(src.targetId) : nullptr;
            if (tgt && tgt->dead) tgt = nullptr;
            if (!tgt) {
                q.unguided = true;
                q.dest = src.dest;
                if (pt.targetGroundSpread > 0.0f) {
                    q.dest.x += g_rng.range(-pt.targetGroundSpread, pt.targetGroundSpread);
                    q.dest.y += g_rng.range(-pt.targetGroundSpread, pt.targetGroundSpread);
                }
            } else if (!pt.targetGround) {
                q.targetId = tgt->id;
                q.dest = tgt->pos;
                q.aimZ = tgt->posZ;
            }
            applyTemplate(q, pt);
            if (tgt && pt.targetGround)
                aimAtGroundPoint(q, nullptr, *tgt, pt,
                                 pt.targetSpeed != -1.0f ? pt.targetSpeed : q.speed, false);
            // UnitSpawnList.java:174 calls the same factory a barrel does, so a handed-down round runs its
            // own section's effectOnCreate at the point it was born.
            spawnBirthFx(q);
            projectiles.push_back(q);
        }
    }
}

// OrderableUnit.hasBlockingUnitNearby:5049-5070 with isUnitOverlappingRadius:5033-5046, which is what
// UnitSpawner.a():366 asks through canPlaceAtCurrentPosition. `staticBody` is the new hull's bI(): a
// building is only blocked by another building (:5051, z is false for it), while a mobile hull is blocked
// by anything whose radius overlaps -- 9px flat against a building, else radius + radius with an 11px floor.
static bool blockedForSpawn(Game& g, Vec2 at, float rad, bool staticBody) {
    for (auto& o : g.units) {
        if (o.dead || o.riding()) continue;
        if (staticBody && !o.tpl->isStatic()) continue;
        float r = o.tpl->isStatic() ? 9.0f : std::max(11.0f, rad + o.tpl->radius);
        float dx = o.pos.x - at.x, dy = o.pos.y - at.y;
        if (dx * dx + dy * dy < r * r) return true;
    }
    return false;
}

// UnitSpawner.a():203-449, the run side of [projectile_N] spawnUnit (Projectile.update:1074-1076 hands it the
// impact point, a height of 0, the shot's own angle and the shooter's team). The shipped mod set writes seven
// of these lists and none of them sits on a section a barrel points at directly: the six nautilus ones belong
// to the scout-bot bomb, which the submarine's missile spawns and which really does drop a robotCrab where it
// lands, and the seventh is nuke_launcher.ini:296, [projectile_randomTankNuke] -- its `tank` is a type the set
// has, but no turret and no spawnProjectiles list names that section, so it never fires.
//
// Skipped as unreadable from here: the unit-cap guards (:251-280, RP has no cap), neutralTeam/aggressiveTeam
// (:307-313, RP has no neutral or unknown team), techLevel, gridAlign (an editor tile snap), falling,
// addResources, transportedUnitsToTransfer, spawnSource/copyWaypointsFrom.
void Game::spawnUnitsOnHit(Projectile& p, Vec2 at) {
    if (!p.from || p.fromProj < 0 || p.fromProj >= (int)p.from->projectiles.size()) return;
    const std::vector<ProjectileTpl::SpawnUnit>& list = p.from->projectiles[p.fromProj].spawnUnits;
    if (list.empty()) return;
    // :1074 gates on `this.j != null`, and in Java that is a reference test rather than an alive one -- a shot
    // keeps hold of its shooter's object after that hull has left the world and still reads its colour off it.
    // RP hands the colour down with the round instead (fire():1925, spawnFromList():2001), so a sunk shooter
    // still drops its cargo, and only setToTeamOfLastAttacker needs the hull itself.
    Unit* src = p.sourceId >= 0 ? get(p.sourceId) : nullptr;

    int made = 0;                                   // var12, spent on created hulls only (:365)
    for (const ProjectileTpl::SpawnUnit& e : list) {
        // :282-283 -- an entry whose unit type no longer resolves contributes nothing at all.
        const UnitTemplate* bt = g_templates.find(ToLower(e.name));
        if (!bt) continue;
        for (int i = 0; i < e.count; ++i) {
            // :287-292, the roll is only drawn when the entry is not a certainty.
            if (e.chance < 1.0f && g_rng.uniform() > e.chance) continue;
            int team = p.team;
            if (e.toLastAttackerTeam) {
                // :294-303, and note whose attacker this is: the *source unit's* target, not the shot's. With
                // no hull to read it off there is nothing to carry on -- :295 skips the attempt.
                Unit* last = (src && src->targetId >= 0) ? get(src->targetId) : nullptr;
                if (!last) continue;
                team = last->team;
            }
            if (made >= e.maxPerCall) continue;     // :305

            // :321-326 in RWX's order. The height is the passed 0 plus offsetHeight, so a spawned air hull
            // really does start on the ground and climbs on its own; the direction the shot carried is only
            // taken by a mobile hull, since a building keeps the -90 deg body angle its type comes with.
            Vec2 pos{ at.x, at.y };
            float dirDeg = bt->isStatic() ? -90.0f : p.heading * 57.29578f;
            if (!bt->isStatic() && e.startDirAtZero) dirDeg = 0.0f;
            // :333-344, offsetDir and its random spread are degrees, added through addRotation, which wraps
            // the running angle back into [-180, 180).
            float add = e.offsetDir;
            if (e.rndDir != 0.0f) add += g_rng.range(-e.rndDir, e.rndDir);
            dirDeg = std::fmod(dirDeg + add + 180.0f, 360.0f);
            if (dirDeg < 0) dirDeg += 360.0f;
            dirDeg -= 180.0f;
            // :346, the repeat index nudges each hull one px along so a `name*3` never stacks them.
            pos.x += (float)i;
            if (e.rndX != 0.0f) pos.x += g_rng.range(-e.rndX, e.rndX);
            if (e.rndY != 0.0f) pos.y += g_rng.range(-e.rndY, e.rndY);
            pos.x += e.offsetX;
            pos.y += e.offsetY;
            made++;
            // :366-367 -- RWX creates the hull and then takes it straight back out again when skipIfOverlapping
            // says no. Only the surviving half of that is observable, so the test runs before the spawn.
            if (e.skipIfOverlapping && blockedForSpawn(*this, pos, bt->radius, bt->isStatic())) continue;

            int id = spawnUnit(e.name, team, pos, false);
            if (id < 0) continue;
            Unit* nu = get(id);
            nu->facing = dirDeg * 0.0174533f;
            nu->posZ = e.offsetZ;
        }
    }
}

// The hook's own quarry filter, ProjectileInterceptorHook.a():49-65. RWX walks the whole live projectile list
// and keeps the *last* shot that passes rather than the nearest, so the order of the list picks the target.
static Projectile* pickQuarry(Game& g, const Unit& u, const TurretTpl& tur) {
    Projectile* found = nullptr;
    auto distSq = [](Vec2 a, Vec2 b) { float dx = a.x - b.x, dy = a.y - b.y; return dx * dx + dy * dy; };
    for (auto& p : g.projectiles) {
        if (p.dead) continue;
        if (p.tags.empty() && !p.nuke) continue;             // aE == null is never chased (unless a nuke)
        if (p.posZ <= tur.interceptOverHeight) continue;    // ..._andOverHeight
        bool tagged = false;
        for (auto& want : tur.interceptTags)
            for (auto& have : p.tags) if (want == have) tagged = true;
        // AntiNukeLauncher:271 locks onto Projectile.D (nukeWeapon) directly, so a nuke-flagged round is a
        // quarry for a barrel hunting `nuke` even when it carries no explicit tag of its own.
        if (p.nuke) for (auto& want : tur.interceptTags) if (want == "nuke") tagged = true;
        if (!tagged) continue;                              // AnimationTag.a():105-118, any name in common
        if (distSq(u.pos, p.pos) >= tur.interceptUnder * tur.interceptUnder) continue;
        // ..._andTargetingGroundUnderDistance measures the shot's own ground aim point, RWX's n/o, so an
        // anti-nuke only spends a round on a rocket that is coming down near it. A negative value skips the
        // test entirely (:57-58, `|| f < 0`). RP keeps `dest` for both that point and a guided round's live
        // target position, which differs only for a mod that tags a guided weapon -- none does.
        if (tur.interceptGroundUnder >= 0.0f &&
            distSq(u.pos, p.dest) >= tur.interceptGroundUnder * tur.interceptGroundUnder) continue;
        // j == null, or a shooter of another colour: PlayerTeam.d() is the alliance test and RP has no
        // alliances, so an own-team round is the only one left alone.
        Unit* shooter = p.sourceId >= 0 ? g.get(p.sourceId) : nullptr;
        if (shooter && !g.isEnemy(u.team, shooter->team)) continue;
        if (p.life <= 0) continue;                          // h > 0: still something to detonate
        // :66-72 -- a shot another round is already chasing is left to that one, which is what keeps a
        // single incoming nuke from eating the whole anti-nuke stockpile.
        bool taken = false;
        for (auto& o : g.projectiles) if (&o != &p && !o.dead && o.chaseId == p.id) taken = true;
        if (taken) continue;
        found = &p;
    }
    return found;
}

// LaserDefense.attackTarget:265-321, the beam a [turret_N] laserDefenceEnergyUse barrel sweeps with once a
// tick (ProjectileInterceptorHook.b():22-35). Two details of the sweep are worth keeping exactly: it walks
// the live rounds in list order and returns on the first one that passes, so a bubble shoots down the
// oldest round inside it rather than the nearest, and it lands at most one hit per tick per barrel, paying
// laserDefenceEnergyUse only for the hit it made.
void Game::updateLaserDefence(Unit& u, const TurretTpl& tur, int ti) {
    // :22 -- a pool above zero and no recharge latch. No canShoot, no cooldown, no warmup and no attack
    // target in front of this: the beam is not a weapon, and three of the five shipped ones sit on barrels
    // that say `canShoot : false`.
    if (u.energy <= 0.0f || u.energyRecharging) return;
    // :26-28 -- [turret] limitingRange when the barrel names one (every shipped beam does, 80 to 160 px),
    // the hull's [attack] maxAttackRange (OrderableUnit.m():1636-1638) otherwise.
    float range = tur.range > 0.0f ? tur.range : u.tpl->attackRange;
    float r2 = range * range;
    // :24 -- the beam starts at the barrel's muzzle, OrderableUnit.E(i) -> CustomUnit.D(i). Measuring from
    // the pivot instead costs nothing here: every shipped laserDefence barrel carries a `size` of 0 or 1 px.
    Vec2 at = turretPivot(u, ti);
    for (auto& p : projectiles) {
        if (p.dead || p.deflected) continue;
        // :272 -- a beam weapon (Projectile.A) and a round whose deflectionPower came in under 0.5 (C) are
        // both untouchable, and a round is only a target once it has been flying: 8 ticks, or 3 for
        // anything faster than 8 px a tick. That grace is what stops a point defence from sniping a shell
        // out of the barrel it is standing in front of.
        if (p.instant || p.beamImmune) continue;
        if (!(p.ticks > 7 || (p.ticks > 2 && p.speed > 8.0f))) continue;
        if (p.posZ < -1.0f) continue;      // :272 -- a round under the surface is below the beam
        float dx = p.pos.x - at.x, dy = p.pos.y - at.y;
        if (dx * dx + dy * dy >= r2) continue;
        // :274-280 -- hostile in two ways: it is aimed at a hull of our own colour (PlayerTeam.d:1319-1324),
        // or, failing that, whoever fired it is not of our colour (c:1315-1317). The second reads the
        // shooter object, which RWX still holds when the launcher has long since blown up, so the team the
        // shot was born with stands in for it.
        Unit* tgt = p.targetId >= 0 ? get(p.targetId) : nullptr;
        bool hostile = tgt && !isEnemy(u.team, tgt->team);
        if (!hostile && p.sourceId >= 0) hostile = isEnemy(u.team, p.team);
        if (!hostile) continue;
        // :298-300 -- one beam hit off the health, and a round whose health runs out is marked aS: RWX's
        // Projectile.d() takes it out of the live list, and its burst is all the player sees of it. The
        // drawn laser, like every other [effect] visual, stays out of RP's scope.
        p.deflectHp -= 1.01f;
        if (p.deflectHp <= 0.0f) { p.deflected = true; explode(p.pos, false, 0.7f); }
        u.energy -= tur.laserDefenceEnergyUse;         // :315
        // :31-34 -- the latch is thrown by the pool going under, not by the energyNeedsToRechargeToFull key
        // the firing path consults, so a spent beam parks the barrel until the pool is whole again.
        if (u.energy < 0.0f) { u.energy = 0.0f; u.energyRecharging = true; }
        return;
    }
}

// ProjectileInterceptorHook.a():41-66, the hunt a [turret_N] interceptProjectiles_withTags barrel runs every
// tick. Read it as RWX wrote it: the only gate before the scan is customUnit.a(TurretConfig):1340-1347, the
// "can this barrel pay for a shot" test -- so there is no canShoot check (three of the five shipped
// interceptors bolt the hunt onto a barrel that says `canShoot : false`), no cooldown, no warmup, no
// [attack] range and no attack target of its own.
void Game::updateInterceptors(Unit& u) {
    const UnitTemplate& tpl = *u.tpl;
    int n = std::min((int)tpl.turrets.size(), (int)u.tur.size());
    for (int i = 0; i < n; ++i) {
        const TurretTpl& tur = tpl.turrets[i];
        // The hook is one method for both projectile-catching barrels, and :22-35 runs before :36-38, so a
        // hull with a beam and a hunt on two barrels pays for the beam first.
        if (tur.laserDefence) updateLaserDefence(u, tur, i);
        if (tur.interceptTags.empty()) continue;
        // ProjectileInterceptorHook.b():22 gates on a(TurretConfig):1340-1347 -- energy AND resourceUsage.
        // An anti-nuke whose ammo pool is empty (antiNukeLauncherC resourceUsage ammo=1) sits out the attack.
        if (tur.energyUsage > 0.0f && (tur.energyUsage > u.energy || u.energyRecharging)) continue;
        if (tur.resourceUsage.any() && !canPayResourceCost(*this, u, tur.resourceUsage)) continue;
        Projectile* q = pickQuarry(*this, u, tur);
        if (!q) continue;
        // :57-64 -- b(TurretConfig):1350-1359 pays for the round (fire() takes the energy half of that),
        // then the shot is launched at the quarry with no unit behind it, which is what sets RWX's aC, and
        // Projectile.q is the link the flight code steers by. Both halves of the quarry are copied out first:
        // the round fire() appends can reallocate the very list `q` points into.
        Vec2 quarry = q->pos;
        int quarryId = q->id;
        if (Projectile* shot = fire(u, nullptr, i, quarry)) shot->chaseId = quarryId;
    }
}

// RWX Projectile.a(range, lead, tags):1776-1820, reached from update:699-702 whenever the chased unit
// is gone and the weapon sets autoTargetingOnDeadTarget. The search centre is the point `lead` px in
// front of the shot along its own heading (az), not the shot itself, and the nearest enemy the firing
// barrel could engage wins. RWX also filters on the shooter's tech level and tags
// (updateRepairOrReclaim..:1798-1803); no shipped weapon uses those, so this keeps the domain test.
static Unit* retargetShot(Game& g, Projectile& p) {
    Vec2 dir{ std::cos(p.heading), std::sin(p.heading) };
    Vec2 at = p.pos + dir * p.autoRetargetLead;
    Unit* best = nullptr;
    float bestD = p.autoRetargetRange * p.autoRetargetRange;
    for (auto& u : g.units) {
        if (!u.tpl || u.dead || u.riding() || !u.operational() || !g.isEnemy(p.team, u.team)) continue;
        bool fly = u.airborne(), sea = u.submerged();
        if (!((fly && p.atkFly) || (!fly && !sea && p.atkLand) || (sea && p.atkSea))) continue;
        float dx = at.x - u.pos.x, dy = at.y - u.pos.y;
        float d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = &u; }
    }
    if (best) p.targetId = best->id;
    return best;
}

// RWX Projectile.update: the shot owns a heading and moves along it, so a shot with turnSpeed >= 0
// only curves at that rate and can miss; turnSpeed < 0 (the default) re-aims every tick. The step is
// clamped to the target distance, which also zeroes the gap so the following hit test lands it
// (update:857-866,975).
void Game::updateProjectile(Projectile& p) {
    if (p.dead) return;
    // RWX update:673-675 -- aS, the mark a laser defence leaves on a round it has shot down, is read at the
    // very top and answered with remove(): the round leaves the live list, is never drawn or chased again,
    // and gets no ring. RP runs the beam in the unit pass and this pass right after it, so the round is
    // dropped here instead of taking the one last step RWX would still let it finish -- a step that almost
    // never lands, since being inside the bubble is not the same as being at the target.
    if (p.deflected) { p.dead = true; return; }
    // Projectile.update:816-840 -- moveWithParent runs every tick, hit or not: the round adds the firing
    // turret's muzzle delta since last tick to its own position so a beam's muzzle end rides a moving hull.
    if (p.moveWithParent && p.parentUnitId >= 0) {
        Unit* par = get(p.parentUnitId);
        if (par && !par->dead && par->tpl) {
            int ti = p.parentTurret;
            if (ti < 0 || ti >= (int)par->tpl->turrets.size()) ti = 0;
            Vec2 m = muzzlePoint(*par, ti);
            float mz = par->posZ;
            p.pos = p.pos + (m - p.lastMuzzle);
            p.posZ += mz - p.lastMuzzleZ;
            p.lastMuzzle = m; p.lastMuzzleZ = mz;
        }
    }
    if (p.hit) {
        if (p.isBeam) {
            // RWX :1397-1416 -- the blast ring still grows, but a beam is not removed when the ring finishes
            // (:1405 guards remove on !B && !M && !X); it stays drawn until life runs out, which :696 spends
            // every tick and :1411 answers with remove(). The directDamage already landed once at impact.
            if (p.expandTotal > 0 && p.expandT > 0) {
                p.expandT -= 1.0f;
                splash(p, 1.0f - p.expandT / p.expandTotal);
            }
            if (--p.life <= 0) p.dead = true;
            return;
        }
        // The shot has landed: only the blast ring keeps growing, and it is no longer drawn.
        p.expandT -= 1.0f;
        splash(p, 1.0f - p.expandT / p.expandTotal);
        if (p.expandT <= 0) p.dead = true;
        return;
    }
    Unit* t = p.targetId >= 0 ? get(p.targetId) : nullptr;
    if (t && t->dead) t = nullptr;
    // RWX update:699-702: a weapon with autoTargetingOnDeadTarget re-locks every tick it has no live
    // target; the rest keep chasing the corpse and land on its last position.
    if (!t && p.autoRetarget) t = retargetShot(*this, p);
    // RWX keeps chasing the corpse, so a shot whose target dies mid-flight still lands where it died.
    // An unguided shell has no unit to chase and keeps its frozen point (:786-793).
    if (t) p.dest = t->pos;
    // :794-801 once there is no unit to follow the aim height drops back to the ground, so a shell that
    // lost its aircraft sinks to 0 and flies on as a ground shot rather than hovering at 35 px.
    p.aimZ = t ? t->posZ : 0.0f;
    // RWX Projectile.update:765-774 -- an interceptor round reads its quarry where a guided round reads its
    // unit: the aim point is the chased shot and the aim height its height. Once the quarry is gone the round
    // falls back to the frozen point it was fired at (:794-801), and it is the aC flag that keeps it alive to
    // do so -- :676-679 drops a shot that has neither a unit nor aC.
    Projectile* chase = p.interceptor ? findProjectile(p.chaseId) : nullptr;
    if (chase) { p.dest = chase->pos; p.aimZ = chase->posZ; }
    // :755-801 -- every branch of the chase reads the aim point as the target's position *plus the
    // shot's own carried offset*, guided or frozen alike.
    Vec2 aim{ p.dest.x + p.aimOff.x, p.dest.y + p.aimOff.y };
    float dist = p.pos.dist(aim);
    float want = std::atan2(aim.y - p.pos.y, aim.x - p.pos.x);
    p.heading = p.turnSpeed >= 0 ? lerpAngle(p.heading, want, p.turnSpeed * 0.0174533f) : want;
    Vec2 dir{ std::cos(p.heading), std::sin(p.heading) };
    // :842-856 -- the shot's own vertical speed lifts it and gravity takes a fixed height off it, both
    // before the horizontal step, so the arc the rest of the tick reads is already under way. Gravity is
    // only spent above the ground, and it is the height itself rather than the vertical speed that drops
    // -- which is why a lobbed round climbs at (velZ - gravity) per tick and never turns over by itself.
    if (p.velZ != 0.0f) p.posZ += p.velZ;
    if (p.posZ > 0.0f) {
        if (p.gravity != 0.0f) p.posZ -= p.gravity;
        if (p.trueGravity != 0.0f) p.velZ -= p.trueGravity;
    }
    float dz = p.aimZ - p.posZ;
    bool stepped = false;
    float arc = p.heading;              // RWX f29, the angle the sprite is drawn at (:1417-1421)
    // :857-866 -- an arcing shell may not move sideways until it is over the delay height, so it climbs
    // out of the barrel. Stepping clamps the gap away, which also hands the whole step to the height.
    if (!p.ballistic || p.ballisticDelayHeight < p.posZ || p.ballisticApex) {
        stepped = true;
        float step = p.speed;
        if (dist < step) { step = dist; dist = 0; }
        p.pos = p.pos + dir * step;
    }
    if (p.ballistic) {
        // :867-888 -- the climb goes to ballistic_height at the flight speed and hands over to a dive at
        // the aim height once the target is within 20 px. Below the delay height the shell is drawn
        // pointing straight up, and a dive that has not arrived yet points it straight down.
        float rate = p.speed;           // aL is pinned to -1 at spawn (CustomUnit.java:1496)
        stepped = true;
        if (!p.ballisticApex) {
            p.posZ = moveTowards(p.posZ, p.ballisticHeight, rate);
            if (p.posZ < p.ballisticDelayHeight) arc = -1.5708f;
            if (p.posZ >= p.ballisticHeight) p.ballisticApex = true;
        } else if (dist * dist < 400.0f) {
            p.posZ = moveTowards(p.posZ, p.aimZ, rate);
            if (std::fabs(p.aimZ - p.posZ) > 0.5f) arc = 1.5708f;
        }
    } else if (dz != 0.0f) {
        // :889-900 -- part of every step goes into climbing or diving, taken in the same proportion as the
        // 2D gap still open, so the shot comes in along the straight 3D line rather than arriving on the
        // ground and only then rising. Once the 2D gap is closed the whole step is available for height.
        stepped = true;
        float zStep = dist > 0.1f ? std::min(std::fabs(dz) / dist * p.speed, p.speed) : p.speed;
        p.posZ += std::max(-zStep, std::min(dz, zStep));
        dz = p.aimZ - p.posZ;
    }
    // :902-904 -- only a step that was actually taken spends the ramp, and it is spent after the move.
    if (stepped && p.speedTarget > 0) p.speed = moveTowards(p.speed, p.speedTarget, p.speedAccel);
    if (p.wobbleAmp != 0) {
        float off = std::sin((p.ticks * 360.0f / p.wobbleFreq + p.wobblePhase * 360.0f) * 0.0174533f) * p.wobbleAmp;
        p.pos = p.pos + Vec2{ -dir.y, dir.x } * off;
    }
    p.vel = dir * p.speed;
    p.ticks++;
    // :1421 -- the sprite is dragged around at 12 deg/tick, so a shell that just fired off a turning
    // turret or come off the top of an arc keeps its old nose for a few ticks.
    p.drawAngle = lerpAngle(p.drawAngle, arc, 12.0f * 0.0174533f);
    // RWX update:910-955: while the shot is still in the air a trailed weapon drops one puff at its own
    // position every trailEffectRate ticks. RWX builds these at EffectQuality.low against a 500-slot
    // pool it refuses above 470 live effects (EffectManager.java:112, 869-880), so the cap here mirrors
    // that instead of letting a barrage grow without bound. The nuke variant (G/F 0.5 -> 2.1,
    // update:940-943) is unreachable in RP because no RP weapon sets nukeWeapon.
    if (p.trail) {
        p.trailT += 1.0f;
        if (p.trailT > p.trailRate) {
            p.trailT = 0;
            if (effects.size() < 470) {
                Effect e;
                // RWX builds the puff at the shot's drawn position, which is posY - posZ.
                e.pos = Vec2{ p.pos.x, p.pos.y - p.posZ }; e.trail = true; e.life = 70.0f;
                e.sink = 0.1f;      // Projectile:934 Q = 0.1
                // Projectile:939 Y = randomFloatInRange(-180, 180) degrees
                e.rot = g_rng.range(-3.14159265f, 3.14159265f);
                effects.push_back(e);
            }
        }
    }
    // RWX lands within a flat 6px of a unit, or within 0.8 of a building's radius (update:961-967), and
    // :971-978 adds the other half of the test -- the height gap has to be under 3 px too, so a shell
    // that arrives still 20 px below a plane flies straight on past it. A shell that carries its own
    // vertical speed or gravity gets that window widened by both (:971-974), which is exactly what lets
    // a lobbed round detonate a few px short of the ground instead of having to touch it.
    float hitR = 6.0f;
    if (t && t->tpl) {
        float r = t->tpl->radius;
        if (t->tpl->isBuilding) hitR = std::max(6.0f, r * 0.8f);
        // :764,968-970 -- a shielded hull is read as the bigger target its bubble makes it, 1.1 radii
        // flat. That overwrites the 6px floor, so a small shielded unit is a tighter target than an
        // unshielded one.
        if (t->shield > 10.0f) hitR = r * 1.1f;
    }
    float zTol = 3.0f;
    if (p.velZ != 0.0f || p.gravity != 0.0f) zTol = 3.0f + std::fabs(p.velZ) + std::fabs(p.gravity);
    if (dist < hitR && std::fabs(dz) < zTol) {
        p.impactZ = p.posZ;
        impact(p, t, p.pos);
        // RWX :1141-1148 -- the interceptor then spends itself on its quarry: either the chased shot's
        // remaining life is zeroed (interceptProjectile_removeTargetLifeOnly, so :1411 just drops it) or it
        // is destroyed outright, which plays that shot's own detonation effect but lands nothing. The round
        // is removed on the spot, so its 75-tick expanding ring never grows past the one sliver of a splash
        // this tick's b(frac) gets out (:1397-1408 runs after the remove, on W already one tick down).
        if (chase) {
            if (p.interceptLifeOnly) chase->life = 0;
            else chase->dead = true;
            p.dead = true;
            return;
        }
        if (p.expandTotal <= 0 && !p.isBeam) p.dead = true;
        return;
    }
    // RWX update:1411-1415: a shot that runs out of life just vanishes; only explodeOnEndOfLife makes it
    // detonate there, which no shipped weapon sets. Before it vanishes it runs the spawnProjectilesOnEndOfLife
    // list its own section carries, which is how heavy_sub.ini:120's 30-tick torpedo turns into the 300-tick
    // torpedoLocked and nautilus.ini:372's life 0 carrier round turns into two scout-bot bombs. RWX's extra
    // gate `(!bn || V)` costs nothing here: a round that already hit returns above and never reaches this
    // line, while RWX's keeps growing its ring and can only hand off once that ring has finished.
    if (--p.life <= 0) {
        p.dead = true;
        if (p.from && p.fromProj >= 0 && p.fromProj < (int)p.from->projectiles.size()) {
            const std::vector<ProjectileTpl::SpawnEntry>& list = p.from->projectiles[p.fromProj].onEndOfLife;
            if (!list.empty()) spawnFromList(p, *p.from, list, p.recursion + 1);
        }
        // Nothing may read `p` past this point: the rounds spawnFromList appends can move the vector it sits in.
    }
}

// EffectManager.createLargeExplosion:204-222 and createSmallExplosion:181-202 -- the two generic hit
// flashes. The names read backwards against what they draw: the "large" one is the ordinary hit's single
// half-size flash on explode_big2 (frames 3..7), the "small" one is largeHitEffect's full-size flame on
// explode_big (frames 0..12, random turn, 0.8..1.0 size, 0.7 alpha). Both count V=35 down against W=25,
// so the fade only begins once V drops below 25 -- and the non-looping animation ends the effect first,
// after 10 or 26 ticks at 0.5 frames/tick (Effect.update:329-334).
static EffectTpl hitFlash(bool large) {
    EffectTpl e;
    e.strip = large ? 13 : 1;
    e.life = 35.0f;
    e.fadeDenom = 25.0f;
    e.alpha = large ? 1.0f : 0.7f;
    float sc = large ? 0.5f : g_rng.range(0.8f, 1.0f);
    e.scaleFrom = sc; e.scaleTo = sc;
    e.animStart = large ? 3 : 0;
    e.animEnd = large ? 7 : 12;
    e.animSpeed = 0.5f;
    e.attached = false;
    e.startDirAtZero = true;
    if (!large) e.dirOffsetRandom = 180.0f;   // Y = random(-180,180)
    return e;
}

// RWX update:1021-1140: the blast is centred on the impact point, the direct damage goes to the
// chased unit, and the splash either fills its radius at once or grows over areaExpandTime.
void Game::impact(Projectile& p, Unit* direct, Vec2 at) {
    p.hit = true;
    p.impactPos = at;
    // Projectile.update:1102-1103 -> BaseUnit.f:2512 -- teleportSource moves the SHOOTER to the impact point
    // (the blink weapons), not the target. RWX only refreshes fog and fires a teleported event; it does not
    // clear velocity or path, so RP just relocates the hull.
    if (p.teleportSource && p.sourceId >= 0) {
        Unit* src = get(p.sourceId);
        if (src && !src->dead) src->pos = at;
    }
    // :1068 runs the shot's own list ahead of the damage at :1105, which is what lets :1043 read the bubble
    // the hull still has rather than the one this hit is about to drain.
    bool ownBlast = spawnHitFx(p, direct, at);
    // :1163-1166 -- flameWeapon drops a fire emitter on the impact INSTEAD of any blast, and it does so
    // even when a custom list or the shield ring already played (the emitter branch only clears z10).
    if (p.flameWeapon) spawnFlameHit(p.pos);
    // :1167-1185 -- the generic flash is ONE animated cell, not the seven-flame death blast. An ordinary
    // hit is createLargeExplosion (explode_big2, frames 3..7, half size); largeHitEffect -- which reads
    // backwards but is literal in RWX -- swaps it for createSmallExplosion (explode_big, frames 0..12,
    // 0.8..1.0 size, 0.7 alpha) plus one shockwave scaled by the blast radius when that radius exceeds
    // 10px. A shot that brought an explodeEffect list -- or hit a bubble -- replaces all of it (:1170,
    // :1173), which is what ownBlast reports.
    if (!ownBlast && !p.flameWeapon) {
        if (p.nuke) {
            // Projectile.update:1186-1366 -- a nuke with no custom explodeEffect swaps the generic flash for
            // a mushroom-cloud chain: a bright white light, a full-size fireball and a ground shockwave grown
            // to the blast radius. RP renders the light/fireball/shockwave layers; the scorch mark and sound
            // are out of scope. The damage itself is unchanged (areaDamage/areaRadius still drive it).
            EffectTpl fire = hitFlash(false);
            fire.scaleFrom = fire.scaleTo = std::max(1.5f, p.areaRadius / 40.0f);
            fire.alpha = 0.9f;
            spawnFx(fire, p.from, at, p.impactZ, 0.0f, nullptr);
            EffectTpl sw = muzzleEffect(5);
            sw.scaleTo = std::max(2.0f, p.areaRadius / 20.0f);
            sw.alpha = 0.8f;
            sw.layer = p.impactZ > 5.0f ? 2 : 1;
            sw.attached = false;
            spawnFx(sw, p.from, at, p.impactZ, 0.0f, nullptr);
            EffectTpl glow = muzzleEffect(6);
            glow.scaleTo = std::max(2.0f, p.areaRadius / 30.0f);
            spawnFx(glow, p.from, at, p.impactZ, 0.0f, nullptr);
        } else if (p.largeHit) {
            if (p.areaRadius > 10) {
                // createSmallExplosionInternal(...,0): the uncoloured shockwave, grown to areaRadius/25
                // and dimmed to 0.7, under the units unless the burst hung more than 5px up (:1174-1180).
                EffectTpl sw = muzzleEffect(5);
                sw.scaleTo = p.areaRadius / 25.0f;
                sw.alpha = 0.7f;
                sw.layer = p.impactZ > 5.0f ? 2 : 1;
                sw.attached = false;
                spawnFx(sw, p.from, at, p.impactZ, 0.0f, nullptr);
            }
            spawnFx(hitFlash(false), p.from, at, p.impactZ, 0.0f, nullptr);
        } else {
            spawnFx(hitFlash(true), p.from, at, p.impactZ, 0.0f, nullptr);
        }
    }
    if (direct && p.damage > 0) damage(*direct, p.damage * damageScale(p, *direct), &p);
    // The carried hull goes down after the damage rather than at RWX's :1074, ahead of it: a spawned unit is
    // not something the shot should be asked to react to, and `direct` is only held across this call because
    // Game::units is a deque. What a spawned hull can change -- a splash victim list, a fresh auto-target --
    // is not read before the splash below, so the two orders observe the same world.
    spawnUnitsOnHit(p, at);
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
        // Projectile:1463 -- a hull in someone's hold is not in the world, splash included.
        if (!u.tpl || u.dead || u.riding()) continue;
        // :1470-1481 -- which teams the ring reaches is per-shot: CustomUnit.java:1391 pins aa on every
        // ordinary weapon (only teams of another colour are hurt) unless [projectile_N] friendlyFire clears
        // it (CustomProjectileTemplate.java:60-67), and ab is the mirror image only that same key's
        // "only-ignoreEnemy" ever sets. A launcher nuke and a nukeOnDeath blast carry neither, so they mow
        // down their own team just as readily as the enemy's.
        if (u.team == p.team ? !p.damagesAllies : p.alliesOnly) continue;
        // :1482 -- a burst that goes off at or above the surface does not carry into the water under it,
        // however deep the hull has dived, unless the weapon was built to hunt them.
        if (u.posZ < -5.0f && p.impactZ >= -2.0f && !p.hitsUnderwater) continue;
        // :1459-1491 -- the burst stays in the domain it went off in, judged by height: aX >= 5 is an
        // air burst and only aircraft feel it, a ground burst only catches what is still on the ground --
        // unless ad clears the gate (areaHitAirAndLandAtSameTime, :1392-1394, and every nuke blast).
        if (!p.hitsAirAndLand && u.airborne() != (p.impactZ >= 5.0f)) continue;
        float d = u.pos.dist(p.impactPos);
        // :1493-1502 -- areaRadiusFromEdge measures the ring to the target's hull instead of to its
        // centre, which is what lets a 25px ant and a 90px factory count as the same distance; the
        // centre form is the plain broad test. Either way the ring itself is the limit.
        if (p.radiusFromEdge) d = std::max(0.0f, d - u.tpl->radius);
        if (d > ring) continue;
        // :1503 -- and a hull inside the hole the weapon leaves around its own blast takes nothing.
        if (d < p.ignoreCloserThan) continue;
        if (p.expandTotal > 0) {
            bool seen = false;
            for (int id : p.splashHits) if (id == u.id) { seen = true; break; }
            if (seen) continue;
            p.splashHits.push_back(u.id);
        }
        // :1509-1517 -- the falloff is on the ring's full radius, not the grown part of it, and
        // areaDamageNoFalloff replaces the whole expression with 1.
        float mult = p.noFalloff ? 1.0f : std::min(1.0f, 1.0f - d / p.areaRadius + 0.1f);
        damage(u, p.areaDamage * mult * damageScale(p, u), &p);
    }
}

// The whole RWX damage pipeline, which RP used to do in one subtraction:
// CustomUnit.setTarget:4187-4199 takes a flat armour off the hit (never below
// armourMinDamageToKeep, and armourIgnoreAmount buys that reduction back), then
// BaseUnit.setTarget:1559-1601 lets the bubble drink shieldMult * damage first - whatever runs past
// an empty bubble is cut by deflectMult, so a weapon with deflectMult 1 never touches the hull at all
// - and only the remainder, scaled by hullMult, is subtracted from health.
float Game::damage(Unit& target, float dmg, const Projectile* p) {
    if (dmg <= 0 || target.dead) return 0.0f;
    const Projectile def;
    if (!p) p = &def;
    target.tookDamageEvent = true;
    // BaseUnit.setTarget:1541-1557 -- an attachment with redirectDamageToParent hands the hit to its parent
    // first (only the bubble part with _shieldOnly), and takes whatever comes back.
    if (target.parentId >= 0) {
        const AttachmentTpl* att = attachmentConfig(target);
        Unit* parent = get(target.parentId);
        if (att && att->redirectDamage && parent && !parent->dead) {
            Projectile copy = *p;
            if (att->shieldOnly) copy.hullMult = 0.0f;
            dmg = std::max(0.0f, damage(*parent, dmg, &copy));
            if (target.dead) return 0.0f;
            if (dmg <= 0.0f) return 0.0f;
        }
    }
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
    // :1588-1601 -- what is left over is handed back to a redirecting child: the full remainder when the
    // hull soaked nothing (hullMult 0), else whatever ran past the last hit point.
    float rest = toHull;
    float hit = toHull * p->hullMult;
    if (hit > 0.0f) {
        if (target.hp < hit) { rest -= target.hp; target.hp = 0.0f; }
        else { target.hp -= hit; rest -= hit; }
    }
    if (target.hp <= 0.0f) {
        if (!tpl.disableDeathOnZeroHp) killUnit(target);
        else if (target.hp < -1.0f) target.hp = -1.0f;
    }
    return rest;
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
        e.life = 13.0f / e.frameSpeed;
        e.delay = big ? g_rng.range(0.0f, 20.0f) : 0.0f;
        effects.push_back(e);
    }
}

// ---- the [effect_*] instances a shoot_flame list spawns ----

// EffectTemplate.a(float):357-362 and a(float,float):364-369: a zero spread draws no random number at all,
// which matters because the generator is shared with everything else in the tick.
static float fxRnd(float v) { return v == 0.0f ? 0.0f : g_rng.range(-v, v); }
static float fxSpan(float lo, float hi) { return lo == hi ? lo : g_rng.range(lo, hi); }
// Utility.getRandomIntInRange is inclusive at BOTH ends, unlike g_rng.range.
static int fxRndInt(int lo, int hi) {
    if (hi <= lo) return lo;
    int r = lo + (int)(g_rng.uniform() * (float)(hi - lo + 1));
    return r > hi ? hi : r;
}
// Utility.moveTowardsZero:222-230
static float fxToZero(float v, float step) {
    if (v > step) return v - step;
    if (v < -step) return v + step;
    return 0.0f;
}

void Game::spawnFx(const EffectTpl& spec, const UnitTemplate* src, Vec2 at, float h, float dirDeg,
                   const FxOwner* owner) {
    // :446-457 + Effect.update:543-548 -- a displacement effect needs the screen texture and
    // post_displacement.frag, which RP has no buffer for, so it is never made rather than drawn wrong.
    if (spec.layer == 3) return;
    // EffectManager.loadContent:870-882 -- the 500-slot PC pool caps a high-priority effect, which is what
    // [effect_*] defaults to (:474-481), at 480 live ones. getNewEffect answers null past that.
    if (fx.size() >= 480) return;
    if (spec.spawnChance < 1.0f && g_rng.uniform() > spec.spawnChance) return;   // :149-155

    Fx f;
    f.spec = spec;      // by value: the physics bounce below rewrites the copy's layer
    f.src = src;
    f.life = spec.life + fxRnd(spec.lifeRandom);                                // :237-239
    f.lifeMax = f.life;
    f.w = spec.fadeDenom > 0.0f ? spec.fadeDenom : f.life;
    f.frame = spec.frame;                                                       // :243-249
    if (spec.frameRandom != 0) {
        f.frame += fxRndInt(-spec.frameRandom, spec.frameRandom);
        if (f.frame < 0) f.frame = 0;
    }
    // :250-257 -- fA is the pivot angle and Effect.Y the drawn one. alwayStartDirAtZero only flattens the
    // sprite: the offsets and speeds below still turn by fA.
    float fA = dirDeg + spec.pivotOffset + fxRnd(spec.pivotOffsetRandom);
    f.dir = (spec.startDirAtZero ? 0.0f : fA) + spec.dirOffset + fxRnd(spec.dirOffsetRandom);
    f.ij = at;
    f.k = h;
    f.ij.x += spec.oxAbs + fxRnd(spec.oxAbsRnd);                                // :258-263
    f.ij.y += spec.oyAbs + fxRnd(spec.oyAbsRnd);
    // :264-271 -- the relative pair is a rotation of (x,y) by fA, so yOffsetRelative pushes ALONG the barrel
    // and xOffsetRelative pushes across it.
    float ca = std::cos(fA * 0.0174532924f), sa = std::sin(fA * 0.0174532924f);
    float ox = spec.oxRel + fxRnd(spec.oxRelRnd), oy = spec.oyRel + fxRnd(spec.oyRelRnd);
    f.ij.x += ca * oy - sa * ox;
    f.ij.y += sa * oy + ca * ox;
    f.k += spec.hOffset + fxSpan(-spec.hOffsetRandom, spec.hOffsetRandom);      // :272
    f.delay = spec.delay + fxSpan(-spec.delayRandom, spec.delayRandom);         // :314-315
    f.vel = Vec2{ spec.xAbs + fxRnd(spec.xAbsRnd), spec.yAbs + fxRnd(spec.yAbsRnd) };   // :320-321
    float sx = spec.xRel + fxRnd(spec.xRelRnd), sy = spec.yRel + fxRnd(spec.yRelRnd);   // :322-329
    f.vel.x += ca * sy - sa * sx;
    f.vel.y += sa * sy + ca * sx;
    f.hVel = spec.hSpeed + fxRnd(spec.hSpeedRandom);                            // :330
    f.dirVel = spec.dirSpeed + fxRnd(spec.dirSpeedRandom);                      // :331
    f.anim = spec.animStart != spec.animEnd;                                    // :332-334
    f.frameFrom = spec.animStart;                                              // :335-338
    if (spec.animStartRandom != 0) f.frameFrom += fxRndInt(0, spec.animStartRandom);
    f.framePos = (float)spec.animStart;                                        // :340
    f.animSpeed = spec.animSpeed + fxRnd(spec.animSpeedRandom);                 // :343-344
    // :345-347 + EffectManager.attachEffectToGameObject:315-323 -- the instance keeps a RELATIVE position
    // from here on and the draw adds the GameObject's live one back.
    if (owner && spec.attached) {
        f.unitId = owner->unitId;
        f.projId = owner->projId;
        f.hull = owner->pos; f.hullZ = owner->posZ;
        f.ij = f.ij - owner->pos;
        f.k -= owner->posZ;
    }
    fx.push_back(f);
}

void Game::spawnFxEntry(const FlameSpec& fs, const UnitTemplate* src, Vec2 at, float h, float dirDeg,
                        const FxOwner* owner) {
    if (fs.builtIn == 0) {
        if (src && fs.tpl >= 0 && fs.tpl < (int)src->effects.size())
            spawnFx(src->effects[fs.tpl], src, at, h, dirDeg, owner);
    } else if (fs.builtIn == 7) {
        // :171-173 -- createExplosion is the seven-flame blast and the entry returns null, so nothing of it
        // is attached and nothing rides layer 2 as an effect of its own.
        explode(Vec2{ at.x, at.y - h }, true);
    } else if (fs.builtIn != 9) {
        // 1..3 flame sizes, 4 smoke, 5 shockwave, 6 light, 8 smallExplosion. 9 is noneSpawnItems (:194-195).
        spawnFx(muzzleEffect(fs.builtIn), src, at, h, dirDeg, owner);
    }
}

void Game::spawnMuzzleFx(const TurretTpl& tur, Vec2 muzzle, float h, float dirDeg, Unit& u) {
    // CustomUnit.java:1590-1591 -- the glow is not part of the list and createLightEffect(x,y,z,color) has no
    // GameObject argument, so it stays where the barrel was even if the hull drives off.
    if (tur.shootLight) {
        EffectTpl light = muzzleEffect(6);
        light.color = tur.shootLight;
        spawnFx(light, u.tpl, muzzle, h, dirDeg, nullptr);
    }
    // :1593-1594 -- CustomUnitSpawnList.a() runs every entry in order, each `*N` repeat its own instance.
    // parseFlameList has already taken the first flame size out for TurretState, so what is left is the rest
    // of the list and runs exactly as RWX would.
    FxOwner own; own.unitId = u.id; own.pos = u.pos; own.posZ = u.posZ;
    for (const FlameSpec& fs : tur.flames) spawnFxEntry(fs, u.tpl, muzzle, h, dirDeg, &own);
}

bool Game::spawnHitFx(Projectile& p, Unit* victim, Vec2 at) {
    if (!p.from || p.fromProj < 0 || p.fromProj >= (int)p.from->projectiles.size()) return false;
    const ProjectileTpl& pt = p.from->projectiles[p.fromProj];
    // :1042-1048 -- more than 10 shield swaps the whole list, it does not add to it.
    bool onShield = victim && victim->shield > 10.0f;
    bool has = onShield ? pt.hasExplodeEffectOnShield : pt.hasExplodeEffect;
    const std::vector<FlameSpec>& list = onShield ? pt.explodeEffectOnShield : pt.explodeEffect;
    // :1069 hands the list the shot's own drawn heading (aT, degrees) and the victim as the GameObject, so an
    // attached effect rides the hull that was hit. aV/aW/aX are ground coordinates plus a height, which is
    // the pair spawnFx wants. The ray-damage branch at :1058-1067 moves that point for a mod-registered
    // beam; no shipped ini binds one, so the impact point stands as it is.
    if (has) {
        float dirDeg = p.heading * 57.29578f;
        FxOwner own;
        if (victim) { own.unitId = victim->id; own.pos = victim->pos; own.posZ = victim->posZ; }
        for (const FlameSpec& fs : list) spawnFxEntry(fs, p.from, at, p.impactZ, dirDeg, victim ? &own : nullptr);
        return true;
    }
    // :1151-1162 -- a shielded hull with no OnShield list of its own gets one pale ring instead: the
    // shockwave creator recoloured -1127220 (ARGB FFEECCCC), cut to 10 ticks and half size, or 25 and full
    // size when the weapon asked for largeHitEffect. It is not attached and :1160 lifts it onto layer 2.
    if (onShield) {
        EffectTpl ring = muzzleEffect(5);
        ring.color = 0xEECCCCFF;
        ring.life = p.largeHit ? 25.0f : 10.0f;
        ring.scaleTo = p.largeHit ? 1.0f : 0.5f;
        ring.attached = false;
        spawnFx(ring, p.from, at, p.impactZ, 0.0f, nullptr);
        return true;
    }
    return false;
}

// Projectile.java:1163-1166 -- EffectEmitter.b(posX,posY) with its life cut from the factory's 330 to 21
// ticks. The emitter starts with c=10 against f=10, so the first puff lands on tick 1 and the cycle reset
// (g=h=0) puts the next one 11 ticks later -- two puffs in all, at delays 0 and 11. Each is the static
// EffectEmitter.b(Effect,false) template (:103-120): fire.png frame 0, V=W=50, fadeIn 10 then fadeOut,
// layer 2, rising at R=0.2 with the atmospheric damp, P/Q jittered +-0.1/+-0.03 and the spawn point +-4px
// on both ground axes. The emitter sits at the round's ground position, height 0, even for an air burst.
void Game::spawnFlameHit(Vec2 at) {
    EffectTpl e;
    e.strip = 7;
    e.life = 50.0f;
    e.fadeIn = 10.0f;
    e.atmospheric = true;
    e.hSpeed = 0.2f;
    e.xAbsRnd = 0.1f; e.yAbsRnd = 0.03f;
    e.oxAbsRnd = 4.0f; e.oyAbsRnd = 4.0f;
    e.attached = false;
    e.startDirAtZero = true;
    spawnFx(e, nullptr, at, 0.0f, 0.0f, nullptr);
    e.delay = 11.0f;
    spawnFx(e, nullptr, at, 0.0f, 0.0f, nullptr);
}

void Game::spawnBirthFx(Projectile& p) {
    if (!p.from || p.fromProj < 0 || p.fromProj >= (int)p.from->projectiles.size()) return;
    const ProjectileTpl& pt = p.from->projectiles[p.fromProj];
    if (!pt.hasEffectOnCreate) return;
    // :1469 -- the muzzle point, the shot's own heading in degrees (az) and the shot itself as the GameObject.
    // An attached entry keeps a relative offset from the round and is drawn wherever the round has got to,
    // which is how five shipped weapons carry a glow downrange; Effect.draw:303 ends it when the round leaves.
    FxOwner own; own.projId = p.id; own.pos = p.pos; own.posZ = p.posZ;
    float dirDeg = p.heading * 57.29578f;
    for (const FlameSpec& fs : pt.effectOnCreate)
        spawnFxEntry(fs, p.from, p.pos, p.posZ, dirDeg, &own);
}

void Game::updateFx() {
    for (Fx& f : fx) {
        f.delay = fxToZero(f.delay, 1.0f);                                      // :298-301
        if (f.delay > 0.0f) continue;
        f.life -= 1.0f;                                                         // :302
        if (f.unitId >= 0) {
            const Unit* u = get(f.unitId);
            if (u) { f.hull = u->pos; f.hullZ = u->posZ; }
            // :303-305 -- RWX tests GameObject.isDestroyed, which is set when the hull leaves the live list,
            // not when it starts dying: an attached flame keeps playing all the way down a death fall.
            else if (!f.spec.liveAfterAttachedDies) f.life = -1.0f;
        } else if (f.projId >= 0) {
            // The same field with a Projectile as the GameObject (CustomUnit.java:1469). A round is compacted
            // out of the deque the tick it dies, which is when RWX's remove() sets isDestroyed.
            const Projectile* q = findProjectile(f.projId);
            if (q) { f.hull = q->pos; f.hullZ = q->posZ; }
            else if (!f.spec.liveAfterAttachedDies) f.life = -1.0f;
        }
        if (f.life < 0.0f) { f.dead = true; continue; }                          // :306-309
        if (f.anim) {                                                           // :310-339
            f.framePos += f.frameBack ? -f.animSpeed : f.animSpeed;
            if (f.spec.animPingPong) {
                if (f.frameBack) {
                    if (f.framePos < (float)f.frameFrom) {
                        if (!f.spec.animLooping) { f.dead = true; continue; }
                        f.frameBack = false;
                        f.framePos = (float)f.frameFrom;
                    }
                } else if (f.framePos >= (float)(f.spec.animEnd + 1)) {
                    f.frameBack = true;
                    f.framePos = (float)f.spec.animEnd;
                }
            } else if (f.framePos >= (float)(f.spec.animEnd + 1)) {
                if (!f.spec.animLooping) { f.dead = true; continue; }
                f.framePos = (float)f.frameFrom;
            }
            f.frame = (int)f.framePos;
        }
        if (f.spec.atmospheric) {                                               // :340-343
            f.hVel -= f.hVel * 0.002f;
            f.vel.x -= 0.0015f;      // RWX dampens P only, despite the comment calling it a rise
        }
        if (f.spec.physics) {                                                   // :344-363
            if (f.k > 0.0f) f.hVel -= 0.1f * f.spec.physicsGravity;
            else {
                if (f.hVel < 0.0f) f.hVel = fxToZero(-f.hVel * 0.5f, 1.3f);
                if (f.k < 0.0f) f.k = 0.0f;
                if (f.hVel < 0.2f) f.spec.layer = 1;    // a landed spark drops to the ground layer
                f.vel.x = fxToZero(f.vel.x, 0.15f);
                f.vel.y = fxToZero(f.vel.y, 0.15f);
                f.dirVel = fxToZero(f.dirVel, 1.0f);
            }
        }
        f.ij = f.ij + f.vel;                                                    // :364-367
        f.k += f.hVel;
        f.dir += f.dirVel;
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
    // CustomUnit.ds():548-551 -> h(transportUnitsKillOnDeath):553-562. The hold empties before anything
    // else, so a passenger that dies with its carrier is already off the list when its own death runs.
    if (!u.cargo.empty()) releaseCargo(u, u.tpl && u.tpl->transportKillOnDeath);
    // A hull that dies in someone's hold just leaves that list; its slot is what mattered, not its corpse.
    if (u.transportId >= 0) {
        if (Unit* t = get(u.transportId))
            t->cargo.erase(std::remove(t->cargo.begin(), t->cargo.end(), u.id), t->cargo.end());
        u.transportId = -1;
    }
    for (auto& o : units) {
        if (o.orderTargetId == u.id) o.orderTargetId = -1;
        if (o.nanoTargetId == u.id) o.nanoTargetId = -1;
        if (o.loadTargetId == u.id) o.loadTargetId = -1;
        if (o.loadUpTargetId == u.id) o.loadUpTargetId = -1;
    }
    for (auto& p : projectiles) if (p.targetId == u.id) p.targetId = -1;
    releaseAttachments(u);
}

void Game::killUnit(Unit& u) {
    if (u.dead) return;
    bool big = u.tpl && u.tpl->radius > 20;
    // CustomUnit.e():943-952 answers whether the death is allowed to keep the object: yes for
    // slowDeathFall, yes for an image_wreak hull (which shows the wreck sprite from then on), no for
    // anything else -- and the fall itself is driven purely by height, since update:1673 only asks for
    // posZ > 0. So a plain hovering hull with a wreck image still drops out of the sky.
    bool hangs = u.posZ > 0.0f && u.tpl &&
        (u.tpl->slowDeathFall || !u.tpl->imageWreak.empty());
    removeUnit(u);
    u.deathFalling = hangs;
    u.corpseWreck = hangs && !u.tpl->slowDeathFall;
    explode(Vec2{ u.pos.x, u.pos.y - u.posZ }, big);
    // :922-929 -- only a grounded wreck scorches the ground, and only explodeOnDeath asks for the mark.
    if (u.tpl && u.tpl->explodeOnDeath && !u.tpl->hideScorchMark && !hangs && !overWater(*this, u.pos))
        addScorch(u.pos);
    // RWX CustomUnit.java:946: with image_wreak the corpse stays behind instead of vanishing. A falling
    // one leaves it at touchdown instead (update:1726-1729).
    if (u.tpl && !u.tpl->imageWreak.empty() && !hangs)
        wrecks.push_back(Wreck{ u.pos, u.facing, u.tpl });
    // [core] nukeOnDeath (CustomUnit.java:864-876) leaves a nuke on the dying hull. It reuses the
    // launcher's projectile (NukeLauncher.a) with aH cleared, and aH is exactly the climb-then-dive arc
    // (Projectile.update:857 gates the horizontal step, :867 the climb), so the shot never flies -- it
    // detonates on its birth position, which is where :1022-1025 puts the blast centre anyway. What is
    // left is the ring: Y/Z are the ini's damage/range, W = X = 75 (NukeLauncher.java:401) so it grows
    // over 75 ticks, and the shot carries neither aa nor ab but does have ad, meaning it mows through
    // its own team and catches aircraft and ground alike. :1363-1365 scorches the ground unless the
    // detonation sits on liquid.
    if (u.tpl && u.tpl->nukeOnDeath) {
        Projectile bl;
        bl.team = u.team;
        bl.sourceId = u.id;
        bl.pos = bl.dest = bl.impactPos = u.pos;
        bl.impactZ = u.posZ;
        bl.areaDamage = u.tpl->nukeOnDeathDamage;
        bl.areaRadius = u.tpl->nukeOnDeathRange;
        bl.expandTotal = bl.expandT = 75.0f;
        bl.damagesAllies = bl.hitsAirAndLand = true;
        bl.hit = true;
        bl.id = nextProjectileId++;
        projectiles.push_back(bl);
        explode(Vec2{ u.pos.x, u.pos.y - u.posZ }, true);
        if (!overWater(*this, u.pos)) addScorch(u.pos);
    }
}

// RWX CustomUnit.update:1672-1768 -- the descent of a corpse that died above the ground.
void Game::updateDeathFall(Unit& u) {
    const UnitTemplate& tpl = *u.tpl;
    // :1674 the slow spiral down only happens while the hull is still flying: RWX asks for a nonzero
    // rotation (its throttle) or leftover velocity, otherwise the corpse just drops.
    bool gliding = tpl.slowDeathFall && (u.throttle != 0.0f || u.vel.len() > 1e-6f);
    // :1675 / :1718 frameAnimationDelay carries the falling speed here: a steady 0.017 px per tick
    // squared while gliding, otherwise fallingAccelerationDead (default 0.06) keeps adding to it.
    u.fallVel += gliding ? 0.017f : tpl.fallingAccelerationDead;
    u.posZ -= u.fallVel;

    if (gliding && tpl.moveType == MoveType::AIR) {
        // :1677-1679 getMovementDeltaOffset:5136-5146 -- the wreck slides on the step its movement would
        // have taken, written straight into posX/posY with no passability or collision test.
        u.pos = u.pos + (tpl.moveSlidingMode ? u.vel
            : Vec2{ std::cos(u.facing), std::sin(u.facing) } * (tpl.moveSpeed * u.throttle));
        // :1684-1700 a smoke puff on a 9-tick countdown that resets to a random 1..3
        if (tpl.slowDeathFallSmoke && ++u.smokeTimer > 9) {
            u.smokeTimer = (int)g_rng.range(1.0f, 3.0f);
            Effect e;
            e.pos = Vec2{ u.pos.x, u.pos.y - u.posZ };
            e.rot = g_rng.range(-3.14159265f, 3.14159265f);
            e.trail = true;
            e.life = 60.0f;             // EffectEmitter-side mirror of W=60
            e.puffFrom = 0.9f;          // G=0.9 -> F=1.2, and no fade-in (r=true, s unset)
            e.puffFadeIn = 0.0f;
            effects.push_back(e);
        }
        // :1702-1714 and a fire puff every 7 ticks, jittered +-4 px around the hull
        if (++u.debrisTimer > 7) {
            u.debrisTimer = 0;
            Effect e;
            e.pos = u.pos + Vec2{ g_rng.range(-4.0f, 4.0f), g_rng.range(-4.0f, 4.0f) };
            e.pos.y -= u.posZ;
            e.rot = g_rng.range(-3.14159265f, 3.14159265f);
            e.fire = true;
            e.frame = (int)g_rng.range(0.0f, 3.999f);   // EffectEmitter.b: ap = random 0..3
            e.life = 50.0f;
            effects.push_back(e);
        }
    }

    if (u.posZ > 0.0f) return;
    // :1721-1733 touchdown: the hull now shows image_wreak, or disappears with no wreck at all.
    u.posZ = 0.0f;
    if (!tpl.imageWreak.empty()) wrecks.push_back(Wreck{ u.pos, u.facing, u.tpl });
    // :1734-1748 a corpse that had picked up real speed detonates where it landed.
    if (u.fallVel > 0.5f && tpl.explodeOnDeathGroundCollision) {
        explode(u.pos, false);
        if (!overWater(*this, u.pos) && !tpl.hideScorchMark) addScorch(u.pos);
    }
    u.fallVel = 0;
    u.deathFalling = false;
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
            for (auto& u : units) if (u.team == teamId && u.tpl && !u.riding() && !u.tpl->isBuilding && !u.tpl->isBuilder && u.tpl->canAttack && u.operational()) army.push_back(u.id);
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

// ---------------------------------------------------------------------------
// Attachments, actions and message flushing.
//
// These were declared in Game.h but never defined, which the linker reports as
// eleven LNK2019 symbols. The behaviour below follows RWX's original logic:
// the attachment half maps almost one-to-one onto AttachmentManagerHook, while
// the action half is the legacy string-keyed [action_N] system RP parses in
// Templates.cpp (RWX has since moved to CustomAction/LogicBoolean objects).
// No call here may reach another declared-but-undefined helper (logicCondition
// et al.), or it would simply move the link error one step down the chain.
// ---------------------------------------------------------------------------

// AttachmentManagerHook.a(CustomUnit,short):364-370 -- the slot definition is
// indexed by the child's attachmentSlot into the *parent's* config.
const AttachmentTpl* Game::attachmentConfig(const Unit& u) const {
    if (u.parentId < 0 || u.attachmentSlot < 0) return nullptr;
    const Unit* parent = get(u.parentId);
    if (!parent || !parent->tpl) return nullptr;
    const std::vector<AttachmentTpl>& slots = parent->tpl->attachments;
    if (u.attachmentSlot >= (int)slots.size()) return nullptr;
    return &slots[u.attachmentSlot];
}

// BaseUnit:1411-1418 and AttachmentSlotDefinition.m -- a hull is draggable unless
// its own config says unselectable, or an attachment slot hides it. RP's parser
// already resolves the attachment's isUnselectable fallback, so the stored flag
// is the effective one.
bool Game::selectable(const Unit& u) const {
    if (!u.tpl || u.dead || u.riding()) return false;
    if (u.tpl->unselectable) return false;
    if (u.parentId >= 0) {
        const AttachmentTpl* att = attachmentConfig(u);
        if (att && (att->unselectable || att->hidden)) return false;
    }
    return true;
}

// BaseUnit.isUnselectableAsTarget:2274-2276 -- same as selectable but keyed on
// untargetable / cannotBeAttacked; an attachment must also be damageable to be a
// hull a barrel will list as a target.
bool Game::targetable(const Unit& u) const {
    if (!u.tpl || u.dead || u.riding()) return false;
    if (u.tpl->untargetable || u.tpl->cannotBeAttacked) return false;
    if (u.parentId >= 0) {
        const AttachmentTpl* att = attachmentConfig(u);
        if (att) {
            if (att->untargetable || att->hidden || !att->damageable) return false;
        }
    }
    return true;
}

// CustomUnit.createAttachments (AttachmentManagerHook.a(CustomUnit):283-336):
// for every slot with an onCreateSpawnUnitOf list, spawn its first hull, size the
// parent's attachment list up to the slot, park the hull there and hand it the
// slot/team/build state. attachmentSpawnDepth (RWX's recursion guard) stops a
// chain of attachment configs that spawn each other.
void Game::createAttachments(Unit& u, int depth) {
    if (!u.tpl || depth > 4 || u.tpl->attachments.empty()) return;
    const std::vector<AttachmentTpl>& slots = u.tpl->attachments;
    for (int slot = 0; slot < (int)slots.size(); ++slot) {
        const AttachmentTpl& att = slots[slot];
        if (att.spawn.empty()) continue;
        // Grow the slot vector so `attachments[slot]` is always valid; empty slots
        // stay -1 (RWX's FastArrayList null entries).
        if ((int)u.attachments.size() <= slot) u.attachments.resize(slot + 1, -1);
        const ProjectileTpl::SpawnUnit& e = att.spawn[0];
        const UnitTemplate* bt = g_templates.find(ToLower(e.name));
        if (!bt) continue;
        if (e.chance < 1.0f && g_rng.uniform() > e.chance) continue;
        // Attachment offsets are in the parent's local frame (x across, y along
        // the body) and rotate with it -- the same (cos*x - sin*y, sin*x + cos*y)
        // that updateAttachments uses, applied once at spawn so a hull starts on
        // its slot instead of at the parent's centre.
        float c = std::cos(u.facing), s = std::sin(u.facing);
        Vec2 at{ u.pos.x + c * att.x - s * att.y, u.pos.y + s * att.x + c * att.y };
        at.x += e.offsetX; at.y += e.offsetY;
        ++attachmentSpawnDepth;
        int id = spawnUnit(e.name, u.team, at, u.underConstruction && att.incompleteWithParent);
        --attachmentSpawnDepth;
        if (id < 0) continue;
        Unit* child = get(id);
        if (!child) continue;
        child->parentId = u.id;
        child->attachmentSlot = slot;
        child->posZ = u.posZ + att.height + e.offsetZ;
        // idleDir is the body angle relative to the parent, in degrees.
        child->facing = wrapAngle(u.facing + att.idleDir * 0.0174533f);
        child->targetFacing = child->facing;
        if (u.underConstruction && att.incompleteWithParent) child->buildProgress = u.buildProgress;
        u.attachments[slot] = id;
    }
}

// AttachmentManagerHook.b(CustomUnit,float):124-249 -- every tick the child is
// placed at the parent's position plus the slot offset rotated by the parent's
// facing, at parent height + slot height. A rotating parent adds its per-frame
// angle delta to a non-locked child (addRotation), while a locked one is snapped
// to facing + idleDir / reverseDir. Dead children are unlinked.
void Game::updateAttachments(Unit& u) {
    if (u.attachments.empty() || u.dead || !u.tpl) return;
    float c = std::cos(u.facing), s = std::sin(u.facing);
    // RWX caches the previous rotation in customUnit.D to form the delta; RP
    // keeps it in attachmentFacing.
    float delta = angleDiff(u.attachmentFacing, u.facing);
    for (int slot = 0; slot < (int)u.attachments.size(); ++slot) {
        int id = u.attachments[slot];
        if (id < 0) continue;
        Unit* ch = get(id);
        if (!ch) { u.attachments[slot] = -1; continue; }
        if (ch->dead) { ch->parentId = -1; u.attachments[slot] = -1; continue; }
        if (slot >= (int)u.tpl->attachments.size()) continue;
        const AttachmentTpl& att = u.tpl->attachments[slot];
        float rx = c * att.x - s * att.y;
        float ry = s * att.x + c * att.y;
        ch->pos.x = u.pos.x + rx;
        ch->pos.y = u.pos.y + ry;
        ch->posZ  = u.posZ + att.height;
        if (ch->underConstruction && att.incompleteWithParent) ch->buildProgress = u.buildProgress;
        // lockRotation snaps to facing + the (possibly reversed) idle direction;
        // otherwise rotateWithParent passes the parent's turn through.
        if (att.lockRotation) {
            float want = u.facing + (u.isRotating ? att.reverseDir : att.idleDir) * 0.0174533f;
            ch->facing = wrapAngle(want);
            ch->targetFacing = ch->facing;
        } else if (att.rotateWithParent) {
            ch->facing = wrapAngle(ch->facing + delta);
            ch->targetFacing = ch->facing;
        }
        // Children inherit the parent's target under prioritizeTarget /
        // onlyParentTarget; RP only needs the resolved target id here.
        if ((att.prioritizeTarget || att.onlyParentTarget || att.alwaysParentTarget) && u.targetId >= 0)
            ch->targetId = u.targetId;
    }
    u.attachmentFacing = u.facing;
}

// AttachmentManagerHook.a(CustomUnit,boolean):251-269 -- on parent removal every
// slot is unlinked; the hull is only removed from the world when the slot does
// not ask to keepAliveWhenParentDies.
void Game::releaseAttachments(Unit& u) {
    for (int& id : u.attachments) {
        if (id < 0) continue;
        Unit* ch = get(id);
        const AttachmentTpl* att = nullptr;
        if (ch && ch->attachmentSlot >= 0) {
            const Unit* parent = ch->parentId >= 0 ? get(ch->parentId) : nullptr;
            if (parent && parent->tpl && ch->attachmentSlot < (int)parent->tpl->attachments.size())
                att = &parent->tpl->attachments[ch->attachmentSlot];
        }
        if (ch) {
            ch->parentId = -1;
            ch->attachmentSlot = -1;
            if (!att || !att->keepAlive) removeUnit(*ch);
        }
        id = -1;
    }
    u.attachments.clear();
}

// Local comma-list splitter -- the identical helper in Templates.cpp is file-static.
static std::vector<std::string> splitCommaNames(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(',', start);
        std::string tok = Trim(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!tok.empty()) out.push_back(tok);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

// Apply an `addResources` payload such as "credits=250", "hp:-1000",
// "setFlag=1", "unsetFlag=1-10" or a bare number (credits). RP only models
// credits, health, energy and a flat flag/resource store, so unsupported keys
// are ignored rather than acted on. Kept local because nothing else parses it.
static void applyAddResources(Game& g, Unit& u, const std::string& raw) {
    std::string s = Trim(ToLower(raw));
    if (s.empty()) return;
    auto number = [](const std::string& v) { return (float)atof(v.c_str()); };
    // A bare, plain number is the credits shorthand ("addResources : 2000").
    if (s.find_first_not_of("0123456789.-+eE") == std::string::npos && !s.empty()) {
        g.reward(u.team, number(s));
        return;
    }
    for (auto& part : splitCommaNames(s)) {
        size_t eq = part.find_first_of("=:");
        if (eq == std::string::npos) continue;
        std::string key = Trim(part.substr(0, eq));
        std::string val = Trim(part.substr(eq + 1));
        if (key == "credits") {
            g.reward(u.team, number(val));
        } else if (key == "hp" || key == "health") {
            u.hp += number(val);
        } else if (key == "energy") {
            u.energy += number(val);
        } else if (key == "shield") {
            u.shield += number(val);
        } else if (key == "setflag" || key == "unsetflag") {
            bool set = key == "setflag";
            size_t dash = val.find('-');
            int lo = (int)number(val);
            int hi = dash == std::string::npos ? lo : (int)number(val.substr(dash + 1));
            for (int f = lo; f <= hi && f >= 0 && f < 64; ++f) {
                if (set) u.flags |= (1ULL << f);
                else u.flags &= ~(1ULL << f);
            }
        } else {
            // Named resources (e.g. `slotid`) are kept for condition expressions.
            u.resources[key] += number(val);
        }
    }
}

// CustomActionDef / the legacy [action_N] availability rules: an action is
// offered when it is not hidden, its per-action and global cooldowns have run
// out, its price is affordable, and its require/visible conditions hold. RP has
// no expression evaluator wired up yet, so a non-empty `require` is treated as
// satisfied (the permissive fallback) rather than calling an undefined helper.
bool Game::actionAvailable(const Unit& u, int index) const {
    if (!u.tpl || index < 0 || index >= (int)u.tpl->actions.size()) return false;
    const UnitAction& a = u.tpl->actions[index];
    // Only a hidden action or a conversion to a missing type disqualifies it;
    // `supported` merely means "not modelled", which must not block a click.
    if (a.hidden) return false;
    if (!a.convertTo.empty() && !g_templates.find(ToLower(a.convertTo))) return false;
    auto cd = u.actionCooldowns.find(a.name);
    if (cd != u.actionCooldowns.end() && cd->second > 0.0f) return false;
    if (u.allActionCooldown > 0.0f) return false;
    // canAfford() is non-const, so read the team's balance directly here.
    if (a.price > 0.0f) {
        bool afford = false;
        for (const Team& t : teams) if (t.id == u.team) { afford = t.credits >= a.price; break; }
        if (!afford) return false;
    }
    return true;
}

// CustomUnit:2953-2999 (legacy [action_N] execution): pay already happened in
// orderUpgrade; here the action's effects are applied -- conversion, resource /
// energy changes, event/queue triggers, cooldowns and the parent-shield drain.
// `convertUnit` is intentionally implemented inline so no extra symbol is needed.
void Game::runAction(Unit& u, const UnitAction& a, int depth) {
    if (depth > 4 || u.dead) return;
    // Self conversion: reload the template and rebuild the per-template state.
    if (!a.convertTo.empty()) {
        const UnitTemplate* to = g_templates.find(ToLower(a.convertTo));
        if (to && to != u.tpl) {
            u.tpl = to;
            u.tplKey = ToLower(a.convertTo);
            initTurrets(u);
            initLegs(u);
            u.hp = std::min(u.hp, u.maxHp = to->maxHp);
        }
    }
    applyAddResources(*this, u, a.addResources);
    if (a.addEnergy != 0.0f) u.energy += a.addEnergy;
    if (a.parentShield != 0.0f && u.parentId >= 0) {
        if (Unit* parent = get(u.parentId)) parent->shield = std::max(0.0f, parent->shield + a.parentShield);
    }
    if (a.setBuilt >= 0.0f) { u.underConstruction = false; u.buildProgress = a.setBuilt; }
    if (!a.playAnimation.empty()) { u.animRequested = true; }
    // Cooldowns: the action's own slot plus the global queue lock.
    if (a.cooldown > 0.0f) u.actionCooldowns[a.name] = a.cooldown;
    if (a.allCooldown > 0.0f) u.allActionCooldown = a.allCooldown;
    // Follow-up actions by name; splitNames/ToLower match how the parser stored
    // alsoTriggerAction / alsoQueueAction.
    for (auto& name : splitCommaNames(ToLower(a.triggerActions))) {
        if (!u.tpl) break;
        for (size_t i = 0; i < u.tpl->actions.size(); ++i)
            if (ToLower(u.tpl->actions[i].name) == name) { runAction(u, u.tpl->actions[i], depth + 1); break; }
    }
    for (auto& name : splitCommaNames(ToLower(a.queueActions))) {
        if (!u.tpl) break;
        for (size_t i = 0; i < u.tpl->actions.size(); ++i)
            if (ToLower(u.tpl->actions[i].name) == name) { runAction(u, u.tpl->actions[i], depth + 1); break; }
    }
    if (!a.messageTo.empty())
        actionMessages.push_back({ u.id, a.messageTags, depth });
}

// CustomUnitActionHandler + the message events: fire the named actions whose
// autoTriggerOnEvent list contains `event` (optionally gated by a tag), and keep
// a one-hop guard so a message that re-fires itself cannot loop forever.
void Game::actionEvent(Unit& u, const std::string& event, const std::string& tags, int depth) {
    if (depth > 4 || !u.tpl || u.dead) return;
    const std::string ev = ToLower(event);
    // Snapshot: runAction may replace u.tpl through convertTo, invalidating a
    // range-for over the live action vector.
    const std::vector<UnitAction> actions = u.tpl->actions;
    for (const UnitAction& a : actions) {
        if (a.events.empty() || !a.supported) continue;
        // events is a comma list; an entry may be `newMessage(withTag='x')`.
        bool wants = false;
        for (auto& tok : splitCommaNames(a.events)) {
            if (tok.rfind(ev, 0) == 0) { wants = true; break; }
        }
        if (!wants) continue;
        if (!tags.empty() && a.tags.empty()) continue;
        if (!a.tags.empty() && !tags.empty()) {
            bool match = false;
            for (auto& t : splitCommaNames(tags)) if (a.tags.find(t) != std::string::npos) { match = true; break; }
            if (!match) continue;
        }
        runAction(u, a, depth + 1);
    }
}

// CustomUnit auto-trigger pass: decrement cooldowns once per tick, then run
// actions whose `autoTrigger` condition is met when their checkRate slice comes
// up. The condition is read into the same predicate atoms as the unload gate
// (overWater / isOverLiquid / isMoving); atoms RP does not know stay always-true.
// `created`/`complete` are raised once from their latches and event-driven actions
// are handled by actionEvent instead.
void Game::updateActions(Unit& u) {
    if (!u.tpl || u.dead) return;
    if (u.autoCooldown > 0.0f) u.autoCooldown -= 1.0f;
    if (u.allActionCooldown > 0.0f) u.allActionCooldown -= 1.0f;
    for (auto it = u.actionCooldowns.begin(); it != u.actionCooldowns.end(); ) {
        if (it->second > 0.0f) it->second -= 1.0f;
        if (it->second <= 0.0f) it = u.actionCooldowns.erase(it); else ++it;
    }
    // One-shot lifecycle events, queued by the spawn/complete paths via the
    // bool latches on Unit (RWX UnitEventType.created / completeAndActive).
    if (!u.createdEvent) {
        u.createdEvent = true;
        actionEvent(u, "created");
    }
    if (!u.completeEvent && u.operational()) {
        u.completeEvent = true;
        actionEvent(u, "completeAndActive");
    }
    if (u.tookDamageEvent) {
        u.tookDamageEvent = false;
        actionEvent(u, "tookDamage");
    }
    bool slice = u.tpl->autoCheckRate <= 1 || (tick % u.tpl->autoCheckRate) == 0;
    if (!slice) return;
    // Snapshot for the same reason as actionEvent -- a convertTo swaps the tpl.
    const std::vector<UnitAction> actions = u.tpl->actions;
    for (const UnitAction& a : actions) {
        // hidden only hides the UI button; RWX still polls a hiddenAction's autoTrigger condition
        // (CustomUnitConfigParser:3036 adds it to autoTriggerConditions regardless of isVisible).
        if (!a.supported || a.autoTrigger.empty()) continue;
        if (!condTestsOk(*this, u, a.autoCond)) continue;
        if (u.allActionCooldown > 0.0f) continue;
        auto cd = u.actionCooldowns.find(a.name);
        if (cd != u.actionCooldowns.end() && cd->second > 0.0f) continue;
        if (a.price > 0.0f && !canAfford(u.team, a.price)) continue;
        if (!a.events.empty()) continue;  // event-driven, not polled
        runAction(u, a);
    }
}

// Frame-end delivery of queued messages: RWX resolves sendMessageTo (parent,
// children, ...) and hands the message to each target's actionEvent under the
// newMessage event. RP has no message UI, so delivery is only the action hook.
void Game::flushMessages() {
    if (actionMessages.empty()) return;
    std::deque<ActionMessage> pending;
    pending.swap(actionMessages);
    for (const ActionMessage& m : pending) {
        Unit* src = get(m.unitId);
        if (!src) continue;
        if (src->parentId >= 0) {
            if (Unit* parent = get(src->parentId)) actionEvent(*parent, "newMessage", m.tags, m.depth + 1);
        }
        for (int id : src->attachments) {
            if (id < 0) continue;
            if (Unit* child = get(id)) actionEvent(*child, "newMessage", m.tags, m.depth + 1);
        }
    }
}
