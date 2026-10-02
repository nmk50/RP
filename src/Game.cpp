#include "Game.h"
#include "Pathfinding.h"
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

static Team* teamOf(Game& g, int id) { for (auto& t : g.teams) if (t.id == id) return &t; return nullptr; }

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

int Game::spawnUnit(const std::string& tplKey, int team, Vec2 pos, bool underConstruction) {
    const UnitTemplate* tpl = g_templates.find(ToLower(tplKey));
    if (!tpl) return -1;
    Unit u;
    u.id = nextId++;
    u.tplKey = ToLower(tplKey);
    u.tpl = tpl;
    u.team = team;
    u.pos = pos;
    u.hp = u.maxHp = tpl->maxHp;
    u.footprint[0] = tpl->footL; u.footprint[1] = tpl->footU; u.footprint[2] = tpl->footR; u.footprint[3] = tpl->footD;
    u.turretRot = -1.5708f;
    u.underConstruction = underConstruction;
    units.push_back(u);
    return u.id;
}

bool Game::canAfford(int team, float cost) { Team* t = teamOf(*this, team); return t && t->credits >= cost; }
void Game::spend(int team, float cost) { Team* t = teamOf(*this, team); if (t) t->credits -= cost; }
void Game::reward(int team, float credits) { Team* t = teamOf(*this, team); if (t) t->credits += credits; }

static void computePath(Game& g, Unit& u, Vec2 target) {
    int sx = (int)(u.pos.x / TILE_SIZE), sy = (int)(u.pos.y / TILE_SIZE);
    int gx = (int)(target.x / TILE_SIZE), gy = (int)(target.y / TILE_SIZE);
    u.path = findPath(g.map, sx, sy, gx, gy, u.tpl->moveType);
    u.pathIdx = 0;
}

void Game::orderMove(std::vector<int> ids, Vec2 target) {
    for (int id : ids) {
        Unit* u = get(id); if (!u || !u->tpl || u->isStatic()) continue;
        u->orderTargetId = -1; u->attackMove = false; u->constructingId = -1; u->targetUnitId = -1;
        computePath(*this, *u, target);
    }
}

void Game::orderAttackMove(std::vector<int> ids, Vec2 target) {
    orderMove(ids, target);
    for (int id : ids) { Unit* u = get(id); if (u && u->tpl && !u->isStatic()) u->attackMove = true; }
}

void Game::orderAttackTarget(std::vector<int> ids, int targetId) {
    Unit* tgt = get(targetId); if (!tgt) return;
    for (int id : ids) {
        Unit* u = get(id); if (!u || !u->tpl || u->isStatic() || !u->tpl->canAttack) continue;
        u->orderTargetId = targetId; u->attackMove = false; u->constructingId = -1;
        if (u->pos.dist(tgt->pos) > u->tpl->attackRange * 0.9f) computePath(*this, *u, tgt->pos);
    }
}

void Game::orderStop(std::vector<int> ids) {
    for (int id : ids) { Unit* u = get(id); if (!u) continue; u->path.clear(); u->pathIdx = 0; u->orderTargetId = -1; u->attackMove = false; u->constructingId = -1; }
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
        if (!map.inBounds(x, y) || !map.canPlaceBuilding(x, y)) return false;
    if (tpl->placeOnlyOnResPool) {
        bool res = false;
        for (int y = miny; y < miny + fh && !res; ++y) for (int x = minx; x < minx + fw && !res; ++x)
            if (map.inBounds(x, y) && map.at(x, y).resource) res = true;
        if (!res) return false;
    }
    Vec2 center{ (tx + 0.5f) * TILE_SIZE, (ty + 0.5f) * TILE_SIZE };
    if (!buildingSpotFree(center, *tpl)) return false;
    spend(b->team, tpl->price);
    int bid = spawnUnit(unitName, b->team, center, true);
    if (bid < 0) { reward(b->team, tpl->price); return false; }
    b->constructingId = bid;
    return true;
}

bool Game::orderProduce(int factoryId, const std::string& unitName) {
    Unit* f = get(factoryId); if (!f || !f->tpl || !f->tpl->isFactory || !f->operational()) return false;
    const UnitTemplate* tpl = g_templates.find(ToLower(unitName)); if (!tpl) return false;
    if (!canAfford(f->team, tpl->price)) return false;
    spend(f->team, tpl->price);
    f->prodQueue.push_back(ToLower(unitName));
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
    for (auto& e : effects) { e.t += 1.0f / TICK_RATE; if (e.t >= e.dur) e.dead = true; }
    units.erase(std::remove_if(units.begin(), units.end(), [](const Unit& u) { return u.dead; }), units.end());
    projectiles.erase(std::remove_if(projectiles.begin(), projectiles.end(), [](const Projectile& p) { return p.dead; }), projectiles.end());
    effects.erase(std::remove_if(effects.begin(), effects.end(), [](const Effect& e) { return e.dead; }), effects.end());
    if (tick % 30 == 0) runAI();
    checkWinLoss();
}

void Game::updateUnit(Unit& u) {
    if (u.dead || !u.tpl) return;
    const UnitTemplate& tpl = *u.tpl;
    if (u.underConstruction && u.buildProgress >= 1.0f) u.underConstruction = false;
    if (tpl.creditIncomeRate > 0 && u.operational() && tick % 10 == 0)
        reward(u.team, tpl.creditIncomeRate * (10.0f / 40.0f));

    if (tpl.isFactory && u.operational() && !u.prodQueue.empty()) {
        const UnitTemplate* item = g_templates.find(u.prodQueue.front());
        u.prodProgress += item ? item->buildSpeed : tpl.buildSpeed;
        if (u.prodProgress >= 1.0f) {
            u.prodProgress = 0; std::string name = u.prodQueue.front(); u.prodQueue.erase(u.prodQueue.begin());
            Vec2 sp = u.hasRally ? u.rallyPoint : u.pos + Vec2{ 0, (tpl.footprintTilesH() / 2 + 1.5f) * TILE_SIZE };
            spawnUnit(name, u.team, sp);
        }
    }

    if (tpl.isBuilder && u.constructingId >= 0) {
        Unit* bs = get(u.constructingId);
        if (!bs || bs->dead) u.constructingId = -1;
        else if (bs->operational()) u.constructingId = -1;
        else {
            float d = u.pos.dist(bs->pos);
            if (d > tpl.nanoRange) computePath(*this, u, bs->pos + Vec2{ 0, -TILE_SIZE * 2 });
            else {
                // RWX: nanoBuildSpeed multiplies with the target's buildSpeed (per-tick rate)
                float target = bs->tpl->buildSpeed > 0 ? bs->tpl->buildSpeed : 0.001f;
                float rate = (tpl.nanoBuildSpeed > 0 ? tpl.nanoBuildSpeed : 1.0f) * target;
                bs->buildProgress = std::min(1.0f, bs->buildProgress + rate);
                if (bs->buildProgress >= 1.0f) bs->underConstruction = false;
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

    if (!u.isStatic() && u.pathIdx < (int)u.path.size()) {
        Vec2 wp = u.path[u.pathIdx];
        Vec2 d = wp - u.pos; float dist = d.len(); float sp = tpl.moveSpeed;
        if (dist <= sp + 1.0f) { u.pathIdx++; if (u.pathIdx >= (int)u.path.size()) { u.path.clear(); u.pathIdx = 0; } }
        else {
            u.pos = u.pos + d.norm() * sp; u.animFrame += 0.2f;
            float want = std::atan2(d.y, d.x);
            u.facing = lerpAngle(u.facing, want, tpl.maxTurnSpeed * 0.05236f);
        }
    }

    if (u.shootCooldown > 0) u.shootCooldown -= 1;
    if (tpl.canAttack && u.operational() && tgt) {
        float want = Vec2::angleTo(u.pos, tgt->pos);
        u.turretRot = lerpAngle(u.turretRot, want, tpl.turretTurnSpeed * 0.05236f);
        float dist = u.pos.dist(tgt->pos);
        bool aimed = tpl.isFixedFiring || std::fabs(lerpAngle(u.turretRot, want, 1e9f) - u.turretRot) < 0.5f;
        if (dist <= tpl.attackRange + tgt->tpl->radius && aimed && u.shootCooldown <= 0) { fire(u, *tgt); u.shootCooldown = tpl.shootDelay; }
    }

    if (u.attackMove && u.path.empty() && combatTarget < 0) u.attackMove = false;
    if (u.orderTargetId >= 0 && tgt && u.path.empty() && !u.isStatic() && u.pos.dist(tgt->pos) > tpl.attackRange * 0.9f)
        computePath(*this, u, tgt->pos);
}

void Game::fire(Unit& u, Unit& target) {
    const UnitTemplate& tpl = *u.tpl;
    TurretTpl tur = tpl.turrets.empty() ? TurretTpl{} : tpl.turrets[0];
    int pi = (tur.projectile > 0 && tur.projectile <= (int)tpl.projectiles.size()) ? tur.projectile - 1 : -1;
    ProjectileTpl pt = pi >= 0 ? tpl.projectiles[pi] : ProjectileTpl{};
    if (pi < 0) { pt.directDamage = 10; pt.speed = 6; pt.life = 60; }
    Projectile p;
    p.pos = u.pos + Vec2{ std::cos(u.turretRot), std::sin(u.turretRot) } * (tpl.radius * 0.8f);
    p.team = u.team; p.sourceId = u.id; p.targetId = target.id; p.dest = target.pos;
    p.damage = pt.directDamage; p.areaDamage = pt.areaDamage; p.areaRadius = pt.areaRadius;
    p.speed = pt.speed; p.life = pt.life; p.frame = pt.frame; p.drawSize = pt.drawSize;
    p.explodeOnHit = pt.explodeOnHit || pt.areaDamage > 0; p.largeHit = pt.largeHit;
    p.vel = (target.pos - p.pos).norm() * p.speed;
    projectiles.push_back(p);
}

void Game::updateProjectile(Projectile& p) {
    if (p.dead) return;
    Unit* t = p.targetId >= 0 ? get(p.targetId) : nullptr;
    Vec2 aim = (t && !t->dead) ? t->pos : p.dest;
    Vec2 d = aim - p.pos; float dist = d.len();
    if (dist < p.speed + 2.0f) { impact(p, aim); p.dead = true; return; }
    p.vel = d.norm() * p.speed;
    p.pos = p.pos + p.vel;
    if (--p.life <= 0) { if (p.explodeOnHit) impact(p, p.pos); p.dead = true; }
}

void Game::impact(Projectile& p, Vec2 point) {
    effects.push_back({ point, 0, p.largeHit ? 0.6f : 0.3f, p.areaRadius > 0 ? p.areaRadius : 8, p.largeHit });
    if (p.damage > 0) { Unit* t = p.targetId >= 0 ? get(p.targetId) : nullptr; if (t && !t->dead) damage(*t, p.damage); }
    if (p.areaDamage > 0)
        for (auto& u : units) {
            if (!u.tpl || u.dead || !isEnemy(p.team, u.team)) continue;
            if (u.pos.dist(point) <= p.areaRadius) damage(u, p.areaDamage);
        }
}

void Game::damage(Unit& target, float dmg) {
    if (dmg <= 0 || target.dead) return;
    target.hp -= dmg;
    if (target.hp <= 0) killUnit(target);
}

void Game::killUnit(Unit& u) {
    if (u.dead) return;
    u.dead = true;
    bool big = u.tpl && u.tpl->radius > 20;
    effects.push_back({ u.pos, 0, big ? 0.6f : 0.35f, u.tpl ? u.tpl->radius * 1.2f : 12, big });
    for (auto& o : units) { if (o.orderTargetId == u.id) o.orderTargetId = -1; if (o.constructingId == u.id) o.constructingId = -1; }
    for (auto& p : projectiles) if (p.targetId == u.id) p.targetId = -1;
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
                if (u.team != teamId || !u.tpl || !u.tpl->isBuilder || u.dead || u.constructingId >= 0) continue;
                for (int y = 1; y < map.h - 1 && u.constructingId < 0; ++y)
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
