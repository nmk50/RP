// Template loader: scans assets for .ini unit definitions and builds UnitTemplate records.
#include "UnitTemplate.h"
#include "Ini.h"
#include <raylib.h>
#include <set>
#include <sstream>
#include <functional>

static std::vector<float> parseCSV(const std::string& s) {
    std::vector<float> out; std::stringstream ss(s); std::string tok;
    while (std::getline(ss, tok, ',')) { tok = Trim(tok); if (!tok.empty()) out.push_back((float)std::atof(tok.c_str())); }
    return out;
}

// RWX colour strings are Android style: #RGB, #RRGGBB or #AARRGGBB. Stored here as 0xRRGGBBAA.
static uint32_t parseRgba(const std::string& raw) {
    std::string h = Trim(raw);
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto get = [&](size_t i, size_t len) -> int {
        int v = 0;
        for (size_t j = 0; j < len; ++j) { int n = nib(h[i + j]); if (n < 0) return -1; v = v * 16 + n; }
        return len == 1 ? v * 17 : v;
    };
    auto cmp = [&](size_t i, size_t len) -> uint32_t { int v = get(i, len); return v < 0 ? 255u : (uint32_t)v; };
    if (h.size() == 3) return (cmp(0, 1) << 24) | (cmp(1, 1) << 16) | (cmp(2, 1) << 8) | 255;
    if (h.size() == 6) return (cmp(0, 2) << 24) | (cmp(2, 2) << 16) | (cmp(4, 2) << 8) | 255;
    if (h.size() == 8) return (cmp(2, 2) << 24) | (cmp(4, 2) << 16) | (cmp(6, 2) << 8) | cmp(0, 2);
    return 0xFFFFFFFF;
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

static std::vector<std::string> splitNames(const std::string& s) {
    std::vector<std::string> out; std::stringstream ss(s); std::string tok;
    while (std::getline(ss, tok, ',')) { tok = Trim(tok); if (!tok.empty()) out.push_back(tok); }
    return out;
}

// RWX time keys (shootDelay, warmup, wobbleFrequency, ...) take an optional 's' postfix which
// IniFile.parseUnitValue:761 turns into 60 ticks.
static float timeTicks(const std::string& key, const IniSection& s, float def) {
    std::string v = ToLower(Trim(s.get(key, "")));
    if (v.empty()) return def;
    float mult = 1.0f;
    if (v.back() == 's') { v.pop_back(); mult = TICK_RATE; }
    try { return std::stof(v) * mult; } catch (...) { return def; }
}

// RWX getInvertedTime (IniFile.java:756-778), used by buildSpeed and selfBuildRate: "60s" means the
// whole job takes 60s, so the per-tick progress is 1/(60*60); a bare number is already per tick.
static float perTickRate(const IniSection& s, const std::string& key, float def) {
    std::string v = ToLower(Trim(s.get(key, "")));
    if (v.empty()) return def;
    if (v.back() == 's') {
        float secs = (float)atof(v.substr(0, v.size() - 1).c_str());
        return secs > 0 ? 1.0f / (secs * TICK_RATE) : def;
    }
    return (float)atof(v.c_str());
}

// RWX [section] @copyFromSection : A, B — fill a section from the named template sections, keys it
// already has winning; each template is itself expanded, 10 levels at most
// (CustomUnitConfigParser.readAllCustomUnitConfigs:379-404).
static void expandFromSection(IniFile& ini, const std::string& dstName, const std::string& srcName, int depth) {
    if (depth > 10) return;
    IniSection* src = ini.find(srcName);
    IniSection* dst = ini.find(dstName);
    if (!src || !dst) return;
    std::vector<std::pair<std::string, std::string>> keys = src->kv;   // dst may be src
    for (auto& kv : keys) {
        if (ToLower(kv.first) == "@copyfromsection") continue;
        if (!dst->has(kv.first)) dst->set(kv.first, kv.second);
    }
    for (auto& next : splitNames(src->get("@copyfromsection")))
        expandFromSection(ini, dstName, next, depth + 1);
}

static void applyCopyFromSections(IniFile& ini) {
    std::vector<std::string> marked;
    for (auto& s : ini.sections) if (s.has("@copyfromsection")) marked.push_back(s.name);
    for (auto& n : marked)
        for (auto& src : splitNames(ini.find(n)->get("@copyfromsection")))
            expandFromSection(ini, n, src, 0);
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
        // CustomUnitConfigParser.java:1682-1688 - maxShield is read as an int with no scaling and
        // shieldRegen defaults to 0.25 per tick even for units that never get a bubble.
        t.maxShield = (int)c->getNum("maxshield", 0);
        t.shieldRegen = (float)c->getNum("shieldregen", 0.25);
        t.startShieldAtZero = c->getBool("startshieldatzero", false);
        t.shieldRenderRadius = (float)c->getNum("shieldrenderradius", t.radius);
        t.shieldFlashRate = (float)c->getNum("shielddeflectiondisplayrate", 4);
        t.shieldOnlyDeflectionFlash = c->getBool("shielddisplayonlydeflection", false);
        t.armour = (float)c->getNum("armour", 0);
        t.armourMinKeep = (float)c->getNum("armourmindamagetokeep", 1);
        t.techLevel = (int)c->getNum("techlevel", 1);
        t.buildSpeed = perTickRate(*c, "buildspeed", 0.001f);
        t.isBuilding = c->getBool("isbuilding", false);
        t.isBuilder = c->getBool("isbuilder", false);
        t.fogSight = (float)c->getNum("fogofwarsightrange", 0);
        t.energyUse = (float)c->getNum("energyusage", 0);
        t.explodeOnDeath = c->getBool("explodeondeath", true);
        t.hideScorchMark = c->getBool("hidescorchmark", false);
        auto fp = parseCSV(c->get("footprint", c->get("constructionfootprint", "0,0,0,0")));
        if (fp.size() >= 4) { t.footL = (int)fp[0]; t.footU = (int)fp[1]; t.footR = (int)fp[2]; t.footD = (int)fp[3]; }
        t.placeOnlyOnResPool = c->getBool("placeonlyonrespool", c->getBool("placeonlyonrespools", c->getBool("placeonlyonresource", false)));
        t.placeOnlyOnWater = c->getBool("placeonlyonwater", false);
        std::string gen = c->get("generation_resources");
        if (gen.empty()) gen = c->get("resourcegeneration");
        // "credits=8" -> 8
        size_t eq = gen.find('='); if (eq != std::string::npos) t.creditIncomeRate = (float)atof(gen.c_str() + eq + 1);
        if (t.creditIncomeRate == 0) t.creditIncomeRate = (float)c->getNum("generation_credits", 0);
        parseBuildLinks(*c, "canBuild", t.canBuild);
        parseBuildLinks(*c, "builtFrom", t.builtFrom);
    }
    // self actions: [action_upgradeT2] sections and core action_N_* keys (RWX upgrade form)
    {
        auto parseSpeed = [](const std::string& v, float def) -> float {
            std::string s = ToLower(Trim(v));
            if (!s.empty() && s.back() == 's') { float secs = (float)atof(s.c_str()); return secs > 0 ? 1.0f / (secs * TICK_RATE) : def; }
            try { return (float)std::stof(s); } catch (...) { return def; }
        };
        for (IniSection* s : ini.startingWith("action_")) {
            UnitAction a;
            a.convertTo = Trim(s->get("convertto"));
            if (a.convertTo.empty()) continue;
            a.price = (float)s->getNum("price", 0);
            a.buildSpeed = parseSpeed(s->get("buildspeed", ""), 0.001f);
            a.pos = (int)s->getNum("pos", 0);
            a.text = s->get("text", "");
            t.actions.push_back(a);
        }
        if (const IniSection* c2 = ini.find("core")) {
            std::map<int, UnitAction> byIdx;
            for (auto& kv : c2->kv) {
                std::string lk = ToLower(kv.first);
                if (lk.rfind("action_", 0) != 0) continue;
                size_t p2 = lk.find('_', 7);
                if (p2 == std::string::npos) continue;
                int idx = atoi(lk.substr(7, p2 - 7).c_str());
                if (idx <= 0) continue;
                std::string key = lk.substr(p2 + 1);
                UnitAction& a = byIdx[idx];
                if (key == "convertto") a.convertTo = Trim(kv.second);
                else if (key == "price") a.price = (float)atof(kv.second.c_str());
                else if (key == "buildspeed") a.buildSpeed = parseSpeed(kv.second, 0.001f);
                else if (key == "text") a.text = kv.second;
                else if (key == "pos") a.pos = (int)atoi(kv.second.c_str());
            }
            for (auto& kv : byIdx) if (!kv.second.convertTo.empty()) t.actions.push_back(kv.second);
        }
        for (auto& a : t.actions) {
            if (!a.text.empty() && a.text.rfind("i:", 0) != 0) continue; // literal label
            size_t n = 0; while (n < a.convertTo.size() && n < t.name.size() && a.convertTo[n] == t.name[n]) n++;
            std::string rest = a.convertTo.substr(n);
            a.text = rest.empty() ? "Upgrade" : ("Upgrade " + rest);
        }
        std::sort(t.actions.begin(), t.actions.end(), [](const UnitAction& x, const UnitAction& y) { return x.pos < y.pos; });
    }
    if (const IniSection* g = ini.find("graphics")) {
        t.image = g->get("image");
        t.imageWreak = g->get("image_wreak");
        t.imageTurret = g->get("image_turret");
        t.imageScale = (float)g->getNum("imagescale", g->getNum("scaleimagesto_scale", 1));
        t.turretImageScale = (float)g->getNum("turretimagescale", 1);
        t.imageShadow = g->get("image_shadow");
        t.imageShield = g->get("image_shield");
        t.showShieldBar = g->getBool("showshieldbar", true);
        t.shadowOffX = (float)g->getNum("shadowoffsetx", 0);
        t.shadowOffY = (float)g->getNum("shadowoffsety", 0);
        // :1583-1584, and the shadow key's default is the body key's value. These two really are
        // spelled with underscores in the mod ini's, unlike most [graphics] keys.
        t.lockBodyRot = g->getBool("lock_body_rotation_with_main_turret", false);
        t.lockShadowRot = g->getBool("lock_shadow_rotation_with_main_turret", t.lockBodyRot);
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
        t.turretRotateWithBody = a->getBool("turretrotatewithbody", true);
        t.attackRange = (float)a->getNum("maxattackrange", 0);
        // CustomUnitConfigParser.java:2042 getTime("attack","shootDelay", 50.0f)
        t.shootDelay = timeTicks("shootdelay", *a, 50);
        t.turretSize = (float)a->getNum("turretsize", 0);
        // :2015 turretTurnSpeed default 8.0 deg/tick; per-turret turnSpeed overrides it
        // (CustomUnit.java:2297-2303), which Templates.cpp does when the [turret] section has one.
        t.turretTurnSpeed = (float)a->getNum("turretturnspeed", 8);
    }
    if (const IniSection* m = ini.find("movement")) {
        t.moveType = parseMoveType(m->get("movementtype", "NONE"));
        // :1959-1961, :2008-2009 every numeric [movement] default is 1.0f
        t.moveSpeed = (float)m->getNum("movespeed", 1);
        t.accel = (float)m->getNum("moveaccelerationspeed", 1);
        t.decel = (float)m->getNum("movedecelerationspeed", 1);
        t.maxTurnSpeed = (float)m->getNum("maxturnspeed", 1);
        t.turnAccel = (float)m->getNum("turnacceleration", 1);
        t.reverseSpeed = (float)m->getNum("reversespeedpercentage", 0.6);
        // :2010-2011, both plain booleans defaulting to false
        t.moveSlidingMode = m->getBool("moveslidingmode", false);
        t.moveIgnoringBody = m->getBool("moveignoringbody", false);
    }
    if (t.isBuilding) t.moveType = MoveType::NONE;

    // nano (builder) params
    if (const IniSection* c = ini.find("core")) {
        t.nanoRange = (float)c->getNum("nanorange", 0);
        // :1778 nanoBuildSpeed defaults to 1.0
        t.nanoBuildSpeed = (float)c->getNum("nanobuildspeed", 1);
        // :1774 nanoRepairSpeed defaults to 0.2 hp/tick for every unit
        t.nanoRepairSpeed = (float)c->getNum("nanorepairspeed", 0.2);
        // unset nanoRange (-1 in RWX) falls through CustomUnit.y():1641-1646 to BaseUnit.y():1895
        // which is 85
        if (t.isBuilder && t.nanoRange == 0) t.nanoRange = 85;
        t.canRepairBuildings = c->getBool("canrepairbuildings", false);
        t.canRepairUnits = c->getBool("canrepairunits", false);
        t.isUnrepairableUnit = c->getBool("isunrepairableunit", false);
        // :1760-1763 nanoRangeForRepair; -1/unset means the repair reaches as far as nanoRange
        t.nanoRangeForRepair = (float)c->getNum("nanorangeforrepair", 0);
        t.selfBuildRate = perTickRate(*c, "selfbuildrate", 0);
        t.dieOnConstruct = c->getBool("dieonconstruct", false);
        // :1780 selfRegenRate is a plain getFloat, not an inverted time: the value is HP per tick
        t.selfRegenRate = (float)c->getNum("selfregenrate", 0);
        // RWX mods often omit isBuilder on nano units (e.g. megaBuilder). nanoBuildSpeed now defaults
        // to 1.0 like RWX (:1778), so only a declared key counts as evidence here.
        if (!t.isBuilder && !t.isBuilding && (t.nanoRange > 0 || c->has("nanobuildspeed"))) t.isBuilder = true;
    }

    // [projectile_<name>] is keyed by whatever follows the underscore, case-insensitively, and the
    // numbering a turret could otherwise refer to is simply the order the sections appear in the file
    // (CustomUnitConfigParser.java:2058-2092). Parsing them before the turrets keeps the name lookup
    // complete by the time a turret asks for `projectile: <name>`.
    std::map<std::string, int> projIdx;
    {
        for (auto* sp : ini.startingWith("projectile_")) {
            const IniSection& s = *sp;
            std::string key = ToLower(Trim(s.name.substr(11)));
            if (key.empty() || projIdx.count(key)) continue;   // RWX rejects a repeated name outright
            projIdx[key] = (int)t.projectiles.size();
            ProjectileTpl p;
            p.directDamage = (float)s.getNum("directdamage", 0);
            p.areaDamage = (float)s.getNum("areadamage", 0);
            p.areaRadius = (float)s.getNum("arearadius", 0);
            p.speed = (float)s.getNum("speed", 6);
            p.life = (int)s.getNum("life", 60);
            p.frame = (int)s.getNum("frame", -1);
            p.drawType = (int)s.getNum("drawtype", 0);
            p.shadowFrame = (int)s.getNum("shadowframe", -1);
            p.drawSize = (float)s.getNum("drawsize", 1);
            p.color = parseRgba(s.get("color", "#FFFFFF"));
            // CustomProjectileTemplate.java:142-152 reads trailEffect as a string, so only a literal
            // "true" arms the built-in puff; "false" and a custom spawn list both leave it off (RWX
            // lets a spawn list emit its own effects, which RP does not run).
            p.trail = ToLower(Trim(s.get("traileffect", ""))) == "true";
            p.trailRate = (float)s.getNum("traileffectrate", 3.0);
            // :158-160 pre-sets the glow to ARGB 0xFFEEEE00 for trailed shots, and :169 then reads
            // lightColor with that as its default, so an explicit key still wins.
            p.lightColor = p.trail ? 0xEEEE00FFu : 0u;
            std::string lc = Trim(s.get("lightcolor", ""));
            if (!lc.empty() && lc != "-1") p.lightColor = parseRgba(lc);
            p.lightSize = (float)s.getNum("lightsize", 0.5);
            p.largeHit = s.getBool("largehiteffect", false);
            p.turnSpeed = (float)s.getNum("turnspeed", -1);
            p.targetSpeed = (float)s.getNum("targetspeed", -1);
            p.targetSpeedAccel = (float)s.getNum("targetspeedacceleration", 0.1);
            p.speedSpread = (float)s.getNum("speedspread", 0);
            p.instant = s.getBool("instant", false);
            p.wobbleAmp = (float)s.getNum("wobbleamplitude", 0);
            p.wobbleFreq = timeTicks("wobblefrequency", s, 5);
            // CustomProjectileTemplate.java:194-196 with ProjectileTemplate.java:109-111 defaults. RWX
            // throws above 1500 (CustomProjectileTemplate.java:202-203); clamping keeps a bad mod loading.
            p.autoRetarget = s.getBool("autotargetingondeadtarget", false);
            p.autoRetargetRange = std::min(1500.0f, (float)s.getNum("autotargetingondeadtargetrange", 120));
            p.autoRetargetLead = (float)s.getNum("autotargetingondeadtargetlead", 15);
            // CustomProjectileTemplate.java:235, and the key really is areaExpandTime -- reading the
            // misspelling here left 7 assets' growing splash ring at the default -1.
            p.areaExpandTime = (float)s.getNum("areaexpandtime", p.areaExpandTime);
            p.buildingDamageMult = (float)s.getNum("buildingdamagemultiplier", 1);
            p.shieldDamageMult = (float)s.getNum("shielddamagemultiplier", 1);
            // CustomProjectileTemplate.java:232 - the deflection key is spelled wrong in the parser, so
            // that is the only spelling RWX honours; accept the corrected one as a fallback.
            p.shieldDeflectMult = (float)s.getNum("shielddefectionmultiplier",
                s.getNum("shielddeflectionmultiplier", 1));
            p.hullDamageMult = (float)s.getNum("hulldamagemultiplier", 1);
            p.armourIgnore = (float)s.getNum("armourignoreamount", 0);
            t.projectiles.push_back(p);
        }
        if (t.projectiles.empty()) {
            // RWX hands a unit with no [projectile_*] a stand-in named "1" carrying directDamage 10
            projIdx["1"] = 0;
            ProjectileTpl p; p.directDamage = 10;
            t.projectiles.push_back(p);
        }
    }

    // [turret_<name>]: same suffix keying. `copyFrom: <name>` starts from another turret's values and
    // this section's own keys are then applied on top (TurretConfig.java:310-333) - which is also why
    // x/y alone are always read from the section itself and never inherited.
    {
        std::vector<const IniSection*> tsec;
        std::map<std::string, int> turIdx;
        for (auto* sp : ini.startingWith("turret_")) {
            std::string key = ToLower(Trim(sp->name.substr(7)));
            if (key.empty() || turIdx.count(key)) continue;
            turIdx[key] = (int)tsec.size();
            tsec.push_back(sp);
        }
        std::vector<TurretTpl> arr(tsec.size());
        std::vector<char> done(tsec.size(), 0), visiting(tsec.size(), 0);
        std::function<void(int)> parseTurret = [&](int i) {
            if (done[i] || visiting[i]) return;
            visiting[i] = 1;
            const IniSection& s = *tsec[i];
            TurretTpl tt;
            bool copied = false;
            if (s.has("copyfrom")) {
                auto it = turIdx.find(ToLower(Trim(s.get("copyfrom"))));
                if (it != turIdx.end() && it->second != i) {
                    parseTurret(it->second);            // RWX resolves a forward reference too
                    if (done[it->second]) { tt = arr[it->second]; copied = true; }
                }
            }
            if (!copied) {                             // [attack] defaults, not inherited by a copy
                tt.turnSpeed = t.turretTurnSpeed;
                tt.canAttackFlying = t.canAttackFlying;
                tt.canAttackLand = t.canAttackLand;
            }
            tt.x = (float)s.getNum("x", 0);
            tt.y = (float)s.getNum("y", 0);
            // size is an alias of barrelY; the -1 sentinel means "[attack] turretSize", see fire()
            if (s.has("barrely") || s.has("size"))
                tt.barrelY = (float)s.getNum("barrely", s.getNum("size", -1));
            if (s.has("barrelx")) tt.barrelX = (float)s.getNum("barrelx", 0);
            if (s.has("barreloffsetx_onoddshots")) tt.barrelXOdd = (float)s.getNum("barreloffsetx_onoddshots", 0);
            if (s.has("image")) tt.image = Trim(s.get("image", ""));
            if (s.has("image_drawoffsetx")) tt.drawOffsetX = (float)s.getNum("image_drawoffsetx", 0);
            if (s.has("image_drawoffsety")) tt.drawOffsetY = (float)s.getNum("image_drawoffsety", 0);
            if (s.has("projectile")) {
                auto it = projIdx.find(ToLower(Trim(s.get("projectile"))));
                if (it != projIdx.end()) tt.projectile = it->second;
            }
            if (s.has("invisible")) tt.invisible = s.getBool("invisible", false);
            if (s.has("canshoot")) tt.canShoot = s.getBool("canshoot", true);
            if (s.has("turnspeed")) tt.turnSpeed = (float)s.getNum("turnspeed", tt.turnSpeed);
            if (s.has("turnspeedacceleration")) tt.turnAccel = (float)s.getNum("turnspeedacceleration", tt.turnAccel);
            if (s.has("turnspeeddeceleration")) tt.turnDecel = (float)s.getNum("turnspeeddeceleration", tt.turnDecel);
            // TurretConfig.m: `delay` is this barrel's own shoot delay, and linkDelayWithTurret hands
            // the job over to the parent barrel (RWX writes 9000 in that case).
            if (s.has("delay")) tt.shootDelay = timeTicks("delay", s, tt.shootDelay);
            if (s.has("warmup")) tt.warmup = timeTicks("warmup", s, tt.warmup);
            if (s.has("warmupcalldownrate")) tt.warmCallDown = (float)s.getNum("warmupcalldownrate", tt.warmCallDown);
            if (s.has("warmupnoreset")) tt.warmNoReset = s.getBool("warmupnoreset", false);
            // 99999 is RWX's "no limit" sentinel (TurretConfig.java:484-496), not a real range.
            if (s.has("limitingrange")) {
                float lr = (float)s.getNum("limitingrange", 0);
                if (lr > 0 && lr < 99999.0f) tt.range = lr;
            }
            if (s.has("limitingminrange")) tt.minRange = (float)s.getNum("limitingminrange", tt.minRange);
            if (s.has("canattackmaxangle")) tt.maxAngle = (float)s.getNum("canattackmaxangle", tt.maxAngle);
            if (s.has("recoiloffset")) tt.recoil = (float)s.getNum("recoiloffset", tt.recoil);
            if (s.has("recoilouttime")) tt.recoilOut = (float)s.getNum("recoilouttime", tt.recoilOut);
            if (s.has("recoilreturntime")) tt.recoilReturn = timeTicks("recoilreturntime", s, tt.recoilReturn);
            if (s.has("attachedto")) {
                auto it = turIdx.find(ToLower(Trim(s.get("attachedto"))));
                if (it != turIdx.end() && it->second != i) tt.attached = it->second;
            }
            if (s.has("linkdelaywithturret")) {
                auto it = turIdx.find(ToLower(Trim(s.get("linkdelaywithturret"))));
                if (it != turIdx.end() && it->second != i) tt.linkDelay = it->second;
            }
            if (s.has("idledir")) tt.idleDir = (float)s.getNum("idledir", 0);
            if (s.has("shouldresetturret")) tt.shouldResetTurret = s.getBool("shouldresetturret", true);
            if (s.has("canattackflyingunits")) tt.canAttackFlying = s.getBool("canattackflyingunits", true);
            if (s.has("canattacklandunits")) tt.canAttackLand = s.getBool("canattacklandunits", true);
            if (s.has("canattackunderwaterunits")) tt.canAttackUnderwater = s.getBool("canattackunderwaterunits", false);
            // RWX shoot_flame accepts a comma list; only the built-in flame sizes are rendered here
            if (s.has("shoot_flame")) {
                std::string fl = ToLower(s.get("shoot_flame", ""));
                size_t comma = fl.find(','); if (comma != std::string::npos) fl = Trim(fl.substr(0, comma));
                tt.shootFlame = fl == "small" ? 1 : fl == "medium" ? 2 : fl == "large" ? 3 : 0;
            }
            arr[i] = tt;
            visiting[i] = 0;
            done[i] = 1;
        };
        for (size_t i = 0; i < tsec.size(); ++i) parseTurret((int)i);
        for (size_t i = 0; i < tsec.size(); ++i) if (done[i]) t.turrets.push_back(arr[i]);
    }

    if (const IniSection* ai = ini.find("ai")) {
        t.useAsBuilder = ai->getBool("useasbuilder", false) || t.isBuilder;
        t.useAsAttacker = ai->getBool("useasattacker", false);
        t.useAsHarvester = ai->getBool("useasharvester", false) || t.creditIncomeRate > 0;
    }
    // If attack enabled but no turrets defined, synthesize RWX's fallback turret ("1", first projectile)
    if (t.canAttack && t.turrets.empty()) {
        TurretTpl tt;
        tt.canAttackFlying = t.canAttackFlying; tt.canAttackLand = t.canAttackLand;
        tt.turnSpeed = t.turretTurnSpeed;
        t.turrets.push_back(tt);
    }
    // RWX CustomUnitConfigParser:2142-2193 - when every shooting barrel sets its own limitingRange,
    // [attack] maxAttackRange is raised to the largest of them so the unit's target search still
    // reaches the long barrels; each barrel then enforces its own range on top of that.
    {
        float widest = 0; bool any = false, all = true;
        for (auto& tt : t.turrets) if (tt.canShoot) {
            if (tt.range > 0) { any = true; if (tt.range > widest) widest = tt.range; }
            else all = false;
        }
        if (any && all && widest > t.attackRange) t.attackRange = widest;
    }
    // Fill in the values RWX only applies once a section has been read (TurretConfig.java:344-351,
    // 484-501): a barrel without its own `delay` inherits [attack] shootDelay, an attachedTo barrel
    // never runs its own timer, and limitingRange falls back to the unit's maxAttackRange.
    for (size_t i = 0; i < t.turrets.size(); ++i) {
        TurretTpl& tt = t.turrets[i];
        if (tt.attached >= (int)t.turrets.size()) tt.attached = -1;
        if (tt.linkDelay >= (int)t.turrets.size()) tt.linkDelay = -1;
        if (tt.linkDelay >= 0) tt.shootDelay = 9000;
        else if (tt.shootDelay < 0) tt.shootDelay = t.shootDelay;
        if (tt.range <= 0) tt.range = t.attackRange;
        if (tt.turnDecel < 0) tt.turnDecel = tt.turnAccel;
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
            applyCopyFromSections(ini);
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
