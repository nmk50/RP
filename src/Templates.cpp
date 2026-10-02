// Template loader: scans assets for .ini unit definitions and builds UnitTemplate records.
#include "UnitTemplate.h"
#include "Ini.h"
#include <raylib.h>
#include <set>
#include <sstream>

static std::vector<float> parseCSV(const std::string& s) {
    std::vector<float> out; std::stringstream ss(s); std::string tok;
    while (std::getline(ss, tok, ',')) { tok = Trim(tok); if (!tok.empty()) out.push_back((float)std::atof(tok.c_str())); }
    return out;
}

static MoveType parseMoveType(const std::string& s) {
    std::string v = ToLower(Trim(s));
    if (v == "land") return MoveType::LAND;
    if (v == "air") return MoveType::AIR;
    if (v == "water") return MoveType::WATER;
    if (v == "hover") return MoveType::HOVER;
    if (v == "over_cliff" || v == "over_cliff_water") return MoveType::OVER_CLIFF;
    return MoveType::NONE; // BUILDING / NONE
}

// parse canBuild_N / builtFrom_N style pairs into a vector
static void parseBuildLinks(const IniSection& core, const std::string& prefix, std::vector<BuildOption>& out) {
    std::map<int, std::string> names;
    std::map<int, float> poss;
    std::string lp = ToLower(prefix) + "_";
    for (auto& kv : core.kv) {
        std::string k = ToLower(kv.first);
        if (k.rfind(lp, 0) != 0) continue;
        std::string rest = k.substr(lp.size()); // e.g. "1_name"
        size_t us = rest.find('_'); if (us == std::string::npos) continue;
        int idx = atoi(rest.substr(0, us).c_str());
        std::string tag = rest.substr(us + 1);
        if (tag == "name") names[idx] = Trim(kv.second);
        else if (tag == "pos") poss[idx] = (float)std::atof(kv.second.c_str());
    }
    for (auto& n : names) {
        if (n.second.empty()) continue;
        BuildOption b; b.unitName = n.second; b.pos = poss.count(n.first) ? poss[n.first] : (float)n.first;
        out.push_back(b);
    }
}

static std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

// RWX [core] copyFrom : other.ini — merge base file's keys under ours (ours win)
static void applyCopyFrom(IniFile& ini, const std::string& iniPath, int depth) {
    if (depth > 4) return;
    IniSection* c = ini.find("core");
    if (!c) return;
    std::string src = Trim(c->get("copyfrom"));
    if (src.empty()) return;
    c->idx.erase("copyfrom");
    std::string p = dirOf(iniPath) + "/" + src;
    IniFile base;
    if (!ParseIni(p, base)) return;
    applyCopyFrom(base, p, depth + 1);
    for (auto& s : base.sections) {
        IniSection& d = ini.getOrCreate(s.name);
        for (auto& kv : s.kv) if (!d.has(kv.first)) d.set(kv.first, kv.second);
    }
}

static UnitTemplate buildTemplate(IniFile& ini, const std::string& iniPath) {
    UnitTemplate t;
    t.sourceDir = dirOf(iniPath);

    if (const IniSection* c = ini.find("core")) {
        t.name = c->get("name");
        t.overrideName = c->get("overrideandreplace");
        t.displayText = c->get("displaytext", t.name);
        if (t.displayText.empty()) t.displayText = t.name;
        t.price = (float)c->getNum("price", t.price);
        t.maxHp = (float)c->getNum("maxhp", 100);
        t.mass = (float)c->getNum("mass", 1000);
        t.radius = (float)c->getNum("radius", 12);
        t.displayRadius = (float)c->getNum("displayradius", t.radius);
        t.techLevel = (int)c->getNum("techlevel", 1);
        double bs = c->getNum("buildspeed", 0.001);
        // buildSpeed can be like "65.6s" (seconds) or per-tick float
        std::string bsv = ToLower(c->get("buildspeed"));
        if (EndsWith(bsv, "s")) { // convert seconds -> per-tick progress
            float secs = (float)atof(bsv.c_str());
            t.buildSpeed = secs > 0 ? 1.0f / (secs * TICK_RATE) : 0.001f;
        } else t.buildSpeed = (float)bs;
        t.isBuilding = c->getBool("isbuilding", false);
        t.isBuilder = c->getBool("isbuilder", false);
        t.fogSight = (float)c->getNum("fogofwarsightrange", 0);
        t.energyUse = (float)c->getNum("energyusage", 0);
        auto fp = parseCSV(c->get("footprint", c->get("constructionfootprint", "0,0,0,0")));
        if (fp.size() >= 4) { t.footL = (int)fp[0]; t.footU = (int)fp[1]; t.footR = (int)fp[2]; t.footD = (int)fp[3]; }
        t.placeOnlyOnResPool = c->getBool("placeonlyonrespools", c->getBool("placeonlyonresource", false));
        std::string gen = c->get("generation_resources");
        if (gen.empty()) gen = c->get("resourcegeneration");
        // "credits=8" -> 8
        size_t eq = gen.find('='); if (eq != std::string::npos) t.creditIncomeRate = (float)atof(gen.c_str() + eq + 1);
        if (t.creditIncomeRate == 0) t.creditIncomeRate = (float)c->getNum("generation_credits", 0);
        parseBuildLinks(*c, "canBuild", t.canBuild);
        parseBuildLinks(*c, "builtFrom", t.builtFrom);
    }
    if (const IniSection* g = ini.find("graphics")) {
        t.image = g->get("image");
        t.imageWreak = g->get("image_wreak");
        t.imageTurret = g->get("image_turret");
        t.imageScale = (float)g->getNum("imagescale", g->getNum("scaleimagesto_scale", 1));
        t.turretImageScale = (float)g->getNum("turretimagescale", 1);
        t.totalFrames = (int)g->getNum("total_frames", 1);
        if (t.totalFrames < 1) t.totalFrames = 1;
        t.drawLayer = g->get("drawlayer");
    }
    if (const IniSection* a = ini.find("attack")) {
        t.canAttack = a->getBool("canattack", false);
        t.canAttackFlying = a->getBool("canattackflyingunits", false);
        t.canAttackLand = a->getBool("canattacklandunits", true);
        t.canAttackUnderwater = a->getBool("canattackunderwaterunits", false);
        t.isFixedFiring = a->getBool("isfixedfiring", false);
        t.attackRange = (float)a->getNum("maxattackrange", 0);
        t.shootDelay = (float)a->getNum("shootdelay", 60);
        t.turretSize = (float)a->getNum("turretsize", 0);
        t.turretTurnSpeed = (float)a->getNum("turretturnspeed", 3);
    }
    if (const IniSection* m = ini.find("movement")) {
        t.moveType = parseMoveType(m->get("movementtype", "NONE"));
        t.moveSpeed = (float)m->getNum("movespeed", 0);
        t.accel = (float)m->getNum("moveaccelerationspeed", 0.07);
        t.decel = (float)m->getNum("movedecelerationspeed", 0.17);
        t.maxTurnSpeed = (float)m->getNum("maxturnspeed", 4);
        t.turnAccel = (float)m->getNum("turnacceleration", 0.25);
    }
    if (t.isBuilding) t.moveType = MoveType::NONE;

    // nano (builder) params
    if (const IniSection* c = ini.find("core")) {
        t.nanoRange = (float)c->getNum("nanorange", 0);
        t.nanoBuildSpeed = (float)c->getNum("nanobuildspeed", 0);
        t.nanoRepairSpeed = (float)c->getNum("nanorepairspeed", 0);
        if (t.isBuilder && t.nanoRange == 0) t.nanoRange = 90;
        // RWX mods often omit isBuilder on nano units (e.g. megaBuilder)
        if (!t.isBuilder && !t.isBuilding && (t.nanoRange > 0 || t.nanoBuildSpeed > 0)) t.isBuilder = true;
    }

    // turrets: gather by index, apply copyFrom, then read
    {
        std::map<int, const IniSection*> tsec;
        for (auto& sp : ini.startingWith("turret_")) {
            int idx = atoi(sp->name.c_str() + 7);
            tsec[idx] = sp;
        }
        int maxIdx = tsec.empty() ? 0 : tsec.rbegin()->first;
        std::vector<TurretTpl> arr(maxIdx + 1);
        std::vector<bool> present(maxIdx + 1, false);
        for (auto& e : tsec) {
            int idx = e.first;
            TurretTpl tt;
            const IniSection& s = *e.second;
            tt.x = (float)s.getNum("x", 0);
            tt.y = (float)s.getNum("y", 0);
            tt.projectile = (int)s.getNum("projectile", 0);
            tt.invisible = s.getBool("invisible", false);
            tt.canShoot = s.getBool("canshoot", true);
            tt.turnSpeed = (float)s.getNum("turnspeed", t.turretTurnSpeed);
            tt.warmup = (float)s.getNum("warmup", 0);
            if (s.has("canattackflyingunits")) tt.canAttackFlying = s.getBool("canattackflyingunits", true); else tt.canAttackFlying = t.canAttackFlying;
            if (s.has("canattacklandunits")) tt.canAttackLand = s.getBool("canattacklandunits", true); else tt.canAttackLand = t.canAttackLand;
            tt.canAttackUnderwater = s.getBool("canattackunderwaterunits", false);
            // copyFrom another turret index
            if (s.has("copyfrom")) {
                int from = (int)s.getNum("copyfrom", 0);
                if (from >= 0 && from <= maxIdx && present[from]) tt = arr[from];
            }
            arr[idx] = tt; present[idx] = true;
        }
        for (size_t i = 1; i < arr.size(); ++i)
            if (present[i]) t.turrets.push_back(arr[i]);
    }

    // projectiles: gather by index
    {
        std::map<int, const IniSection*> psec;
        for (auto& sp : ini.startingWith("projectile_")) {
            int idx = atoi(sp->name.c_str() + 11);
            psec[idx] = sp;
        }
        int maxIdx = psec.empty() ? 0 : psec.rbegin()->first;
        std::vector<ProjectileTpl> arr(maxIdx + 1);
        std::vector<bool> present(maxIdx + 1, false);
        ProjectileTpl def; def.life = (int)t.attackRange;
        for (auto& e : psec) {
            int idx = e.first; const IniSection& s = *e.second;
            ProjectileTpl p = def;
            p.directDamage = (float)s.getNum("directdamage", 0);
            p.areaDamage = (float)s.getNum("areadamage", 0);
            p.areaRadius = (float)s.getNum("arearadius", 0);
            p.speed = (float)s.getNum("speed", 6);
            p.life = (int)s.getNum("life", 60);
            p.frame = (int)s.getNum("frame", 0);
            p.drawSize = (float)s.getNum("drawsize", 1);
            p.largeHit = s.getBool("largehiteffect", false);
            std::string exp = ToLower(s.get("explodeeffect", ""));
            p.explodeOnHit = p.areaDamage > 0 || p.areaRadius > 0;
            if (s.has("copyfrom")) {
                int from = (int)s.getNum("copyfrom", 0);
                if (from >= 0 && from <= maxIdx && present[from]) { ProjectileTpl b = arr[from]; p.directDamage = s.has("directdamage") ? p.directDamage : b.directDamage; arr[idx] = p; present[idx] = true; continue; }
            }
            arr[idx] = p; present[idx] = true;
        }
        for (size_t i = 1; i < arr.size(); ++i)
            if (present[i]) t.projectiles.push_back(arr[i]);
    }

    if (const IniSection* ai = ini.find("ai")) {
        t.useAsBuilder = ai->getBool("useasbuilder", false) || t.isBuilder;
        t.useAsAttacker = ai->getBool("useasattacker", false);
        t.useAsHarvester = ai->getBool("useasharvester", false) || t.creditIncomeRate > 0;
    }
    // If attack enabled but no turrets defined, synthesize a default turret using projectile_1
    if (t.canAttack && t.turrets.empty()) {
        TurretTpl tt; tt.projectile = t.projectiles.empty() ? 0 : 1;
        tt.canAttackFlying = t.canAttackFlying; tt.canAttackLand = t.canAttackLand;
        tt.turnSpeed = t.turretTurnSpeed;
        t.turrets.push_back(tt);
    }
    t.isFactory = t.isBuilding && !t.canBuild.empty();
    return t;
}

void TemplateStore::loadAll(const std::string& assetsRoot) {
    struct Dir { std::string sub; };
    // builtin_mods are optional mods, disabled by default in RWX (builtin_mods_enabled) - not loaded
    const char* roots[] = { "units" };
    std::vector<std::pair<std::string, std::string>> aliases; // aliasKey -> realKey
    for (const char* sub : roots) {
        std::string base = assetsRoot + "/" + sub;
        FilePathList fl = LoadDirectoryFilesEx(base.c_str(), nullptr, true);
        for (uint32_t i = 0; i < fl.count; ++i) {
            std::string path = fl.paths[i];
            if (!EndsWith(ToLower(path), ".ini")) continue;
            IniFile ini;
            if (!ParseIni(path, ini)) continue;
            if (!ini.find("core")) continue;
            applyCopyFrom(ini, path, 0);
            UnitTemplate t = buildTemplate(ini, path);
            if (t.name.empty()) continue;
            byName[ToLower(t.name)] = t;
            order.push_back(ToLower(t.name));
            if (!t.overrideName.empty()) {
                byName[ToLower(t.overrideName)] = t; // alias
                aliases.push_back({ ToLower(t.overrideName), ToLower(t.name) });
            }
        }
        UnloadDirectoryFiles(fl);
    }

    // reverse-link production graph: unit.builtFrom_X -> factory.canBuild
    auto hasOpt = [](const UnitTemplate& f, const std::string& lower) {
        for (auto& b : f.canBuild) if (ToLower(b.unitName) == lower) return true;
        return false;
    };
    for (auto& np : order) {
        auto nit = byName.find(np);
        if (nit == byName.end()) continue;
        std::vector<BuildOption>& bfs = nit->second.builtFrom;
        for (auto& bfe : bfs) {
            std::string bf = bfe.unitName;
            // comma list: "mechFactoryT2, landFactory"
            size_t start = 0;
            while (start <= bf.size()) {
                size_t c = bf.find(',', start);
                std::string one = Trim(bf.substr(start, c == std::string::npos ? std::string::npos : c - start));
                if (c == std::string::npos) start = bf.size() + 1; else start = c + 1;
                if (one.empty()) continue;
                std::string lo = ToLower(one);
                if (lo == "none" || lo == "ignore" || lo == np) continue;
                auto fit = byName.find(lo);
                if (fit == byName.end() || !fit->second.isBuilding) continue;
                if (hasOpt(fit->second, np)) continue;
                BuildOption b; b.unitName = nit->second.name; b.pos = (float)fit->second.canBuild.size() + 1;
                fit->second.canBuild.push_back(b);
                fit->second.isFactory = true;
            }
        }
    }
    // re-sync alias copies with (possibly extended) real templates
    for (auto& a : aliases) {
        auto rit = byName.find(a.second);
        if (rit != byName.end()) byName[a.first] = rit->second;
    }
}

TemplateStore g_templates;
