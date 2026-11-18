// Builtins.cpp -- RWX's hard-coded units.
//
// UnitTypeEnum.java hands every one of these to a Java class (`createUnitInstanceWithBoolean`), so the
// shipped asset tree has no .ini for them: `assets/units/` only carries the custom units. RP therefore
// registers them here, before the ini scan, and an ini of the same name (or one that says
// `overrideAndReplace : tank`) still wins because it is loaded afterwards.
//
// The numbers come straight from the Java classes; the citation on each block is the line the value was
// read from. Movement values for the six units that used to live in assets/units/rp_builtin keep the
// tuned numbers those ini files had, since they were already validated in game.
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

ProjectileTpl& shot(UnitTemplate& t, float damage, float speed, int life, int frame = 1) {
    ProjectileTpl p;
    p.directDamage = damage;
    p.speed = speed;
    p.life = life;
    p.frame = frame;
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
        shot(t, 12, 6, 30);
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
        shot(t, 65, 5, 80, 2);
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
        shot(t, 62, 4, 190, 3);
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
        shot(t, 40, 5, 190, 3);
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
        shot(t, 35, 6, 80);
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
        shot(t, 20, 6, 20);
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
        shot(t, 25, 7, 60, 2);
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
        // CommandCenter.java:86-88 art base/base_dead; :259-267 range 280, reload 70; damage 70,
        // projectile speed 5 / life 180.
        UnitTemplate& t = add(st, "commandCenter", "Command Center");
        t.price = 3000; t.maxHp = 4000; t.mass = 4000;
        t.radius = 30; t.displayRadius = 30;
        t.totalFrames = 4;
        t.image = "base.png"; t.imageWreak = "base_dead.png";
        t.imageShadow = "NONE";
        t.buildSpeed = 0.0005f;
        buildingCommon(t, -1, -1, 1, 1, 22);
        t.canAttack = true; t.canAttackFlying = true; t.canAttackLand = true;
        t.canAttackUnderwater = false;
        t.isFixedFiring = true; t.attackRange = 280; t.shootDelay = 70; t.turretSize = 18;
        t.turretTurnSpeed = 3;
        invisibleTurret(t, 0, 8, 18);
        shot(t, 70, 5, 180);
        canBuild(t, "builder", 1);
    }
    {
        // LandFactory.java: radius 30, hp 1200, price 700.
        UnitTemplate& t = add(st, "landFactory", "Land Factory");
        t.price = 700; t.maxHp = 1200; t.mass = 4000;
        t.radius = 30; t.displayRadius = 30;
        t.image = "land_factory_front.png"; t.imageWreak = "land_factory_dead.png";
        t.imageShadow = "NONE";
        t.buildSpeed = 0.001f;
        buildingCommon(t, -1, -1, 1, 1, 18);
        t.isFactory = true;
        canBuild(t, "tank", 2);
        canBuild(t, "artillery", 4);
        canBuild(t, "heavyTank", 6);
    }
    {
        // AirFactory.java: radius 30, hp 1000, price 1000; 5 animation frames.
        UnitTemplate& t = add(st, "airFactory", "Air Factory");
        t.price = 1000; t.maxHp = 1000; t.mass = 4000;
        t.radius = 30; t.displayRadius = 20;
        t.totalFrames = 5;
        t.image = "air_factory.png"; t.imageWreak = "air_factory_dead.png";
        t.imageShadow = "NONE";
        t.buildSpeed = 0.001f;
        buildingCommon(t, -1, -1, 1, 1, 18);
        t.isFactory = true;
        canBuild(t, "c_interceptor", 1);
        canBuild(t, "lightGunship", 2);
        canBuild(t, "bomber", 3);
        canBuild(t, "c_helicopter", 4);
    }
    {
        // SeaFactory.java: radius 45, hp 1000, price 1000, footprint 3x4 on water.
        UnitTemplate& t = add(st, "seaFactory", "Sea Factory");
        t.price = 1000; t.maxHp = 1000; t.mass = 4000;
        t.radius = 45; t.displayRadius = 45;
        t.image = "sea_factory.png"; t.imageWreak = "sea_factory_dead.png";
        t.imageShadow = "NONE";
        t.buildSpeed = 0.0007f;
        buildingCommon(t, -1, -1, 1, 2, 18);
        t.placeOnlyOnWater = true;
        t.isFactory = true;
        canBuild(t, "lightSub", 1);
        canBuild(t, "nautilusSubmarine", 2);
        canBuild(t, "heavyAAShip", 3);
        canBuild(t, "heavyMissileShip", 4);
        canBuild(t, "heavyBattleship", 5);
    }
    // ------------------------------------------------------------------ builder / tanks
    {
        // BuilderUnit.java: sprite 20x20, radius 10, displayRadius radius+2, hp 170, price 500.
        UnitTemplate& t = add(st, "builder", "Builder");
        t.price = 500; t.maxHp = 170; t.mass = 1200;
        t.radius = 10; t.displayRadius = 12;
        t.image = "builder.png"; t.imageWreak = "builder_dead.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 1; t.shadowOffY = 1;
        t.isBuilder = true;
        t.nanoRange = 90; t.nanoBuildSpeed = 2.0f; t.nanoRepairSpeed = 0.5f;
        t.canRepairBuildings = true;
        landMovement(t, 0.8f, 0.07f, 0.17f, 4.0f, 0.25f);
        canBuild(t, "extractor", 3);
        canBuild(t, "turret", 3.1f);
        canBuild(t, "turret_artillery", 3.15f);
        canBuild(t, "c_turret_t1_lightning", 3.18f);
        canBuild(t, "antiAirTurret", 3.2f);
        canBuild(t, "landFactory", 4);
        canBuild(t, "mechFactory", 4.2f);
        canBuild(t, "fabricator", 4.4f);
        canBuild(t, "airFactory", 4.5f);
        canBuild(t, "seaFactory", 4.6f);
        canBuild(t, "turretT2", 5);
        canBuild(t, "c_turret_t2_artillery", 5.1f);
        canBuild(t, "c_turret_t2_lightning", 5.2f);
        canBuild(t, "turret_flamethrower", 5.3f);
        canBuild(t, "antiAirTurretT2", 5.5f);
        canBuild(t, "turretT3", 6);
        canBuild(t, "c_antiAirTurretT3", 6.1f);
        canBuild(t, "laboratory", 7);
        canBuild(t, "outpostT1", 8);
    }
    {
        // HeavyTankUnit.java: radius 15, displayRadius radius+1, hp 600; :51 mass 7000; damage 50,
        // range 160, reload 70; 3 moving frames; turret art heavy_tank_turret.png.
        UnitTemplate& t = add(st, "heavyTank", "Heavy Tank");
        t.price = 800; t.maxHp = 600; t.mass = 7000;
        t.radius = 15; t.displayRadius = 16;
        t.totalFrames = 3;
        t.image = "heavy_tank.png"; t.imageWreak = "heavy_tank_dead.png";
        t.imageTurret = "heavy_tank_turret.png";
        t.imageShadow = "AUTO"; t.shadowOffX = 1; t.shadowOffY = 1;
        t.techLevel = 2;
        t.buildSpeed = 0.0011f;
        landMovement(t, 0.8f, 0.05f, 0.12f, 3.0f, 0.2f);
        t.canAttack = true; t.attackRange = 160; t.shootDelay = 70; t.turretSize = 21;
        TurretTpl& tur = visibleTurret(t, 0, 0, 21, "");
        tur.turnSpeed = 3.0f; tur.turnAccel = 1.0f;
        tur.recoil = -2; tur.recoilOut = 4; tur.recoilReturn = 6;
        shot(t, 50, 5, 60);
    }
    // ------------------------------------------------------------------ remaining UnitTypeEnum hulls
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
        shot(t, 50, 6, 60, 2);
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
        shot(t, 35, 6, 60, 2);
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
