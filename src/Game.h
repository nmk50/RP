// Core simulation: teams, units, projectiles, orders, economy, combat.
#pragma once
#include <string>
#include <vector>
#include "Common.h"
#include "TileMap.h"
#include "UnitTemplate.h"

struct Unit {
    int id = -1;
    std::string tplKey;
    const UnitTemplate* tpl = nullptr;
    int team = 0;
    Vec2 pos;
    float facing = 0;        // body direction (radians)
    float turretRot = 0;     // current turret angle
    float hp = 1, maxHp = 1;
    std::vector<Vec2> path;  // world-space waypoints
    int pathIdx = 0;
    bool attackMove = false;
    int orderTargetId = -1;  // explicit attack target
    int moveGoalId = -1;     // chase a moving unit target
    int constructingId = -1; // builder -> building it is constructing
    int targetUnitId = -1;   // repair/reclaim/harvest target
    std::vector<std::string> prodQueue;
    float prodProgress = 0;
    bool underConstruction = false;
    float buildProgress = 0; // 0..1 for buildings being constructed
    float shootCooldown = 0;
    Vec2 rallyPoint; bool hasRally = false;
    bool selected = false;
    bool dead = false;
    float animFrame = 0;
    int footprint[4] = { 0,0,0,0 };
    // self upgrade action (RWX [action_upgrade*])
    int upgradeIdx = -1;
    float upgradeProgress = 0;
    // movement state (RWX-style follow/repath/stuck recovery)
    Vec2 moveGoal;
    bool hasGoal = false;
    int repathTimer = 0;
    int stuckTimer = 0;
    Vec2 prevPos;
    // RWX BaseUnit.targetRotation / .direction: heading the hull is still turning toward,
    // and its angular velocity (ramped by [movement] turnAcceleration).
    float targetFacing = 0, turnVel = 0;
    bool hasFacingTarget = false;
    // RWX BaseUnit.rotation: signed 0..1 throttle ramped by moveAcceleration/DecelerationSpeed,
    // negative while flip-turning; isRotating marks the >100 deg reverse-and-pivot manoeuvre.
    float throttle = 0;
    bool isRotating = false;
    // RWX shoot_flame: the flame effect is attached to the unit, so it keeps its offset from the
    // firing turret and its angle while it plays.
    int muzzleType = 0;
    float muzzleT = 0, muzzleAng = 0;
    Vec2 muzzleOff;
    bool isStatic() const { return tpl && tpl->moveType == MoveType::NONE; }
    bool operational() const { return !underConstruction || buildProgress >= 1.0f; }
};

// world-space turret pivot; sprite-space (x,y) offset rotated with the body, k = px scale
Vec2 turretPivot(const Unit& u, const TurretTpl& tur, float k);

struct Projectile {
    Vec2 pos, vel;
    int team = 0;
    float damage = 0, areaDamage = 0, areaRadius = 0;
    int sourceId = -1;
    int targetId = -1;      // homing unit (-1 = move to dest)
    Vec2 dest;
    float speed = 6;
    int life = 60;
    int frame = -1;
    int drawType = 0;
    int shadowFrame = -1;
    float drawSize = 1;
    uint32_t color = 0xFFFFFFFF;   // 0xRRGGBBAA sprite tint
    uint32_t lightColor = 0;       // 0xRRGGBBAA additive glow, 0 = none
    float lightSize = 0.5f;
    bool largeHit = false;
    bool dead = false;
    // RWX Projectile flight state (Projectile.update): the shot owns a heading that either re-aims
    // at the target every tick (turnSpeed < 0) or turns toward it by turnSpeed deg/tick, and a
    // speed that ramps toward targetSpeed. az / O / r / s.
    float heading = 0, turnSpeed = -1;
    float speedTarget = -1, speedAccel = 0.1f;
    // wobble: a sine drift sideways to the heading, phase I is random per shot (update:905)
    float wobbleAmp = 0, wobbleFreq = 5, wobblePhase = 0;
    int ticks = 0;
    float buildingMult = 1;                  // ai: buildingDamageMultiplier
    // Once the shot lands (bn) the splash ring grows over areaExpandTime ticks (ao/W) centred on
    // the impact point; the projectile itself is no longer drawn (update:1038 sets shouldDraw=false).
    bool hit = false;
    Vec2 impactPos;
    float expandT = 0, expandTotal = 0;
    bool airBurst = false;                   // RWX impact height: splash stays in that domain
    std::vector<int> splashHits;             // ap: units already caught by the growing ring
};

// RWX EffectManager.createSmallExplosion: explode_big.png strip, frames 0..12 at 0.5/tick,
// with a random spin, scale and (for big explosions) a delayed start.
struct Effect {
    Vec2 pos;
    float t = 0;              // ticks since spawn
    float delay = 0;          // RWX delayedStartTimer, in ticks
    float rot = 0;            // sprite rotation, radians
    float scale = 1;          // 0.8..1 like RWX
    float frameSpeed = 0.5f;  // frames per tick
    bool dead = false;
};

// nano build particle: flies from builder to the target building (RWX OrderableUnit nano effect)
struct Particle {
    Vec2 pos, vel;
    float t = 0, dur = 20;
    bool reclaim = false; // red when reclaiming, green when building
    bool dead = false;
};

// RWX ScorchMark: a permanent ground decal left by an exploding unit — blood_mark.png is a row of
// four 50x40 cells and the mark picks one of them at random.
struct Scorch { Vec2 pos; int variant = 0; };

// RWX CustomUnit.java:946: a unit with image_wreak stays on the map as an inert wreck sprite.
struct Wreck { Vec2 pos; float facing = 0; const UnitTemplate* tpl = nullptr; };

struct Team {
    int id = 0;
    std::string name;
    int color = 0;   // palette index
    float credits = 0;
    bool isPlayer = false;
    bool alive = true;
};

class Game {
public:
    TileMap map;
    std::vector<Unit> units;
    std::vector<Projectile> projectiles;
    std::vector<Effect> effects;
    std::vector<Particle> particles;
    std::vector<Scorch> scorches;   // permanent ground decals, drawn with the map
    std::vector<Wreck> wrecks;      // dead units that left an image_wreak behind
    std::vector<Team> teams;
    long tick = 0;
    bool gameOver = false;
    std::string gameOverText;
    int nextId = 1;

    bool init(const std::string& mapPath, std::string* err);
    void update();

    // queries
    Unit* get(int id);
    const Unit* get(int id) const;
    std::vector<Unit*> findEnemyUnits(int team, Vec2 center, float radius, bool flying, bool land, bool underwater);
    std::vector<Unit*> findUnitsInRect(int team, Vec2 a, Vec2 b);
    std::vector<Unit*> selectedUnits();

    // orders
    void orderMove(std::vector<int> ids, Vec2 target);
    void orderAttackMove(std::vector<int> ids, Vec2 target);
    int  orderAttackTarget(std::vector<int> ids, int targetId); // returns #units assigned
    bool orderConstruct(std::vector<int> builderIds, int buildingId);
    void orderStop(std::vector<int> ids);
    bool orderBuild(int builderId, const std::string& unitName, int tx, int ty);
    bool orderProduce(int factoryId, const std::string& unitName);
    bool orderUpgrade(int unitId, int actionIdx);
    bool buildingSpotFree(Vec2 center, const UnitTemplate& t) const;
    bool canAfford(int team, float cost);
    void spend(int team, float cost);
    void reward(int team, float credits);
    int  spawnUnit(const std::string& tplKey, int team, Vec2 pos, bool underConstruction = false);

    // helpers
    bool isEnemy(int a, int b) const { return a != b; }
    Vec2 nearestEnemyPos(int team);
    int playerTeam() const { return 0; }

    void damage(Unit& target, float dmg);
    void impact(Projectile& p, Unit* direct, Vec2 at);
    void splash(Projectile& p, float frac);
    void checkWinLoss();

private:
    void updateUnit(Unit& u);
    void updateProjectile(Projectile& p);
    void killUnit(Unit& u);
    void explode(Vec2 at, bool big, float scale = 1.0f);
    void addScorch(Vec2 at);
    void fire(Unit& u, Unit& target);
    void runAI();
    std::vector<Unit*> queryNearestEnemy(int team, Vec2 pos, float maxDist, const UnitTemplate* shooter);
};
