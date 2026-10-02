// Data-driven unit templates parsed from RWX .ini files.
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "Common.h"

enum class MoveType { NONE, LAND, AIR, WATER, HOVER, OVER_CLIFF };

struct TurretTpl {
    float x = 0, y = 0;
    int projectile = 0;      // index into projectiles (1-based like RWX, 0 = none/nano)
    bool invisible = false;
    bool canShoot = true;
    float turnSpeed = 4.0f;
    float warmup = 0;
    bool canAttackFlying = true, canAttackLand = true, canAttackUnderwater = false;
};

struct ProjectileTpl {
    float directDamage = 0, areaDamage = 0, areaRadius = 0;
    float speed = 6;
    int life = 60;
    int frame = 0;           // sprite frame in projectiles sheet
    float drawSize = 1;
    bool largeHit = false;
    bool explodeOnHit = false;
    bool pushForce = false;
};

struct BuildOption { std::string unitName; float pos = 1; };

// [action_upgradeT2] / action_N_* self-actions, e.g. unit upgrades (RWX displayType:upgrade)
struct UnitAction {
    std::string convertTo;
    std::string text;
    float price = 0;
    float buildSpeed = 0.001f; // per-tick progress
    int pos = 0;
};

struct UnitTemplate {
    std::string name;
    std::string overrideName;
    std::string displayText;
    std::string image, imageWreak, imageTurret;
    std::string sourceDir;   // directory of the ini, for image resolution

    float price = 100, maxHp = 100, mass = 1000;
    float radius = 12, displayRadius = 12;
    int techLevel = 1;
    float buildSpeed = 0.001f;
    bool isBuilding = false, isBuilder = false, isFactory = false;
    int footL = 0, footU = 0, footR = 0, footD = 0; // footprint in tiles (relative to center tile)
    float fogSight = 0;
    float energyUse = 0;

    float imageScale = 1, turretImageScale = 1;
    int totalFrames = 1;
    std::string drawLayer;

    bool canAttack = false;
    bool canAttackFlying = false, canAttackLand = true, canAttackUnderwater = false;
    bool isFixedFiring = false;
    float attackRange = 0, shootDelay = 60, turretSize = 0, turretTurnSpeed = 3;

    MoveType moveType = MoveType::NONE;
    float moveSpeed = 0, accel = 0.07f, decel = 0.17f, maxTurnSpeed = 4, turnAccel = 0.25f;

    float nanoRange = 0, nanoBuildSpeed = 0, nanoRepairSpeed = 0;

    std::vector<TurretTpl> turrets;
    std::vector<ProjectileTpl> projectiles;
    std::vector<BuildOption> canBuild;
    std::vector<BuildOption> builtFrom;
    std::vector<UnitAction> actions;

    bool placeOnlyOnResPool = false;
    float creditIncomeRate = 0; // for extractors
    bool useAsBuilder = false, useAsAttacker = false, useAsHarvester = false;

    // resolved texture id (raylib), -1 until loaded / missing
    int texId = -1, texWreakId = -1, texTurretId = -1;

    bool blocksGround() const { return moveType == MoveType::NONE; } // buildings/static
    bool isStatic() const { return moveType == MoveType::NONE; }
    int footprintTilesW() const { return footR - footL + 1; }
    int footprintTilesH() const { return footD - footU + 1; }
    bool isWaterUnit() const { return moveType == MoveType::WATER; }
    bool isAirUnit() const { return moveType == MoveType::AIR || moveType == MoveType::HOVER; }
};

class TemplateStore {
public:
    std::unordered_map<std::string, UnitTemplate> byName; // keyed by name and override alias
    std::vector<std::string> order;                        // load order (canonical names)

    void loadAll(const std::string& assetsRoot);
    const UnitTemplate* find(const std::string& name) const {
        auto it = byName.find(name);
        return it == byName.end() ? nullptr : &it->second;
    }
};

extern TemplateStore g_templates;
