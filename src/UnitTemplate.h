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
    float barrelXOdd = 0;     // barrelOffsetX_onOddShots: sideways jump on every other shot
    float drawOffsetX = 0, drawOffsetY = 0;
    // RWX TurretConfig.projectileId: index into the unit's projectiles, chosen by the
    // [projectile_<name>] section a turret names with `projectile: <name>`
    int projectile = 0;
    // [turret_N] image, empty = the unit's [graphics] image_turret (CustomUnit.d(i):691)
    std::string image;
    bool invisible = false;
    bool canShoot = true;
    float turnSpeed = 4.0f;
    // turnSpeedAcceleration (w(i), -1 = snap to the turn speed): the barrel has angular inertia and
    // may overshoot, which is what makes heavy turrets lag behind a fast target.
    float turnAccel = -1, turnDecel = -1;
    // RWX UnitMovementData.rotation countdown: TurretConfig.m, from `delay` or [attack] shootDelay.
    // linkDelayWithTurret sets 9000 so the parent's barrage is what restarts this barrel's timer.
    float shootDelay = -1;
    float warmup = 0;
    float warmCallDown = 4;   // warmupCallDownRate: how fast an interrupted charge bleeds off
    bool warmNoReset = false; // warmupNoReset: the charge survives between barrages
    // limitingRange / limitingMinRange / canAttackMaxAngle: what this barrel may hit and how well it
    // must be aligned (5 deg by default) before it is considered aimed.
    float range = 0, minRange = 0, maxAngle = 5;
    // recoilOffset / recoilOutTime / recoilReturnTime: the barrel slides along its aim after a shot
    float recoil = 0, recoilOut = 0, recoilReturn = 0;
    // attachedTo / linkDelayWithTurret, resolved to turret indexes (-1 = none)
    int attached = -1, linkDelay = -1;
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
    // RWX [projectile_N] trailEffect is read as a *string* (CustomProjectileTemplate.java:142-152):
    // only an exact "true" turns on the built-in smoke puff, while "false" and a custom spawn list
    // both leave it off (RWX lets those lists spawn their own effects). trailEffectRate (:157,
    // default 3 ticks per ProjectileTemplate.java:100) spaces the puffs, and when the flag is on
    // lightColor is pre-set to ARGB 0xFFEEEE00 before the ini gets a chance to override it (:158-160,169).
    bool trail = false;
    float trailRate = 3.0f;
    // RWX [projectile_N] flight model (Projectile.update). turnSpeed (template.O, default -1) is
    // degrees per tick the heading may rotate toward the target; -1 re-aims every tick, 0 flies dead
    // straight. speed ramps toward targetSpeed by targetSpeedAcceleration per tick.
    float turnSpeed = -1;
    float targetSpeed = -1, targetSpeedAccel = 0.1f, speedSpread = 0;
    // instant: the shot never moves and lands on the firing tick (Projectile.update:841,979)
    bool instant = false;
    // wobbleAmplitude (px per tick sideways) / wobbleFrequency (ticks per full sine)
    float wobbleAmp = 0, wobbleFreq = 5;
    // autoTargetingOnDeadTarget: once the chased unit is gone the shot looks for a new enemy around
    // the point lead px ahead of its own heading, within range (Projectile.java:699-702, 1776-1820).
    bool autoRetarget = false;
    float autoRetargetRange = 120, autoRetargetLead = 15;
    // areaExpandTime: the splash ring grows from nothing to areaRadius over these ticks and each
    // unit is caught once (Projectile.b(f2):1443 with the Projectile.ap list)
    float areaExpandTime = -1;
    // buildingDamageMultiplier: only scales damage dealt to buildings (Projectile.a:228)
    float buildingDamageMult = 1;
    // How a shot spends itself against a shielded hull (BaseUnit.setTarget:1567-1589): it drains
    // shieldDamageMultiplier * damage off the bubble, the part that runs past an empty bubble is cut
    // by shieldDeflectionMultiplier before it reaches the hull, and hullDamageMultiplier scales that
    // hull hit (a nuke's EMP variant uses 0). armourIgnoreAmount subtracts from the target's armour.
    float shieldDamageMult = 1, shieldDeflectMult = 1, hullDamageMult = 1;
    float armourIgnore = 0;
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
    // RWX [core] shields (BaseUnit.shield / unitEnergyMax): maxShield is the pool size, shieldRegen
    // refills it every tick - default 0.25, i.e. 15 per second - and startShieldAtZero leaves a fresh
    // unit's bubble empty. shieldRenderRadius (default radius) is how big the bubble is drawn, and
    // the white flash it gets when hit fades at shieldDeflectionDisplayRate per tick.
    int maxShield = 0;
    float shieldRegen = 0.25f;
    bool startShieldAtZero = false;
    float shieldRenderRadius = 0;
    float shieldFlashRate = 4.0f;
    bool shieldOnlyDeflectionFlash = false;   // shieldDisplayOnlyDeflection
    bool showShieldBar = true;                // [graphics] showShieldBar
    std::string imageShield;                  // [graphics] image_shield, empty = drawable/shield_mid.png
    // [core] armour: a flat amount taken off every hit before the shield, with armourMinDamageToKeep
    // as the floor so weak weapons still chip away.
    float armour = 0, armourMinKeep = 1;
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
    // [attack] turretRotateWithBody: the hull's turn is added to every barrel's aim (default true)
    bool turretRotateWithBody = true;
    // RWX parser defaults: CustomUnitConfigParser.java:2042 (shootDelay 50 ticks), :2015
    // (turretTurnSpeed 8 deg/tick), :1959-1961/:2008-2009 (every numeric [movement] key is 1.0f),
    // :1774 (nanoRepairSpeed 0.2 hp/tick).
    float attackRange = 0, shootDelay = 50, turretSize = 0, turretTurnSpeed = 8;

    MoveType moveType = MoveType::NONE;
    float moveSpeed = 1, accel = 1, decel = 1, maxTurnSpeed = 1, turnAccel = 1;
    float reverseSpeed = 0.6f; // RWX [movement] reverseSpeedPercentage: allows the flip-turn
    // [movement] moveIgnoringBody (:2011) steers by the wanted bearing instead of the hull's facing, so
    // the unit strafes and backs up while the body keeps turning (OrderableUnit.java:876-878,
    // 2913-2915, 3003-3005). [movement] moveSlidingMode (:2010) instead makes the motion an integrated
    // velocity with a 1.41x steering rate and no throttle ramp (:893-918, :3009-3010) -- glide, not
    // strafing. Both default false.
    bool moveIgnoringBody = false, moveSlidingMode = false;

    float nanoRange = 0, nanoBuildSpeed = 0, nanoRepairSpeed = 0.2f;
    // RWX repair gates: CustomUnitConfigParser.java:1746-1747 (both default false) and :1732
    // (isUnrepairableUnit), plus nanoRangeForRepair (:1760-1763, unset -1 means use nanoRange).
    bool canRepairBuildings = false, canRepairUnits = false, isUnrepairableUnit = false;
    float nanoRangeForRepair = 0;
    // [core] selfBuildRate (:1781, getInvertedTime) raises a foundation without a builder
    // (CustomUnit.update:1774-1782); [core] dieOnConstruct (:1782) gives the builder to the site it
    // places (CustomUnit.m():3668-3679).
    float selfBuildRate = 0;
    bool dieOnConstruct = false;
    // [core] selfRegenRate (:1780, plain getFloat) is HP per tick recovered with no builder and no
    // shield involved; it runs for every live unit, foundation included (CustomUnit.update:1821-1826).
    float selfRegenRate = 0;

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
