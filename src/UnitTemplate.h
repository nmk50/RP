// Data-driven unit templates parsed from RWX .ini files.
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include "Common.h"
#include "TeamColoring.h"

enum class MoveType { NONE, LAND, AIR, WATER, HOVER, OVER_CLIFF, OVER_CLIFF_WATER };
// [attack] attackMovement (CustomUnitConfigParser.java:2017-2027): normal / strafing / bomber.
// RWX has no runtime reader for strafing (the real lateral motion is moveIgnoringBody), so only
// bomber changes behaviour in RP as well.
enum class AttackMovement { Normal, Strafing, Bomber };

// UnitSize.java:5-13 -- the enum OrderableUnit.a(UnitSize,boolean):4834-4888 switches on to choose the
// death explosion. Unknown means "the ini said nothing", which keeps RP's radius-based default.
enum class UnitSize { Unknown, VerySmall, Small, Normal, Large, LargeUnit, Building, BuildingNoShockwaveOrSmoke, VeryLargeBuilding };

// [core] transportUnitsCanUnloadUnits is a LogicBoolean (CustomUnitConfigParser.java:1837-1842), and the
// shipped ini files only ever spell out a handful of forms of it: the bare `false` (nautilus, whose cargo
// is meant to stay aboard for good), `if self.isOverLiquid()` / `if not self.isOverLiquid()` (the nautilus
// surface variant and the gunship) and, when the key is absent, the built-in
// `if not self.isOverLiquid() and not self.isMoving()` (logic_notOverLiquidAndNotMoving,
// CustomUnitConfig.java:754). RP reads those atoms into a test list instead of carrying a logic-boolean
// VM; a predicate it does not know is kept as always-true, since RWX would have rejected the ini outright.
enum class CondPred { overLiquid, overWater, moving };
struct UnloadCond {
    bool never = false;
    std::vector<std::pair<CondPred, bool>> tests;   // predicate, negated -- all of them must hold
};

// A [effect_NAME] section: RWX EffectTemplate (game/units/custom/EffectTemplate.java). Every member
// starts at RWX's own constructor default (:105-143) so an ini that says nothing still behaves like
// RWX. The *Random members are the +-spread RWX draws with randomFloatInRange(-v, v) when v != 0
// (a(float):357-362), which is why an absent key costs nothing.
struct EffectTpl {
    std::string name;
    // image : SHARED:spark.png wins over stripIndex; stripIndex is resolved by name or number against
    // RWX's 20 built-in sheets (EffectManager.loadContent:667-868) and is what a custom image replaces.
    std::string image;
    int strip = 0;
    int totalFrames = 1, frameW = 0, frameH = 0;   // 0 = take the cell size off the texture
    float life = 200, lifeRandom = 0;             // V, in ticks
    // W, the denominator of the age/scale/fade curves (Effect.draw:437,482,521). The built-in hit flashes
    // count V=35 down against W=25, so the fade only starts once V drops below W; 0 keeps W = V, which is
    // what every parsed [effect_*] and most creators use.
    float fadeDenom = 0;
    float scaleFrom = 1, scaleTo = 1;             // G -> F over the life
    float alpha = 1;                              // E
    uint32_t color = 0;                           // 0xRRGGBBAA, 0 = uncoloured (RWX's -1 sentinel)
    bool fadeOut = true;                          // r
    float fadeIn = 0;                             // s / t
    bool attached = true;                         // attachedToUnit
    bool liveAfterAttachedDies = true;
    bool atmospheric = false, physics = false;
    float physicsGravity = 1;                     // w
    // EffectManager draws an effect in one of its own passes; EffectTemplate.a():446-457 turns the two keys
    // into ar = 1 for drawUnderUnits, 3 for drawType: displacement and 2 for everything else. RP has no
    // displacement buffer, so a layer-3 effect is parsed and then left undrawn.
    int layer = 2;
    float spawnChance = 1;
    float delay = 0, delayRandom = 0;             // U, delayedStartTimer
    float pivotOffset = 0, pivotOffsetRandom = 0;   // degrees, added to the muzzle angle to get the effect's own
    float dirOffset = 0, dirOffsetRandom = 0;       // angle Y (also degrees), which dirSpeed Z turns per tick
    int frame = 0, frameRandom = 0;               // ap out of the sheet
    int animStart = 0, animEnd = 0;               // ae is raised only when these differ
    int animStartRandom = 0;                      // animateFrameStartRandomAdd, which moves af but not ak
    float animSpeed = 0.5f, animSpeedRandom = 0;  // frames per tick
    bool animLooping = false, animPingPong = false;
    // The four speeds, split the way RWX splits them: the absolute pair lands on P/Q as written and the
    // relative pair is turned by the effect's angle first (:320-329).
    float xAbs = 0, yAbs = 0, xAbsRnd = 0, yAbsRnd = 0;
    float xRel = 0, yRel = 0, xRelRnd = 0, yRelRnd = 0;
    float hSpeed = 0, hSpeedRandom = 0;           // R, px per tick of height
    float dirSpeed = 0, dirSpeedRandom = 0;       // Z, degrees per tick
    // The two offset pairs work the same way (:258-271), plus a height offset.
    float oxAbs = 0, oyAbs = 0, oxAbsRnd = 0, oyAbsRnd = 0;
    float oxRel = 0, oyRel = 0, oxRelRnd = 0, oyRelRnd = 0;
    float hOffset = 0, hOffsetRandom = 0;
    float imageAnchorY = 0.5f;                    // 0 top / 0.5 center / 1 bottom
    float teamColorRatio = 0;
    // alwayStartDirAtZero (and RWX's typo alias) pins the effect's angle at 0 instead of taking the muzzle
    // direction -- which is exactly how the built-in shockwave comes out of EffectManager: its creator never
    // touches Effect.Y, and free() leaves it at 0.
    bool startDirAtZero = false;
};

// One entry of the [turret_N] shoot_flame spawn list. CustomUnitSpawnList.c():46-58 splits the string on
// commas, peels a "*N" repeat off each item and resolves the rest through resolveEffect(:1175-1239), so a
// list is that template repeated N times. builtIn is 1..3 for the flame sizes TurretState already plays,
// 4 smoke / 5 shockwave for the two flavours RWX builds itself, and 0 for a parsed [effect_*] section, which
// tpl then indexes UnitTemplate::effects. A list only ever keeps one flame size in TurretState (see
// parseFlameList), so everything the flame slot cannot show lands in the turret's flames vector.
struct FlameSpec {
    int builtIn = 0;   // 4 smoke, 5 shockwave; 0 = use tpl
    int tpl = -1;
};

// EffectManager.effectTemplates[] (loadContent:667-868), the twenty sheets a stripIndex selects. `whole` is
// SpriteSheet.k, which hands the draw the entire texture instead of a cell; the six geometry members are the
// sheet's b/c/d/e/f/g, i.e. cell, origin and step in texture pixels, and SpriteSheet.h stays at
// Integer.MAX_VALUE for every one of them, so a built-in strip is always a single row.
struct FxSheetSpec {
    const char* name;
    const char* file;
    int cw, ch, ox, oy, sx, sy;
    bool whole;
};
extern const FxSheetSpec kFxSheets[20];

// stripIndex: found by sheet name (case-insensitive) or by the plain number "0".."19", and "" is the number
// 0 because RWX reads a missing key as the empty string (findEffectTemplateIndex:889-901). -1 = no such sheet.
int fxStripIndex(const std::string& key);

// EffectManager's three flavours a shoot_flame list can ask for by name, expressed as the same EffectTpl the
// ini sections fill in so one spawn path drives both: 4 smoke (createMuzzleFlash:291-312), 5 shockwave
// (createSmallExplosionInternal:447-463) and 6 the muzzle light (createLightEffectInternal:356-374).
// EffectTemplate.a():202 then overrules the two creators' own ar=1 with 2 and attaches both to the shooter,
// and because the built-in effects keep EffectTemplate.defaultEffectTemplate as their template, whose
// liveAfterAttachedDies is never assigned, they die with the hull.
EffectTpl muzzleEffect(int kind);

// RWX UnitPrice (game/units/custom/price/UnitPrice.java) as [turret_N] resourceUsage uses it: a per-shot
// cost paid out of the hull's pools. Parse b():165-233 splits on comma/pipe, a bare number is credits, and
// each key is credits/energy/hp/shield/ammo/hasFlag/hasMissingFlag/setFlag/unsetFlag or a named resource.
// canPay b(unit):285-305 only ever tests a *positive* requirement (a negative cost is a refund paid without
// a gate), and the flag masks are built by a():134-155 as 1<<id over an optional lo-hi range. pay a(unit):
// 378-389 subtracts unconditionally then clamps d():315-334 (energy/shield into [0,max], hp<=max, ammo>=0).
// RWX stores ammo in BaseUnit.unitLevel; RP keeps it in the named-resource pool u.resources["ammo"], the
// same store addResources/generation_resources ammo:N fills, so a silo spends what it produces.
struct ResourceCost {
    float credits = 0, energy = 0, hp = 0, shield = 0, ammo = 0;
    uint64_t setFlag = 0, unsetFlag = 0, hasFlag = 0, hasMissingFlag = 0;
    std::vector<std::pair<std::string, float>> custom;
    bool any() const {
        return credits != 0 || energy != 0 || hp != 0 || shield != 0 || ammo != 0
            || setFlag || unsetFlag || hasFlag || hasMissingFlag || !custom.empty();
    }
};

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
    // RWX's built-in hulls fire one barrel with two different rounds and pick between them on the
    // target rather than on the turret: HeavyTankUnit.java:164-160 sends a shell at the ground and a
    // homing missile at anything flying, MegaTankUnit.java:96-132 does the same, and MissileShip.java:122
    // / AttackSubmarine.java:350 swap their missile for a short-range round at a submerged hull. The
    // tests are isFlying (BaseUnit.i()) and isSubmerged (Q(): posZ < -1), which is exactly RP's
    // Unit::airborne()/submerged(). -1 = no alternate, keep `projectile`.
    int projectileAir = -1;
    int projectileUnderwater = -1;
    // [turret_N] image, empty = the unit's [graphics] image_turret (CustomUnit.d(i):691)
    std::string image;
    // [turret_N] chargeEffectImage (TurretConfig.java:179,563-566, copied by copyFrom at :276) -- while
    // the barrel charges its warmup, this sprite is drawn centred on the muzzle D(i), scaled by
    // speed/warmup on both axes, unrotated and with a null paint: no team dye, no construction tint
    // (CustomUnit:2482-2489 -> GameViewUtils.a(unit,texture,f,i):163-173). Empty = nothing is drawn.
    std::string chargeEffectImage;
    std::string shootAnimation;
    // [turret_N] image_applyTeamColors (TurretConfig.java:551-554) -- a tri-state, since RWX reads the key as
    // a nullable Boolean and only falls back to [graphics] teamColorsOnTurret when it is absent.
    int applyTeamColors = -1;
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
    // [turret_N] energyUsage (TurretConfig.java:372-375, TurretConfig.u): what one shot takes off the
    // unit's energy pool. A barrel with a cost bigger than the pool left cannot fire at all.
    float energyUsage = 0;
    float warmCallDown = 4;   // warmupCallDownRate: how fast an interrupted charge bleeds off
    bool warmNoReset = false; // warmupNoReset: the charge survives between barrages
    // limitingRange / limitingMinRange / canAttackMaxAngle: what this barrel may hit and how well it
    // must be aligned (5 deg by default) before it is considered aimed.
    float range = 0, minRange = 0, maxAngle = 5;
    // recoilOffset / recoilOutTime / recoilReturnTime: the barrel slides along its aim after a shot
    float recoil = 0, recoilOut = 0, recoilReturn = 0;
    // attachedTo / linkDelayWithTurret, resolved to turret indexes (-1 = none)
    int attached = -1, linkDelay = -1;
    // [turret_N] slave (TurretConfig.java:426-432): a slave must also be attachedTo a parent, and sets
    // childTurret=parentTurret, which CustomUnitConfigParser:2130-2132 turns into the `x` field that
    // getLinkedTurretIndex (CustomUnit:2853-2855) returns. Only a slave has x != -1, and only a slave gets
    // the HARD angle copy at OrderableUnit.advanceMovementLevelTowardTarget:3242-3243 (targetX = master's
    // targetX); a plain attached barrel instead rides the parent's per-tick turn delta. slaveMaster holds
    // that master index (-1 = not a slave). The slave fires on the master's isActive and target.
    int slaveMaster = -1;
    // [turret_N] limitingAngle (TurretConfig.java:213,485-490, default -1 = off): a firing cone measured
    // from the parent's aim (when attached) or the hull facing, plus idleDir. CustomUnit.a():1277-1285
    // rejects a target whose bearing from the hull centre lies more than limitingAngle degrees off that
    // reference. Checked both at acquisition and again at the fire gate (:3259), which is what lets an
    // attached slave that never acquires on its own still refuse a shot outside its arc.
    float limitingAngle = -1;
    // [turret_N] resourceUsage (TurretConfig.java:50,377-380): a per-shot UnitPrice gate+pay, see ResourceCost.
    ResourceCost resourceUsage;
    // RWX [turret_N]: idle rest angle relative to the hull, and whether the turret returns to it
    float idleDir = 0;
    bool shouldResetTurret = true;
    // TurretConfig.java:397-405 / CustomUnit.java:2668 -- after a fast turn the rest angle flips
    // by idleDirReversing instead of returning to idleDir. When the ini never writes the key RWX
    // fills it in itself: 0 for a barrel attached to another, idleDir + 180 otherwise. hasIdleDirReversing
    // records whether the section (or the copyFrom source it inherited) actually spelled it out.
    float idleDirReversing = 0;
    bool hasIdleDirReversing = false;
    // TurretConfig.java:365-367 / CustomUnit.java:2724-2727 -- a charging barrel loses
    // warmupShootDelayTransfer * speed / warmup of its charge time while the hull is moving.
    float warmupShootDelayTransfer = 0;
    // TurretConfig.java:438 / CustomUnit.java:1287 -- a LogicBoolean gate on acquiring/firing.
    UnloadCond canAttackCond;
    // TurretConfig.java:544-546 -- the barrel flagged as the builder's nano/default turret.
    bool mainNanoTurret = false;
    // [turret_N] idleSpin (TurretConfig.java:219,517, a plain float default 0) -- with no target the barrel
    // turns continuously at idleSpin deg/tick instead of holding a rest angle: CustomUnit.b(i,f):4030-4041
    // adds idleSpin*f to targetX and wraps it into (-180,180], returning false so the outer rest-reset is
    // skipped. It runs only when the idle sweep above did not (idleSweepAngle 0 or its condition failed),
    // and C(i):2672-2674 answers 999 for a spinning barrel so nothing tries to rest it. Like the sweep it
    // waits for velocityY to decay (:848), so a spinning turret also holds its last aim ~70 ticks after combat.
    float idleSpin = 0;
    // [turret_N] idleSweepAngle (TurretConfig.java:220-222,518-536) -- with no target the barrel rocks
    // about its rest angle instead of holding it: CustomUnit.b(i,f):3996-4029 walks it out to +angle,
    // parks, then after idleSweepDelay(+random) ticks flips to -angle and back. idleSweepSpeed is not a
    // deg/tick of its own -- it is handed to the rotate helper as the tick scale (:4011-4013), so the
    // whole turnSpeed/acceleration dynamics run that fraction as fast. addRandomDelay defaults to -1 and
    // is resolved at parse time to 1 / 5 / 20 by the size of the delay (:524-532).
    float idleSweepAngle = 0;
    float idleSweepDelay = 10;
    float idleSweepSpeed = 0;
    float idleSweepAddRandomDelay = -1;
    float idleSweepAddRandomAngle = 0;
    // idleSweepCondition is a LogicBoolean; the only shipped shape is `if not self.isMoving()`
    // (mech_gun, mech_bunker), read into the same atom list the unload condition uses. Empty = always.
    UnloadCond idleSweepCond;
    // [turret_N] aimOffsetSpread, inherited from [attack] when the barrel is not a copyFrom
    // (TurretConfig.java:331, per-turret override at :487). OrderableUnit.M(i):4812-4827 turns this into
    // the barrel's aim jitter after every shot: the barrel stops pointing at the hull centre and instead
    // aims at a whole pixel offset by up to target.radius * spread, which is what spreads a barrage over
    // the target instead of stacking every round on the same dot. 0.6 by default, so a shell lands
    // anywhere within 60% of the target's radius; only aaBeamGunship and turret_t2_flame set 0.
    float aimOffsetSpread = 0.6f;
    // RWX shoot_flame built-in effects: 0 none, 1 small, 2 medium, 3 large (flame.png frames)
    int shootFlame = 0;
    // ... and the rest of the same spawn list, which is where smoke, shockwave and every CUSTOM:name entry
    // lands, along with a second flame size should an ini ever ask for one.
    std::vector<FlameSpec> flames;
    // [turret_N] shoot_light (TurretConfig.java:424) is a colour, not a spawn list: CustomUnit:1590-1591
    // answers it with createLightEffect on the muzzle point, which runs before the shoot_flame list.
    uint32_t shootLight = 0;
    bool canAttackFlying = true, canAttackLand = true, canAttackUnderwater = false;
    // [turret_N] canAttackNotTouchingWaterUnits (TurretConfig:437, always-true folded back to null at :577)
    // is the fourth domain test and it is measured on the target's *height over a water tile*, not on its
    // movement type: CustomUnit:1308 rejects a land-domain target unless isTouchingWater (BaseUnit:2082,
    // water tile and posZ<=2). Written false by the subs' barrels, so a submarine can chew a surface ship
    // but cannot touch a tank standing on the shore -- while flying and submerged targets still pass, since
    // those two domains return before this test is ever reached (:1296-1305).
    bool canAttackNotTouchingWater = true;
    // [turret_N] laserDefenceEnergyUse (TurretConfig.java:503-507) bolts a point-defence beam on the barrel:
    // every tick it may shoot one incoming round down and pays this much off the unit's energy pool for it.
    // The key is `null` when absent -- it is a Float, not a float -- and setting it registers the very same
    // ProjectileInterceptorHook that carries the tag hunt below, which is why both passes share a loop here
    // (:22-35 for the beam, :36-38 for the hunt). Five shipped designs use it, all of them on an invisible
    // `canShoot : false` barrel, so the beam is not a weapon: no cooldown, no warmup and no attack target.
    // The beam reaches [turret] limitingRange when the barrel names one -- every shipped barrel does, at 160
    // for the spider module, carrier and experimental spider, 140 for the landed gunship and 80 for the heavy
    // artillery -- and the hull's [attack] maxAttackRange otherwise (OrderableUnit.m():1636-1638). Of the five
    // only four can actually shoot: the landed gunship writes no [core] energyMax, and the gate is a pool above
    // zero (ProjectileInterceptorHook.b():22).
    bool laserDefence = false;
    float laserDefenceEnergyUse = 0;
    // [turret_N] interceptProjectiles_withTags (TurretConfig.java:508-514) turns the barrel into a
    // projectile hunt: the tag list is the switch, and the three geometry keys below are only read inside
    // that branch, so an ini that names tags still gets RWX's own defaults for the distances. Five shipped
    // designs use it -- the antiNukeLauncherC silo at 1000/2200/50, the modular spider's anti-nuke module at
    // 1000/1500/50, and the experimental carrier, gunship and nautilus at 500/1600 or 1100/50 -- eight store
    // entries, because the gunship's landed variant and the nautilus's land and surface variants each
    // re-declare the same [turret_antiNuke]. All of them hunt `nuke` tagged shots and three sit on
    // canShoot:false barrels, which is why the runtime hunt ignores that key. Runtime is
    // ProjectileInterceptorHook.a():41-66; see Game::updateInterceptors.
    std::vector<std::string> interceptTags;
    // ..._andTargetingGroundUnderDistance: the quarry's own ground aim point has to lie this close to the
    // barrel, which is what makes an anti-nuke defend its neighbourhood instead of meeting a rocket
    // anywhere on the map. A negative value means the test is not run at all.
    float interceptGroundUnder = -1;
    float interceptUnder = 2000;        // ..._andUnderDistance: how close the quarry itself must be
    float interceptOverHeight = -999;   // ..._andOverHeight: how high it must already have climbed
};

struct ProjectileTpl {
    // CustomProjectileTemplate.java:73-74 -- fog flags. RP has no fog-of-war yet, so these
    // are parsed and stored for the future reveal/visibility rules but have no runtime effect.
    bool shouldRevealFog = false, alwaysVisibleInFog = false;
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
    // [projectile_N] beam family (CustomProjectileTemplate.java:89-113,179-189). beamImage turns the shot
    // into a tiled beam (X flag) drawn from muzzle to the target's live position; laserEffect/lightingEffect
    // are the line/lighning variants. A custom beam deals its directDamage ONCE on hit (the continuous look
    // comes from instant + instantReuseLast re-hitting each shot), and stays visible until `life` runs out
    // rather than vanishing on impact (Projectile.update:1038,1411). laser/lighting must aim at a unit, not
    // targetGround (:184-189); RP keeps loading a bad mod rather than throwing.
    std::string beamImage, beamImageStart, beamImageEnd;
    bool beamImageStartRotated = false, beamImageEndRotated = false;
    float beamImageOffsetRate = 0;          // texture scroll along the beam, px/tick (:89)
    bool laserEffect = false, lightingEffect = false;
    bool lightCastOnGround = false;         // attached light drawn at posY instead of posY-posZ (:171)
    // [projectile_N] nukeWeapon (:72 -> ProjectileTemplate.p, default false): the D flag. Its one gameplay
    // reader is the anti-nuke lock (AntiNukeLauncher:271 hunts D==true); the rest is the nuke blast visual,
    // fog exemption and minimap alert. Damage/radius still come from areaDamage/areaRadius.
    bool nukeWeapon = false;
    // [projectile_N] teleportSource (:249): on hit the SOURCE unit is moved to the impact point
    // (Projectile.update:1102-1103 -> BaseUnit.f:2512), not the target. The blink weapons use it.
    bool teleportSource = false;
    // [projectile_N] instantReuseLast (:121-137): keep one cached round per turret and reset it on each shot
    // instead of spawning a new one, so an instant beam persists as a single instance. alsoChangeTurretAim
    // folds the beam's sweep offset into the barrel aim; acrossTurrets reuses a same-template round from
    // another barrel; keepAreaDamageList stops the reuse clearing the splash's already-hit list.
    bool instantReuseLast = false, instantReuseLastAlsoChangeTurretAim = false;
    bool instantReuseLastAcrossTurrets = false, instantReuseLastKeepAreaDamageList = false;
    // [projectile_N] moveWithParent (:138): the round rides the firing turret's world position, adding the
    // parent's per-tick muzzle delta to its own pos (Projectile.update:816-840), so a beam's muzzle end
    // stays glued to a moving hull.
    bool moveWithParent = false;
    // [projectile_N] deflectionPower (CustomProjectileTemplate.java:71 into ProjectileTemplate.java:77, whose
    // default is 1.0) is how many times a laser defence beam has to hit this round before it goes down:
    // CustomUnit.java:1401-1404 reads it one way only -- below 0.5 the round is immune outright, otherwise the
    // value is the round's deflection health, which LaserDefense.attackTarget:298-300 cuts by 1.01 per beam
    // hit. So the default of 1.0 means an ordinary round dies to the first hit, and the 19 `deflectionPower :
    // -1` lines among the 22 the shipped data writes (31 store entries once copyFrom hands them to the
    // nautilus, gunship and spider variants -- the nuke, both anti-nuke missiles, the fabricator shells, the
    // flame gel and the torpedoes) mean the beam cannot touch it at all. The bomber's two guns at 3 and the
    // carrier's one round at 2 shrug off that many hits.
    float deflectionPower = 1.0f;
    // wobbleAmplitude (px per tick sideways) / wobbleFrequency (ticks per full sine)
    float wobbleAmp = 0, wobbleFreq = 5;
    // autoTargetingOnDeadTarget: once the chased unit is gone the shot looks for a new enemy around
    // the point lead px ahead of its own heading, within range (Projectile.java:699-702, 1776-1820).
    bool autoRetarget = false;
    float autoRetargetRange = 120, autoRetargetLead = 15;
    // areaExpandTime: the splash ring grows from nothing to areaRadius over these ticks and each
    // unit is caught once (Projectile.b(f2):1443 with the Projectile.ap list)
    float areaExpandTime = -1;
    // [projectile_N] friendlyFire (:60-68) is one key with two shapes: the string "only-ignoreEnemy"
    // sets l, and anything else that parses as a boolean sets k and clears l. CustomUnit.java:
    // 1387-1392 turns them into the shot's aa/ab -- l means "allies only", otherwise aa = !k, i.e. a
    // weapon that ignores nobody's team colours needs k set. Projectile.b(:1469-1481) is the reader:
    // a same-colour hull survives unless aa is clear, a different-colour one unless ab is.
    // fabricatorT1.ini:106 and nuke_launcher.ini:208 are the only shipped users, both `true`.
    bool friendlyFire = false;              // k
    bool friendlyFireIgnoresEnemy = false;  // l
    // areaHitAirAndLandAtSameTime (:69) is m, and CustomUnit.java:1394-1397 answers it by forcing
    // ad true / ae false, which is the only way past the height gate the constructor sets at
    // Projectile:196 (ae starts true, so by default an air burst spares the ground and a ground burst
    // spares the aircraft).
    bool areaHitAirAndLandAtSameTime = false;   // m
    // areaHitUnderwaterAlways (:70) -> ac: the burst reaches a hull more than 5px under the surface
    // only when the weapon asks (Projectile:1482).
    bool areaHitUnderwaterAlways = false;       // n
    // areaDamageNoFalloff (:57) -> g: the ring pays the full areaDamage to every hull in it, at any
    // distance (Projectile.a:1514-1516).
    bool areaDamageNoFalloff = false;           // g
    // areaIgnoreUnitsCloserThan (:58) -> j: a hull nearer to the blast than this takes nothing at all
    // (:1503), which is how a self-detonating weapon spares the thing that detonated it.
    float areaIgnoreUnitsCloserThan = 0;        // j
    // areaRadiusFromEdge (:59) -> h: the ring is measured to the target's hull edge, not its centre,
    // and the broad query that feeds it widens by 150px to catch the big ones (:1445-1447, :1493-1502).
    bool areaRadiusFromEdge = false;            // h
    // buildingDamageMultiplier: only scales damage dealt to buildings (Projectile.a:228)
    float buildingDamageMult = 1;
    // How a shot spends itself against a shielded hull (BaseUnit.setTarget:1567-1589): it drains
    // shieldDamageMultiplier * damage off the bubble, the part that runs past an empty bubble is cut
    // by shieldDeflectionMultiplier before it reaches the hull, and hullDamageMultiplier scales that
    // hull hit (a nuke's EMP variant uses 0). armourIgnoreAmount subtracts from the target's armour.
    float shieldDamageMult = 1, shieldDeflectMult = 1, hullDamageMult = 1;
    float armourIgnore = 0;
    bool largeHit = false;
    // [projectile_N] flameWeapon (CustomProjectileTemplate.java, copied at CustomUnit.java:1377): the hit
    // drops a short fire emitter instead of any blast (Projectile.java:1163-1166).
    bool flameWeapon = false;
    // [projectile_N] explodeEffect / explodeEffectOnShield (CustomProjectileTemplate.java:236-243) are the
    // same spawn-list grammar as shoot_flame, run at the impact point with the *victim* as the GameObject
    // (Projectile.java:1045-1070), so an attached effect rides the hull that was hit. Presence matters as
    // much as content: a list that exists -- even `NONE`, RWX's noneSpawnItems sentinel -- replaces the
    // generic blast outright (:1170, :1173, :1186), which is what the two `has` flags decide. A victim
    // carrying more than 10 shield swaps to the OnShield list, and when that one is absent RWX draws a
    // small pale flash instead (:1151-1162).
    bool hasExplodeEffect = false, hasExplodeEffectOnShield = false;
    std::vector<FlameSpec> explodeEffect, explodeEffectOnShield;
    // [projectile_N] effectOnCreate (CustomProjectileTemplate.java:153-156) is the same grammar again, run
    // once at the muzzle with the SHOT as the GameObject (CustomUnit.java:1468-1469) rather than a hull, so
    // an attached entry rides the round downrange and ends when the round does.
    bool hasEffectOnCreate = false;
    std::vector<FlameSpec> effectOnCreate;
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
    // [projectile_N] tags (CustomProjectileTemplate.java:251) is an AnimationTag name list, copied onto
    // every shot at CustomUnit.java:1503. RWX uses tags for animations and for the retargeting filters;
    // the one shipped use that reaches gameplay is the [turret_N] interceptProjectiles_withTags hunt
    // below, which is why nuke_launcher.ini:167 marks its rocket `nuke`. AnimationTag.a():33-51 splits on
    // commas, drops empty names and lowercases the rest, and an empty list is read as no tags.
    std::vector<std::string> tags;
    // interceptProjectile_removeTargetLifeOnly (:56) -- what an interceptor round does to its quarry: the
    // default destroys the chased shot (Projectile.b():303-317, which plays that shot's own detonation
    // effect), while this instead zeroes its remaining life so Projectile.update:1411 simply drops it.
    bool interceptRemoveTargetLifeOnly = false;
    // [projectile_N] spawnProjectilesOnEndOfLife (CustomProjectileTemplate.java:166, read by
    // UnitSpawnList.b():26-113 and run at Projectile.update:1411-1415) turns a round that flew out its
    // `life` short of its target into one or more fresh rounds at the same point. The shipped data uses it
    // as a two-stage missile: heavy_sub.ini:120 fires a 30-tick torpedo that becomes a 300-tick, 6 deg/tick
    // `torpedoLocked`, and nautilus.ini:372 launches a life 0, damage 0 carrier round that immediately
    // splits into two 123-damage `scoutBotProjectileSplit`, each of which relaunches itself as the 9 deg/tick
    // `scoutBotProjectileSplitFastTurn` at 90 ticks. `spawnProjectilesOnExplode` and `...OnCreate` (:167,
    // :168) are the same list shape at the other two moments; no shipped weapon writes either.
    struct SpawnEntry {
        std::string name;          // the [projectile_*] section, matched case-insensitively (UnitSpawnList:63)
        int proj = -1;             // resolved index into UnitTemplate::projectiles; -1 stays unbuilt, as RWX logs it
        int count = 1;             // `name*3` (:52-56, the entry simply runs three times)
        float chance = 1.0f;       // spawnChance
        int maxPerCall = 2000000000;  // maxSpawnLimit, RWX's Integer.MAX_VALUE
        int recursionLimit = 3;    // recursionLimit, measured against the round's own generation
        float offsetX = 0, offsetY = 0;   // offsetX/offsetY, also spelt xOffsetAbsolute/yOffsetAbsolute
        float offsetZ = 0;         // offsetHeight
        float offsetDir = 0;       // offsetDir, degrees
        float relX = 0, relY = 0;  // xOffsetRelative/yOffsetRelative, along and across the incoming heading
        float rndX = 0, rndY = 0;  // offsetRandomX/offsetRandomY (offsetRandomXY sets both)
        float rndDir = 0;          // offsetRandomDir, degrees either side
    };
    std::vector<SpawnEntry> onEndOfLife;

    // UnitSpawner.b():47-171, the grammar of a [projectile_N] spawnUnit value. SpawnConfig.java:49-77 is the
    // defaults this mirrors: count 1, chance 1.0, limit Integer.MAX_VALUE, every offset 0, every flag false.
    // RWX's two parsers are near-clones of each other and this deliberately stays a second one rather than a
    // shared abstraction -- the key sets differ (no recursionLimit or relative offsets here, but team, overlap
    // and falling flags instead) and each mirrors its own Java line-for-line.
    struct SpawnUnit {
        std::string name;          // a unit type name, matched case-insensitively (:66-70)
        std::string addResources;
        int count = 1;             // `name*3` (:71-77)
        float chance = 1.0f;       // spawnChance (:98)
        int maxPerCall = 2000000000;  // maxSpawnLimit (:100, RWX's Integer.MAX_VALUE)
        float offsetX = 0, offsetY = 0;   // offsetX/offsetY (:122-125)
        float offsetZ = 0;         // offsetHeight (:131)
        float offsetDir = 0;       // offsetDir, degrees (:135)
        float rndX = 0, rndY = 0;  // offsetRandomX/offsetRandomY (offsetRandomXY sets both, :126-130)
        float rndDir = 0;          // offsetRandomDir, degrees either side (:133)
        bool neutralTeam = false;        // (:88, TEAM_ALL -- RP has no neutral team, see Game::spawnUnitsOnHit)
        bool aggressiveTeam = false;     // (:92, TEAM_UNKNOWN, same story)
        bool toLastAttackerTeam = false; // setToTeamOfLastAttacker (:90)
        bool skipIfOverlapping = false;  // (:106)
        bool startDirAtZero = false;     // alwaysStartDirAtZero, and RWX's typo alias (:116-121)
    };
    std::vector<SpawnUnit> spawnUnits;
};

// `tech` is CustomActionDef.techLevel (CustomUnitConfigParser:2562): the option is filed into the
// owner's specialActionLists[tech-1], so a hull whose own core.techLevel is lower never sees it.
struct BuildOption { std::string unitName; float pos = 1; std::string visible; bool forceNano = false; int tech = 1; };

// One keyframe of a body animation. `t` is in ticks: a key written "0.8s" is 48 (the trailing 's' is
// worth 60, IniFile.parseUnitValue:761-783), and the animation clock steps by 1 per logic tick because
// GameLogic.update passes f = deltaSpeed * speedMultiplier, i.e. one tick at normal speed
// (GameLogic.java:1495-1523, where gameTimeMillis grows by f * 16.666666).
struct AnimKey { float t = 0, v = 0; };

enum class LegAnimProperty { X, Y, DIRECTION, HEIGHT, ALPHA };
struct LegAnimTrack {
    int index = -1;
    LegAnimProperty property = LegAnimProperty::X;
    std::vector<AnimKey> keys;
};

struct BodyAnim {
    std::vector<AnimKey> frame, scale;
    std::vector<LegAnimTrack> legs;
    float maxTime = 0;
    float blendIn = 0, blendOut = 0, queuedUnitPlayAt = 0;
    bool pingPong = false;

    bool valid() const { return !frame.empty() || !scale.empty() || !legs.empty(); }
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
    std::string name, convertTo, text, textUnit, textPostfix, buildAnimation;
    std::string visible, active, require, autoTrigger, events, tags;
    std::string addResources, setStats, triggerActions, queueActions, triggerCondition;
    std::string messageTo, messageTags, playAnimation;
    std::vector<FlameSpec> effects;
    float price = 0, buildSpeed = 0.001f, pos = 0;
    float setBuilt = -1, cooldown = 0, allCooldown = 0, addEnergy = 0;
    float parentShield = 0;
    int checkRate = 1;
    bool hidden = false, supported = true, cannotMove = false, highPriority = false;
    UnloadCond autoCond;   // parsed autoTrigger gate (empty tests = always true)
};

struct LegTpl {
    std::string name;
    bool placeholder = true;
    bool isLeg = true, hidden = false;
    float x = 0, y = 0;
    float attachX = 0, attachY = 0;
    float liftHeight = 2, targetHeight = 0;
    bool targetHeightRelative = true;
    float endDirOffset = 0, resetAngle = 0;
    bool lockMovement = false;
    float estimatingMultiplier = 1, alpha = 1;
    float moveSpeed = 1, moveWarmUp = 0, rotateSpeed = 3;
    std::string imageLeg, imageFoot, imageShadow;
    bool footTeamColors = false, legTeamColors = false;
    float heightSpeed = 0.3f;
    bool drawFootOnTop = false, dust = true;
    bool drawLegZoomedOut = true, drawFootZoomedOut = true, explicitZoom = false;
    float holdDisMin = 7;
    int maxMovingLegs = 3;
    bool holdMoveOnlyIfFurthest = true, checkNeighbours = true, favourOpposite = false;
    float holdDisMax = 16, hardLimit = 50;
    bool drawOverBody = false, drawUnderAllUnits = false;
    float drawDirOffset = 0, spinRate = 0;
    std::vector<int> neighbours;
};

struct AttachmentTpl {
    std::string name, showActions;
    std::vector<ProjectileTpl::SpawnUnit> spawn;
    float x = 0, y = 0, height = 0, idleDir = 0, reverseDir = 0;
    bool rotateWithParent = true, lockRotation = false, resetRotation = false;
    bool lockLegs = true, freezeLegs = false;
    bool damageable = false, unselectable = true, untargetable = true;
    bool visible = true, hidden = false, showMiniHp = true, hideHp = false;
    bool redirectDamage = false, shieldOnly = false, keepAlive = false;
    bool incompleteWithParent = true, keepOnConvert = false, keepTeam = false;
    bool onTop = true, onBottom = false, addTransported = false;
    bool canAttack = true, prioritizeTarget = true, onlyParentTarget = false;
    bool alwaysParentTarget = false;
};

struct UnitTemplate {
    std::string name;
    std::vector<std::string> tags;
    std::vector<AttachmentTpl> attachments;
    bool unselectable = false, untargetable = false, cannotBeAttacked = false;
    // CustomUnitConfig.canNotBeDamaged (:207, parsed :1738 with canNotBeDirectlyAttacked as its default):
    // a separate hook from cannotBeAttacked -- this one is checked inside takeDamage, so the hull can be
    // aimed at and hit but nothing comes off its health bar.
    bool cannotBeDamaged = false;
    bool disableDeathOnZeroHp = false;
    float autoCooldown = 60, fallingAcceleration = 0.03f;
    int autoCheckRate = 1;
    std::string overrideName;
    std::string displayText;
    std::string image, imageWreak, imageTurret;
    // RWX [graphics] image_shadow: "AUTO" = the body silhouette in black, offset by shadowOffsetX/Y world px
    std::string imageShadow;
    float shadowOffX = 0, shadowOffY = 0;
    // [graphics] image_offsetX/Y/H (:1312-1316) are only ever read through CustomUnit.getUnitBounds():2834-2840,
    // which slides the view-space body rect by (offsetX, offsetY - offsetH) -- and every caller of that rect is a
    // draw method (CustomUnit.c():2426, OrderableUnit:4371, BaseBuilding.c():346/d():364, Tree.c():212,
    // FireUnit.c():230). The hull sprite is drawn *into* that rect, so the art slides off the hit-box while the
    // shadow (:2405), the shield bubble (:2510, plain pos/posZ) and the turret pivots stay where they are.
    // A wreck takes the other branch (:946-951 sets ew, drawn at plain pos at :2417-2423), so it ignores this.
    int imageOffsetX = 0, imageOffsetY = 0;
    float imageOffsetH = 0;
    bool hasImageOffset = false;
    // getUnitBounds():2837 -- hasImageOffset is what gates the whole translate.
    float bodyOffsetDrawX() const { return hasImageOffset ? (float)imageOffsetX : 0.0f; }
    float bodyOffsetDrawY() const { return hasImageOffset ? (float)imageOffsetY - imageOffsetH : 0.0f; }
    // [graphics] lock_body_rotation_with_main_turret (:1583) draws the body sprite along the main
    // turret's aim instead of the heading the unit travels on (CustomUnit.getRenderRotation:2345-2347),
    // so a bug or mech keeps facing its prey while strafing past it. Only the sprite turns -- the hull's
    // game rotation still drives movement and turret pivots. lock_shadow_rotation_with_main_turret
    // (:1584) does the same for the shadow and takes the body's value as its default.
    bool lockBodyRot = false, lockShadowRot = false;
    // [graphics] lock_leg_rotation_with_main_turret (:1585) -- the leg rig aims along the main turret
    // instead of the heading (CustomUnitLegController.java:32-34, :150-152).
    bool lockLegRot = false;
    std::vector<LegTpl> legs;
    // RWX bakes ten variants of every hull sprite (PlayerTeam.getTeamColorTextures():1421-1508) and draws the
    // one at the owner's colour index, so a red team's tank really is dyed red. Index 0 is the raw art, which
    // is why the green team looks undyed. CustomUnitConfigParser.java:1405 always builds the hull array.
    int teamColoringMode = kTCModePureGreen;
    // :1406-1408 -- the [graphics] image_turret array is only baked when this is on *and* image_turret exists.
    bool teamColorsOnTurret = false;
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
    // RWX [core] energy (CustomUnitStats.maxEnergy, read at CustomUnitConfigParser:1689-1694): a second
    // pool that weapons spend from instead of credits. energyRegen refills it per tick; once a shot has
    // drained the pool below what the next shot needs, energyNeedsToRechargeToFull latches the unit off
    // until the pool is whole again and refills it at energyRegenWhenRecharging (default the same rate).
    // dieOnZeroEnergy kills the unit instead of parking it at an empty pool.
    float maxEnergy = 0;
    float energyRegen = 0;
    float energyStartingPercentage = 1.0f;
    float energyRegenWhenRecharging = 0;
    bool energyNeedsToRechargeToFull = false;
    bool dieOnZeroEnergy = false;
    bool startEnergyAtZero = false;
    bool showEnergyBar = true;                // [graphics] showEnergyBar, :1951
    // [core] armour: a flat amount taken off every hit before the shield, with armourMinDamageToKeep
    // as the floor so weak weapons still chip away.
    float armour = 0, armourMinKeep = 1;
    int techLevel = 1;
    float buildSpeed = 0.001f;
    // [core] nanoFactorySpeed (CustomUnitConfigParser.java:1779, default 1.0): a multiplier on the whole
    // production queue, applied by FactoryQueueManager.a:249 as `action buildSpeed * nanoFactorySpeed *
    // tick` to the *producer's* progress. It belongs to whoever builds, not to what is built, so a mobile
    // factory like the nautilus (2) churns out lightSubs twice as fast as a seaFactory does.
    float nanoFactorySpeed = 1.0f;
    // [core] exit_moveAwayAmount / exit_dirOffset (CustomUnitConfigParser.java:1717-1720). A finished
    // queue item is born 5 px south of its producer's centre (FactoryQueueManager.a:135-136) and
    // immediately ordered down the exit line for exit_moveAwayAmount pixels -- 70 by default, and the
    // reason RWX's carriers and gunships raise it to 140 to throw their launches clear of the hull. The
    // line is the hull angle + exit_dirOffset + 90 for a building and plain 90 for anything else, because
    // CustomUnit.a:4300-4310 only reads dirOffset `if (unitConfig.isBuildingUnit && ... != null)`.
    // (exit_x/exit_y/exit_heightOffset are below: the factory path only ever reads them through
    // HovercraftUnit.a, while the transport-unload place uses all three.)
    float exitDirOffset = 0, exitMoveAway = 70;
    bool exitDirSet = false;
    // [core] exit_x / exit_y / exit_heightOffset / exitHeightIgnoreParent (CustomUnitConfigParser.java
    // :1715-1719). The unload point is this offset from the carrier's centre, rotated into the carrier's
    // frame by CustomUnit.b():728-731 as `pos + (cos(a)*exit_y - sin(a)*exit_x, sin(a)*exit_y +
    // cos(a)*exit_x)` with a = hull angle + exit_dirOffset (180 degrees when the key is absent, :721-723)
    // -- note the two axes swap, so exit_y is the along-the-nose distance and exit_x the sideways one.
    float exitX = 0, exitY = 9, exitHeightOffset = 0;
    bool exitHeightIgnoreParent = false;

    // ---- transport (CustomUnit.java:312-456, 718-809, 1953-1980) ----
    // [core] maxTransportingUnits (:1818) is the cargo capacity in *slots*, not in units: a hull occupies
    // its own [core] transportSlotsNeeded (:1846, default 1) unless the carrier sets
    // transportUnitsEachUnitAlwaysUsesSingleSlot (:1830). So a dropship's 12 slots hold twelve tanks or
    // one experimental spider (8) plus a mech (2) plus a builder (2).
    int maxTransportingUnits = 0;
    int transportSlotsNeeded = 1;
    bool transportEachUnitSingleSlot = false;
    // transportUnitsRequireMovementType (:1824-1828) restricts the cargo to the listed movement types.
    // Empty = anything, which is what the two defaults below key off.
    std::vector<MoveType> transportRequireMovementType;
    // transportUnitsBlockAirAndWaterUnits (:1831, default true only when no movement type is listed) drops
    // aircraft and ships; transportUnitsBlockOtherTransports (:1832, default true) stops one transport
    // boarding another. Both are applied by GameViewUtils.a():119-130, which also rejects a target that is
    // a building, already riding, or attached to something.
    bool transportBlockAirAndWater = true, transportBlockOtherTransports = true;
    // transportUnitsUnloadDelayBetweenEachUnit (:1822, getTime, 30 ticks): how often one hull is put down
    // while unloading. transportUnitsKeepWaypoints (:1835) leaves the cargo's orders alone instead of
    // clearing them and giving the exit-line move. transportUnitsKillOnDeath (:1834, default true) takes
    // the cargo down with the carrier, and transportUnitsHealBy (:1836) mends it per tick while aboard.
    float transportUnloadDelay = 30;
    bool transportKeepWaypoints = false, transportKillOnDeath = true;
    float transportHealBy = 0;
    // transportUnitsKeepBuiltUnits (:1833) makes a mobile factory load what it builds instead of setting it
    // down; it only matters for a hull that is both a factory and a transport.
    bool transportKeepBuiltUnits = false;
    UnloadCond transportCanUnload;
    // [graphics] showTransportBar (:1953, default true) -- whether the cargo readout is drawn over the
    // carrier (CustomUnit.getTransportedUnitsWeight:531-544 returns -1 without it).
    bool showTransportBar = true;
    bool isBuilding = false, isBuilder = false, isFactory = false;
    int footL = 0, footU = 0, footR = 0, footD = 0; // footprint in tiles (relative to center tile)
    // RWX BaseUnit.getTileOffsetX/Y (:2338-2345) answer halfTileWorldSize, and CustomUnit.getTileOffsetX/Y
    // (:4092-4101) overwrite that answer with [core] buildingToFootprintOffsetX/Y (parsed :1709-1710, and
    // the parse default is the same 10 px). The offset is how far a building's position sits inside its
    // footprint's top-left tile corner: placement adds it (BaseBuilding.canBuildingBePlacedAt:72-74,
    // CustomUnitConfig:1301) and the tile index subtracts it again (BaseUnit.a(TileMap,Point):2355-2358).
    // turret_common_land and its four anti-air children ask for 20, laboratory for offsetY 0, so a hull
    // that ignores the key lands 10 px off its RWX art while still blocking the same tiles.
    float buildingToFootprintOffsetX = TILE_SIZE * 0.5f;
    float buildingToFootprintOffsetY = TILE_SIZE * 0.5f;
    // The world position a building takes when its footprint's top-left tile is (tx,ty).
    Vec2 footprintPos(int tx, int ty) const {
        return { tx * (float)TILE_SIZE + buildingToFootprintOffsetX,
                 ty * (float)TILE_SIZE + buildingToFootprintOffsetY };
    }
    // [core] fogOfWarSightRange in TILES, not pixels: CustomUnitConfigParser.java:1713 defaults it to
    // 15 and OrderableUnit.s():4926-4928 answers the same 15 for the hard-coded hulls, while
    // TileMap.updateFogVisibilityForTeamCircle:1519-1533 compares the value against tile indices.
    float fogSight = 15;
    // CustomUnitConfigParser.java:1714 -- -1 means "use fogSight", which is what an unfinished
    // building falls back to (CustomUnit.c(boolean):3967-3970).
    float fogSightWhileNotBuilt = -1;
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
    // [animation_build] onActions: underConstructionWithLinkedBuiltTime -- CustomUnit.java:1791-1795
    // pins the clock to buildProgress instead of ticks, so a foundation pops from scale 0 to 1.2 as it
    // is paid for. `animUnderConstruction` is the :1788-1790 variant, driven by ticks at priority 8.
    BodyAnim animBuild, animUnderConstruction, animQueued;
    std::map<std::string, BodyAnim> animations;
    std::string drawLayer;
    bool dustEffect = false, splashEffect = false;
    bool dustEffectReverse = true, splashEffectReverse = true;
    std::vector<FlameSpec> movementEffect, movementEffectReverse;
    float movementEffectRate = 11.0f;
    bool movementEffectReverseFlipEffects = false;

    bool canAttack = false;
    bool canAttackFlying = false, canAttackLand = true, canAttackUnderwater = false;
    // [attack] canAttackNotTouchingWaterUnits (parser:2291-2293, where a written `true` is thrown away and
    // reads back as the default). OrderableUnit.canAttackUnitType:3356 puts it between the flying/underwater
    // domains and canAttackSurfaceUnits, so a sub with `false` acquires only targets standing in water --
    // isTouchingWater is BaseUnit:2082's water-tile-and-posZ<=2 test, not a movement type. See TurretTpl's
    // copy of this flag for the identical firing-side gate at CustomUnit:1308.
    bool canAttackNotTouchingWater = true;
    bool isFixedFiring = false;
    // [attack] turretMultiTargeting (CustomUnitConfigParser.java:2030): with more than one barrel the
    // unit lets each barrel keep its own locked target instead of every barrel following the order
    // target -- and only for barrels that cannot engage the order target at all (see
    // OrderableUnit.syncAttackTargetToMovementLevels:3072-3105). CustomUnit.java:4218 gates it behind
    // getTechLevel() > 1, i.e. it does nothing for a single-barrel hull.
    bool turretMultiTargeting = false;
    // [attack] turretRotateWithBody: the hull's turn is added to every barrel's aim (default true)
    bool turretRotateWithBody = true;
    // RWX parser defaults: CustomUnitConfigParser.java:2042 (shootDelay 50 ticks), :2015
    // (turretTurnSpeed 8 deg/tick), :1959-1961/:2008-2009 (every numeric [movement] key is 1.0f),
    // :1774 (nanoRepairSpeed 0.2 hp/tick).
    float attackRange = 0, shootDelay = 50, turretSize = 1.0f, turretTurnSpeed = 8;
    // [attack] aimOffsetSpread (:2041, default 0.6f) -- the hull-wide aim jitter every barrel inherits at
    // TurretConfig.java:331. See TurretTpl::aimOffsetSpread for what it does.
    float aimOffsetSpread = 0.6f;
    // [attack] isMelee (:2046-2056) installs no damage rule at all. It turns meleeEngangementDistance on,
    // which is 250 unless the ini overwrites it and stays 0 for a non-melee hull (writing the distance
    // without isMelee:true is a parse error in RWX), and that bonus has exactly two consumers:
    // getPassiveTargetSearchRangeBonus:4625 feeds it to getTargetSearchRange:3475, so a bug with a 9px
    // bite *notices* a tank from 259px away, and isChaseAttackMode:4631 answers true, which at
    // updateAttackTargeting:3215-3220 paths the hull onto the target with pathTargetRadius 0 instead of
    // stopping at range. What it does NOT do is lengthen the bite: a melee hull still only fires inside
    // getEngagementRange:4482, which adds the two hull radii to maxAttackRange for melee and nothing else.
    bool isMelee = false;
    float meleeRange = 0;
    AttackMovement attackMovement = AttackMovement::Normal;
    // [attack] dieOnAttack (CustomUnitConfigParser.java:2279): the hull dies immediately after
    // it fires (CustomUnit.java:1605-1607), distinct from removeOnAttack which leaves no effect.
    bool dieOnAttack = false;

    MoveType moveType = MoveType::NONE;
    float moveSpeed = 1, accel = 1, decel = 1, maxTurnSpeed = 1, turnAccel = 1;
    float reverseSpeed = 0.6f; // RWX [movement] reverseSpeedPercentage: allows the flip-turn
    // [movement] moveIgnoringBody (:2011) steers by the wanted bearing instead of the hull's facing, so
    // the unit strafes and backs up while the body keeps turning (OrderableUnit.java:876-878,
    // 2913-2915, 3003-3005). [movement] moveSlidingMode (:2010) instead makes the motion an integrated
    // velocity with a 1.41x steering rate and no throttle ramp (:893-918, :3009-3010) -- glide, not
    // strafing. Both default false.
    bool moveIgnoringBody = false, moveSlidingMode = false;
    // [movement] ignoreMoveOrders (:1962-1973) makes canExecuteMovementCommands() return false:
    // the hull still fires but refuses move / patrol / attack-move orders (CustomUnit.java:607-616).
    bool ignoreMoveOrders = false;
    // [movement] joinsGroupFormations (:2013, default true): a false hull is excluded from
    // FormationGroup and therefore gets no golden-angle slot offset (FormationGroup.java:65).
    bool joinsGroupFormations = true;
    // [movement] moveSlidingDir (:2012, getLogicBooleanUnit is really a plain parseInt, default -1).
    // RWX parses and stores it (CustomUnitConfig.dZ:483) but never reads it back anywhere -- the actual
    // glide drift comes from moveSlidingMode above and the velocity vector, not this angle. The mod-api
    // MovementSpec.kt:75 even notes "not found handling logic in RW". Kept as a dead field for parity so
    // an ini that sets it parses cleanly; the 181/50/etc. values have no runtime effect, exactly as in RWX.
    int moveSlidingDir = -1;
    // [movement] landOnGround (:1982-1991) takes true / false / onlyIdle, and only an AIR hull reads it:
    // CustomUnit.update:2003-2007 lets one settle down to posZ 2 once it has stood still for more than
    // three ticks off open water, with onlyIdle adding "and no waypoint left" (:2005). That is not just
    // cosmetic, because isAirborne is a height test (i():636-638) -- a landed bomber is ground for every
    // turret's domain check, so ordinary land guns can hit it. bugFly, bugPickup and bomber use it; the
    // gunships write false.
    bool landOnGround = false, landOnGroundOnlyIdle = false;
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
    // [core] autoRepair (:1748-1750) is not a self-heal and not a rate: writing it true bolts
    // AutoRepairRenderHook.a onto the hull, and that hook's b():14-20 spends its whole body as a timer --
    // every 40 ticks, and only while the hull has no waypoint to follow, it runs RepairBay.a(this, f, 0,
    // false), the very search the repair bay building uses. That search (AutoRepairCallback:71-95) walks
    // friendlies inside y() (nanoRange, or BaseUnit's 85 when the ini leaves it out) and picks the *lowest
    // health* one it may repair, ignoring construction sites that would cost credits (:78-80) and anything
    // with a resource rate (:89), then queues a repeating repair command. So an idle mech engineer,
    // gunship, spider or carrier mends the worst-hurt ally nearby instead of standing about; the five
    // shipped hulls that write it all also say canRepairUnits and canRepairBuildings.
    bool autoRepair = false;
    // [core] canReclaimResources (:1807, default false; the shipped data writes it on the four builders and
    // the bug mod's robotCrab only) is the gate BaseUnit.h(BaseUnit,boolean):3719-3730 puts in front of the
    // reclaim command, and the three rates below are what it costs. They are not one number:
    // OrderableUnit.updateRepairOrReclaimCommand:2380-2429 takes [core] nanoUnbuildSpeed (:1777, 1.0 *
    // 0.001 * 5.1 = 0.0051 of the site per tick) off an unfinished foundation, and [core] nanoReclaimSpeed
    // (:1775, default nanoRepairSpeed * 5.1) off a finished hull's *health*, or
    // target.resourceRate * [core] resourceReclaimMultiplier (:1776, CustomUnit.calculateUnitSpeed:2252-2258)
    // when the target produces credits. Credits come back two different ways (:2405-2446), see reclaimPrice.
    bool canReclaimResources = false;
    float nanoReclaimSpeed = 0, nanoUnbuildSpeed = 0.0051f, resourceReclaimMultiplier = 1;
    // [core] reclaimPrice (:1494) is written by robotCrab alone in the shipped data, and RWX tells "0" from
    // absent: UnitPrice.b() turns an all-zero string into its non-null empty sentinel (UnitPrice:229-232),
    // so a hull *with* the key refunds reclaimPrice -- dripped as it dies if it produces credits (:2408-2420)
    // or paid in full on collapse if it does not (:2432-2435) -- while a hull without it refunds
    // 80% of its build price on collapse only (:2437-2442).
    bool hasReclaimPrice = false;
    float reclaimPrice = 0;
    // [core] selfBuildRate (:1781, getInvertedTime) raises a foundation without a builder
    // (CustomUnit.update:1774-1782); [core] dieOnConstruct (:1782) gives the builder to the site it
    // places (CustomUnit.m():3668-3679).
    float selfBuildRate = 0;
    bool dieOnConstruct = false;
    // [core] selfRegenRate (:1780, plain getFloat) is HP per tick recovered with no builder and no
    // shield involved; it runs for every live unit, foundation included (CustomUnit.update:1821-1826).
    float selfRegenRate = 0;
    // [core] nukeOnDeath (:1792-1795, defaults false/250/5400) detonates a nuke under the dying hull
    // (CustomUnit.java:864-876): NukeLauncher.a(unit, x, y, x, y) with aH cleared so the missile skips its
    // climb and dives straight onto its own footprints, then Z=range and Y=damage overwrite the defaults.
    // The blast damages friendlies as readily as enemies (that Projectile carries neither aa nor ab), hits
    // air and land together (ad=true, ae=false), and its ring grows over the launcher's W=X=75 ticks.
    bool nukeOnDeath = false;
    float nukeOnDeathRange = 250, nukeOnDeathDamage = 5400;
    // :1795 -- this only suppresses the blast while the lobby's "no nukes" room rule is on. Single player
    // leaves that rule false (RoomSettings default), and RP has no lobby at all, so it is parsed and inert.
    bool nukeOnDeathDisableWhenNoNuke = false;

    std::vector<TurretTpl> turrets;
    std::vector<ProjectileTpl> projectiles;
    // Every [effect_NAME] section of this unit's ini, in file order -- the pool a shoot_flame (or any other
    // spawn list) name is looked up in by CustomUnitConfig.resolveEffect:1189-1196.
    std::vector<EffectTpl> effects;
    // [core] death spawn lists. A non-empty string means the key was written; "NONE" is a
    // valid written value that suppresses the default death fx, hence the separate has flags.
    std::vector<FlameSpec> effectOnDeath, effectOnDeathIfUnbuilt, effectOnDeathGroundCollision;
    bool hasEffectOnDeath = false, hasEffectOnDeathIfUnbuilt = false, hasEffectOnDeathGroundCollision = false;
    // [core] explodeTypeOnDeath (CustomUnitConfigParser.java:1797) is a UnitSize, not a free string:
    // CustomUnit.e():923-937 hands it to OrderableUnit.a(UnitSize,boolean):4834-4888, which picks the
    // shockwave / smoke / sound set by size. RP has one puff effect, so the size is folded down to
    // big + a scale (see deathExplosionSize in Game.cpp). explodeTypeOnDeath keeps the raw lowercased
    // token for --unusedkeys and for the unknown-size fallback.
    std::string explodeTypeOnDeath;
    UnitSize explodeSizeOnDeath = UnitSize::Unknown;
    bool isBio = false;
    int mainNanoTurret = -1;
    std::vector<BuildOption> canBuild;
    std::vector<BuildOption> builtFrom;
    std::vector<UnitAction> actions;

    bool placeOnlyOnResPool = false;
    bool placeOnlyOnWater = false; // RWX SeaFactory: must sit on water
    // [core] generation_credits / generation_resources "credits=8": the lump paid out once per
    // generation_delay ticks (CustomUnit.update:1869-1884), not a per-second rate -- RWX only divides it
    // by delay/40 for the HUD readout at getCreditIncomeRate():1214-1219.
    float creditIncomeRate = 0; // for extractors
    // [core] resourceRate (:1546, plain getFloat, default 0) is a different number from the generation lump:
    // BaseUnit.getResourceRate()/y() read only this one, so in the shipped data it is nonzero on the crystal
    // deposit (0.1) and the credit crate (5) and *zero on every extractor*. It is what marks a reclaim target
    // as sellable (OrderableUnit:2364-2366) and what CustomUnit.calculateUnitSpeed:2252-2258 drains off it.
    float resourceRate = 0;
    // [core] generation_delay, default 40 (CustomUnitConfigParser.java:1520-1527); 0 is forced up to 1 and
    // a negative is a hard parse error there.
    int generationDelay = 40;
    bool useAsBuilder = false, useAsAttacker = false, useAsHarvester = false;

    // resolved texture id (raylib), -1 until loaded / missing
    int texId = -1, texWreakId = -1, texTurretId = -1;

    bool blocksGround() const { return moveType == MoveType::NONE; } // buildings/static
    bool isStatic() const { return moveType == MoveType::NONE; }
    int footprintTilesW() const { return footR - footL + 1; }
    int footprintTilesH() const { return footD - footU + 1; }
    bool isWaterUnit() const { return moveType == MoveType::WATER; }
    bool isAirUnit() const { return moveType == MoveType::AIR || moveType == MoveType::HOVER; }
    // CustomUnit.canTransportUnits():316-318
    bool canTransportUnits() const { return maxTransportingUnits > 0; }
    // GameViewUtils.a():123 blocks aircraft and ships by their *class*, not their height: a hover hull is
    // a HoverLandUnit, i.e. a LandUnit subclass, so it boards a transport like any tank even though RP's
    // isAirUnit() lumps it in with the aircraft.
    bool isAirOrWaterHull() const { return moveType == MoveType::AIR || moveType == MoveType::WATER; }
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
// Whether `owner` shows `target` in its build/produce menu. Two rules, both from the RWX parser:
//  1. CustomUnitConfigParser:2562 -- the entry's `tech` files it under specialActionLists[tech-1], so a
//     hull only sees entries whose tech is within its own core.techLevel.
//  2. Placement vs production follows the *target*: structures need a builder, mobile units need a
//     factory. The roles are not exclusive -- combatEngineer/mechEngineer/experimentalGunship are
//     isBuilder AND isFactory in the shipped assets -- so filtering on the owner's own flags with an
//     `&&`/mutual-exclusion test hid every option such a hull had.
inline bool buildOptionAllowed(const UnitTemplate& owner, const UnitTemplate& target, int tech) {
    if (tech > 1 && tech > owner.techLevel) return false;
    if (target.isBuilding) return owner.isBuilder;
    return owner.isFactory;
}
// --unusedkeys: after each .ini is parsed, log every key the template builder never asked for.
// Those are the fragments the .ini carries but this build silently drops.
extern bool g_iniUnusedKeyReport;

// UnitSpawnList.b():26-113, the grammar of a spawnProjectilesOn* value: "name, name*3(key = value, ...)".
// Exposed for the harness in out/sim_main.cpp, which cannot feed it through an ini file.
std::vector<ProjectileTpl::SpawnEntry> parseSpawnList(const std::string& raw);

// UnitSpawner.b():47-171, the same shape read by [projectile_N] spawnUnit: "name, name*3(key = value, ...)",
// where "NONE" and a missing string are an empty list again (:50-53). Exposed for the harness too.
std::vector<ProjectileTpl::SpawnUnit> parseUnitSpawnList(const std::string& raw);

// CustomUnitSpawnList.c():36-59 plus CustomUnitConfig.resolveEffect:1175-1239 for one spawn-list value --
// [turret_N] shoot_flame and [projectile_N] explodeEffect(OnShield) share the grammar. `effects` is the unit's
// own [effect_*] pool, which a plain name searches before the built-ins and a CUSTOM: name searches alone.
// `shootFlame` is TurretState's single flame slot: a barrel passes its address and the first small/medium/large
// lands there, while a projectile passes nullptr because RWX runs every entry through the same list. A `none`
// value clears the list, and `missing` counts the names RWX would have thrown ConfigParseException over.
void parseFlameList(const std::string& raw, const std::vector<EffectTpl>& effects,
                    int* shootFlame, std::vector<FlameSpec>& flames, int& missing);
