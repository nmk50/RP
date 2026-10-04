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
    // targetGround (CustomProjectileTemplate.java:48): the shot is never guided onto the unit. The
    // firing code freezes a lead-predicted ground point instead (CustomProjectileTemplate.a():377-405)
    // and leaves Projectile.l null, so only the splash can hurt anything. targetGroundSpread (:224)
    // scatters that point by up to this many px on each axis, which is why artillery misses.
    bool targetGround = false;
    float targetGroundSpread = 0;
    // initialUnguidedSpeedHeight (:115) is a vertical speed in px/tick the shell keeps, while gravity
    // (:118) takes a fixed height off it every tick above the ground and trueGravity (:119) decelerates
    // the vertical speed itself. Projectile.update:844-856. Only gravity and the initial height are used
    // by the shipped assets; trueGravity is what makes a shell arc back down on its own.
    float launchVelZ = 0, gravity = 0, trueGravity = 0;
    // ballistic (:141) throws the shell up to [ballistic_height] before it may close on its target, and
    // the horizontal step is skipped until it is above [ballistic_delaymove_height] -- the round climbs
    // straight out of the barrel, then arcs over (:857-888). Both default to -1 in the template, which
    // means "leave the shot's own 40 / 60" (CustomUnit.java:1490-1495 with Projectile:204-205).
    bool ballistic = false;
    float ballisticDelayHeight = -1, ballisticHeight = -1;
};

struct BuildOption { std::string unitName; float pos = 1; };

// One keyframe of a body animation. `t` is in ticks: a key written "0.8s" is 48 (the trailing 's' is
// worth 60, IniFile.parseUnitValue:761-783), and the animation clock steps by 1 per logic tick because
// GameLogic.update passes f = deltaSpeed * speedMultiplier, i.e. one tick at normal speed
// (GameLogic.java:1495-1523, where gameTimeMillis grows by f * 16.666666).
struct AnimKey { float t = 0, v = 0; };

// RWX AnimationConfig (units/custom/AnimationConfig.java) cut down to the two key lists that move the
// hull sprite: the frame index (CustomUnit.animationFrameIndex) and a scale that multiplies
// [graphics] imageScale (CustomUnit.getDrawScale:2328 `imageScale * currentFrameTime`). Both ini forms
// compile to this: the old-style [graphics] animation_moving_* keys build the lists straight from
// start/end/speed/scale_start/scale_end (AnimationConfig.java:103-146) and a [animation_*] section
// spells them out as body_0.8s : {frame:3} keyframes instead (AnimationConfig.a():211-248).
struct BodyAnim {
    std::vector<AnimKey> frame, scale;   // CustomUnitAnimationSet.e, sorted by time
    float maxTime = 0;                   // AnimationConfig.n, the loop length in ticks
    bool pingPong = false;               // AnimationConfig.g

    bool valid() const { return !frame.empty() || !scale.empty(); }
    // CustomUnitAnimationSet.b(time):116-135 -- hold the first and last key, linear in between
    static float valueAt(const std::vector<AnimKey>& k, float t) {
        if (k.empty()) return 0.0f;
        if (t <= k.front().t) return k.front().v;
        if (t >= k.back().t) return k.back().v;
        for (size_t i = 1; i < k.size(); ++i) {
            if (t > k[i].t) continue;
            const AnimKey& a = k[i - 1]; const AnimKey& b = k[i];
            return a.v + (b.v - a.v) * (t - a.t) / (b.t - a.t);
        }
        return k.back().v;
    }
    float frameAt(float t) const { return valueAt(frame, t); }
    float scaleAt(float t) const { return scale.empty() ? 1.0f : valueAt(scale, t); }
};

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
    // [graphics] lock_body_rotation_with_main_turret (:1583) draws the body sprite along the main
    // turret's aim instead of the heading the unit travels on (CustomUnit.getRenderRotation:2345-2347),
    // so a bug or mech keeps facing its prey while strafing past it. Only the sprite turns -- the hull's
    // game rotation still drives movement and turret pivots. lock_shadow_rotation_with_main_turret
    // (:1584) does the same for the shadow and takes the body's value as its default.
    bool lockBodyRot = false, lockShadowRot = false;
    std::string sourceDir;   // directory of the ini, for image resolution

    float price = 100, maxHp = 100, mass = 1000;
    float radius = 12, displayRadius = 12;
    // [core] softCollisionOnAll (CustomUnitConfigParser.java:1721) divides the overlap a unit corrects
    // per tick: at 12 a hull sinks out of an intersection a twelfth of the way each frame instead of
    // being shoved clear at once, which is what makes the big experimental units feel heavy. The
    // resolve takes the larger of the pair's two values (OrderableUnit.java:1112-1121).
    int softCollisionOnAll = 0;
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
    // [graphics] default_frame (CustomUnitConfigParser.java:1311) is the frame the hull reverts to
    // whenever no frame animation is playing -- CustomUnitAnimationController.b(true):262 uses it as the
    // reset value, so a tank that stops dead does not freeze on whatever tread frame it last showed.
    int defaultFrame = 0;
    // The three animations the update loop picks between (CustomUnit.java:1893-1897 for moving/idle,
    // :1335 for the shot recoil), plus any [animation_*] section whose onActions names the same slot.
    BodyAnim animMoving, animIdle, animAttack;
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
    // [movement] targetHeight / targetHeightDrift / heightChangeRate / startingHeightOffset
    // (CustomUnitConfigParser.java:1993-2006). An AIR hull cruises at targetHeight and bobs a sine of
    // +-targetHeightDrift around it; both default to 35 and 1.5 for air and 0 otherwise. A negative
    // heightChangeRate (-1, the default) asks for the automatic ease-out climb instead of a fixed rate.
    float targetHeight = 0, targetHeightDrift = 0, heightChangeRate = -1, startingHeightOffset = 0;
    // [movement] fallingAccelerationDead (:2007, default 0.06) is the gravity a corpse picks up on its
    // way down; [movement] slowDeathFall (:1957, default false) instead descends at a steady
    // 0.017 px/tick^2 while the hull still has rotation or velocity, sliding on its last step, and only
    // then swaps to image_wreak / disappears at touchdown (CustomUnit.java:1672-1768).
    // slowDeathFallSmoke (:1958, default true) trails smoke on that descent.
    float fallingAccelerationDead = 0.06f;
    bool slowDeathFall = false, slowDeathFallSmoke = true;
    // [core] explodeOnDeathGroundCollision (:1800, default true, and the historical
    // explodeOnDeathGroundCollosion spelling is still read as its alias): a corpse that hit the ground
    // with speed (frameAnimationDelay > 0.5) detonates there (CustomUnit.java:1734-1748).
    bool explodeOnDeathGroundCollision = true;

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
