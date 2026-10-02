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
    int frame = 0;
    float drawSize = 1;
    bool explodeOnHit = false;
    bool largeHit = false;
    bool dead = false;
};

struct Effect {
    Vec2 pos; float t = 0, dur = 0.3f; float radius = 10; bool large = false; bool dead = false;
};

// nano build particle: flies from builder to the target building (RWX OrderableUnit nano effect)
struct Particle {
    Vec2 pos, vel;
    float t = 0, dur = 20;
    bool reclaim = false; // red when reclaiming, green when building
    bool dead = false;
};

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
    void impact(Projectile& p, Vec2 point);
    void checkWinLoss();

private:
    void updateUnit(Unit& u);
    void updateProjectile(Projectile& p);
    void killUnit(Unit& u);
    void fire(Unit& u, Unit& target);
    void runAI();
    std::vector<Unit*> queryNearestEnemy(int team, Vec2 pos, float maxDist, const UnitTemplate* shooter);
};
