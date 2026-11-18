// Builtins.cpp -- RWX's hard-coded units.
//
// UnitTypeEnum.java hands every one of these to a Java class (`createUnitInstanceWithBoolean`), so the
// shipped asset tree has no .ini for them: `assets/units/` only carries the custom units. RP therefore
// registers them here, before the ini scan, and an ini of the same name (or one that says
// `overrideAndReplace : tank`) still wins because it is loaded afterwards.
//
// The numbers come straight from the Java classes; the citation on each block is the line the value was
// read from. This also covers the six units that used to live in assets/units/rp_builtin: their movement,
// build speed (UnitTypeEnum.D()) and build/production menus (addAvailableActions) now follow the Java
// source instead of the hand-tuned ini values, so the deleted filler inis are fully replaced by code.
#include "UnitTemplate.h"
#include <cctype>
#include <cmath>
#include <string>

namespace {

UnitTemplate& add(TemplateStore& st, const char* name, const char* text) {
    UnitTemplate t;
    t.name = name;
    t.displayText = text;
    // sourceDir stays empty: RWX loads built-in art straight from drawable/, and resolveImage falls back
    // to assets/drawable for a template with no directory of its own.
    t.sourceDir = "";
    t.teamColoringMode = kTCModePureGreen;
    // byName and order are keyed by the lower-cased name, exactly like the ini scan does
    // (Templates.cpp: `byName[ToLower(t.name)] = t`), because spawnUnit looks up ToLower(tplKey).
    std::string key = name;
    for (char& c : key) c = (char)std::tolower((unsigned char)c);
    st.byName[key] = t;
    st.order.push_back(key);
    return st.byName[key];
}

// A barrel that has no art of its own (RWX `d(int)` returning null): it still aims and fires, it just
// draws nothing. GunBoat.java:87-89 is the canonical case.
TurretTpl& invisibleTurret(UnitTemplate& t, float x, float y, float size) {
    TurretTpl tur;
    tur.x = x; tur.y = y;
    tur.invisible = true;
    tur.barrelY = size;
    tur.turnSpeed = 4.0f;
    t.turrets.push_back(tur);
    return t.turrets.back();
}

TurretTpl& visibleTurret(UnitTemplate& t, float x, float y, float size, const char* image) {
    TurretTpl tur;
    tur.x = x; tur.y = y;
    tur.barrelY = size;
    tur.turnSpeed = 3.0f;
    tur.image = image;
    t.turrets.push_back(tur);
    return t.turrets.back();
}

// RWX builds the built-in rounds in Java by poking Projectile fields directly; the field map is
// (Projectile.java: r/t/U/h/P/x/ar, :858-903 movement):
//   U = directDamage, t = initial speed, r = target speed (:903), s = speed accel (:903),
//   h = life, P = sprite frame, x = drawSize, ar = ARGB colour.
// RP stores colour as 0xRRGGBBAA (parseRgba / Renderer.cpp:826), RWX as 0xAARRGGBB (KoolArgbColor.a),
// so ARGB -> RP swaps the alpha from the top byte to the bottom one. The JADX build replaces small
// integers in ar with Android keycode names: STB_INPUT=180, TV_DATA_SERVICE=230, TV_TERRESTRIAL_ANALOG=235.
// A round with no P is a plain coloured dot in RWX (ProjectileTemplate.java:84) -> frame=-1 here.
constexpr uint32_t argbToRp(unsigned a, unsigned r, unsigned g, unsigned b) {
    return (r << 24) | (g << 16) | (b << 8) | a;
}

struct ShotSpec {
    float damage;        // U
    float speed;         // t
    int life;            // h
    float targetSpeed;   // r, 0 = no acceleration (flies at constant t)
    float accel;         // s
    int frame;           // P, -1 = plain dot
    float drawSize;      // x
    uint32_t color;      // ar, already argbToRp()
};

ProjectileTpl& shot(UnitTemplate& t, const ShotSpec& s) {
    ProjectileTpl p;
    p.directDamage = s.damage;
    p.speed = s.speed;
    p.life = s.life;
    if (s.targetSpeed > 0.0f) p.targetSpeed = s.targetSpeed;
    if (s.accel > 0.0f) p.targetSpeedAccel = s.accel;
    p.frame = s.frame;
    p.drawSize = s.drawSize;
    p.color = s.color;
    t.projectiles.push_back(p);
    return t.projectiles.back();
}

void canBuild(UnitTemplate& t, const char* name, float pos) {
    BuildOption b;
    b.unitName = name;
    b.pos = pos;
    t.canBuild.push_back(b);
}

void landMovement(UnitTemplate& t, float speed, float accel, float decel, float turn, float turnAccel) {
    t.moveType = MoveType::LAND;
    t.moveSpeed = speed; t.accel = accel; t.decel = decel;
    t.maxTurnSpeed = turn; t.turnAccel = turnAccel;
}

void waterMovement(UnitTemplate& t, float speed, float accel, float decel, float turn, float turnAccel) {
    t.moveType = MoveType::WATER;
    t.moveSpeed = speed; t.accel = accel; t.decel = decel;
    t.maxTurnSpeed = turn; t.turnAccel = turnAccel;
}

void airMovement(UnitTemplate& t, float speed, float accel, float decel, float turn, float turnAccel) {
    t.moveType = MoveType::AIR;
    t.moveSpeed = speed; t.accel = accel; t.decel = decel;
    t.maxTurnSpeed = turn; t.turnAccel = turnAccel;
    t.targetHeight = 35.0f;          // RWX's default cruise height for an air hull
    t.targetHeightDrift = 1.5f;
}

void buildingCommon(UnitTemplate& t, int footL, int footU, int footR, int footD, float sight) {
    t.isBuilding = true;
    t.moveType = MoveType::NONE;
    t.footL = footL; t.footU = footU; t.footR = footR; t.footD = footD;
    t.fogSight = sight;
}

}  // namespace

void registerBuiltinUnits(TemplateStore& st) {
    // ------------------------------------------------------------------ sea
    {
        // GunBoat.java:92-97 sprite 15x27, radius 12, displayRadius radius-2, hp 170; :41 mass 1500;
        // :119-166 damage 12, range 120, reload 60, speed 1.5, turn 2.8, accel 0.06, decel 0.2.
        UnitTemplate& t = add(st, "gunBoat", "Gun Boat");
        t.price = 300; t.maxHp = 170; t.mass = 1500;
        t.radius = 12; t.displayRadius = 10;
        t.image = "gun_boat.png"; t.imageWreak = "gun_boat_dead.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 1; t.shadowOffY = 1;
        waterMovement(t, 1.5f, 0.06f, 0.2f, 2.8f, 0.35f);
        t.canAttack = true; t.attackRange = 120; t.shootDelay = 60; t.turretSize = 12;
        invisibleTurret(t, 0, 0, 12);
        // GunBoat.java:118-135 U=q()=12, t=8, h=30, x=1, ar=a(255,180,180,0) gold dot (no P -> plain).
        shot(t, {12, 8, 30, 0, 0, -1, 1, argbToRp(255, 180, 180, 0)});
    }
    {
        // BattleShip.java:105-108 radius 20, hp 1200; :51 mass 9000; damage 65, range 240, speed 0.8,
        // turn 1.8, accel 0.03, decel 0.1. Its turret art is battle_ship_t2_turret.png.
        UnitTemplate& t = add(st, "battleShip", "Battleship");
        t.price = 1500; t.maxHp = 1200; t.mass = 9000;
        t.radius = 20; t.displayRadius = 20;
        t.image = "battle_ship_t2.png"; t.imageWreak = "battle_ship_t2_dead.png";
        t.imageTurret = "battle_ship_t2_turret.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 2; t.shadowOffY = 2;
        waterMovement(t, 0.8f, 0.03f, 0.1f, 1.8f, 0.08f);
        t.canAttack = true; t.attackRange = 240; t.shootDelay = 60; t.turretSize = 26;
        visibleTurret(t, 0, -10, 26, "");
        visibleTurret(t, 0, 10, 26, "");
        // BattleShip.java:129-147 U=q()=65, t=4, h=80, x=2, ar=a(255,180,180,0) gold; no P.
        shot(t, {65, 4, 80, 0, 0, -1, 2, argbToRp(255, 180, 180, 0)});
    }
    {
        // MissileShip.java:81-86 sprite 17x31, radius 15, displayRadius radius-2, hp 350; damage 62,
        // range 200, reload 170, speed 1.2, turn 1.9, accel 0.05, decel 0.1. MapTile.java:163 loads the
        // map name `scoutShip` as this class, so both names point at one template.
        UnitTemplate& t = add(st, "scoutShip", "Scout Ship");
        t.price = 900; t.maxHp = 350; t.mass = 4000;
        t.radius = 15; t.displayRadius = 13;
        t.image = "scout_ship.png"; t.imageWreak = "scout_ship_dead.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 1; t.shadowOffY = 1;
        waterMovement(t, 1.2f, 0.05f, 0.1f, 1.9f, 0.2f);
        t.canAttack = true; t.attackRange = 200; t.shootDelay = 170; t.turretSize = 14;
        invisibleTurret(t, 0, 0, 14);
        // MissileShip.java:127-134 primary missile U=62, t=2, h=190, ar=a(255,230,230,50) yellow.
        shot(t, {62, 2, 190, 0, 0, -1, 1, argbToRp(255, 230, 230, 50)});
        st.byName["missileship"] = t;   // UnitTypeEnum.missileShip is the same class
    }
    {
        // AttackSubmarine.java: radius 15, displayRadius radius-2, hp 260; turn 1.2, accel 0.018,
        // decel 0.1, turnAccel 0.06. The hull rides at targetHeight -8 (see light_sub.ini, the custom
        // unit built from the same role), which is what makes it fade under the water.
        UnitTemplate& t = add(st, "attackSubmarine", "Attack Submarine");
        t.price = 800; t.maxHp = 260; t.mass = 4000;
        t.radius = 15; t.displayRadius = 13;
        t.image = "attack_submarine.png"; t.imageWreak = "attack_submarine_dead.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 1; t.shadowOffY = 1;
        waterMovement(t, 1.0f, 0.018f, 0.1f, 1.2f, 0.06f);
        t.targetHeight = -8.0f; t.targetHeightDrift = 0.4f;
        t.canAttack = true; t.attackRange = 160; t.shootDelay = 60; t.turretSize = 14;
        invisibleTurret(t, 0, 0, 14);
        // AttackSubmarine.java:356-363 primary torpedo U=42, t=2, h=190, ar=a(255,230,230,50) yellow.
        shot(t, {42, 2, 190, 0, 0, -1, 1, argbToRp(255, 230, 230, 50)});
    }
    // ------------------------------------------------------------------ air
    {
        // Gunship.java:64-69 sprite 25x35, radius 15, hp 260; damage 35, range 140, reload 40,
        // turn 4.0, accel 0.2, decel 0.1, turnAccel 0.4.
        UnitTemplate& t = add(st, "gunShip", "Gunship");
        t.price = 800; t.maxHp = 260; t.mass = 3000;
        t.radius = 15; t.displayRadius = 15;
        t.image = "gunship.png"; t.imageWreak = "gunship_dead.png";
        t.imageShadow = "gunship_shadow.png";
        airMovement(t, 1.4f, 0.2f, 0.1f, 4.0f, 0.4f);
        t.canAttack = true; t.attackRange = 140; t.shootDelay = 40; t.turretSize = 14;
        invisibleTurret(t, 0, 0, 14);
        // Gunship.java:119-135 U=q()=35, t=4, h=80, x=2, ar=a(255,150,230,40) (blue keycode 230).
        shot(t, {35, 4, 80, 0, 0, -1, 2, argbToRp(255, 150, 230, 40)});
    }
    {
        // Dropship.java: radius 20, hp 500; :51 mass 16000; range 140, reload 40, speed 2.3, turn 1.4,
        // accel 0.03, decel 0.05; :472 canTransportUnits.
        UnitTemplate& t = add(st, "dropship", "Dropship");
        t.price = 800; t.maxHp = 500; t.mass = 16000;
        t.radius = 20; t.displayRadius = 20;
        t.image = "dropship.png"; t.imageWreak = "dropship_dead.png";
        t.imageShadow = "dropship_shadow.png";
        airMovement(t, 2.3f, 0.03f, 0.05f, 1.4f, 0.2f);
        t.maxTransportingUnits = 2;
        t.transportSlotsNeeded = 1;
        t.canAttack = false;
    }
    // ------------------------------------------------------------------ land
    {
        // Ladybug.java:77-82 sprite 17x26, radius 5, displayRadius radius+3, hp 130; range 43, reload 17,
        // speed 1.7, accel 0.07, decel 0.12, turn 5.5. A bug bites at contact range.
        UnitTemplate& t = add(st, "ladyBug", "Ladybug");
        t.price = 400; t.maxHp = 130; t.mass = 800;
        t.radius = 5; t.displayRadius = 8;
        t.image = "ladybug.png";
        landMovement(t, 1.7f, 0.07f, 0.12f, 5.5f, 0.3f);
        t.canAttack = true; t.isMelee = true; t.meleeRange = 43; t.attackRange = 43;
        t.shootDelay = 17; t.turretSize = 4;
        invisibleTurret(t, 0, 0, 4);
        // Ladybug.java:123-127 a(BaseUnit) = Projectile.a(this, target, 14.0f, null, false): an instant
        // contact bite for 14, no travelling round. :131 m()=43 reach, :136 b()=17 reload.
        ProjectileTpl& bite = shot(t, {14, 6, 1, 0, 0, -1, 1, 0xFFFFFFFF});
        bite.instant = true;
    }
    {
        // ExperimentalHoverTank.java: radius 30, displayRadius radius+1, hp 3500; :51 mass 80000;
        // range 180, reload 8, speed 0.6, turn 1.1, accel 0.02, decel 0.02, turnAccel 0.03.
        // Three barrels and its own shield bubble (experimental_hovertank_shield.png).
        UnitTemplate& t = add(st, "experimentalHoverTank", "Experimental Hover Tank");
        t.price = 21000; t.maxHp = 3500; t.mass = 80000;
        t.radius = 30; t.displayRadius = 31;
        t.image = "experimental_hovertank.png";
        t.imageWreak = "experimental_hovertank_dead.png";
        t.imageTurret = "experimental_hovertank_turret.png";
        t.imageShield = "experimental_hovertank_shield.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 3; t.shadowOffY = 3;
        t.maxShield = 2000; t.shieldRegen = 1.0f; t.shieldRenderRadius = 34;
        t.moveType = MoveType::HOVER;
        t.moveSpeed = 0.6f; t.accel = 0.02f; t.decel = 0.02f;
        t.maxTurnSpeed = 1.1f; t.turnAccel = 0.03f;
        t.canAttack = true; t.attackRange = 180; t.shootDelay = 8; t.turretSize = 26;
        visibleTurret(t, 0, -8, 26, "");
        visibleTurret(t, 0, 0, 30, "");
        visibleTurret(t, 0, 8, 26, "");
        // ExperimentalHoverTank.java:207-237 q()=0 but the weapon is a continuous beam:
        // projectileA.U=380 (:233), A=true instant (:220), J=70 impact range (:225), h=reload+10.
        // RP models it like the shipped laser_tank: instant + laserEffect + instantReuseLast, which draws
        // the light beam and re-hits each shot (RP has no per-tick channel, so damage lands per barrage).
        ProjectileTpl& beam = shot(t, {380, 6, 10, 0, 0, -1, 1, argbToRp(255, 50, 230, 50)});
        beam.instant = true; beam.laserEffect = true; beam.instantReuseLast = true;
    }
    {
        // CrystalResource.java: radius 11, displayRadius radius+1, hp 600, price 5000. A placeable
        // crystal that pays out credits, so it is a static hull with an income rate and no weapons.
        UnitTemplate& t = add(st, "crystalResource", "Crystal");
        t.price = 5000; t.maxHp = 600; t.mass = 9000;
        t.radius = 11; t.displayRadius = 12;
        t.image = "crystal.png";
        t.moveType = MoveType::NONE;
        t.isBuilding = true;
        t.canAttack = false;
        t.resourceRate = 1.0f;
        t.creditIncomeRate = 1;
    }
    // ------------------------------------------------------------------ buildings
    {
        // CommandCenter.java:94-111 -- T(53)/U(68) (base.png is 212x68, so four frames), radius 30,
        // displayRadius 30, hp 4000, buildingTargetRect (-1,-1,1,1), buildingVelocityRect (-1,-1,1,2).
        // :86-90 art base/base_dead, with base_back.png as the overlay layer. :231-267 damage 70,
        // range 280, reload 70, projectile speed 5 / life 180. :290 builds a builder at slot 1.
        UnitTemplate& t = add(st, "commandCenter", "Command Center");
        t.price = 3000; t.maxHp = 4000; t.mass = 4000;
        t.radius = 30; t.displayRadius = 30;
        t.totalFrames = 4;
        t.image = "base.png"; t.imageWreak = "base_dead.png";
        t.imageShadow = "NONE";
        t.buildSpeed = 0.001f;   // UnitTypeEnum.commandCenter D()
        buildingCommon(t, -1, -1, 1, 1, 22);
        t.canAttack = true; t.canAttackFlying = true; t.canAttackLand = true;
        t.canAttackUnderwater = false;
        t.isFixedFiring = true; t.attackRange = 280; t.shootDelay = 70; t.turretSize = 18;
        t.turretTurnSpeed = 3;
        invisibleTurret(t, 0, 8, 18);
        // CommandCenter.java:231-254 U=q()=70, t=2 initial, r=5 target (:247), h=180,
        // ar=a(255,230,230,50) yellow missile. s is unset in Java, so t ramps to r at RP's default accel.
        shot(t, {70, 2, 180, 5, 0, -1, 1, argbToRp(255, 230, 230, 50)});
        canBuild(t, "builder", 1);
    }
    {
        // LandFactory.java:177-186 -- the factory's real queue: builder 1, tank 2, hoverTank 3,
        // artillery 4, then at upgrade level 2 hovercraft 5 / heavyTank 6 / heavyHoverTank 7 /
        // laserTank 8. RP has no per-building upgrade level, so the whole list is offered.
        // Constructor: radius 30, displayRadius 30, hp 1200, target rect (-1,-1,1,1).
        UnitTemplate& t = add(st, "landFactory", "Land Factory");
        t.price = 700; t.maxHp = 1200; t.mass = 4000;
        t.radius = 30; t.displayRadius = 30;
        t.image = "land_factory_front.png"; t.imageWreak = "land_factory_dead.png";
        t.imageShadow = "NONE";
        t.buildSpeed = 0.001f;
        buildingCommon(t, -1, -1, 1, 1, 18);
        t.isFactory = true;
        canBuild(t, "builder", 1);
        canBuild(t, "tank", 2);
        canBuild(t, "hoverTank", 3);
        canBuild(t, "artillery", 4);
        canBuild(t, "hovercraft", 5);
        canBuild(t, "heavyTank", 6);
        canBuild(t, "heavyHoverTank", 7);
        canBuild(t, "laserTank", 8);
    }
    {
        // AirFactory.java:186-191 -- a level-1 air factory builds nothing at all; dropship 3.2,
        // gunShip 4 and amphibiousJet 5 only appear once it is upgraded. RP has no upgrade level, so
        // the list is always offered. Constructor: T(40)/U(61) (air_factory.png is 200x63, five
        // frames), radius 30, displayRadius 30, hp 1000, target rect (-1,-1,1,1).
        UnitTemplate& t = add(st, "airFactory", "Air Factory");
        t.price = 1000; t.maxHp = 1000; t.mass = 4000;
        t.radius = 30; t.displayRadius = 30;
        t.totalFrames = 5;
        t.image = "air_factory.png"; t.imageWreak = "air_factory_dead.png";
        t.imageShadow = "NONE";
        t.buildSpeed = 0.001f;
        buildingCommon(t, -1, -1, 1, 1, 18);
        t.isFactory = true;
        canBuild(t, "dropship", 3.2f);
        canBuild(t, "gunShip", 4);
        canBuild(t, "amphibiousJet", 5);
    }
    {
        // SeaFactory.java:145-155 -- builderShip 1, gunBoat 2, missileShip 3, hovercraft 4,
        // battleShip 5, attackSubmarine 6. Constructor: radius 45, displayRadius 45, hp 1000,
        // target rect (-1,-1,1,2) on water.
        UnitTemplate& t = add(st, "seaFactory", "Sea Factory");
        t.price = 1000; t.maxHp = 1000; t.mass = 4000;
        t.radius = 45; t.displayRadius = 45;
        t.image = "sea_factory.png"; t.imageWreak = "sea_factory_dead.png";
        t.imageShadow = "NONE";
        t.buildSpeed = 0.001f;   // UnitTypeEnum.seaFactory D()
        buildingCommon(t, -1, -1, 1, 2, 18);
        t.placeOnlyOnWater = true;
        t.isFactory = true;
        canBuild(t, "builderShip", 1);
        canBuild(t, "gunBoat", 2);
        canBuild(t, "missileShip", 3);
        canBuild(t, "hovercraft", 4);
        canBuild(t, "battleShip", 5);
        canBuild(t, "attackSubmarine", 6);
    }
    // ------------------------------------------------------------------ builder / tanks
    {
        // BuilderUnit.java:308-350 -- T(20)/U(20) (builder.png is 20x20), radius 10,
        // displayRadius radius+2, hp 170. getMoveSpeed 0.8 on land / 0.6 over liquid,
        // getMaxTurnSpeed 3.8 / 1.7, turn acceleration 0.35, acceleration 0.04, deceleration 0.1.
        // The reach comes from BaseUnit.y():1895 = 85, which is what a built-in builder inherits
        // (CustomUnit.y():1641-1646 falls back to it whenever [core] nanoRange is unset).
        // UnitTypeEnum.builder D() = 0.002 is its build speed.
        // :358-372 the build menu: extractor 1, turret 2, antiAirTurret 3, landFactory 4, airFactory 5,
        // seaFactory 6, laserDefence 7, repairbay 8, fabricator 9, experimentalLandFactory 10,
        // NukeLaucher 14, AntiNukeLaucher 15.
        UnitTemplate& t = add(st, "builder", "Builder");
        t.price = 500; t.maxHp = 170; t.mass = 1200;
        t.radius = 10; t.displayRadius = 12;
        t.image = "builder.png"; t.imageWreak = "builder_dead.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 1; t.shadowOffY = 1;
        t.buildSpeed = 0.002f;
        t.isBuilder = true;
        t.nanoRange = 85; t.nanoBuildSpeed = 2.0f; t.nanoRepairSpeed = 0.5f;
        t.canRepairBuildings = true;
        landMovement(t, 0.8f, 0.04f, 0.1f, 3.8f, 0.35f);
        canBuild(t, "extractor", 1);
        canBuild(t, "turret", 2);
        canBuild(t, "antiAirTurret", 3);
        canBuild(t, "landFactory", 4);
        canBuild(t, "airFactory", 5);
        canBuild(t, "seaFactory", 6);
        canBuild(t, "laserDefence", 7);
        canBuild(t, "repairbay", 8);
        canBuild(t, "fabricator", 9);
        canBuild(t, "experimentalLandFactory", 10);
        canBuild(t, "NukeLaucher", 14);
        canBuild(t, "AntiNukeLaucher", 15);
    }
    {
        // HeavyTankUnit.java:298-306 -- radius 15, displayRadius radius+1, hp 600; :51 mass 7000.
        // a(c[7], 3) splits heavy_tank.png (66x36) into three frames. damage 50, range 160, reload 70,
        // projectile life 60, speed 0.8, turn 1.9, acceleration 0.05, deceleration 0.1.
        UnitTemplate& t = add(st, "heavyTank", "Heavy Tank");
        t.price = 800; t.maxHp = 600; t.mass = 7000;
        t.radius = 15; t.displayRadius = 16;
        t.totalFrames = 3;
        t.image = "heavy_tank.png"; t.imageWreak = "heavy_tank_dead.png";
        t.imageTurret = "heavy_tank_turret.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 1; t.shadowOffY = 1;
        t.techLevel = 2;
        t.buildSpeed = 0.0011f;
        landMovement(t, 0.8f, 0.05f, 0.1f, 1.9f, 0.2f);
        t.canAttack = true; t.attackRange = 160; t.shootDelay = 70; t.turretSize = 21;
        TurretTpl& tur = visibleTurret(t, 0, 0, 21, "");
        tur.turnSpeed = 3.0f; tur.turnAccel = 1.0f;
        tur.recoil = -2; tur.recoilOut = 4; tur.recoilReturn = 6;
        // HeavyTankUnit.java:158-182 main gun U=q()=50, t=4, h=60, x=2,
        // ar=a(235,150,230,40) (Terrrestrial=235 alpha, blue=230). Secondary missile on upgrade omitted.
        shot(t, {50, 4, 60, 0, 0, -1, 2, argbToRp(235, 150, 230, 40)});
    }
    // ------------------------------------------------------------------ remaining UnitTypeEnum hulls
    {
        // HoverTankUnit.java:60-71 constructor: radius 7, displayRadius radius+2, hp 150; no mass override
        // (hover land hull inherits a light default, use 800 like the other light tanks). :98-137 damage 23,
        // range 140, reload 90, projectile life 85 / speed 6. :141-172 moveSpeed 1.0, accel 0.04, decel D()=0.09,
        // turn 180 (JADX deg units -- HoverLandUnit turns about its own axis), turnAccel B()=0.5. :56-58 d(int)=null
        // (no turret art, invisible barrel). :36-38 art hover_tank / hover_tank_dead / hover_tank_shadow.
        // :94 / :176-183 it is a sliding, order-ignoring hover hull that bobs at posZ~4.
        UnitTemplate& t = add(st, "hoverTank", "Hover Tank");
        t.price = 600; t.maxHp = 150; t.mass = 800;
        t.radius = 7; t.displayRadius = 9;
        t.image = "hover_tank.png"; t.imageWreak = "hover_tank_dead.png";
        t.imageShadow = "hover_tank_shadow.png";
        t.moveType = MoveType::HOVER;
        t.moveSpeed = 1.0f; t.accel = 0.04f; t.decel = 0.09f;
        t.maxTurnSpeed = 180.0f; t.turnAccel = 0.5f;
        t.targetHeight = 4.0f; t.targetHeightDrift = 1.5f;
        t.canAttack = true; t.canAttackFlying = true; t.attackRange = 140; t.shootDelay = 90; t.turretSize = 7;
        invisibleTurret(t, 0, 0, 7);
        // HoverTankUnit.java:98-121 U=q()=23, t=6, s=0.2, h=85, P=6 (plasma sprite frame),
        // x=1, ar=a(255,50,230,50). This is the one built-in with an explicit projectile sprite.
        shot(t, {23, 6, 85, 0, 0.2f, 6, 1, argbToRp(255, 50, 230, 50)});
    }
    {
        // HovercraftUnit.java:170-182 constructor T(20)/U(32), radius 15, displayRadius radius, hp 450;
        // :423 mass 12000. :342-372 moveSpeed 0.9 on land / 1.2 over liquid, turn 1.4 / 1.8, accel 0.03,
        // decel D()=0.05, turnAccel B()=0.1. :237-241 / :458 it is a transport, 4 cargo slots, no weapon
        // (:381 canAttack=false). :146-148 art hovercraft / hovercraft_dead / hovercraft_shadow.
        UnitTemplate& t = add(st, "hovercraft", "Hovercraft");
        t.price = 2000; t.maxHp = 450; t.mass = 12000;
        t.radius = 15; t.displayRadius = 15;
        t.image = "hovercraft.png"; t.imageWreak = "hovercraft_dead.png";
        t.imageShadow = "hovercraft_shadow.png";
        t.moveType = MoveType::HOVER;
        t.moveSpeed = 0.9f; t.accel = 0.03f; t.decel = 0.05f;
        t.maxTurnSpeed = 1.4f; t.turnAccel = 0.1f;
        t.targetHeight = 3.0f; t.targetHeightDrift = 1.5f;
        t.maxTransportingUnits = 4; t.transportSlotsNeeded = 1;
        t.canAttack = false;
    }
    {
        // BuilderShip.java:108-123 constructor radius 13, displayRadius radius, hp 500; :59 mass 6000.
        // :141-175 moveSpeed 0.8, accel 0.03, decel D()=0.1, turn 1.9, turnAccel B()=0.12. :225 canAttack=false.
        // :275-286 build menu: extractor 1, turret 2, antiAirTurret 3, landFactory 4, airFactory 5,
        // seaFactory 6, fabricator 7, laserDefence 8, repairbay 9. :65-67 art builder_ship / _dead / _turret.
        UnitTemplate& t = add(st, "builderShip", "Builder Ship");
        t.price = 700; t.maxHp = 500; t.mass = 6000;
        t.radius = 13; t.displayRadius = 13;
        t.image = "builder_ship.png"; t.imageWreak = "builder_ship_dead.png";
        t.imageTurret = "builder_ship_turret.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 3; t.shadowOffY = 3;
        waterMovement(t, 0.8f, 0.03f, 0.1f, 1.9f, 0.12f);
        t.techLevel = 1;
        t.buildSpeed = 0.001f;   // UnitTypeEnum.builderShip D()
        t.isBuilder = true;
        t.nanoRange = 145; t.nanoBuildSpeed = 2.0f; t.nanoRepairSpeed = 0.5f;  // :299 y()=145
        t.canRepairBuildings = true;
        canBuild(t, "extractor", 1);
        canBuild(t, "turret", 2);
        canBuild(t, "antiAirTurret", 3);
        canBuild(t, "landFactory", 4);
        canBuild(t, "airFactory", 5);
        canBuild(t, "seaFactory", 6);
        canBuild(t, "fabricator", 7);
        canBuild(t, "laserDefence", 8);
        canBuild(t, "repairbay", 9);
    }
    {
        // LaserDefense.java:177-190 constructor a(level1Textures[0],2) = 2 frames, radius 19,
        // displayRadius radius, hp 500, target/velocity rect (0,0,1,1). :341-341 m() range 160 (L1) / 210 (L2).
        // :182 currentEnergy starts 1.0; :233 energy regen 4.0E-4/tick (L1), +2.0E-4 (L2); :249 it intercepts
        // incoming rounds at 0.11 energy each (L1) / 0.05 (L2). It is NOT a weapon (:369 canAttack=false):
        // RP bolts point defence on an invisible canShoot:false [turret] with laserDefenceEnergyUse (see
        // Game::updateLaserDefence and the shipped laser_defense.ini). :166-168 art laser_defence / _t2 / _dead.
        UnitTemplate& t = add(st, "laserDefence", "Laser Defence");
        t.price = 750; t.maxHp = 500; t.mass = 4000;
        t.radius = 19; t.displayRadius = 19;
        t.totalFrames = 2;
        t.image = "laser_defence.png"; t.imageWreak = "laser_defence_dead.png";
        t.imageShadow = "NONE";
        buildingCommon(t, 0, 0, 1, 1, 18);
        t.canAttack = false;
        t.maxEnergy = 1.0f; t.energyRegen = 0.0004f; t.energyStartingPercentage = 1.0f;
        t.showEnergyBar = false;  // the energy is an internal interceptor pool, not a manna bar (:380)
        TurretTpl& pd = invisibleTurret(t, 0, 0, 1);
        pd.canShoot = false; pd.laserDefence = true; pd.laserDefenceEnergyUse = 0.11f;
        pd.range = 160.0f;
    }
    {
        // MegaTankUnit.java:64-67 sprite 20x25, radius 12, displayRadius radius+1, hp 550; :51 mass 7000;
        // range 140, reload 70, projectile damage 50 / life 60, speed 0.8, turn 1.2.
        UnitTemplate& t = add(st, "megaTank", "Mega Tank");
        t.price = 800; t.maxHp = 550; t.mass = 7000;
        t.radius = 12; t.displayRadius = 13;
        t.image = "mega_tank.png"; t.imageWreak = "mega_tank_dead.png";
        t.imageTurret = "mega_tank_turret.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 1; t.shadowOffY = 1;
        landMovement(t, 0.8f, 0.05f, 0.1f, 1.2f, 0.2f);
        t.canAttack = true; t.attackRange = 140; t.shootDelay = 70; t.turretSize = 18;
        visibleTurret(t, 0, 0, 18, "");
        // MegaTankUnit.java:101-112 main gun U=50 (literal), t=3, h=60, x=2, ar=a(255,150,230,40).
        // Secondary missile (:114-125 U=40) on upgrade omitted.
        shot(t, {50, 3, 60, 0, 0, -1, 2, argbToRp(255, 150, 230, 40)});
    }
    {
        // TankDestroyer.java sprite 16x30, radius 11, displayRadius radius+2, hp 350; range 150,
        // reload 70, damage 35 / life 60, speed 1.0, turn 1.9, accel 0.07, decel 0.12.
        UnitTemplate& t = add(st, "tankDestroyer", "Tank Destroyer");
        t.price = 800; t.maxHp = 350; t.mass = 4000;
        t.radius = 11; t.displayRadius = 13;
        t.image = "tank2.png"; t.imageWreak = "tank2_dead.png";
        t.imageTurret = "tank2_turret.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 1; t.shadowOffY = 1;
        landMovement(t, 1.0f, 0.07f, 0.12f, 1.9f, 0.2f);
        t.canAttack = true; t.attackRange = 150; t.shootDelay = 70; t.turretSize = 16;
        visibleTurret(t, 0, 0, 16, "");
        // TankDestroyer.java:90-97 U=35 (literal), t=3, h=60; no ar -> RWX default white, no x/P.
        shot(t, {35, 3, 60, 0, 0, -1, 1, 0xFFFFFFFF});
    }
    {
        // WallVertical.java: radius 15, hp 700, price 100 -- a one-tile wall segment.
        UnitTemplate& t = add(st, "wall_v", "Wall");
        t.price = 100; t.maxHp = 700; t.mass = 4000;
        t.radius = 15; t.displayRadius = 15;
        t.image = "wall_v.png";
        t.imageShadow = "NONE";
        buildingCommon(t, 0, 0, 0, 0, 4);
        t.canAttack = false;
    }
    {
        // SupplyDepot.java: radius 20, hp 800, price 1000 -- the transport supply building.
        UnitTemplate& t = add(st, "supplyDepot", "Supply Depot");
        t.price = 1000; t.maxHp = 800; t.mass = 4000;
        t.radius = 20; t.displayRadius = 20;
        t.image = "supply_depot.png"; t.imageWreak = "supply_depot_dead.png";
        t.imageShadow = "NONE";
        buildingCommon(t, -1, -1, 1, 1, 14);
        t.canAttack = false;
    }
    {
        // FogRevealer.java: price 1000 and no art at all -- an invisible hull that only opens the fog
        // around itself, so it is untouchable and draws nothing.
        UnitTemplate& t = add(st, "fogRevealer", "Fog Revealer");
        t.price = 1000; t.maxHp = 100; t.mass = 1000;
        t.radius = 10; t.displayRadius = 10;
        t.moveType = MoveType::NONE;
        t.fogSight = 40;
        t.unselectable = true; t.untargetable = true; t.cannotBeAttacked = true;
        t.canAttack = false;
    }
    // ------------------------------------------------------------------ scenery
    {
        // Tree.java:102-108 -- radius 3, displayRadius radius+1, hp 100, no .ini at all. Drawn by
        // Renderer::drawTree from the shared trees.png atlas rather than a body sprite.
        UnitTemplate& t = add(st, "tree", "Tree");
        t.price = 0; t.maxHp = 100;
        t.radius = 3; t.displayRadius = 4;
        t.image = "trees.png";
        t.unselectable = true; t.untargetable = true; t.cannotBeAttacked = true;
        t.teamColoringMode = kTCModeDisabled;
    }
}
