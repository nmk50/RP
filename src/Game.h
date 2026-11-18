// Core simulation: teams, units, projectiles, orders, economy, combat.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "Common.h"
#include "TileMap.h"
#include "UnitTemplate.h"

// One barrel's runtime state. RWX keeps this in UnitMovementData[] movementLevels, one entry per
// [turret_N], so every barrel aims, warms up, counts its own delay and picks its own target
// (OrderableUnit.syncAttackTargetToMovementLevels / advanceMovementLevelTowardTarget).
struct TurretState {
    float angle = 0;       // movementLevels[i].targetX, world radians
    float angVel = 0;      // .velocityX, deg/tick while turnSpeedAcceleration is in use
    float cooldown = 0;    // .rotation, ticks left before this barrel may fire again
    float warm = 0;        // .speed, warmup ticks charged
    bool active = false;   // .isActive: aimed well enough to fire
    bool flip = false;     // .m: alternates each shot, drives barrelOffsetX_onOddShots
    int targetId = -1;     // .targetUnit
    // movementLevels[i].h/.i -- the aim jitter OrderableUnit.M(i):4812-4827 re-rolls after every shot:
    // whole pixels within +-target.radius*aimOffsetSpread of the hull, and the point both the barrel's
    // aim (:3169-3173) and the shot itself (Projectile.K/L) are measured from.
    Vec2 aimOff;
    // shoot_flame is attached to the shooter, so it keeps its own offset and angle while it plays
    int flameType = 0;
    float flameT = 0, flameAng = 0;
    Vec2 flameOff;
    // movementLevels[i].velocityY -- aiming holds it at 70 (OrderableUnit:3176 m.a(70)), and the idle
    // step (:846-861) only runs once it has decayed back to 0, so a barrel keeps its last aim for ~70
    // ticks after the target is gone before it sweeps or resets.
    float aimHold = 0;
    // movementLevels[i].k / .l -- the idle sweep timer and offset (CustomUnit.b(i,f):4010-4025). l is 0
    // fresh, +-idleSweepAngle while walking out, and +-inf once parked at a limit waiting to flip.
    float sweepTimer = 0, sweepOffset = 0;
    // movementLevels[i].targetY -- the angle the barrel held when it last aimed, the sweep centre a
    // shouldResetTurret:false barrel rocks about instead of C(i) (:4005-4006).
    float lastAim = 0;
    // CustomUnit.G[i] (:1557): the cached round an instantReuseLast barrel resets on each shot instead of
    // spawning a fresh one, kept as a projectile id (-1 = none).
    int beamProjId = -1;
};

// Foot/target offsets use world axes; z is relative to the hull, angle is radians and spin is degrees.
struct LegState {
    Vec2 foot, animOffset;
    float z = 0.1f;
    float angle = 0, spin = 0, alpha = 1;
    float warm = 0;
    Vec2 target;
    float distSq = 0;
    bool stepping = false, onWater = false, dustDone = true;
    bool reset = true, shrink = false, dirty = true;
};

struct Unit {
    int id = -1;
    std::string tplKey;
    const UnitTemplate* tpl = nullptr;
    int team = 0;
    // RWX Tree.b(int,int): the atlas (0 palm / 1 trees / 2 trees_snow) and the sub type that picks the
    // atlas row. -1 means "not given", which Tree fills with a deterministic roll off the object id.
    int treeType = 1, treeSub = -1;
    Vec2 pos;
    float facing = 0;        // body direction (radians)
    std::vector<TurretState> tur;   // one entry per tpl->turrets
    // RWX OrderableUnit.turretRotation: a 20-29 tick countdown between passive target sweeps, so an
    // army of multi-barrel units does not re-query the whole map every tick.
    int turretScanTimer = 0;
    float hp = 1, maxHp = 1;
    // RWX BaseUnit.shield / unitEnergyMax / unitShieldMax: the bubble, its capacity and the white
    // deflection flash it accumulates when a hit is soaked up.
    float shield = 0, shieldMax = 0, shieldFlash = 0;
    // RWX BaseUnit.currentEnergy plus the CustomUnit.v latch (:1353): once a shot has spent the pool
    // below what the next one costs, energyNeedsToRechargeToFull keeps the barrels locked until the
    // pool is whole again.
    float energy = 0;
    bool energyRecharging = false;
    // RWX CustomUnit.o (:1869-1874) -- the per-unit generation accumulator, which is what makes a
    // generation_delay of 120 pay its lump every 120 ticks rather than every 40.
    float genAcc = 0;
    std::vector<Vec2> path;  // world-space waypoints
    int pathIdx = 0;
    bool attackMove = false;
    int orderTargetId = -1;  // explicit attack target
    // RWX BaseUnit.targetUnit as resolved for this tick, explicit or auto-acquired. Soft collisions
    // skip a unit's own target (OrderableUnit.java:1047) so a tank can sit against what it shoots.
    int targetId = -1;
    int moveGoalId = -1;     // chase a moving unit target
    int nanoTargetId = -1;   // builder -> its RWX UnitCommandType.repair target: resumes the site
                             // while buildProgress<1 and heals hp afterwards
                             // (OrderableUnit.updateRepairOrReclaimCommand:2301/2351)
    // The same slot serves UnitCommandType.reclaim, and RWX tells the two apart only by the command type
    // (OrderableUnit:2300), so RP keeps one flag beside it: set with the target, cleared with it.
    bool nanoReclaim = false;
    // OrderableUnit.java:2404-2410 `this.ab`: the reclaim payout is a fraction of the price every tick, so
    // RWX banks the remainder on the builder and only hands whole credits to the team once it passes 1.
    float creditFrac = 0;
    // RWX wayPointTimer as the reclaim command sees it (OrderableUnit:2368-2378): for the first 100 ticks of
    // the order a target that is already more than half gone -- a site under 50% built, a hull under 50% hp
    // -- is left alone, so a builder never finishes off something that was nearly lost anyway.
    int nanoOrderAge = 0;
    std::vector<std::string> prodQueue;
    float prodProgress = 0;
    bool underConstruction = false;
    float buildProgress = 0; // 0..1 for buildings being constructed
    Vec2 rallyPoint; bool hasRally = false;
    // RWX attachment / action runtime: slot child links, flag bits, named resources and cooldowns.
    int parentId = -1, attachmentSlot = -1;
    std::vector<int> attachments;
    float attachmentFacing = 0;
    uint64_t flags = 0;
    std::unordered_map<std::string, double> resources;
    std::unordered_map<std::string, float> actionCooldowns;
    float autoCooldown = 0, allActionCooldown = 0, moveSpeedOverride = -1;
    bool createdEvent = false, completeEvent = false;
    // CustomUnit.a(UnitEventType):4257-4280 only queues the event; it runs on the next unit update.
    bool tookDamageEvent = false;
    // ---- transport (CustomUnit.java:312-456, 718-809, 1953-1980) ----
    // BaseUnit.unitTransportTarget: set for a hull that is aboard something. Such a unit leaves the
    // collision world (BaseUnit.bR():951 gives it group -1), is neither drawn nor picked nor hit
    // (CustomUnit.a(GameEngine):4656, Projectile:992/1463), and keeps its orders frozen until the carrier
    // puts it back down. `cargo` is CustomUnit.transportedUnits, in boarding order, and the unload pops the
    // *last* entry (g():422-429).
    int transportId = -1;
    std::vector<int> cargo;
    // isUnloading + transportUnloadTimer: getTransportedUnits():406-412 arms the timer at 30 ticks, and the
    // update at :1966-1979 counts it down only while transportUnitsCanUnloadUnits holds, dropping one hull
    // each time it reaches 0 and re-arming it at transportUnitsUnloadDelayBetweenEachUnit. A carrier that is
    // unloading refuses new passengers (d():336).
    bool unloading = false;
    float unloadTimer = 0;
    // UnitCommandType.loadInto (UnitCommand.java:279-283): walk to the carrier and board it.
    int loadTargetId = -1;
    // UnitCommandType.loadUp: the reverse, used on PC by right-clicking a friendly hull with a carrier
    // selected (GameUI.java:2732) -- the carrier walks onto it and takes it aboard.
    int loadUpTargetId = -1;
    bool riding() const { return transportId >= 0; }
    bool selected = false;
    bool dead = false;
    // RWX CustomUnitAnimationController state for the hull sprite. `animCur` is currentAnimation,
    // animPrio its animationId and animLoop forceLoop; animTime is currentTime, which starts at -1 and
    // is stepped by animSpeed * ticks (:132-146). animFrame / animScale are the two values the key
    // lists write back into the unit -- animationFrameIndex and currentFrameTime (:276-279).
    const BodyAnim* animCur = nullptr;
    float animTime = 0, animSpeed = 1, animScale = 1;
    int animFrame = 0;
    int animPrio = -1;
    bool animPlaying = false, animLoop = false, animRequested = false;
    bool animEnding = false;
    float animBlend = 0, animBlendRate = 0, animBaseScale = 1;
    std::vector<float> animBaseLegs;
    // CommandCenter's own frame clock (RWX CommandCenter.update:203-220): a hardcoded ping-pong over the
    // four base.png cells, not an [animation_*] strip -- 5 ticks a step, 70 ticks parked at 0 per cycle.
    float ccFrameTimer = 20.0f;
    int ccFrameIdx = 0;
    // CustomUnitLegController state: the limbs plus the hull pose of the previous tick (dP/dQ/dR/dS),
    // which is how the gait learns the hull moved without asking the movement code.
    std::vector<LegState> legs;
    Vec2 legPrevPos;
    float legPrevFacing = 0, legPrevZ = 0;
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
    // RWX BaseUnit.velocityX/velocityY. Only [movement] moveSlidingMode integrates it into the
    // position (OrderableUnit.java:893-918); a normal hull steps straight off its throttle.
    Vec2 vel;
    // RWX BaseUnit.worldX/worldY (:135) and .collisionGroup (BaseUnit.bR():944-958). A soft-collision
    // push is buffered in `push` and folded into `pos` at the start of the next tick's collision pass,
    // clamped to +-9 px per axis (OrderableUnit.java:922-930). collisionGroup is 3 airborne, 2 water,
    // 1 land, -1 for a unit that is dead or riding in a transport, and only units sharing a group push
    // each other (:1047).
    Vec2 push;
    int collisionGroup = 1;
    // OrderableUnit.java:873, 920 -- set when the unit had throttle, velocity or a buffered push this
    // tick. A unit that never moves is not asked to resolve collisions at all.
    bool moved = false;
    // OrderableUnit.java:3019 -- isMoving is unitStateTracker.stateFlag2, which the movement update
    // raises only while a waypoint/path node is being followed, or a live attack target is being
    // walked to (:2841-2850). It is order state, not momentum: a hull still coasting on its sliding
    // velocity is already idle. CustomUnit.I():598-602 keeps buildings out of it altogether.
    bool driven = false;
    // CustomUnit.update:1981-1984 -- `s`, the ticks since the hull last moved. [movement] landOnGround
    // waits for it to pass 3 before letting an AIR hull settle down (see UnitTemplate::landOnGround).
    float idleTime = 0;
    float movementEffectTimer = 0, splashEffectTimer = 0;
    // AutoRepairRenderHook.b's `customUnit.u`: [core] autoRepair counts up whether the hull is busy or
    // not, and only zeroes itself on the tick it actually spends the search, so a hull that was ordered
    // around looks for someone to mend the moment it runs out of things to do.
    float autoRepairTick = 0;
    // RWX GameObject.posZ (:43, :89) -- height above the ground in px, and the phase of the AIR sine
    // bob, which RWX keeps in frameAnimationTimer (:1996-2000, +2 deg per tick, wrapping at 360).
    // Everything below 4 px still counts as ground for collisions, targeting and splashes
    // (CustomUnit.i():636-638), and at or below -1 a unit is submerged (Q():641-643).
    float posZ = 0, heightPhase = 0;
    // RWX CustomUnit.update:1672-1768: a hull that dies above the ground with [movement]
    // slowDeathFall is not finished -- it keeps descending (`fallVel` is the frameAnimationDelay that
    // stands in for the falling velocity here), slides on its last movement step, trails smoke, and
    // only turns into a wreck or disappears once posZ reaches 0. `smokeTimer` and `debrisTimer` are
    // unitAnimationRotation (:1684, a puff every 9 ticks) and unitAnimationOffset (:1702, every 7).
    bool deathFalling = false;
    // e():946-951 gives a non-slowDeathFall hull that dies in the air its image_wreak texture right at
    // death, so the thing that falls is already a burnt wreck.
    bool corpseWreck = false;
    float fallVel = 0;
    int smokeTimer = 0, debrisTimer = 0;
    bool isStatic() const { return tpl && tpl->moveType == MoveType::NONE; }
    bool operational() const { return !underConstruction || buildProgress >= 1.0f; }
    bool airborne() const { return tpl && tpl->moveType == MoveType::AIR && posZ >= 4.0f; }
    bool submerged() const { return posZ <= -1.0f; }
};

// RWX CustomUnit.a(i, false):2870-2916 -- the pivot a barrel is drawn and fired from: the parent
// barrel's pivot along the attachedTo chain, slid by this barrel's recoil, plus [turret_N] x/y rotated
// into that frame (x right of the nose, y forward). The offsets are world px, never sprite px.
Vec2 turretPivot(const Unit& u, int i);
// RWX CustomUnit.D(i):4465-4480 -- the muzzle tip: pivot + barrelY along the muzzle direction + barrelX
// across it. Where a shot is born and where chargeEffectImage is drawn.
Vec2 muzzlePoint(const Unit& u, int i);
// barrel slide along its own aim, recoilOutTime/recoilReturnTime ticks after a shot
float turretRecoil(const Unit& u, int i);

// RWX CustomUnit.canRepairTarget:3648-3653 -> getTransportedUnitCount:3630-3644: a builder may fix a
// friendly target that is not itself unrepairable, buildings needing canRepairBuildings and mobile
// units needing canRepairUnits.
bool canRepairTarget(const UnitTemplate& builder, const Unit& target);

// OrderableUnit.getEngagementRange:4482-4487 -- [attack] maxAttackRange, plus both hull radii when the
// shooter is melee. The only distance test a shot passes (isWithinEngagementRange:3289, and the whole
// firing loop of updateAttackTargeting:3154-3157 sits behind it), so it is also how far a hull will
// bother to walk. See Game.cpp for the melee comment.
float engagementRangeOf(const Unit& shooter, const Unit& target);

struct Projectile {
    Vec2 pos, vel;
    // A stable handle for the shot. `projectiles` is compacted at the end of every tick, so an index is
    // useless across it, and RWX's interceptor link (Projectile.q, a plain object pointer) has to be an id
    // here. Assigned by Game::fire and the nukeOnDeath blast from Game::nextProjectileId.
    int id = 0;
    int team = 0;
    float damage = 0, areaDamage = 0, areaRadius = 0;
    int sourceId = -1;
    int targetId = -1;      // homing unit (-1 = move to dest)
    // RWX Projectile.aG/autoTargetingOnDeadTarget: once the chased unit is gone the shot re-locks on
    // the nearest enemy around the point lead px ahead of its heading (Projectile.java:699-702,
    // 1776-1820). The search needs the firing barrel's domains because the barrel's own state is not
    // reachable from a projectile id.
    bool autoRetarget = false;
    float autoRetargetRange = 120, autoRetargetLead = 15;
    bool atkFly = false, atkLand = true, atkSea = false;
    Vec2 dest;
    // RWX Projectile.K/L (CustomUnit.java:1587-1588): the firing barrel's aim jitter travels with the
    // shot, so a homing round chases target.pos + aimOff for its whole flight (:755-801) rather than
    // converging back on the hull centre.
    Vec2 aimOff;
    float speed = 6;
    int life = 60;
    // Where this round came from, kept as an index pair rather than a copy because the spawn list below is a
    // list of *names* resolved against one unit's [projectile_*] sections: RWX reaches the same place through
    // the shot's own Projectile.j.unitConfig (UnitSpawnList:169), and a hand-me-down round is built by that
    // same unit, so it inherits both halves and can run its own list a generation later. `from` is the
    // template, `fromProj` the entry in it; both stay null for the two rounds RP builds by hand (the nuke
    // blast and the stand-in [projectile_1] of a unit with none).
    const struct UnitTemplate* from = nullptr;
    int fromProj = -1;
    // RWX Projectile.aD, the generation counter a spawn list carries forward (Projectile:108, handed out as
    // -1 by the pool at :279 and set to the caller's aD + 1 by UnitSpawnList.a():163). A round that runs out
    // its life mid-flight uses it to stop a self-relaunching weapon going forever: recursionLimit defaults
    // to 3, and both shipped chains spend exactly one or two of those generations.
    int recursion = -1;
    int frame = -1;
    int drawType = 0;
    int shadowFrame = -1;
    float drawSize = 1;
    uint32_t color = 0xFFFFFFFF;   // 0xRRGGBBAA sprite tint
    uint32_t lightColor = 0;       // 0xRRGGBBAA additive glow, 0 = none
    float lightSize = 0.5f;
    // RWX Projectile.aM (copied from [projectile_N] trailEffect at CustomUnit.java:1467): while the shot
    // is flying and drawn it drops a smoke puff every trailEffectRate ticks (update:910-955).
    bool trail = false;
    float trailRate = 3.0f, trailT = 0;
    bool largeHit = false;
    bool flameWeapon = false;            // Projectile.G -- a fire emitter replaces the hit blast
    // RWX Projectile.aa / ab (b(BaseUnit):1470-1481). aa is what keeps a weapon inside the enemy teams:
    // CustomUnit.java:1391 pins it unless [projectile_N] friendlyFire clears it, while ab -- set only by
    // friendlyFire = only-ignoreEnemy -- inverts that and keeps the ring on its own team. A launcher nuke
    // and every nukeOnDeath blast carry neither flag, so their own team catches the ring as readily as
    // the enemy's.
    bool damagesAllies = false;
    bool alliesOnly = false;
    // ad / ae (:1485-1491): the constructor starts with ae set (:196), which is the burst-height domain
    // gate -- an air burst only for aircraft, a ground burst only for what stands on the ground.
    // areaHitAirAndLandAtSameTime (CustomUnit.java:1394-1397) and a nuke blast both swap that for ad,
    // and one ring then mows down aircraft and ground hulls together.
    bool hitsAirAndLand = false;
    // ac (:1482): a burst does not reach a hull more than 5px under the surface unless the weapon was
    // built to hunt them (areaHitUnderwaterAlways).
    bool hitsUnderwater = false;
    // g / j / h from the firing template (:1493-1516): no distance falloff at all, a hole around the
    // blast centre nothing can be hurt through, and a ring measured to the hull edge instead of its
    // centre.
    bool noFalloff = false;
    float ignoreCloserThan = 0;
    bool radiusFromEdge = false;
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
    // ak/al/am/an: how this shot splits its damage between a target's shield and its hull, and how
    // much of the target's armour it ignores. See Game::damage.
    float shieldMult = 1, deflectMult = 1, hullMult = 1, armourIgnore = 0;
    // Once the shot lands (bn) the splash ring grows over areaExpandTime ticks (ao/W) centred on
    // the impact point; the projectile itself is no longer drawn (update:1038 sets shouldDraw=false).
    bool hit = false;
    Vec2 impactPos;
    float expandT = 0, expandTotal = 0;
    // RWX Projectile.posZ / the aim height f3 (Projectile.update:754-801): the shot owns a height too,
    // chases it along the same 3D line as its 2D step (:889-900), and only lands when the gap is under
    // 3 px (:971-978) -- an instant weapon skips that gate (:979-982). impactZ is the burst height aX
    // (:1025, :1035), and a splash stays in the domain it detonated in: at or above 5 px it is an air
    // burst, which only aircraft feel (:1459-1491).
    float posZ = 0, aimZ = 0, impactZ = 0;
    // RWX Projectile.w / g.G / g.H (:844-856): the shot's own vertical speed, the height gravity removes
    // from it each tick, and the acceleration on that vertical speed. A lobbed round needs all three
    // because the vertical speed never reverses on its own -- what brings it back down is the aim-height
    // chase (:889-900), which is only allowed once gravity has been subtracted.
    float velZ = 0, gravity = 0, trueGravity = 0;
    // aC (:676,754): an unguided shell flies to the frozen point in `dest` rather than to a unit, and
    // carries no l, so it never lands a direct hit -- only its splash damages (CustomProjectileTemplate
    //.a():377-405). RWX still reads a unit there once autoTargetingOnDeadTarget re-locks it (:775-785).
    bool unguided = false;
    // RWX aE (:1503), the [projectile_N] tag names this shot carries. A plain string list rather than the
    // interned AnimationTag set: the only shipped reader is the interceptor hunt below.
    std::vector<std::string> tags;
    // ProjectileInterceptorHook.a():57-64 -- aC plus its quarry. `interceptor` is RWX's aC set through the
    // no-unit-target branch of CustomProjectileTemplate.a():362-375, which is what lets the round survive
    // Projectile.update:676-679's "no target left" removal; `chaseId` is Projectile.q, the shot it was
    // launched against, and `interceptLifeOnly` the [projectile_N] flag that decides whether the hit
    // destroys that shot or only empties its life (:1141-1148).
    bool interceptor = false;
    int chaseId = -1;
    bool interceptLifeOnly = false;
    // LaserDefense.attackTarget:265-321, the point-defence beam a [turret_N] laserDefenceEnergyUse barrel
    // fires at incoming rounds. `beamImmune` is RWX's Projectile.C -- set straight from a deflectionPower
    // below 0.5 at CustomUnit.java:1401-1402, and left clear for the 1.0 default -- and `deflectHp` is its
    // H, the same key read at :1404. `deflected` is aS, the mark :298-300 leaves on a round whose health has
    // run out, which Projectile.update:673-675 answers by taking the round out of the live list.
    bool instant = false;
    bool beamImmune = false;
    float deflectHp = 1.0f;
    bool deflected = false;
    // RWX Projectile.D / g.X / B / M and the teleport/reuse/parent flags (CustomUnit.a:1368-1511). A beam
    // (isBeam) lands its directDamage once but stays drawn until `life` expires instead of vanishing on
    // impact; nuke is the anti-nuke lock + blast visual; teleportSource moves the shooter to the impact.
    bool nuke = false;
    bool teleportSource = false;
    bool isBeam = false, laserEffect = false, lightingEffect = false;
    std::string beamImage, beamImageStart, beamImageEnd;
    bool beamStartRotated = false, beamEndRotated = false;
    float beamImageOffsetRate = 0;
    bool lightCastOnGround = false;
    bool reuseLast = false, reuseKeepAreaList = false;
    // moveWithParent (Projectile.au/av/aw/ax/ay): the round adds the firing turret's per-tick muzzle delta
    // to its own pos so a beam's muzzle end rides a moving hull. lastMuzzle caches the previous world pos.
    bool moveWithParent = false;
    int parentUnitId = -1, parentTurret = -1;
    Vec2 lastMuzzle; float lastMuzzleZ = 0;
    // aH/aI/aJ/aK (:857-888): the arc -- climb to ballisticHeight at the flight speed (aL is pinned to
    // -1 at spawn, CustomUnit.java:1496), with no horizontal step below ballisticDelayHeight, then dive
    // once the target is within 20 px. `apex` is aK.
    bool ballistic = false, ballisticApex = false;
    float ballisticDelayHeight = 40, ballisticHeight = 60;
    // aT (:1417-1421): the angle the sprite is drawn at, which chases the flight angle at 12 deg/tick
    // and is pinned straight up or down during the arc's vertical phases.
    float drawAngle = 0;
    std::vector<int> splashHits;             // ap: units already caught by the growing ring
};

// RWX EffectManager.createSmallExplosion: explode_big.png strip, frames 0..12 at 0.5/tick,
// with a random spin, scale and (for big explosions) a delayed start. A projectile smoke puff
// (EffectType.custom built at Projectile.update:921-939) shares the slot: it is effects.png cell 0,
// lives 70 ticks, grows 0.5x -> 1.2x and peaks at alpha 0.5.
struct Effect {
    Vec2 pos;
    float t = 0;              // ticks since spawn
    float delay = 0;          // RWX delayedStartTimer, in ticks
    float rot = 0;            // sprite rotation, radians
    float scale = 1;          // 0.8..1 like RWX
    float frameSpeed = 0.5f;  // frames per tick
    float life = 26.0f;       // ticks before it is dropped (flames: 13 / frameSpeed)
    bool trail = false;       // smoke puff rather than a flame
    // RWX Effect.Q, the per-tick drift of a puff: a projectile trail sinks 0.1 px/tick (Projectile:934)
    // while a dying hull's smoke is pinned at Q=0 (CustomUnit:1699).
    float sink = 0;
    // RWX Effect.ap, the frame picked out of the sheet. A fire puff takes a random one of fire.png's four
    // 20px cells (EffectEmitter.java:97-118, whose aq=7 is that sheet per EffectManager.java:735-744).
    int frame = 0;
    bool fire = false;
    // RWX Effect.G and the fade-in length Effect.t for a puff (Effect.java:437, 521-526). The projectile
    // trail runs 0.5 -> 1.2 over 70 ticks fading in for 5; the death smoke is 0.9 -> 1.2 over 60 with no
    // fade-in at all.
    float puffFrom = 0.5f, puffTo = 1.2f, puffFadeIn = 5.0f;
    bool dead = false;
};

// One live instance of an [effect_*] template or of one of the built-in flavours: RWX
// gameFramework/effects/Effect.java, driven by EffectTemplate.a():228-354 at spawn and stepped tick by tick
// by draw(1f):297-357. The letters in the comments are Effect's own fields, which is what every formula below
// is quoted from. The template is copied rather than pointed at because CustomUnit:1590 hands the muzzle
// light a colour the section never mentions, and RWX's pool is 500 Effect objects (loadContent:870) -- that
// cap is why a barrage cannot grow the list without bound.
// Effect.b, the GameObject an attached instance rides. RWX hands the list one of three things: the hull
// that fired (CustomUnit.java:1594), the hull that was hit (Projectile.java:1069) or the shot itself
// (:1469). Only its live position is read back, so an id plus a snapshot is the whole of it.
struct FxOwner {
    int unitId = -1, projId = -1;
    Vec2 pos; float posZ = 0;
};

struct Fx {
    EffectTpl spec;
    const UnitTemplate* src = nullptr;   // whose [effect_*] section this is, for `image:` path resolution
    int unitId = -1;                     // Effect.b, set only when the effect is attached at spawn
    int projId = -1;                     // the same field when Effect.b is a Projectile instead of a hull
    Vec2 hull; float hullZ = 0;          // that object's live position, re-read each tick for the draw
    Vec2 ij; float k = 0;                // I/J/K: absolute, or relative to `hull` once attached
    Vec2 vel; float hVel = 0;            // P/Q/R
    // Y/Z in DEGREES, the unit RWX keeps them in: Effect.update:541 rotates by Y + 90 straight off the
    // field, and Utility.fastCos/fastSin index a 360-degree table. RP's turret angles are radians, so the
    // muzzle direction is converted on the way in and the renderer adds the 90 on the way out.
    float dir = 0, dirVel = 0;
    float life = 0, lifeMax = 0;         // V, ticks; lifeMax is the rolled V at spawn
    float w = 0;                         // W, the age/scale/fade denominator (spec.fadeDenom or life)
    float delay = 0;                     // U, delayedStartTimer
    bool anim = false;                   // ae
    float animSpeed = 0.5f;              // aj, animateFrameSpeed + its own random roll
    float framePos = 0;                  // ak
    int frameFrom = 0;                   // af
    bool frameBack = false;              // al
    int frame = 0;                       // ap, the cell picked out of the sheet
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
    // A deque, not a vector: updateUnit() spawns units (factory output, builders, death spawns) while it
    // still holds a reference to the unit being updated and pointers to its neighbours, and only a deque
    // keeps those valid when the container grows.
    std::deque<Unit> units;
    // A deque for the same reason: the splash loop holds a reference into this container while damage()
    // can kill something, and a death with [core] nukeOnDeath pushes a fresh blast in (Projectile:1022).
    std::deque<Projectile> projectiles;
    std::vector<Effect> effects;
    // The live instances of the [effect_*] templates and the built-in muzzle flavours that RWX's
    // shoot_flame spawn list puts on screen (EffectManager.createEffectWithTemplateInternal). Kept apart
    // from `effects`, which is the explosion/puff pool.
    std::vector<Fx> fx;
    std::vector<Particle> particles;
    std::vector<Scorch> scorches;   // permanent ground decals, drawn with the map
    std::vector<Wreck> wrecks;      // dead units that left an image_wreak behind
    std::vector<Team> teams;
    long tick = 0;
    bool gameOver = false;
    std::string gameOverText;
    int nextId = 1;
    int nextProjectileId = 1;

    bool init(const std::string& mapPath, std::string* err);
    void update();

    // queries
    Unit* get(int id);
    const Unit* get(int id) const;
    // The shot with this id, or null once it is gone -- a round that has already flagged itself dead counts
    // as gone, since RWX's Projectile.q is dropped from the live list at that point.
    Projectile* findProjectile(int id);
    std::vector<Unit*> findEnemyUnits(int team, Vec2 center, float radius, bool flying, bool land, bool underwater,
                                      bool landMustTouchWater = false);
    std::vector<Unit*> findUnitsInRect(int team, Vec2 a, Vec2 b);
    std::vector<Unit*> selectedUnits();

    // orders
    void orderMove(std::vector<int> ids, Vec2 target);
    void orderAttackMove(std::vector<int> ids, Vec2 target);
    int  orderAttackTarget(std::vector<int> ids, int targetId); // returns #units assigned
    bool orderRepair(std::vector<int> builderIds, int targetId);
    // [core] canReclaimResources: take a friendly hull apart for its price
    // (GameUI.handleUnitTargetClick:2700-2726 -> issueReclaimCommand).
    bool orderReclaim(int builderId, int targetId);
    void orderStop(std::vector<int> ids);
    bool orderBuild(int builderId, const std::string& unitName, int tx, int ty);
    bool orderProduce(int factoryId, const std::string& unitName);
    bool orderUpgrade(int unitId, int actionIdx);
    double logicNumber(const Unit& u, const std::string& expression) const;
    bool logicCondition(const Unit& u, const std::string& expression, bool fallback = true) const;
    const Unit* logicUnit(const Unit& u, const std::string& expression) const;
    const AttachmentTpl* attachmentConfig(const Unit& u) const;
    bool selectable(const Unit& u) const;
    bool targetable(const Unit& u) const;
    bool visible(const Unit& u) const;
    bool actionAvailable(const Unit& u, int index) const;
    std::string actionLabel(const Unit& u, const UnitAction& action) const;
    std::vector<std::pair<int, int>> visibleActions(const Unit& u) const;
    // ---- transport ----
    // CustomUnit.dI():4826-4837, the slots the cargo occupies.
    int transportSlotsUsed(const Unit& t) const;
    // CustomUnit.d(BaseUnit,boolean):335-362 -- would `ride` fit and be allowed aboard `t`?
    bool canLoadInto(const Unit& t, const Unit& ride) const;
    // GameUI.issueLoadIntoCommand:3588 through UnitCommandType.loadInto: the selected hulls walk to the
    // carrier and board it once within getTransportInteractionRange() (21 px, BaseUnit.java:1867).
    // Returns how many took the order.
    int orderLoadInto(std::vector<int> ids, int transportId);
    // GameUI.canSelectedUnitsLoadUp:3872-3885 through UnitCommandType.loadUp: the selected carriers walk
    // onto `rideId` and board it themselves. Returns how many took the order.
    int orderLoadUp(std::vector<int> ids, int rideId);
    // CustomUnit.getTransportedUnits():406-412 -- the unload order: arms the 30 tick drop-off fuse.
    void orderUnload(int transportId);
    // getLoadedUnits():415-420 -- call the unload off again and keep everyone aboard.
    void orderLoadUpAgain(int transportId);
    bool buildingSpotFree(Vec2 center, const UnitTemplate& t) const;
    bool canAfford(int team, float cost);
    void spend(int team, float cost);
    void reward(int team, float credits);
    int  spawnUnit(const std::string& tplKey, int team, Vec2 pos, bool underConstruction = false);

    // helpers
    bool isEnemy(int a, int b) const { return a != b; }
    Vec2 nearestEnemyPos(int team);
    int playerTeam() const { return 0; }

    // ---- fog of war ----
    // RWX keeps one byte per tile per team (PlayerTeam.fogOfWarData) with three meaningful values:
    // 0 = in sight right now, 5 = explored but out of sight, 10 = never seen (TileMap:1056-1058 fills
    // the array with 10, updateFogVisibilityForTeamCircle:1519-1580 drops it to 0 inside a sight
    // circle and ramps 0..10 across the rim). Everything >= 5 hides an enemy hull from the local
    // player (BaseUnit.d(PlayerTeam):1431-1444) and dims the terrain, == 10 hides the terrain
    // outright (TileMap:1731). RP keeps a single layer -- the one the human player sees -- because
    // the AI has no visibility model to feed.
    static constexpr uint8_t FOG_VISIBLE = 0;
    static constexpr uint8_t FOG_EXPLORED = 5;
    static constexpr uint8_t FOG_UNSEEN = 10;
    std::vector<uint8_t> fog;      // w*h, row-major
    bool fogEnabled = true;
    void initFog();                // (re)allocate to map size and start fully unexplored
    void updateFog();              // one pass: dim last tick's sight, then re-light every hull
    void revealFogAt(Vec2 world, float tiles, uint8_t value);
    uint8_t fogAt(int tx, int ty) const;
    uint8_t fogAtWorld(Vec2 p) const;
    // BaseUnit.d(PlayerTeam):1431-1444 -- the local player always sees its own hulls; anything else
    // only while the tile it stands on is still lit.
    bool visibleToPlayer(const Unit& u) const;

    float damage(Unit& target, float dmg, const Projectile* p = nullptr);
    void impact(Projectile& p, Unit* direct, Vec2 at);
    // UnitSpawner.a():203-449, the [projectile_N] spawnUnit list run at Projectile.update:1074-1076: the hulls
    // it carries are put down at the impact point, on the shooter's team.
    void spawnUnitsOnHit(Projectile& p, Vec2 at);
    void splash(Projectile& p, float frac);
    void checkWinLoss();

private:
    void updateUnit(Unit& u);
    void createAttachments(Unit& u, int depth = 0);
    void updateAttachments(Unit& u);
    void releaseAttachments(Unit& u);
    bool convertUnit(Unit& u, const std::string& name);
    void addResources(Unit& u, const std::string& text);
    void runAction(Unit& u, const UnitAction& action, int depth = 0);
    void actionEvent(Unit& u, const std::string& event, const std::string& tags = "", int depth = 0);
    void updateActions(Unit& u);
    void flushMessages();
    struct ActionMessage { int unitId; std::string tags; int depth; };
    std::deque<ActionMessage> actionMessages;
    int attachmentSpawnDepth = 0;
    void updateMovementFx(Unit& u);
    void initLegs(Unit& u);
    void updateLegs(Unit& u, float f);
    float legFacing(const Unit& u) const;
    // the CustomUnit.java:1672-1768 descent of a corpse that died above the ground
    void updateDeathFall(Unit& u);
    // RWX OrderableUnit.updateAllUnitCollisions:451-509 -- the per-tick soft-collision pass that keeps
    // overlapping hulls apart, replacing a symmetric push.
    void resolveSoftCollisions();
    void updateProjectile(Projectile& p);
    void killUnit(Unit& u);
    void removeUnit(Unit& u);
    void explode(Vec2 at, bool big, float scale = 1.0f);
    // EffectTemplate.a():237-347 -- one instance of an [effect_*] template, or of the EffectTemplate a
    // built-in muzzle flavour is written out as. `at` is the muzzle point (RWX's I/J), `h` its height above
    // the ground (posZ, which becomes K) and `dirDeg` the barrel's aim angle f4 in DEGREES, the unit RWX
    // hands the list (movementLevels[i].targetX). `owner` is the GameObject the effect is attached to when
    // the template asks for it; a null owner always leaves the instance free-standing.
    void spawnFx(const EffectTpl& spec, const UnitTemplate* src, Vec2 at, float h, float dirDeg,
                 const FxOwner* owner);
    // The list a barrel's shoot_flame runs when it lets go (CustomUnit.java:1593-1594), preceded by the
    // shoot_light glow (:1590-1591), which is not part of the list and is never attached.
    void spawnMuzzleFx(const TurretTpl& tur, Vec2 muzzle, float h, float dirDeg, Unit& u);
    // EffectTemplate.a():156-207 -- one entry of a spawn list, built-in or [effect_*]. Shared by the barrel's
    // shoot_flame and the shot's two lists, which differ only in the GameObject they hand it.
    void spawnFxEntry(const FlameSpec& fs, const UnitTemplate* src, Vec2 at, float h, float dirDeg,
                      const FxOwner* owner);
    // Projectile.java:1045-1070 -- the list a shot runs where it landed, with the victim as the GameObject.
    // Returns true when RWX's z10 is cleared (:1149-1168): the shot brought a list of its own, or the hull it
    // hit is still carrying a bubble. Either way the generic blast is skipped.
    bool spawnHitFx(Projectile& p, Unit* victim, Vec2 at);
    void spawnFlameHit(Vec2 at);
    // CustomUnit.java:1468-1469 -- the list a shot runs at the muzzle the tick it is born, with the SHOT as
    // the GameObject, so an attached entry rides it downrange. The spawn list calls the same factory
    // (UnitSpawnList.java:174), so a handed-down round runs its own section's list too.
    void spawnBirthFx(Projectile& p);
    // Effect.draw(f=1):297-357, the single-tick step of every instance.
    void updateFx();
    void addScorch(Vec2 at);
    void updateTurrets(Unit& u, Unit* unitTarget);
    Unit* nearestInTurretRange(const Unit& u, int i) const;
    // The shot a barrel lets go, or null when nothing was launched (an instant weapon with no expanding
    // ring left to animate). `target` is null for the two ways RWX fires at a point instead of a hull: an
    // interceptor round (ProjectileInterceptorHook.a():60) and a shell aimed at ground. `groundAt` is then
    // the frozen point the round flies to, RWX's Projectile.n/o.
    Projectile* fire(Unit& u, Unit* target, int ti, Vec2 groundAt = Vec2{});
    // UnitSpawnList.a():132-178, the list a round runs when its life gives out before it arrives
    // (Projectile.update:1411-1415). Every entry builds a fresh shot of the named [projectile_*] template at
    // the dying round's position, handed that round's target or the point it was flying to, and carrying
    // generation `depth` as its own aD so a recursionLimit can stop a self-relaunching chain. `src` goes by
    // value on purpose: the rounds this makes are appended to the very vector the dying one lives in.
    void spawnFromList(Projectile src, const UnitTemplate& owner,
                       const std::vector<ProjectileTpl::SpawnEntry>& list, int depth);
    // [turret_N] interceptProjectiles_withTags -- ProjectileInterceptorHook.b():15-39, the per-tick hunt
    // that lets an anti-nuke launch a round at an incoming one.
    void updateInterceptors(Unit& u);
    // The other half of the same hook, :22-35: a [turret_N] laserDefenceEnergyUse barrel sweeps the rounds
    // inside its beam bubble once a tick and shoots down the first eligible one it finds. LaserDefense:265-321.
    void updateLaserDefence(Unit& u, const TurretTpl& tur, int ti);
    // CustomUnit.update:1966 -- transportUnitsCanUnloadUnits.read(this): the logic boolean that gates the
    // unload countdown.
    bool unloadCondOk(const Unit& t) const;
    // CustomUnit.b(i,f):3998 -- idleSweepCondition.read(this), the gate on the idle sweep. The only
    // shipped atom is `not self.isMoving()`, so it shares the unload condition's predicate list.
    bool sweepCondOk(const Unit& u, const TurretTpl& tur) const;
    // CustomUnit.loadTransportedUnit:374-383, and the unload place b(BaseUnit,boolean,boolean):718-809,
    // which reports whether the hull really got out.
    void loadUnit(Unit& t, Unit& ride);
    bool placeCargo(Unit& t, Unit& ride, bool evenSide);
    // CustomUnit.h(boolean):553-562 -- empty the hold, optionally killing what was in it.
    void releaseCargo(Unit& t, bool kill);
    void runAI();
    std::vector<Unit*> queryNearestEnemy(int team, Vec2 pos, float maxDist, const UnitTemplate* shooter);
};
