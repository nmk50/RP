// Data-driven unit templates parsed from RWX .ini files.
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "Common.h"

enum class MoveType { NONE, LAND, AIR, WATER, HOVER, OVER_CLIFF };

struct TurretTpl {
    float x = 0, y = 0;
    // RWX [turret_N]: size/barrelY = muzzle distance along the aim (defaults to [attack] turretSize),
    // barrelX = lateral offset, image_drawOffsetX/Y = turret sprite shift relative to the pivot.
    float barrelY = -1, barrelX = 0;
    float drawOffsetX = 0, drawOffsetY = 0;
    int projectile = 0;      // index into projectiles (1-based like RWX, 0 = none/nano)
    bool invisible = false;
    bool canShoot = true;
    float turnSpeed = 4.0f;
    float warmup = 0;
    // RWX [turret_N]: idle rest angle relative to the hull, and whether the turret returns to it
    float idleDir = 0;
    bool shouldResetTurret = true;
    // RWX shoot_flame built-in effects: 0 none, 1 small, 2 medium, 3 large (flame.png frames)
    int shootFlame = 0;
    bool canAttackFlying = true, canAttackLand = true, canAttackUnderwater = false;
};

struct ProjectileTpl {
    float directDamage = 0, areaDamage = 0, areaRadius = 0;
    float speed = 6;
    int life = 60;
    // RWX [projectile_N] sprite: frame indexes the single-row sheet (frame index -1 = plain coloured
    // dot, ProjectileTemplate.java:84). drawType picks the sheet: 0 projectiles.png (20px cells),
    // 1 projectiles_large.png (60px), 2 projectiles2.png. shadowFrame draws the same frame in black
    // on the ground. color tints the sprite (RWX "color : #RRGGBB" / "#AARRGGBB").
    int frame = -1;
    int drawType = 0;
    int shadowFrame = -1;
    float drawSize = 1;
    uint32_t color = 0xFFFFFFFF;
    // RWX [projectile_N] lightColor/lightSize: an additive light_50.png glow follows the projectile
    // (EffectManager.createLightEffect: alpha 0.3, size = texture * lightSize, default 0.5). 0 = off.
    uint32_t lightColor = 0;
    float lightSize = 0.5f;
    // RWX [projectile_N] flight model (Projectile.update). turnSpeed (template.O, default -1) is
    // degrees per tick the heading may rotate toward the target; -1 re-aims every tick, 0 flies dead
    // straight. speed ramps toward targetSpeed by targetSpeedAcceleration per tick.
    float turnSpeed = -1;
    float targetSpeed = -1, targetSpeedAccel = 0.1f, speedSpread = 0;
    // instant: the shot never moves and lands on the firing tick (Projectile.update:841,979)
    bool instant = false;
    // wobbleAmplitude (px per tick sideways) / wobbleFrequency (ticks per full sine)
    float wobbleAmp = 0, wobbleFreq = 5;
    // areaExpandTime: the splash ring grows from nothing to areaRadius over these ticks and each
    // unit is caught once (Projectile.b(f2):1443 with the Projectile.ap list)
    float areaExpandTime = -1;
    // buildingDamageMultiplier: only scales damage dealt to buildings (Projectile.a:228)
    float buildingDamageMult = 1;
    bool largeHit = false;
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
    // RWX [graphics] image_shadow: "AUTO" = the body silhouette in black, offset by shadowOffsetX/Y world px
    std::string imageShadow;
    float shadowOffX = 0, shadowOffY = 0;
    std::string sourceDir;   // directory of the ini, for image resolution

    float price = 100, maxHp = 100, mass = 1000;
    float radius = 12, displayRadius = 12;
    int techLevel = 1;
    float buildSpeed = 0.001f;
    bool isBuilding = false, isBuilder = false, isFactory = false;
    int footL = 0, footU = 0, footR = 0, footD = 0; // footprint in tiles (relative to center tile)
    float fogSight = 0;
    float energyUse = 0;
    // RWX [core] explodeOnDeath (default true) / hideScorchMark: an exploding unit leaves a
    // permanent ground mark (ScorchMark.java) unless it is hidden or died over water.
    bool explodeOnDeath = true;
    bool hideScorchMark = false;

    float imageScale = 1, turretImageScale = 1;
    int totalFrames = 1;
    std::string drawLayer;

    bool canAttack = false;
    bool canAttackFlying = false, canAttackLand = true, canAttackUnderwater = false;
    bool isFixedFiring = false;
    float attackRange = 0, shootDelay = 60, turretSize = 0, turretTurnSpeed = 3;

    MoveType moveType = MoveType::NONE;
    float moveSpeed = 0, accel = 0.07f, decel = 0.17f, maxTurnSpeed = 4, turnAccel = 0.25f;
    float reverseSpeed = 0.6f; // RWX [movement] reverseSpeedPercentage: allows the flip-turn

    float nanoRange = 0, nanoBuildSpeed = 0, nanoRepairSpeed = 0;

    std::vector<TurretTpl> turrets;
    std::vector<ProjectileTpl> projectiles;
    std::vector<BuildOption> canBuild;
    std::vector<BuildOption> builtFrom;
    std::vector<UnitAction> actions;

    bool placeOnlyOnResPool = false;
    bool placeOnlyOnWater = false; // RWX SeaFactory: must sit on water
    float creditIncomeRate = 0; // for extractors
    bool useAsBuilder = false, useAsAttacker = false, useAsHarvester = false;

    // resolved texture id (raylib), -1 until loaded / missing
    int texId = -1, texWreakId = -1, texTurretId = -1;
    mutable int texFrameWpx = 0; // body sprite frame width in px (set by renderer, for pivot scaling)

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
