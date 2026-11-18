// Template loader: scans assets for .ini unit definitions and builds UnitTemplate records.
#include "UnitTemplate.h"
#include "Ini.h"
#include "Variables.h"
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

// EffectManager.effectTemplates[], the twenty built-in sheets a stripIndex picks out (loadContent:667-868).
// The six numbers are each SpriteSheet's b, c, d, e, f, g -- cell size, origin and step -- left at the
// class's own defaults of 25, 25, 1, 1, 26, 26 wherever loadContent does not write them, which is why
// lava_bubble starts at 1,1 while flame starts at 0,0. `whole` is SpriteSheet.k: light_50 is the only sheet
// drawn as a single image rather than a cell, and h (columns) stays at Integer.MAX_VALUE for all twenty, so
// none of them wraps to a second row.
const FxSheetSpec kFxSheets[20] = {
    { "effects", "effects.png", 25, 25, 1, 1, 26, 26, false },
    { "explode_big", "explode_big.png", 39, 40, 1, 1, 40, 41, false },
    { "light_50", "light_50.png", 25, 25, 1, 1, 26, 26, true },
    { "flame", "flame.png", 20, 25, 0, 0, 20, 25, false },
    { "dust", "dust.png", 20, 25, 0, 0, 20, 25, false },
    { "smoke_black", "smoke_black.png", 50, 40, 0, 0, 50, 40, false },
    { "shockwave", "shockwave.png", 50, 50, 0, 0, 50, 50, false },
    { "fire", "fire.png", 20, 20, 0, 0, 20, 20, false },
    { "lava_bubble", "lava_bubble.png", 20, 30, 1, 1, 22, 30, false },
    { "effects2", "effects2.png", 28, 28, 0, 0, 29, 29, false },
    { "plasma_shot", "plasma_shot.png", 20, 25, 0, 0, 20, 25, false },
    { "shockwave_large", "shockwave_large.png", 104, 104, 0, 0, 104, 104, false },
    { "explode_bits", "explode_bits.png", 20, 20, 0, 0, 20, 20, false },
    { "explode_big2", "explode_big2.png", 39, 40, 1, 1, 40, 41, false },
    { "explode_bits_bug", "explode_bits_bug.png", 20, 20, 0, 0, 20, 20, false },
    { "projectiles", "projectiles.png", 20, 20, 0, 0, 20, 20, false },
    { "projectiles2", "projectiles2.png", 20, 20, 0, 0, 20, 20, false },
    { "effects3", "effects3.png", 30, 30, 0, 0, 31, 31, false },
    { "smoke_white", "smoke_white.png", 50, 40, 0, 0, 50, 40, false },
    { "shockwave2", "shockwave2.png", 56, 56, 0, 0, 56, 56, false },
};

// findEffectTemplateIndex:889-901: the sheet name compared case-insensitively, or the sheet's number written
// out as a string, and a missing key arrives as the empty string plus "0", which is to say the number 0.
int fxStripIndex(const std::string& key) {
    std::string k = ToLower(Trim(key));
    for (int i = 0; i < 20; ++i) {
        if (k == kFxSheets[i].name) return i;
        if (std::to_string(i) == Trim(key)) return i;
    }
    return -1;
}

static MoveType parseMoveType(const std::string& s) {
    std::string v = ToLower(Trim(s));
    if (v == "land") return MoveType::LAND;
    if (v == "air") return MoveType::AIR;
    if (v == "water") return MoveType::WATER;
    if (v == "hover") return MoveType::HOVER;
    if (v == "over_cliff") return MoveType::OVER_CLIFF;
    if (v == "over_cliff_water") return MoveType::OVER_CLIFF_WATER;
    return MoveType::NONE; // BUILDING / NONE
}

// [core] transportUnitsCanUnloadUnits is a logic-boolean *string* (CustomUnitConfigParser.java:1837-1842).
// RWX runs it through its hook VM; RP only ever meets the shapes the shipped ini files use -- the bare
// `false`, `if self.isOverLiquid()`, `if not self.isOverLiquid()`, and the built-in default
// `if not self.isOverLiquid() and not self.isMoving()` -- so it recognises an `and`-joined list of those
// two predicates, each optionally negated, and drops anything it does not know instead of guessing at it.
// A comma is read as `and`, and no shipped condition uses `or`.
static void parseCondAtoms(const std::string& raw, UnloadCond& out);
static UnloadCond parseUnloadCond(const std::string& raw) {
    UnloadCond out;
    std::string s = ToLower(Trim(raw));
    if (s.rfind("if ", 0) == 0) s = Trim(s.substr(3));
    if (s.empty()) {
        out.tests = { { CondPred::overLiquid, true }, { CondPred::moving, true } };
        return out;
    }
    if (s == "false") { out.never = true; return out; }
    if (s == "true") return out;
    parseCondAtoms(s, out);
    return out;
}

// The `and`-joined atom list itself, shared with [turret_N] idleSweepCondition, whose default is null
// (always true) rather than the unload pair -- so the caller owns the empty/true/false handling.
static void parseCondAtoms(const std::string& raw, UnloadCond& out) {
    std::string s = raw;
    size_t comma = s.find(',');
    if (comma != std::string::npos) s.replace(comma, 1, " and ");
    size_t at = 0;
    while (at <= s.size()) {
        size_t nxt = s.find(" and ", at);
        std::string part = Trim(s.substr(at, nxt == std::string::npos ? std::string::npos : nxt - at));
        at = (nxt == std::string::npos) ? s.size() + 1 : nxt + 5;
        if (part.empty()) continue;
        bool neg = part.rfind("not ", 0) == 0 || part.find(" not ") != std::string::npos;
        if (part.find("isoverliquid") != std::string::npos) out.tests.push_back({ CondPred::overLiquid, neg });
        else if (part.find("overwater") != std::string::npos) out.tests.push_back({ CondPred::overWater, neg });
        else if (part.find("ismoving") != std::string::npos) out.tests.push_back({ CondPred::moving, neg });
        // an atom RP does not know is not evaluated, so it cannot fail either
    }
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
        // parseCanBuildEntry:2601 splits the option on commas, so `canBuild_1_name : lightSub, gunBoat`
        // is two options that share the entry's pos, not one unfindable unit called "lightSub, gunBoat".
        size_t start = 0;
        while (start <= n.second.size()) {
            size_t c = n.second.find(',', start);
            std::string one = Trim(n.second.substr(start, c == std::string::npos ? std::string::npos : c - start));
            if (c == std::string::npos) start = n.second.size() + 1; else start = c + 1;
            if (one.empty()) continue;
            // :2602 an option spelled NONE is a placeholder, not a unit -- it never becomes an action.
            if (ToLower(one) == "none") continue;
            BuildOption b; b.unitName = one; b.pos = poss.count(n.first) ? poss[n.first] : (float)n.first;
            out.push_back(b);
        }
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

// AnimationTag.a():33-51 reads a comma list of tag names: every name is trimmed, interned by c():64-79 --
// which lowercases it -- and kept once, the separator being VariableScope.nullOrMissingString, a plain
// empty string, so a list with no names in it is read back as no tags at all.
static std::vector<std::string> parseTags(const std::string& raw) {
    std::vector<std::string> out;
    for (auto& t : splitNames(raw)) {
        std::string s = ToLower(t);
        bool dup = false;
        for (auto& o : out) if (o == s) { dup = true; break; }
        if (!dup && !s.empty()) out.push_back(s);
    }
    return out;
}

// UnitPrice.b():165-233 -- the resourceUsage grammar. Split on comma or pipe (:172), a token with no '='
// or ':' is credits (:176-178), otherwise key=val. Flag keys run through a():134-155 which ORs 1<<id over
// an optional lo-hi range into a mask. Unknown keys are named resources kept for the cost store (:207-216).
static ResourceCost parseResourceCost(const std::string& raw) {
    ResourceCost rc;
    std::string s = Trim(raw);
    if (s.empty()) return rc;
    for (auto& c : s) if (c == '|') c = ',';
    auto num = [](const std::string& v) { return (float)atof(v.c_str()); };
    auto flagMask = [&](const std::string& v) -> uint64_t {
        uint64_t m = 0;
        size_t dash = v.find('-');
        int lo = (int)num(v), hi = dash == std::string::npos ? lo : (int)num(v.substr(dash + 1));
        for (int f = lo; f <= hi && f >= 0 && f <= 31; ++f) m |= (1ULL << f);
        return m;
    };
    for (auto& part : splitNames(s)) {
        size_t eq = part.find_first_of("=:");
        std::string key, val;
        if (eq == std::string::npos) { key = "credits"; val = part; }
        else { key = ToLower(Trim(part.substr(0, eq))); val = Trim(part.substr(eq + 1)); }
        if (key == "credits") rc.credits = num(val);
        else if (key == "energy") rc.energy = num(val);
        else if (key == "hp" || key == "health") rc.hp = num(val);
        else if (key == "shield") rc.shield = num(val);
        else if (key == "ammo") rc.ammo = num(val);
        else if (key == "hasflag") rc.hasFlag |= flagMask(val);
        else if (key == "hasmissingflag") rc.hasMissingFlag |= flagMask(val);
        else if (key == "setflag") rc.setFlag |= flagMask(val);
        else if (key == "unsetflag") rc.unsetFlag |= flagMask(val);
        else if (!key.empty()) rc.custom.emplace_back(key, num(val));
    }
    return rc;
}

// UnitSpawnList.b():26-113 parses the projectile spawn lists -- "name, name*3(key = value, key = value)".
// The whole string is split on every comma first (:38), which is why RWX cannot read a parameter list that
// contains one; the shipped data never needs to, every entry it writes carrying a single key. "NONE" and an
// empty string both come back as an empty list (:33), which is how a derived section cancels the list it
// inherited (heavy_sub.ini:127 and nautilus.ini:416 both spell it that way). An unknown parameter name is a
// ConfigParseException in RWX (:104); RP drops it and loads the mod.
std::vector<ProjectileTpl::SpawnEntry> parseSpawnList(const std::string& raw) {
    std::vector<ProjectileTpl::SpawnEntry> out;
    // :31-34 -- the whole value is answered before any of it is split: null, the empty string RWX calls
    // nullOrMissingString, and a bare "NONE" in any spelling all come back as a non-null *empty* list. That
    // is the only way a derived section can cancel the list it took from its @copyFromSection, and both
    // shipped chains do exactly that (heavy_sub.ini:127, nautilus.ini:416).
    std::string whole = Trim(raw);
    if (whole.empty() || ToLower(whole) == "none") return out;
    auto num = [](const std::string& v, float def) {
        try { return std::stof(v); } catch (...) { return def; }
    };
    for (auto& tok : splitNames(raw)) {
        ProjectileTpl::SpawnEntry e;
        std::string head = tok, params;
        size_t open = head.find('(');
        if (open != std::string::npos && head.find(')') != std::string::npos) {
            params = head.substr(open + 1, head.find(')') - open - 1);
            head = head.substr(0, open);
        }
        head = Trim(head);
        size_t star = head.find('*');
        if (star != std::string::npos) {
            int c = num(head.substr(star + 1), 1);
            // UnitSpawnList:52-56 hands the number straight to the loop counter of UnitSpawnList.a():154, so
            // a written 0 is a real entry that simply never fires rather than a default one; only garbage
            // (which is a NumberFormatException in RWX) falls back to 1.
            e.count = c > 0 ? c : 0;
            head = head.substr(0, star);
        }
        e.name = Trim(head);
        for (auto& piece : splitNames(params)) {
            size_t eq = piece.find('=');
            if (eq == std::string::npos) continue;
            std::string k = ToLower(Trim(piece.substr(0, eq)));
            std::string v = Trim(piece.substr(eq + 1));
            if (k == "spawnchance") e.chance = num(v, e.chance);
            else if (k == "maxspawnlimit") e.maxPerCall = (int)num(v, (float)e.maxPerCall);
            else if (k == "recursionlimit") e.recursionLimit = (int)num(v, (float)e.recursionLimit);
            else if (k == "offsetx" || k == "xoffsetabsolute") e.offsetX = num(v, e.offsetX);
            else if (k == "offsety" || k == "yoffsetabsolute") e.offsetY = num(v, e.offsetY);
            else if (k == "xoffsetrelative") e.relX = num(v, e.relX);
            else if (k == "yoffsetrelative") e.relY = num(v, e.relY);
            else if (k == "offsetrandomxy") { float f = num(v, 0); e.rndX = f; e.rndY = f; }
            else if (k == "offsetrandomx") e.rndX = num(v, e.rndX);
            else if (k == "offsetrandomy") e.rndY = num(v, e.rndY);
            else if (k == "offsetheight") e.offsetZ = num(v, e.offsetZ);
            else if (k == "offsetrandomdir") e.rndDir = num(v, e.rndDir);
            else if (k == "offsetdir") e.offsetDir = num(v, e.offsetDir);
        }
        out.push_back(e);
    }
    return out;
}

// UnitSpawner.b():47-171 is the second of RWX's two spawn-list readers, and the one [projectile_N]
// spawnUnit goes through (CustomProjectileTemplate.java:244-246). Its shape is the same as
// parseSpawnList's -- comma split, `name*count`, then one single-key parameter list -- but the keys are
// SpawnConfig's (:88-137) rather than UnitSpawnEntry's, and it also refuses "Too many units" for the
// callers that pass z=true. spawnUnit is not one of them, so a projectile entry may name several units.
// The handful of keys RP has nowhere to put is listed where they fall through, below.
std::vector<ProjectileTpl::SpawnUnit> parseUnitSpawnList(const std::string& raw) {
    std::vector<ProjectileTpl::SpawnUnit> out;
    // :50-53 -- null, nullOrMissingString and "NONE" in any spelling all answer with an empty list, which
    // is how a @copyFromSection child cancels a parent's spawner.
    std::string whole = Trim(raw);
    if (whole.empty() || ToLower(whole) == "none") return out;
    auto num = [](const std::string& v, float def) {
        try { return std::stof(v); } catch (...) { return def; }
    };
    auto boolean = [](const std::string& v, bool def) {
        std::string t = ToLower(Trim(v));
        if (t.empty()) return def;
        return t == "true" || t == "1" || t == "yes";   // IniFile.parseBoolean, as IniSection::getBool
    };
    for (auto& tok : splitNames(raw)) {
        ProjectileTpl::SpawnUnit e;
        std::string head = tok, params;
        size_t open = head.find('(');
        if (open != std::string::npos && head.find(')') != std::string::npos) {
            params = head.substr(open + 1, head.find(')') - open - 1);
            head = head.substr(0, open);
        }
        head = Trim(head);
        size_t star = head.find('*');
        if (star != std::string::npos) {
            int c = num(head.substr(star + 1), 1);
            e.count = c > 0 ? c : 0;      // :71-77 hands the number to the loop counter at :302
            head = head.substr(0, star);
        }
        e.name = Trim(head);
        for (auto& piece : splitNames(params)) {
            size_t eq = piece.find('=');
            if (eq == std::string::npos) continue;
            std::string k = ToLower(Trim(piece.substr(0, eq)));
            std::string v = Trim(piece.substr(eq + 1));
            if (k == "addresources") e.addResources = v;
            else if (k == "spawnchance") e.chance = num(v, e.chance);
            else if (k == "maxspawnlimit") e.maxPerCall = (int)num(v, (float)e.maxPerCall);
            else if (k == "offsetx") e.offsetX = num(v, e.offsetX);
            else if (k == "offsety") e.offsetY = num(v, e.offsetY);
            else if (k == "offsetheight") e.offsetZ = num(v, e.offsetZ);
            else if (k == "offsetdir") e.offsetDir = num(v, e.offsetDir);
            else if (k == "offsetrandomxy") { float f = num(v, 0); e.rndX = f; e.rndY = f; }
            else if (k == "offsetrandomx") e.rndX = num(v, e.rndX);
            else if (k == "offsetrandomy") e.rndY = num(v, e.rndY);
            else if (k == "offsetrandomdir") e.rndDir = num(v, e.rndDir);
            else if (k == "neutralteam") e.neutralTeam = boolean(v, e.neutralTeam);
            else if (k == "aggressiveteam") e.aggressiveTeam = boolean(v, e.aggressiveTeam);
            else if (k == "settoteamoflastattacker") e.toLastAttackerTeam = boolean(v, e.toLastAttackerTeam);
            else if (k == "skipifoverlapping") e.skipIfOverlapping = boolean(v, e.skipIfOverlapping);
            else if (k == "alwaysstartdiratzero" || k == "alwaystartdiratzero")
                e.startDirAtZero = boolean(v, e.startDirAtZero);   // :116-121, both spellings
            // :102 techLevel, :104 gridAlign, :108 falling, :110 transportedUnitsToTransfer,
            // :136-146 addResources and the two logic-boolean blocks spawnSource and copyWaypointsFrom
            // are the keys RP has nowhere to put; they fall through with the unknown ones.
        }
        out.push_back(e);
    }
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

// EffectTemplate.a(customUnitConfig, iniFile, section):371-531, the [effect_NAME] parser. RWX gives every key
// the field it lands in as its default, which is the constructor's own value (:105-143), so RP only writes a
// key the ini actually states. The members RP has nowhere to put -- createWhenOffscreen / createWhenZoomedOut
// / createWhenOverLiquid / createWhenOverLand (:373-379), showInFog, priority, blendMode, shadow and
// imageShadow, alsoEmitEffects(OnDeath), trailEffect(Rate), ifSpawnFailsEmitEffects and alsoPlaySound -- are
// left out, and of the 38 sections the shipped assets carry only the three displacement ones use drawType,
// which RP folds onto layer 3 and leaves undrawn rather than faking a shader.
static void parseEffectSections(IniFile& ini, std::vector<EffectTpl>& out) {
    for (IniSection* sp : ini.startingWith("effect_")) {
        const IniSection& s = *sp;
        EffectTpl e;
        e.name = Trim(sp->name.substr(7));
        auto f = [&](const char* k, float def) { return s.has(k) ? (float)s.getNum(k, def) : def; };
        auto bl = [&](const char* k, bool def) { return s.has(k) ? s.getBool(k, def) : def; };
        auto in = [&](const char* k, int def) { return s.has(k) ? (int)s.getNum(k, def) : def; };
        e.spawnChance = f("spawnchance", e.spawnChance);                       // :380
        e.life = f("life", e.life);                                            // :381
        e.lifeRandom = f("liferandom", e.lifeRandom);                           // :382
        e.oxRel = f("xoffsetrelative", e.oxRel);                               // :384
        e.oyRel = f("yoffsetrelative", e.oyRel);                                // :385
        e.hOffset = f("hoffset", e.hOffset);                                    // :386
        // :387 getBooleanFromTwoKeys tries the correct spelling first and the typo second, so either works
        // and an ini that states both still wins with the first one.
        if (s.has("alwaysstartdiratzero") || s.has("alwaystartdiratzero"))
            e.startDirAtZero = s.getBool("alwaysstartdiratzero", s.getBool("alwaystartdiratzero", false));
        e.pivotOffset = f("pivotoffset", e.pivotOffset);                        // :388
        e.pivotOffsetRandom = f("pivotoffsetrandom", e.pivotOffsetRandom);      // :389
        e.dirOffset = f("diroffset", e.dirOffset);                              // :390
        e.oxRelRnd = f("xoffsetrelativerandom", e.oxRelRnd);                    // :391
        e.oyRelRnd = f("yoffsetrelativerandom", e.oyRelRnd);                    // :392
        e.hOffsetRandom = f("hoffsetrandom", e.hOffsetRandom);                  // :393
        e.dirOffsetRandom = f("diroffsetrandom", e.dirOffsetRandom);             // :394
        e.oxAbs = f("xoffsetabsolute", e.oxAbs);                                // :395
        e.oyAbs = f("yoffsetabsolute", e.oyAbs);                                // :396
        e.oxAbsRnd = f("xoffsetabsoluterandom", e.oxAbsRnd);                    // :397
        e.oyAbsRnd = f("yoffsetabsoluterandom", e.oyAbsRnd);                    // :398
        e.xRel = f("xspeedrelative", e.xRel);                                   // :399
        e.yRel = f("yspeedrelative", e.yRel);                                    // :400
        e.hSpeed = f("hspeed", e.hSpeed);                                       // :401
        e.dirSpeed = f("dirspeed", e.dirSpeed);                                  // :402
        e.xRelRnd = f("xspeedrelativerandom", e.xRelRnd);                       // :403
        e.yRelRnd = f("yspeedrelativerandom", e.yRelRnd);                       // :404
        e.hSpeedRandom = f("hspeedrandom", e.hSpeedRandom);                     // :405
        e.dirSpeedRandom = f("dirspeedrandom", e.dirSpeedRandom);               // :406
        e.xAbs = f("xspeedabsolute", e.xAbs);                                    // :407
        e.yAbs = f("yspeedabsolute", e.yAbs);                                     // :408
        e.xAbsRnd = f("xspeedabsoluterandom", e.xAbsRnd);                       // :409
        e.yAbsRnd = f("yspeedabsoluterandom", e.yAbsRnd);                       // :410
        e.scaleTo = f("scaleto", e.scaleTo);                                    // :411
        e.scaleFrom = f("scalefrom", e.scaleFrom);                              // :412
        // :413-416 scaleXFrom/To and scaleYFrom/To default *downward* off the two above: they are only ever
        // written by a mod that wants an asymmetric effect, and no shipped section does.
        e.alpha = f("alpha", e.alpha);                                          // :417
        if (s.has("color")) e.color = parseRgba(s.get("color", ""));            // :418
        e.teamColorRatio = f("teamcolorratio", e.teamColorRatio);               // :423
        // :446-457 ar starts at 2, drawUnderUnits drops it to 1 and drawType: displacement lifts it to 3 --
        // displacement last, so an ini that sets both ends up on layer 3 in RWX too.
        e.layer = bl("drawunderunits", false) ? 1 : 2;
        std::string dt = Trim(s.get("drawtype", ""));
        if (!dt.empty() && ToLower(dt) != "normal") e.layer = 3;
        e.fadeIn = f("fadeintime", e.fadeIn);                                   // :458
        e.fadeOut = bl("fadeout", e.fadeOut);                                   // :459
        e.delay = timeTicks("delayedstarttimer", s, e.delay);                   // :460, a time key
        e.delayRandom = f("delayedstarttimerrandom", e.delayRandom);            // :461
        e.frame = in("frameindex", e.frame);                                    // :462
        e.frameRandom = in("frameindexrandom", e.frameRandom);                  // :463
        // :464-468 a name or a number, and RP keeps RWX's own default string "0" for a missing key.
        int st = fxStripIndex(s.get("stripindex", "0"));
        if (st >= 0) e.strip = st;
        e.attached = bl("attachedtounit", e.attached);                          // :469
        e.liveAfterAttachedDies = bl("liveafterattacheddies", e.liveAfterAttachedDies);  // :470
        e.atmospheric = bl("atmospheric", e.atmospheric);                       // :471
        e.physics = bl("physics", e.physics);                                    // :472
        e.physicsGravity = f("physicsgravity", e.physicsGravity);               // :473
        e.totalFrames = std::max(1, in("total_frames", 1));                     // :482-485
        e.image = Trim(s.get("image", ""));                                     // :486
        e.frameW = in("frame_width", 0);                                        // :492
        e.frameH = in("frame_height", 0);                                       // :493
        e.animStart = in("animateframestart", e.animStart);                     // :515
        e.animStartRandom = in("animateframestartrandomadd", e.animStartRandom); // :516
        e.animEnd = in("animateframeend", e.animEnd);                           // :517
        e.animPingPong = bl("animateframepingpong", e.animPingPong);            // :518
        e.animLooping = bl("animateframelooping", e.animLooping);               // :519
        e.animSpeed = timeTicks("animateframespeed", s, e.animSpeed);           // :520, a time key
        e.animSpeedRandom = timeTicks("animateframespeedrandom", s, e.animSpeedRandom);
        // :436-445 imageAnchor is a string that picks a number; anything else is a parse error RWX would
        // throw, so an unknown word is left at the default rather than guessed at.
        std::string an = ToLower(Trim(s.get("imageanchor", "")));
        if (an == "top") e.imageAnchorY = 0.0f;
        else if (an == "bottom") e.imageAnchorY = 1.0f;
        else if (!an.empty()) e.imageAnchorY = 0.5f;
        out.push_back(e);
    }
}

static int actionCheckRate(const std::string& text) {
    std::string s = ToLower(Trim(text));
    return s == "every8frames" ? 8 : s == "every4frames" ? 4 : 1;
}

static void parseRuntimeSections(IniFile& ini, UnitTemplate& t) {
    if (const IniSection* c = ini.find("core")) {
        t.tags = splitNames(ToLower(c->get("tags")));
        t.unselectable = c->getBool("isunselectable", false);
        t.untargetable = c->getBool("isunselectableastarget", t.unselectable);
        t.cannotBeAttacked = c->getBool("cannotbedirectlyattacked", false);
        t.disableDeathOnZeroHp = c->getBool("disabledeathonzerohp", false);
        t.autoCooldown = timeTicks("autotriggercooldowntime", *c, 60);
        t.autoCheckRate = actionCheckRate(c->get("autotriggercheckrate"));
    }
    for (auto& sec : ini.sections) {
        std::string sn = ToLower(sec.name);
        if (sn.rfind("attachment_", 0) == 0) {
            AttachmentTpl a;
            a.name = sn.substr(11);
            a.x = (float)sec.getNum("x", 0); a.y = (float)sec.getNum("y", 0);
            a.height = (float)sec.getNum("height", 0);
            a.idleDir = (float)sec.getNum("idledir", 0);
            a.reverseDir = (float)sec.getNum("idledirreversing", a.idleDir);
            a.spawn = parseUnitSpawnList(sec.get("oncreatespawnunitof"));
            a.showActions = sec.get("showallactionsfrom");
            a.rotateWithParent = sec.getBool("rotatewithparent", true);
            a.lockRotation = sec.getBool("lockrotation", false);
            a.resetRotation = sec.getBool("resetrotationwhennotattacking", false);
            a.lockLegs = sec.getBool("locklegmovement", true);
            a.freezeLegs = sec.getBool("freezelegmovement", false);
            a.damageable = sec.getBool("canbeattackedanddamaged", false);
            a.unselectable = sec.getBool("isunselectable", true);
            a.untargetable = sec.getBool("isunselectableastarget", a.unselectable);
            a.visible = sec.getBool("isvisible", true);
            a.hidden = sec.getBool("hidden", false);
            a.showMiniHp = sec.getBool("showminihp", true);
            a.hideHp = sec.getBool("hidehp", false);
            a.redirectDamage = sec.getBool("redirectdamagetoparent", false);
            a.shieldOnly = sec.getBool("redirectdamagetoparent_shieldonly", false);
            a.keepAlive = sec.getBool("keepalivewhenparentdies", false);
            a.incompleteWithParent = sec.getBool("createincompleteifparentis", true);
            a.keepOnConvert = sec.getBool("onconvertkeepexistingunitinsameslot", false);
            a.keepTeam = sec.getBool("onparentteamchangekeepcurrentteam", false);
            a.onBottom = sec.getBool("setdrawlayeronbottom", false);
            a.onTop = sec.getBool("setdrawlayerontop", !a.onBottom);
            a.addTransported = sec.getBool("addtransportedunits", false);
            a.canAttack = sec.getBool("canattack", true);
            a.prioritizeTarget = sec.getBool("prioritizeparentsmaintarget", true);
            a.onlyParentTarget = sec.getBool("onlyattackparentsmaintarget", false);
            a.alwaysParentTarget = sec.getBool("alwaysallowedtoattackparentsmaintarget", false);
            t.attachments.push_back(a);
        }
        if (sn.rfind("canbuild_", 0) == 0) {
            // `reclaim` is the built-in ReclaimTargetAction, which RP already shows for canReclaimResources.
            for (auto& name : splitNames(sec.get("name")))
                if (ToLower(name) != "none" && ToLower(name) != "reclaim")
                    t.canBuild.push_back({name, (float)sec.getNum("pos", 1), sec.get("isvisible"), sec.getBool("forcenano", false)});
        }
    }
    // hiddenAction_X first so a derived file's action_X (nautilusLand's trigger_onLand) overrides the keys
    // it inherited instead of becoming a second action with the same name.
    std::vector<IniSection> actions;
    std::vector<bool> hiddenFlags;
    auto addAction = [&](const IniSection& sec, const std::string& name, bool hidden) {
        for (size_t i = 0; i < actions.size(); ++i) {
            if (ToLower(actions[i].name) != name) continue;
            for (auto& kv : sec.kv) actions[i].set(kv.first, kv.second);
            hiddenFlags[i] = hiddenFlags[i] || hidden;
            return;
        }
        actions.push_back(sec);
        actions.back().name = name;
        hiddenFlags.push_back(hidden);
    };
    for (const auto& sec : ini.sections) {
        std::string sn = ToLower(sec.name);
        if (sn.rfind("hiddenaction_", 0) == 0) addAction(sec, sn.substr(13), true);
    }
    for (const auto& sec : ini.sections) {
        std::string sn = ToLower(sec.name);
        if (sn.rfind("action_", 0) == 0) addAction(sec, sn.substr(7), false);
    }
    if (const IniSection* c = ini.find("core")) {
        std::map<std::string, IniSection> legacy;
        for (auto& kv : c->kv) {
            std::string key = ToLower(kv.first);
            if (key.rfind("action_", 0) != 0) continue;
            size_t end = key.find('_', 7);
            if (end == std::string::npos) continue;
            auto& sec = legacy[key.substr(7, end - 7)];
            sec.set(key.substr(end + 1), kv.second);
        }
        for (auto& entry : legacy) addAction(entry.second, entry.first, false);
    }
    for (size_t ai = 0; ai < actions.size(); ++ai) {
        const IniSection& s = actions[ai];
        UnitAction a;
        a.hidden = hiddenFlags[ai];
        a.name = s.name;
        a.convertTo = Trim(s.get("convertto"));
        a.text = s.get("text"); a.textUnit = s.get("textaddunitname"); a.textPostfix = s.get("textpostfix");
        // price is a number or a resource list, `credits=250, energy=1`; only credits is a resource RP has
        std::string price = ToLower(Trim(s.get("price")));
        if (price.find('=') == std::string::npos) a.price = (float)atof(price.c_str());
        else for (auto& part : splitNames(price)) {
            size_t eq = part.find('=');
            std::string k = Trim(part.substr(0, eq));
            if (k == "credits") a.price = (float)atof(part.c_str() + eq + 1);
            else a.supported = false;
        }
        a.buildSpeed = perTickRate(s, "buildspeed", 0.001f);
        if (Trim(s.get("buildspeed")) == "0s" || a.buildSpeed == 0) a.buildSpeed = 1.0e9f;
        a.pos = (float)s.getNum("pos", 0);
        a.visible = s.get("isvisible"); a.active = s.get("isactive");
        a.require = s.get("requireconditional"); a.autoTrigger = s.get("autotrigger");
        a.checkRate = s.has("autotriggercheckrate") ? actionCheckRate(s.get("autotriggercheckrate")) : t.autoCheckRate;
        {
            // autoTrigger is a LogicBoolean in RWX; RP reads the same predicate atoms as the unload
            // condition. `true`/absent leaves the test list empty (always fires), `false` never fires.
            std::string gate = ToLower(Trim(a.autoTrigger));
            if (gate.rfind("if ", 0) == 0) gate = Trim(gate.substr(3));
            if (gate == "false") a.autoCond.never = true;
            else if (!gate.empty() && gate != "true") parseCondAtoms(gate, a.autoCond);
        }
        a.events = ToLower(s.get("autotriggeronevent")); a.tags = ToLower(s.get("tags"));
        a.addResources = s.get("addresources"); a.setStats = s.get("setunitstats");
        if (ToLower(a.addResources).find("ammo") != std::string::npos) a.supported = false;
        a.setBuilt = (float)s.getNum("setbuilt", -1);
        a.addEnergy = (float)s.getNum("addenergy", 0);
        a.triggerActions = ToLower(s.get("alsotriggeraction")); a.queueActions = ToLower(s.get("alsoqueueaction"));
        a.triggerCondition = s.get("alsotriggerorqueueactionconditional");
        a.messageTo = s.get("sendmessageto"); a.messageTags = ToLower(s.get("sendmessagewithtags"));
        a.cooldown = timeTicks("addactioncooldowntime", s, 0);
        a.allCooldown = timeTicks(s.has("addallactionscooldowntime") ? "addallactionscooldowntime" : "addallactioncooldownstime", s, 0);
        a.cannotMove = s.getBool("whenbuilding_cannotmove", false);
        a.highPriority = s.getBool("highpriorityqueue", false);
        a.buildAnimation = ToLower(s.get("whenbuilding_playanimation"));
        a.playAnimation = ToLower(s.get("playanimation"));
        int missing = 0;
        parseFlameList(s.get("spawneffects"), t.effects, nullptr, a.effects, missing);
        if (s.has("takeresources")) {
            std::string take = ToLower(Trim(s.get("takeresources")));
            if (s.getBool("takeresources_includeparent", false) && s.getBool("takeresources_discardcollected", false) && take.rfind("shield=", 0) == 0)
                a.parentShield = -(float)atof(take.c_str() + 7);
            else a.supported = false;
        }
        for (const char* key : {"fireturretxatground", "teleportto", "sendmessagewithdata", "alsotriggerorqueueactionwithtarget",
                                "spawnunits", "attachments_addnewunits", "deleteself", "resourceamount", "setresourceswithlogic"})
            if (s.has(key)) a.supported = false;
        if (a.text.empty() || a.text.rfind("i:", 0) == 0) {
            size_t n = 0;
            while (n < a.convertTo.size() && n < t.name.size() && a.convertTo[n] == t.name[n]) ++n;
            a.text = a.convertTo.empty() ? a.name : "Upgrade " + a.convertTo.substr(n);
        }
        t.actions.push_back(a);
    }
    std::stable_sort(t.actions.begin(), t.actions.end(), [](const UnitAction& a, const UnitAction& b) { return a.pos < b.pos; });
}

// RWX LegConfig.a(LegConfig, config, ini, section, isLeg, list):104-251 plus the neighbour table
// CustomUnitConfigParser.b():3355-3386. Legs and arms share one parser; an arm only differs in its
// two starting defaults (:105-108). Empty sections keep a placeholder slot because copyFrom indexes
// the list including them (:109-115).
static void parseLegSections(IniFile& ini, UnitTemplate& t) {
    std::vector<LegTpl> list[2];
    for (int kind = 0; kind < 2; ++kind) {
        for (int i = 1; i <= 20; ++i) {
            IniSection* sp = ini.find((kind == 0 ? "leg_" : "arm_") + std::to_string(i));
            LegTpl L;
            if (!sp || sp->kv.empty()) { list[kind].push_back(L); continue; }
            const IniSection& s = *sp;
            L.placeholder = false;
            L.isLeg = kind == 0;
            if (!L.isLeg) L.lockMovement = true;
            int cf = (int)s.getNum("copyfrom", 0);
            if (cf > 0 && cf - 1 < (int)list[kind].size() && !list[kind][cf - 1].placeholder)
                L = list[kind][cf - 1];
            L.name = (kind == 0 ? "leg" : "arm") + std::to_string(i);
            L.hidden = s.getBool("hidden", false);
            L.x = (float)s.getNum("x", 0);
            L.y = (float)s.getNum("y", 0);
            if (s.has("attach_x")) L.attachX = (float)s.getNum("attach_x", 0);
            if (s.has("attach_y")) L.attachY = (float)s.getNum("attach_y", 0);
            L.liftHeight = (float)s.getNum("liftingheightoffset", L.liftHeight);
            L.targetHeight = (float)s.getNum("targetheight", L.targetHeight);
            L.targetHeightRelative = s.getBool("targetheightrelative", L.targetHeightRelative);
            L.endDirOffset = (float)s.getNum("enddiroffset", 0);
            L.lockMovement = s.getBool("lockmovement", L.lockMovement);
            L.estimatingMultiplier = (float)s.getNum("estimatingpositionmultiplier", L.estimatingMultiplier);
            L.alpha = (float)s.getNum("alpha", L.alpha);
            if (s.has("movespeed")) L.moveSpeed = (float)s.getNum("movespeed", 1);
            L.moveWarmUp = timeTicks("movewarmup", s, L.moveWarmUp);
            L.rotateSpeed = (float)s.getNum("rotatespeed", L.rotateSpeed);
            L.resetAngle = (float)s.getNum("resetangle", L.resetAngle);
            bool footColors = s.getBool("image_end_teamcolors", false);
            std::string img = Trim(s.get("image_foot", ""));
            if (!img.empty()) { L.imageFoot = img; L.footTeamColors = footColors; }
            img = Trim(s.get("image_end", ""));
            if (!img.empty()) { L.imageFoot = img; L.footTeamColors = footColors; }
            img = Trim(s.get("image_foot_shadow", ""));
            if (!img.empty()) L.imageShadow = img;
            img = Trim(s.get("image_end_shadow", ""));
            if (!img.empty()) L.imageShadow = img;
            img = Trim(s.get("image_leg", ""));
            if (!img.empty()) L.imageLeg = img;
            img = Trim(s.get("image_middle", ""));
            if (!img.empty()) L.imageLeg = img;
            L.legTeamColors = s.getBool("image_middle_teamcolors", false);
            if (s.has("heightspeed")) L.heightSpeed = (float)s.getNum("heightspeed", 0.3f);
            L.drawFootOnTop = s.getBool("draw_foot_on_top", L.drawFootOnTop);
            L.dust = s.getBool("dust_effect", L.dust);
            if (s.has("drawlegwhenzoomedout")) { L.drawLegZoomedOut = s.getBool("drawlegwhenzoomedout", true); L.explicitZoom = true; }
            if (s.has("drawfootwhenzoomedout")) { L.drawFootZoomedOut = s.getBool("drawfootwhenzoomedout", true); L.explicitZoom = true; }
            // RWX applies the radius rule before reading this section's drawOverBody.
            if (!L.explicitZoom && !L.lockMovement && !L.drawOverBody) {
                if (t.radius < 30) L.drawLegZoomedOut = false;
                if (t.radius < 20) L.drawFootZoomedOut = false;
            }
            L.holdDisMin = (float)s.getNum("holddismin", L.holdDisMin);
            L.maxMovingLegs = (int)s.getNum("holddismin_maxmovinglegs", L.maxMovingLegs);
            L.holdMoveOnlyIfFurthest = s.getBool("hold_moveonlyiffurthest", L.holdMoveOnlyIfFurthest);
            L.checkNeighbours = s.getBool("holddismin_checkneighbours", L.checkNeighbours);
            L.favourOpposite = s.getBool("favouroppositesideneighbours", L.favourOpposite);
            L.holdDisMax = (float)s.getNum("holddismax", L.holdDisMax);
            L.hardLimit = (float)s.getNum("hardlimit", L.hardLimit);
            L.drawOverBody = s.getBool("drawoverbody", L.drawOverBody);
            L.drawUnderAllUnits = s.getBool("drawunderallunits", L.drawUnderAllUnits);
            L.drawDirOffset = (float)s.getNum("drawdiroffset", L.drawDirOffset);
            L.spinRate = (float)s.getNum("spinrate", L.spinRate);
            list[kind].push_back(L);
        }
    }
    for (int kind = 0; kind < 2; ++kind)
        for (auto& L : list[kind]) if (!L.placeholder) t.legs.push_back(L);
    // :3355-3386 -- the nearest other mobile leg sets the radius (+2 px); every mobile leg inside it
    // is a neighbour, and a stepping neighbour raises this leg's lift threshold to holdDisMax.
    for (size_t i = 0; i < t.legs.size(); ++i) {
        LegTpl& L = t.legs[i];
        float sx = L.favourOpposite ? 0.1f : 1.0f;
        float best = 0; bool any = false;
        for (size_t j = 0; j < t.legs.size(); ++j) {
            if (j == i || t.legs[j].lockMovement) continue;
            float dx = (L.x - t.legs[j].x) * sx, dy = L.y - t.legs[j].y;
            float d = dx * dx + dy * dy;
            if (!any || d < best) { best = d; any = true; }
        }
        if (!any) continue;
        float r = std::sqrt(best) + 2.0f;
        for (size_t j = 0; j < t.legs.size(); ++j) {
            if (j == i || t.legs[j].lockMovement) continue;
            float dx = (L.x - t.legs[j].x) * sx, dy = L.y - t.legs[j].y;
            if (dx * dx + dy * dy <= r * r) L.neighbours.push_back((int)j);
        }
    }
}

// The three flavours EffectTemplate.a():156-206 makes itself instead of filling a template in. Their numbers
// come from the creators quoted in UnitTemplate.h, and the two attached ones are lifted onto layer 2 by :202
// whatever the creator asked for. The smoke frame pick is written as frameIndex 1 +- 1 because RWX's
// frameIndexRandom draws getRandomIntInRange(-r, r) inclusively (:445-449 of Utility) and createMuzzleFlash
// wants the inclusive 0..2 -- the same three frames by the same roll.
EffectTpl muzzleEffect(int kind) {
    EffectTpl e;
    // One entry per built-in name EffectTemplate.a():156-207 dispatches to. Whatever the creator returns
    // non-null is then forced onto layer 2 and attached to the GameObject the list was handed (:202-205),
    // which for a shoot_flame list is the shooter and for an explodeEffect list is the hull that was hit.
    if (kind >= 1 && kind <= 3) {
        // createFlameEffectInternal:261-288 -- small walks frames 0..3 at 0.5/tick, medium and large 1..4 at
        // 0.4, and medium is the one EffectTemplate.a():161-164 then shrinks to 0.75. r=false: the flame
        // ends when its animation does, not on a fade.
        e.name = kind == 1 ? "small" : kind == 2 ? "medium" : "large";
        e.strip = 3;
        e.life = 20.0f;
        e.fadeOut = false;
        e.animStart = kind == 1 ? 0 : 1;
        e.animEnd = kind == 1 ? 3 : 4;
        e.animSpeed = kind == 1 ? 0.5f : 0.4f;
        if (kind == 2) { e.scaleFrom = 0.75f; e.scaleTo = 0.75f; }
        e.liveAfterAttachedDies = false;
        return e;
    }
    if (kind == 8) {
        // createSmallExplosion:437-444 is createSmallExplosionInternal with the sheet swapped to 11
        // (shockwave_large), so it is the shockwave's numbers on a bigger ring.
        e.name = "smallExplosion";
        e.strip = 11;
        e.life = 30.0f;
        e.scaleFrom = 0.2f; e.scaleTo = 1.3f;
        e.startDirAtZero = true;
        e.liveAfterAttachedDies = false;
        return e;
    }
    e.name = kind == 4 ? "smoke" : kind == 5 ? "shockwave" : "light";
    e.strip = kind == 4 ? 4 : kind == 5 ? 6 : 2;
    e.life = kind == 6 ? 10.0f : 30.0f;
    if (kind == 4) { e.scaleFrom = 0.8f; e.scaleTo = 2.3f; e.frame = 1; e.frameRandom = 1; e.yRel = 0.15f; }
    if (kind == 5) { e.scaleFrom = 0.2f; e.scaleTo = 1.3f; }
    if (kind == 6) { e.alpha = 0.5f; }
    // :251-255: only the two creators that never write Effect.Y leave the angle at 0. The flame and the smoke
    // both take the muzzle direction straight into Y.
    e.startDirAtZero = (kind == 5 || kind == 6);
    e.attached = (kind != 6);
    // A built-in effect keeps EffectTemplate.defaultEffectTemplate as its template and that constructor never
    // assigns liveAfterAttachedDies, so the attached pair dies the tick its hull does.
    e.liveAfterAttachedDies = false;
    return e;
}

// CustomUnitSpawnList.c():36-59 with CustomUnitConfig.resolveEffect:1175-1239 behind it. The string is split
// on commas, each item is trimmed and then split on '*' so "name*3" becomes the very same EffectTemplate
// three times over; CUSTOM: or CUSTOM| searches the unit's own sections alone and BUILTIN: or BUILTIN| skips
// that search, and a bare name tries the sections first. RWX answers anything it cannot resolve with a
// ConfigParseException that drops the whole unit, so RP counts those in `missing` for the harness instead of
// drawing something it was never told.
void parseFlameList(const std::string& raw, const std::vector<EffectTpl>& effects,
                    int* shootFlame, std::vector<FlameSpec>& flames, int& missing) {
    if (shootFlame) *shootFlame = 0;
    flames.clear();
    std::string s = Trim(raw);
    if (s.empty()) return;                 // a missing key is null, and "" is nullOrMissingString: no list
    if (ToLower(s) == "none") return;      // :42-44 noneSpawnItems -- a list that exists and spawns nothing
    size_t start = 0;
    while (start <= s.size()) {
        size_t c = s.find(',', start);
        std::string item = Trim(s.substr(start, c == std::string::npos ? std::string::npos : c - start));
        if (c == std::string::npos) start = s.size() + 1; else start = c + 1;
        if (item.empty()) continue;
        // :47 `trim().split("\\*")`, so the name keeps whatever spacing the star left it -- RWX trims the
        // item, never the piece, and the shipped lists are all written tight.
        size_t star = item.find('*');
        std::string name = star == std::string::npos ? item : item.substr(0, star);
        int reps = 1;
        if (star != std::string::npos) {
            reps = atoi(Trim(item.substr(star + 1)).c_str());
            if (reps < 1) reps = 1;
        }
        bool customOnly = false, builtinOnly = false;
        std::string low = ToLower(name), key = name;
        auto cut = [&](size_t n) { key = Trim(name.substr(n)); };
        // :1177-1189 the four prefixes are matched case-insensitively on the whole item, and each of the two
        // spellings costs exactly as many characters as the other -- "CUSTOM|" is 7 and "BUILTIN|" is 8.
        if (low.rfind("custom:", 0) == 0 || low.rfind("custom|", 0) == 0) { cut(7); customOnly = true; }
        else if (low.rfind("builtin:", 0) == 0 || low.rfind("builtin|", 0) == 0) { cut(8); builtinOnly = true; }

        FlameSpec fs;
        int found = -1;
        if (!builtinOnly) {
            std::string want = ToLower(key);
            for (size_t i = 0; i < effects.size(); ++i)
                if (ToLower(effects[i].name) == want) { found = (int)i; break; }
        }
        if (customOnly) {
            if (found < 0) { missing++; continue; }    // :1192-1196 "Failed to find custom effect"
            fs.tpl = found;
        } else if (found >= 0) {
            fs.tpl = found;
        } else if (key.find(':') != std::string::npos || key.find('|') != std::string::npos) {
            missing++;                                  // :1198-1204 "Unknown effect format"
            continue;
        } else {
            // :1206-1232 the built-in table. resourcePoolSmoke (:176-191) is left out: it drives
            // EffectEmitter's resource-pool plumes rather than one Effect, and no shipped list names it.
            std::string lk = ToLower(key);
            if (lk == "small") fs.builtIn = 1;
            else if (lk == "medium") fs.builtIn = 2;
            else if (lk == "large") fs.builtIn = 3;
            else if (lk == "smoke") fs.builtIn = 4;
            else if (lk == "shockwave") fs.builtIn = 5;
            else if (lk == "largeexplosion") fs.builtIn = 7;
            else if (lk == "smallexplosion") fs.builtIn = 8;
            else if (lk == "none") fs.builtIn = 9;
            else { missing++; continue; }               // :1233 "Failed to find built-in or custom effect"
        }
        for (int i = 0; i < reps; ++i) {
            // TurretState has one flame slot, so the first built-in flame size in the list is the one it
            // plays; a second (no shipped list holds one) rides along in `flames` as a builtIn 1..3 entry.
            // A projectile has no such slot and keeps every entry.
            if (shootFlame && fs.builtIn >= 1 && fs.builtIn <= 3 && *shootFlame == 0) {
                *shootFlame = fs.builtIn;
                continue;
            }
            flames.push_back(fs);
        }
    }
}

// A keyframe time written into the key itself, as the new-style animation sections do
// (`body_0.8s : {frame:3}`). IniFile.parseUnitValue:761-783 charges a trailing 's' 60 ticks and has no
// other unit -- "ms" fails to parse there, so it fails here too. -1 marks a key that is not a time.
static float keyTicks(const std::string& raw) {
    std::string v = ToLower(Trim(raw));
    float mult = 1.0f;
    if (!v.empty() && v.back() == 's') { v.pop_back(); mult = TICK_RATE; }
    try { return std::stof(v) * mult; } catch (...) { return -1.0f; }
}

// Both animation syntaxes compile to tick-based body and limb tracks.
static void parseBodyAnim(const IniSection& s, const std::string& prefix, BodyAnim& out, const UnitTemplate& owner) {
    out = BodyAnim{};
    int start = (int)s.getNum(prefix + "start", 0);            // :104, default frame 0
    int end = (int)s.getNum(prefix + "end", -1);               // :105, -1 = no frame strip
    float speed = (float)s.getNum(prefix + "speed", 40);       // :117, 40 ticks per frame
    float kts = (float)s.getNum(prefix + "keyframetimescale", 1);   // :134
    out.pingPong = s.getBool(prefix + "pingpong", false);
    out.blendIn = perTickRate(s, prefix + "blendin", 0);
    out.blendOut = perTickRate(s, prefix + "blendout", 0);
    out.queuedUnitPlayAt = (float)s.getNum(prefix + "onactionsqueuedunitplayat", 0);
    float span = speed;                                        // :132 f2
    if (end != -1 && end >= start) {
        span = speed * (float)(end - start + 1);               // :139 the strip's whole length
        out.frame.push_back({ 0, (float)start });              // :140
        out.frame.push_back({ span * kts, (float)end + 0.99f });  // :141, +0.99 so the last frame shows
    }
    float ds = (float)s.getNum(prefix + "scale_start", 1);     // :118
    float de = (float)s.getNum(prefix + "scale_end", 1);       // :119
    if (ds != 1.0f || de != 1.0f) {
        out.scale.push_back({ 0, ds });                        // :144
        out.scale.push_back({ span * kts, de });               // :145
    }
    for (auto& kv : s.kv) {
        std::string k = ToLower(kv.first);
        if (k.rfind(prefix, 0) != 0) continue;
        k = k.substr(prefix.size());
        size_t sep = k.find('_');
        if (sep == std::string::npos) continue;
        std::string target = k.substr(0, sep);
        int leg = -1;
        if (target != "body") {
            for (size_t i = 0; i < owner.legs.size(); ++i)
                if (owner.legs[i].name == target) { leg = (int)i; break; }
            if (leg < 0) continue;
        }
        float time = keyTicks(k.substr(sep + 1)) * kts;
        if (time < 0.0f) continue;
        std::string val = Trim(kv.second);
        if (val.size() < 2 || val.front() != '{' || val.back() != '}') continue;
        for (auto& part : splitNames(val.substr(1, val.size() - 2))) {
            size_t c = part.find(':');
            if (c == std::string::npos) continue;
            std::string what = ToLower(Trim(part.substr(0, c)));
            float v = (float)std::atof(Trim(part.substr(c + 1)).c_str());
            std::vector<AnimKey>* keys = nullptr;
            if (target == "body") {
                if (what == "frame") keys = &out.frame;
                else if (what == "scale") keys = &out.scale;
            } else {
                LegAnimProperty prop;
                if (what == "x") prop = LegAnimProperty::X;
                else if (what == "y") prop = LegAnimProperty::Y;
                else if (what == "dir") prop = LegAnimProperty::DIRECTION;
                else if (what == "height") prop = LegAnimProperty::HEIGHT;
                else if (what == "alpha") prop = LegAnimProperty::ALPHA;
                else continue;
                for (auto& track : out.legs)
                    if (track.index == leg && track.property == prop) { keys = &track.keys; break; }
                if (!keys) {
                    out.legs.push_back({ leg, prop, {} });
                    keys = &out.legs.back().keys;
                }
            }
            if (keys) keys->push_back({ time, v });
        }
    }
    auto finish = [&](std::vector<AnimKey>& keys) {
        if (keys.empty()) return;
        std::stable_sort(keys.begin(), keys.end(), [](const AnimKey& a, const AnimKey& b) { return a.t < b.t; });
        if (keys.front().t > 0 && keys.front().v != 0) keys.insert(keys.begin(), { 0, 0 });
        out.maxTime = std::max(out.maxTime, keys.back().t);
    };
    finish(out.frame); finish(out.scale);
    for (auto& track : out.legs) finish(track.keys);
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
        // [core] softCollisionOnAll, :1721 -- a plain int that is only ever used as a divisor.
        t.softCollisionOnAll = (int)c->getNum("softcollisiononall", 0);
        // CustomUnitConfigParser.java:1682-1688 - maxShield is read as an int with no scaling and
        // shieldRegen defaults to 0.25 per tick even for units that never get a bubble.
        t.maxShield = (int)c->getNum("maxshield", 0);
        t.shieldRegen = (float)c->getNum("shieldregen", 0.25);
        t.startShieldAtZero = c->getBool("startshieldatzero", false);
        t.shieldRenderRadius = (float)c->getNum("shieldrenderradius", t.radius);
        t.shieldFlashRate = (float)c->getNum("shielddeflectiondisplayrate", 4);
        t.shieldOnlyDeflectionFlash = c->getBool("shielddisplayonlydeflection", false);
        // :1689-1694 the energy pool, and :1783 dieOnZeroEnergy. energyRegenWhenRecharging inherits
        // energyRegen when the mod leaves it out, and a unit with no energyMax never runs the regen.
        t.maxEnergy = (float)c->getNum("energymax", 0);
        t.energyRegen = (float)c->getNum("energyregen", 0);
        t.startEnergyAtZero = c->getBool("startenergyatzero", false);
        t.energyStartingPercentage = (float)c->getNum("energystartingpercentage", 1);
        t.energyNeedsToRechargeToFull = c->getBool("energyneedstorechargetofull", false);
        t.energyRegenWhenRecharging = (float)c->getNum("energyregenwhenrecharging", t.energyRegen);
        t.dieOnZeroEnergy = c->getBool("dieonzeroenergy", false);
        t.armour = (float)c->getNum("armour", 0);
        t.armourMinKeep = (float)c->getNum("armourmindamagetokeep", 1);
        t.techLevel = (int)c->getNum("techlevel", 1);
        t.buildSpeed = perTickRate(*c, "buildspeed", 0.001f);
        // :1779 a plain float, not an inverted time -- nanoFactorySpeed : 5s would be a parse error in
        // RWX, so read it straight.
        t.nanoFactorySpeed = (float)c->getNum("nanofactoryspeed", 1);
        // :1717-1720 dirOffset has no default at all (null = the exit line is due south), moveAway falls
        // back to 70, so a factory that never mentions either still throws its units 70 px out.
        t.exitDirSet = c->has("exit_diroffset");
        t.exitDirOffset = (float)c->getNum("exit_diroffset", 0);
        t.exitMoveAway = (float)c->getNum("exit_moveawayamount", 70);
        // :1715-1719 the unload point itself: raw px in the carrier's frame, plus the height it lands at.
        t.exitX = (float)c->getNum("exit_x", 0);
        t.exitY = (float)c->getNum("exit_y", 9);
        t.exitHeightOffset = (float)c->getNum("exit_heightoffset", 0);
        t.exitHeightIgnoreParent = c->getBool("exitheightignoreparent", false);
        // :1818-1846 the transport block. maxTransportingUnits and transportSlotsNeeded both go through
        // getLogicBooleanUnit, which RP reads as the plain number it always is in shipped data (a negative
        // capacity is a hard parse error in RWX, :1819-1821).
        t.maxTransportingUnits = (int)c->getNum("maxtransportingunits", 0);
        t.transportSlotsNeeded = (int)c->getNum("transportslotsneeded", 1);
        t.transportUnloadDelay = timeTicks("transportunitsunloaddelaybetweeneachunit", *c, 30);
        t.transportEachUnitSingleSlot = c->getBool("transportunitseachunitalwaysusessingleslot", false);
        // :1824-1828 a comma-separated list of movement types. An unparsable name lands on NONE here,
        // where RWX would raise a ConfigParseException, so it only ever narrows the cargo away.
        {
            std::string mt = ToLower(Trim(c->get("transportunitsrequiremovementtype", "")));
            size_t at = 0;
            while (at < mt.size()) {
                size_t nxt = mt.find(',', at);
                std::string one = Trim(mt.substr(at, nxt == std::string::npos ? std::string::npos : nxt - at));
                if (!one.empty()) t.transportRequireMovementType.push_back(parseMoveType(one));
                at = (nxt == std::string::npos) ? mt.size() : nxt + 1;
            }
        }
        // :1831 the air-and-water block defaults to "no movement type was listed", :1832 always true.
        t.transportBlockAirAndWater =
            c->getBool("transportunitsblockairandwaterunits", t.transportRequireMovementType.empty());
        t.transportBlockOtherTransports = c->getBool("transportunitsblockothertransports", true);
        // :1823 transportUnitsRequireTag is an AnimationTag set, and RWX skips the requirement when the
        // name resolves to nothing (AnimationSet.a():7-9 -- that is exactly how a mod *disables* an
        // inherited requirement, as nautilus's `nonexistent` does). RP keeps no tag store, so every tag
        // name resolves to nothing and the requirement is inert.
        t.transportKeepWaypoints = c->getBool("transportunitskeepwaypoints", false);
        t.transportKeepBuiltUnits = c->getBool("transportunitskeepbuiltunits", false);
        t.transportKillOnDeath = c->getBool("transportunitskillondeath", true);
        t.transportHealBy = (float)c->getNum("transportunitshealby", 0);
        // :1837-1842 the absent key is not `true` but the built-in not-over-liquid-and-not-moving test.
        t.transportCanUnload = parseUnloadCond(c->get("transportunitscanunloadunits", ""));
        t.isBuilding = c->getBool("isbuilding", false);
        t.isBuilder = c->getBool("isbuilder", false);
        t.fogSight = (float)c->getNum("fogofwarsightrange", 0);
        t.explodeOnDeath = c->getBool("explodeondeath", true);
        // :1800 the crash-on-touchdown flag, read through its historical misspelling as an alias.
        t.explodeOnDeathGroundCollision =
            c->getBool("explodeondeathgroundcollision", c->getBool("explodeondeathgroundcollosion", true));
        t.hideScorchMark = c->getBool("hidescorchmark", false);
        auto fp = parseCSV(c->get("footprint", c->get("constructionfootprint", "0,0,0,0")));
        if (fp.size() >= 4) { t.footL = (int)fp[0]; t.footU = (int)fp[1]; t.footR = (int)fp[2]; t.footD = (int)fp[3]; }
        // :1709-1710, getFloat with a 10 px default -- which is exactly the half tile BaseUnit.getTileOffsetX
        // answers for a hull without a CustomUnitConfig, so ignoring the key only shows up on the seven inis
        // that write a different number.
        t.buildingToFootprintOffsetX = (float)c->getNum("buildingtofootprintoffsetx", TILE_SIZE * 0.5);
        t.buildingToFootprintOffsetY = (float)c->getNum("buildingtofootprintoffsety", TILE_SIZE * 0.5);
        t.placeOnlyOnResPool = c->getBool("placeonlyonrespool", c->getBool("placeonlyonrespools", c->getBool("placeonlyonresource", false)));
        t.placeOnlyOnWater = c->getBool("placeonlyonwater", false);
        std::string gen = c->get("generation_resources");
        if (gen.empty()) gen = c->get("resourcegeneration");
        // "credits=8" -> 8
        size_t eq = gen.find('='); if (eq != std::string::npos) t.creditIncomeRate = (float)atof(gen.c_str() + eq + 1);
        if (t.creditIncomeRate == 0) t.creditIncomeRate = (float)c->getNum("generation_credits", 0);
        // :1546 is a separate key from the generation lump and the only thing getResourceRate() reads; the
        // shipped data writes it on crystal_mid (0.1) and creditsCrate (5) alone.
        t.resourceRate = (float)c->getNum("resourcerate", 0);
        // :1520-1527: default 40, a written 0 is forced up to 1, and a negative is a hard parse error there --
        // RP parks it back on the default instead of dropping the unit over it.
        int gd = (int)c->getNum("generation_delay", 40);
        t.generationDelay = gd == 0 ? 1 : (gd < 0 ? 40 : gd);
        // :1792-1795 -- nukeOnDeath's three siblings carry RWX's own defaults, which the shipped carriers
        // mostly override (only laboratory.ini leaves them at 250 / 5400).
        t.nukeOnDeath = c->getBool("nukeondeath", false);
        t.nukeOnDeathRange = (float)c->getNum("nukeondeathrange", 250);
        t.nukeOnDeathDamage = (float)c->getNum("nukeondeathdamage", 5400);
        t.nukeOnDeathDisableWhenNoNuke = c->getBool("nukeondeathdisablewhennonuke", false);
        parseBuildLinks(*c, "canBuild", t.canBuild);
        parseBuildLinks(*c, "builtFrom", t.builtFrom);
    }
    parseLegSections(ini, t);
    if (const IniSection* g = ini.find("graphics")) {
        t.image = g->get("image");
        t.imageWreak = g->get("image_wreak");
        t.imageTurret = g->get("image_turret");
        t.imageScale = (float)g->getNum("imagescale", g->getNum("scaleimagesto_scale", 1));
        t.turretImageScale = (float)g->getNum("turretimagescale", 1);
        t.imageShadow = g->get("image_shadow");
        t.imageShield = g->get("image_shield");
        t.showShieldBar = g->getBool("showshieldbar", true);
        t.showEnergyBar = g->getBool("showenergybar", true);
        // :1953 the cargo bar over a transport, on by default
        t.showTransportBar = g->getBool("showtransportbar", true);
        t.shadowOffX = (float)g->getNum("shadowoffsetx", 0);
        t.shadowOffY = (float)g->getNum("shadowoffsety", 0);
        // :1583-1584, and the shadow key's default is the body key's value. These two really are
        // spelled with underscores in the mod ini's, unlike most [graphics] keys.
        t.lockBodyRot = g->getBool("lock_body_rotation_with_main_turret", false);
        t.lockShadowRot = g->getBool("lock_shadow_rotation_with_main_turret", t.lockBodyRot);
        t.lockLegRot = g->getBool("lock_leg_rotation_with_main_turret", false);
        // :1312-1316 -- offsetX/Y are whole world pixels (getLogicBooleanUnit), offsetH is a float that comes
        // *off* Y. Any one of the three being non-zero is what turns the translate on.
        t.imageOffsetX = (int)g->getNum("image_offsetx", 0);
        t.imageOffsetY = (int)g->getNum("image_offsety", 0);
        t.imageOffsetH = (float)g->getNum("image_offseth", 0);
        if (t.imageOffsetX != 0 || t.imageOffsetY != 0 || t.imageOffsetH != 0.0f) t.hasImageOffset = true;
        // :1318-1338, in that order: pureGreen, then teamColorsUseHue as a hueAdd shortcut, then the explicit
        // mode string (which RWX refuses to combine with the shortcut). Unknown names are a parse error there;
        // RP keeps the default rather than dropping the whole unit over one typo.
        t.teamColoringMode = kTCModePureGreen;
        if (g->getBool("teamcolorsusehue", false)) t.teamColoringMode = kTCModeHueAdd;
        if (g->has("teamcoloringmode")) {
            std::string ms = ToLower(g->get("teamcoloringmode"));
            if (ms == "puregreen") t.teamColoringMode = kTCModePureGreen;
            else if (ms == "hueadd") t.teamColoringMode = kTCModeHueAdd;
            else if (ms == "hueshift") t.teamColoringMode = kTCModeHueShift;
            else if (ms == "disabled") t.teamColoringMode = kTCModeDisabled;
        }
        // :1406-1408: the shared image_turret sprite is only dyed when both of these hold.
        t.teamColorsOnTurret = g->getBool("teamcolorsonturret", false);
        t.totalFrames = (int)g->getNum("total_frames", 1);
        if (t.totalFrames < 1) t.totalFrames = 1;
        t.defaultFrame = (int)g->getNum("default_frame", 0);
        // CustomUnitConfigParser.java:1658-1660 hands each of the three slots its own key prefix.
        parseBodyAnim(*g, "animation_moving_", t.animMoving, t);
        parseBodyAnim(*g, "animation_idle_", t.animIdle, t);
        parseBodyAnim(*g, "animation_attack_", t.animAttack, t);
        t.drawLayer = g->get("drawlayer");
    }
    // :1661-1672 a [animation_*] section is a standalone AnimationConfig, and the one whose onActions
    // names move/idle/attack *replaces* the [graphics] form for that slot (resolveAnimationForAction,
    // CustomUnitConfig.java:1032-1041 -- defining both is a config error, so taking the section here is
    // all RWX allows). The two construction actions are the only other ones the engine itself raises
    // (CustomUnit.java:1788-1795); sections named for anything else (dive, dance, fireNuke) are only
    // reached by trigger actions, which RP does not run.
    for (IniSection* s : ini.startingWith("animation_")) {
        BodyAnim b;
        parseBodyAnim(*s, "", b, t);
        if (!b.valid()) continue;
        t.animations[ToLower(s->name.substr(10))] = b;
        std::string acts;
        for (auto& n : splitNames(ToLower(s->get("onactions", "")))) acts += n + ",";
        if (acts.find("idle,") != std::string::npos) t.animIdle = b;
        else if (acts.find("move,") != std::string::npos) t.animMoving = b;
        else if (acts.find("attack,") != std::string::npos) t.animAttack = b;
        else if (acts.find("underconstructionwithlinkedbuilttime,") != std::string::npos) t.animBuild = b;
        else if (acts.find("underconstruction,") != std::string::npos) t.animUnderConstruction = b;
        else if (acts.find("queuedunits,") != std::string::npos) t.animQueued = b;
    }
    if (const IniSection* a = ini.find("attack")) {
        t.canAttack = a->getBool("canattack", false);
        t.canAttackFlying = a->getBool("canattackflyingunits", false);
        t.canAttackLand = a->getBool("canattacklandunits", true);
        t.canAttackUnderwater = a->getBool("canattackunderwaterunits", false);
        // :2291-2293, and a written `true` is normalised back to the unrestricted default, so only false
        // (the four sub hulls) changes anything.
        t.canAttackNotTouchingWater = a->getBool("canattacknottouchingwaterunits", true);
        t.isFixedFiring = a->getBool("isfixedfiring", false);
        t.turretRotateWithBody = a->getBool("turretrotatewithbody", true);
        // :2030 turretMultiTargeting; the flag on its own does nothing for a one-barrel hull
        t.turretMultiTargeting = a->getBool("turretmultitargeting", false);
        // :2033-2039 maxAttackRange, and when the section leaves it out the parser installs 100 rather
        // than nothing. RWX multiplies both branches by globalScale, which no shipped ini writes.
        t.attackRange = (float)a->getNum("maxattackrange", 100);
        // CustomUnitConfigParser.java:2042 getTime("attack","shootDelay", 50.0f)
        t.shootDelay = timeTicks("shootdelay", *a, 50);
        // :2014 getFloat("attack","turretSize", 1.0f) -- and TurretConfig.java:332 seeds every fresh
        // barrel's barrelY from it, so a turret that names no size/barrelY fires from 1px off its pivot.
        t.turretSize = (float)a->getNum("turretsize", 1.0);
        // :2041 aimOffsetSpread, a plain float with default 0.6 that every barrel then inherits
        // (TurretConfig.java:331). It is the hull-wide aim jitter, not a damage or range thing.
        t.aimOffsetSpread = (float)a->getNum("aimoffsetspread", 0.6);
        // :2015 turretTurnSpeed default 8.0 deg/tick; per-turret turnSpeed overrides it
        // (CustomUnit.java:2297-2303), which Templates.cpp does when the [turret] section has one.
        t.turretTurnSpeed = (float)a->getNum("turretturnspeed", 8);
        // :2046-2056 -- isMelee only ever installs a range: 250 when the ini does not name one, and 0 for
        // a hull that is not melee. RWX throws on meleeEngangementDistance without isMelee:true; RP parks
        // it back on 0 instead of dropping the unit over one stray key.
        t.isMelee = a->getBool("ismelee", false);
        float eng = (float)a->getNum("meleeengangementdistance", -1);
        t.meleeRange = t.isMelee ? (eng >= 0 ? eng : 250.0f) : 0.0f;
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
        // :2012 -- parsed and stored, but a dead config: RWX has no runtime reader for it (see UnitTemplate.h)
        t.moveSlidingDir = (int)m->getNum("moveslidingdir", -1);
        // :1982-1991, a tri-state string matched case-insensitively; anything else throws the parse off
        // in RWX, so here it just falls back to false.
        {
            std::string lg = ToLower(m->get("landonground", "false"));
            t.landOnGround = (lg == "true" || lg == "onlyidle");
            t.landOnGroundOnlyIdle = (lg == "onlyidle");
        }
        // :1993-2006 an AIR hull's cruising height is 35 with a 1.5px sine drift unless the ini says
        // otherwise; everything else sits on the ground at 0 with no drift.
        bool isAir = t.moveType == MoveType::AIR;
        t.targetHeight = (float)m->getNum("targetheight", isAir ? 35 : 0);
        t.targetHeightDrift = (float)m->getNum("targetheightdrift", isAir ? 1.5 : 0);
        // :2006 heightChangeRate; the -1 default is the "use the automatic ease-out" sentinel.
        t.heightChangeRate = (float)m->getNum("heightchangerate", -1);
        t.fallingAcceleration = (float)m->getNum("fallingacceleration", 0.03f);
        t.startingHeightOffset = (float)m->getNum("startingheightoffset", 0);
        // :1957-1958, :2007 the falling corpse: slowDeathFall glides instead of dropping, and only a
        // hull that had neither rotation nor velocity (or no slowDeathFall at all) accelerates at
        // fallingAccelerationDead.
        t.slowDeathFall = m->getBool("slowdeathfall", false);
        t.slowDeathFallSmoke = m->getBool("slowdeathfallsmoke", true);
        t.fallingAccelerationDead = (float)m->getNum("fallingaccelerationdead", 0.06);
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
        // :1748-1750; the flag itself buys the hull a render hook, whose timer is what does the work
        t.autoRepair = c->getBool("autorepair", false);
        // :1760-1763 nanoRangeForRepair; -1/unset means the repair reaches as far as nanoRange
        t.nanoRangeForRepair = (float)c->getNum("nanorangeforrepair", 0);
        // :1807 the reclaim gate, and the three rates the reclaim command spends (:1775-1777). Note the
        // default for nanoReclaimSpeed is written in terms of nanoRepairSpeed, which is why this sits
        // after :1774 rather than beside its siblings.
        t.canReclaimResources = c->getBool("canreclaimresources", false);
        t.nanoReclaimSpeed = (float)c->getNum("nanoreclaimspeed", t.nanoRepairSpeed * 5.1f);
        t.nanoUnbuildSpeed = (float)c->getNum("nanounbuildspeed", 1) * 0.001f * 5.1f;
        t.resourceReclaimMultiplier = (float)c->getNum("resourcereclaimmultiplier", 1);
        // :1494. A written reclaimPrice is a UnitPrice and RWX keeps an all-zero one as a non-null empty
        // price (UnitPrice:229-232), which is what makes robotCrab's "reclaimPrice : 0" refund nothing
        // rather than the usual 80% -- so presence of the key matters, not just its value.
        t.hasReclaimPrice = c->has("reclaimprice");
        if (t.hasReclaimPrice) t.reclaimPrice = (float)c->getNum("reclaimprice", 0);
        t.selfBuildRate = perTickRate(*c, "selfbuildrate", 0);
        t.dieOnConstruct = c->getBool("dieonconstruct", false);
        // :1780 selfRegenRate is a plain getFloat, not an inverted time: the value is HP per tick
        t.selfRegenRate = (float)c->getNum("selfregenrate", 0);
        // RWX mods often omit isBuilder on nano units (e.g. megaBuilder). nanoBuildSpeed now defaults
        // to 1.0 like RWX (:1778), so only a declared key counts as evidence here.
        if (!t.isBuilder && !t.isBuilding && (t.nanoRange > 0 || c->has("nanobuildspeed"))) t.isBuilder = true;
    }

    // [effect_<name>] sections, collected by CustomUnitConfigParser:1588-1592 in file order. They have to be
    // in hand before the projectiles and the barrels are parsed, because explodeEffect and shoot_flame both
    // name one of them.
    parseEffectSections(ini, t.effects);
    parseRuntimeSections(ini, t);
    if (const IniSection* g = ini.find("graphics")) {
        t.splashEffect = g->getBool("splasteffect", false);
        t.dustEffect = g->getBool("dusteffect", false);
        t.splashEffectReverse = g->getBool("splasteffectreverse", true);
        t.dustEffectReverse = g->getBool("dusteffectreverse", true);
        t.movementEffectRate = (float)g->getNum("movementeffectrate", 11.0f);
        t.movementEffectReverseFlipEffects = g->getBool("movementeffectreverseflipeffects", false);
        int missing = 0;
        parseFlameList(g->get("movementeffect", ""), t.effects, nullptr, t.movementEffect, missing);
        parseFlameList(g->get("movementeffectreverse", ""), t.effects, nullptr, t.movementEffectReverse, missing);
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
            p.flameWeapon = s.getBool("flameweapon", false);
            // :236-243 -- both are addConfigExtension, so a key that is absent leaves the inherited value
            // alone and a key that is present replaces it, `NONE` included. No turret flame slot here: RWX
            // runs every entry of the list through EffectTemplate.a().
            if (s.has("explodeeffect")) {
                int miss = 0;
                p.hasExplodeEffect = true;
                parseFlameList(s.get("explodeeffect", ""), t.effects, nullptr, p.explodeEffect, miss);
            }
            if (s.has("explodeeffectonshield")) {
                int miss = 0;
                p.hasExplodeEffectOnShield = true;
                parseFlameList(s.get("explodeeffectonshield", ""), t.effects, nullptr,
                               p.explodeEffectOnShield, miss);
            }
            // :153-156 -- a third list on the same grammar, run at the muzzle with the round as its GameObject.
            if (s.has("effectoncreate")) {
                int miss = 0;
                p.hasEffectOnCreate = true;
                parseFlameList(s.get("effectoncreate", ""), t.effects, nullptr, p.effectOnCreate, miss);
            }
            p.turnSpeed = (float)s.getNum("turnspeed", -1);
            p.targetSpeed = (float)s.getNum("targetspeed", -1);
            p.targetSpeedAccel = (float)s.getNum("targetspeedacceleration", 0.1);
            p.speedSpread = (float)s.getNum("speedspread", 0);
            p.instant = s.getBool("instant", false);
            // CustomProjectileTemplate.java:72,89-113,121-138,171,179-189,249 -- the beam/nuke/teleport family.
            p.nukeWeapon = s.getBool("nukeweapon", false);
            p.teleportSource = s.getBool("teleportsource", false);
            p.instantReuseLast = s.getBool("instantreuselast", false);
            p.instantReuseLastAlsoChangeTurretAim = s.getBool("instantreuselast_alsochangeturretaim", false);
            p.instantReuseLastAcrossTurrets = s.getBool("instantreuselast_acrossturrets", false);
            p.instantReuseLastKeepAreaDamageList = s.getBool("instantreuselast_keepareadamagelist", false);
            p.moveWithParent = s.getBool("movewithparent", false);
            p.laserEffect = s.getBool("lasereffect", false);
            p.lightingEffect = s.getBool("lightingeffect", false);
            p.lightCastOnGround = s.getBool("lightcastonground", false);
            p.beamImage = Trim(s.get("beamimage", ""));
            p.beamImageStart = Trim(s.get("beamimagestart", ""));
            p.beamImageEnd = Trim(s.get("beamimageend", ""));
            p.beamImageStartRotated = s.getBool("beamimagestartrotated", false);
            p.beamImageEndRotated = s.getBool("beamimageendrotated", false);
            p.beamImageOffsetRate = (float)s.getNum("beamimageoffsetrate", 0);
            // :181-183 -- a laserEffect with no beamImage and no explicit colour defaults to ARGB(80,255,0,0).
            if (p.laserEffect && p.beamImage.empty() && !s.has("color")) p.color = 0xFF000050u;
            // :71, straight into ProjectileTemplate.java:77's default of 1.0 -- a round that says nothing is
            // deflectable by one beam hit, and only a value under 0.5 buys immunity (CustomUnit:1401-1404).
            p.deflectionPower = (float)s.getNum("deflectionpower", p.deflectionPower);
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
            // :57-59,69-70 -- the five area_* modifiers, all of which CustomUnit.java:1387-1397 hands to
            // the shot and Projectile.b(:1459-1507) then spends on every hull in the ring.
            p.areaDamageNoFalloff = s.getBool("areadamagenofalloff", p.areaDamageNoFalloff);
            p.areaIgnoreUnitsCloserThan = (float)s.getNum("areaignoreunitscloserthan", p.areaIgnoreUnitsCloserThan);
            p.areaRadiusFromEdge = s.getBool("arearadiusfromedge", p.areaRadiusFromEdge);
            p.areaHitAirAndLandAtSameTime = s.getBool("areahitairandlandatsametime", p.areaHitAirAndLandAtSameTime);
            p.areaHitUnderwaterAlways = s.getBool("areahitunderwateralways", p.areaHitUnderwaterAlways);
            // :60-68 -- one key, two shapes. The string "only-ignoreEnemy" is tried first and sets l;
            // otherwise the value has to parse as a boolean, which sets k and leaves l clear.
            if (s.has("friendlyfire")) {
                if (ToLower(s.get("friendlyfire")) == "only-ignoreenemy") p.friendlyFireIgnoresEnemy = true;
                else p.friendlyFire = s.getBool("friendlyfire", false);
            }
            p.buildingDamageMult = (float)s.getNum("buildingdamagemultiplier", 1);
            p.shieldDamageMult = (float)s.getNum("shielddamagemultiplier", 1);
            // CustomProjectileTemplate.java:232 - the deflection key is spelled wrong in the parser, so
            // that is the only spelling RWX honours; accept the corrected one as a fallback.
            p.shieldDeflectMult = (float)s.getNum("shielddefectionmultiplier",
                s.getNum("shielddeflectionmultiplier", 1));
            p.hullDamageMult = (float)s.getNum("hulldamagemultiplier", 1);
            p.armourIgnore = (float)s.getNum("armourignoreamount", 0);
            // :48,224 -- the unguided ground bomb family (artillery, experimental tank, nukes).
            p.targetGround = s.getBool("targetground", false);
            p.targetGroundSpread = (float)s.getNum("targetgroundspread", 0);
            // :115,118-119 -- the shot's own vertical speed and the two gravity flavours.
            p.launchVelZ = (float)s.getNum("initialunguidedspeedheight", 0);
            p.gravity = (float)s.getNum("gravity", 0);
            p.trueGravity = (float)s.getNum("truegravity", 0);
            // :141,190-191 -- the arc. -1 means the ini is silent, and the shot then keeps its own
            // 40 / 60 px defaults (CustomUnit.java:1490-1495).
            p.ballistic = s.getBool("ballistic", false);
            p.ballisticDelayHeight = (float)s.getNum("ballistic_delaymove_height", -1);
            p.ballisticHeight = (float)s.getNum("ballistic_height", -1);
            // :251 and :56 -- the tag list the shot carries, and which half of the interceptor's job it
            // changes: a quarry tagged here is what a [turret_N] interceptProjectiles_withTags barrel looks
            // for, and removeTargetLifeOnly is what that barrel does to it.
            p.tags = parseTags(s.get("tags", ""));
            p.interceptRemoveTargetLifeOnly = s.getBool("interceptprojectile_removetargetlifeonly", false);
            // :166-168, the three moments a round can hand off to another round. Only the end-of-life list is
            // written by any shipped weapon, and reading `has` rather than the value is what lets a derived
            // section overwrite the one it inherited with `NONE`.
            if (s.has("spawnprojectilesonendoflife"))
                p.onEndOfLife = parseSpawnList(s.get("spawnprojectilesonendoflife"));
            // :244-246, the other spawner: a unit left at the impact point. The names go through
            // UnitTypeReference and are resolved against the *whole* unit set when the round lands
            // (UnitSpawner.a():293), not against this unit's sections, so they stay strings here.
            if (s.has("spawnunit"))
                p.spawnUnits = parseUnitSpawnList(s.get("spawnunit"));
            t.projectiles.push_back(p);
        }
        // RWX holds these as lazy CustomUnitProjectileReference objects (CustomUnitConfig:1604) and looks the
        // name up when the round actually dies, so a section may point at one defined further down the file
        // -- both shipped chains do. projIdx is complete only now, so the match happens here.
        for (auto& pr : t.projectiles)
            for (auto& e : pr.onEndOfLife) {
                auto it = projIdx.find(ToLower(Trim(e.name)));
                e.proj = it != projIdx.end() ? it->second : -1;
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
                // Same seeding for the water gate: in RWX the barrel reads only its own flag (:1308) while
                // the hull's copy gates acquisition, so merging them here keeps a sub's barrels and its
                // target list agreeing.
                tt.canAttackNotTouchingWater = t.canAttackNotTouchingWater;
                // TurretConfig.java:331 -- a fresh barrel takes the hull's aimOffsetSpread; a copyFrom
                // barrel already carries its source's value through the struct copy above (:298).
                tt.aimOffsetSpread = t.aimOffsetSpread;
            }
            tt.x = (float)s.getNum("x", 0);
            tt.y = (float)s.getNum("y", 0);
            // size is an alias of barrelY; the -1 sentinel means "[attack] turretSize", see fire()
            if (s.has("barrely") || s.has("size"))
                tt.barrelY = (float)s.getNum("barrely", s.getNum("size", -1));
            if (s.has("barrelx")) tt.barrelX = (float)s.getNum("barrelx", 0);
            if (s.has("barreloffsetx_onoddshots")) tt.barrelXOdd = (float)s.getNum("barreloffsetx_onoddshots", 0);
            if (s.has("image")) tt.image = Trim(s.get("image", ""));
            // TurretConfig.java:563-566 -- inherits the copyFrom barrel's texture when the key is absent
            tt.chargeEffectImage = Trim(s.get("chargeeffectimage", tt.chargeEffectImage));
            if (s.has("onshoot_playanimation")) tt.shootAnimation = ToLower(Trim(s.get("onshoot_playanimation")));
            // TurretConfig.java:551-554 reads this as a nullable Boolean: absent inherits [graphics]
            // teamColorsOnTurret, so RP keeps it tri-state instead of defaulting it to false.
            if (s.has("image_applyteamcolors"))
                tt.applyTeamColors = s.getBool("image_applyteamcolors", false) ? 1 : 0;
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
            if (s.has("energyusage")) tt.energyUsage = (float)s.getNum("energyusage", tt.energyUsage);
            // TurretConfig.java:377-380 -- only read when present, so a copyFrom barrel that says nothing
            // keeps its source's cost (the struct copy above already brought it across).
            if (s.has("resourceusage")) tt.resourceUsage = parseResourceCost(s.get("resourceusage", ""));
            if (s.has("warmupcalldownrate")) tt.warmCallDown = (float)s.getNum("warmupcalldownrate", tt.warmCallDown);
            if (s.has("warmupnoreset")) tt.warmNoReset = s.getBool("warmupnoreset", false);
            // 99999 is RWX's "no limit" sentinel (TurretConfig.java:484-496), not a real range.
            if (s.has("limitingrange")) {
                float lr = (float)s.getNum("limitingrange", 0);
                if (lr > 0 && lr < 99999.0f) tt.range = lr;
            }
            if (s.has("limitingminrange")) tt.minRange = (float)s.getNum("limitingminrange", tt.minRange);
            if (s.has("canattackmaxangle")) tt.maxAngle = (float)s.getNum("canattackmaxangle", tt.maxAngle);
            // TurretConfig.java:485-490 -- a plain getFloat, -1 (the default) means no cone at all.
            if (s.has("limitingangle")) tt.limitingAngle = (float)s.getNum("limitingangle", tt.limitingAngle);
            if (s.has("recoiloffset")) tt.recoil = (float)s.getNum("recoiloffset", tt.recoil);
            if (s.has("recoilouttime")) tt.recoilOut = (float)s.getNum("recoilouttime", tt.recoilOut);
            if (s.has("recoilreturntime")) tt.recoilReturn = timeTicks("recoilreturntime", s, tt.recoilReturn);
            if (s.has("attachedto")) {
                auto it = turIdx.find(ToLower(Trim(s.get("attachedto"))));
                if (it != turIdx.end() && it->second != i) tt.attached = it->second;
            }
            // TurretConfig.java:426-432 -- slave requires attachedTo and points childTurret at that parent,
            // so the master is exactly the resolved attached index. A slave with no parent is a parse error
            // in RWX; RP leaves slaveMaster at -1 (a plain attached barrel) rather than aborting the mod.
            if (s.getBool("slave", false) && tt.attached >= 0) tt.slaveMaster = tt.attached;
            if (s.has("linkdelaywithturret")) {
                auto it = turIdx.find(ToLower(Trim(s.get("linkdelaywithturret"))));
                if (it != turIdx.end() && it->second != i) tt.linkDelay = it->second;
            }
            if (s.has("idledir")) tt.idleDir = (float)s.getNum("idledir", 0);
            // :487 the barrel may restate the jitter, defaulting to whatever it already holds
            if (s.has("aimoffsetspread")) tt.aimOffsetSpread = (float)s.getNum("aimoffsetspread", tt.aimOffsetSpread);
            if (s.has("shouldresetturret")) tt.shouldResetTurret = s.getBool("shouldresetturret", true);
            // TurretConfig.java:219,517 -- a plain getFloat, negative spins the other way
            tt.idleSpin = (float)s.getNum("idlespin", tt.idleSpin);
            // TurretConfig.java:518-536 -- the idle sweep. Every key takes the inherited value as its
            // default, so a copyFrom barrel that says nothing keeps its source's sweep, and the
            // afterburn variant's bare `idleSweepAngle : 0` switches it off.
            tt.idleSweepAngle = std::max(0.0f, (float)s.getNum("idlesweepangle", tt.idleSweepAngle));
            tt.idleSweepDelay = (float)s.getNum("idlesweepdelay", tt.idleSweepDelay);
            tt.idleSweepSpeed = (float)s.getNum("idlesweepspeed", tt.idleSweepSpeed);
            tt.idleSweepAddRandomDelay = (float)s.getNum("idlesweepaddrandomdelay", tt.idleSweepAddRandomDelay);
            if (tt.idleSweepAddRandomDelay < 0.0f)
                tt.idleSweepAddRandomDelay = tt.idleSweepDelay > 200.0f ? 20.0f
                                           : tt.idleSweepDelay > 50.0f ? 5.0f : 1.0f;
            tt.idleSweepAddRandomAngle = std::max(0.0f, (float)s.getNum("idlesweepaddrandomangle", tt.idleSweepAddRandomAngle));
            if (s.has("idlesweepcondition")) {
                // :521-522 getLogicBoolean + convertAlwaysTrueToNull: `true` and an absent key both mean
                // "no gate at all", which is the empty test list.
                std::string c = ToLower(Trim(s.get("idlesweepcondition", "")));
                if (c.rfind("if ", 0) == 0) c = Trim(c.substr(3));
                if (c == "false") tt.idleSweepCond.never = true;
                else if (c != "true" && !c.empty()) parseCondAtoms(c, tt.idleSweepCond);
            }
            if (s.has("canattackflyingunits")) tt.canAttackFlying = s.getBool("canattackflyingunits", true);
            if (s.has("canattacklandunits")) tt.canAttackLand = s.getBool("canattacklandunits", true);
            if (s.has("canattackunderwaterunits")) tt.canAttackUnderwater = s.getBool("canattackunderwaterunits", false);
            // :437/577 -- the barrel may restate the water gate; `true` is folded back into the default.
            if (s.has("canattacknottouchingwaterunits"))
                tt.canAttackNotTouchingWater = s.getBool("canattacknottouchingwaterunits", true);
            // :503-507 -- an absent key leaves RWX's Float null, which is the whole difference between a
            // normal barrel and a beam, so a copyFrom barrel that says nothing keeps its source's beam.
            if (s.has("laserdefenceenergyuse")) {
                tt.laserDefence = true;
                tt.laserDefenceEnergyUse = (float)s.getNum("laserdefenceenergyuse", 0);
            }
            // :508-514 -- the tag list is the switch for the projectile hunt: RWX only reads the three
            // geometry keys inside that branch, so a barrel that names tags still inherits the defaults for
            // distances it does not spell out. The key is read *with the barrel's own list as the fallback*,
            // which is what lets a copyFrom barrel keep its source's hunt when it says nothing (:508's second
            // argument), so an absent key must not clear what the copy already brought.
            std::vector<std::string> hunt = parseTags(s.get("interceptprojectiles_withtags", ""));
            if (!hunt.empty()) {
                tt.interceptTags = hunt;
                tt.interceptGroundUnder = (float)s.getNum("interceptprojectiles_andtargetinggroundunderdistance", tt.interceptGroundUnder);
                tt.interceptUnder = (float)s.getNum("interceptprojectiles_andunderdistance", tt.interceptUnder);
                tt.interceptOverHeight = (float)s.getNum("interceptprojectiles_andoverheight", tt.interceptOverHeight);
            }
            // :423 -- the whole value is one spawn list, so RP hands it to the same split RWX uses. A copyFrom
            // barrel that says nothing must keep its source's list, which is exactly what :1160-1163 of
            // addConfigExtension does with a null string, so the key is only read when it is present.
            if (s.has("shoot_flame")) {
                int miss = 0;
                parseFlameList(s.get("shoot_flame", ""), t.effects, &tt.shootFlame, tt.flames, miss);
            }
            // :424 shoot_light is a colour rather than a list, and getColorAsInt defaults to the field, so an
            // absent key leaves whatever the copyFrom barrel brought with it.
            if (s.has("shoot_light")) tt.shootLight = parseRgba(s.get("shoot_light", ""));
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
        // :1906-1912: a hull that can reclaim is a harvester to the AI, and an explicit [ai] useAsHarvester
        // is read after that, so the ini still wins. Harvesting credits is not part of the test.
        t.useAsHarvester = t.canReclaimResources;
        if (ai->has("useasharvester")) t.useAsHarvester = ai->getBool("useasharvester", false);
    }
    // If attack enabled but no turrets defined, synthesize RWX's fallback turret ("1", first projectile)
    if (t.canAttack && t.turrets.empty()) {
        TurretTpl tt;
        tt.canAttackFlying = t.canAttackFlying; tt.canAttackLand = t.canAttackLand;
        tt.canAttackNotTouchingWater = t.canAttackNotTouchingWater;
        tt.turnSpeed = t.turretTurnSpeed;
        tt.aimOffsetSpread = t.aimOffsetSpread;
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
    // a forceNano option is placed and nano-built by the hull, never queued, so it does not make a factory
    t.isFactory = false;
    for (auto& b : t.canBuild) if (!b.forceNano) t.isFactory = true;
    return t;
}

void TemplateStore::loadAll(const std::string& assetsRoot) {
    struct Dir { std::string sub; };
    // builtin_mods are optional mods, disabled by default in RWX (builtin_mods_enabled) - not loaded
    const char* roots[] = { "units" };
    std::vector<std::pair<std::string, std::string>> aliases; // aliasKey -> realKey
    // RWX's hard-coded units have no .ini, so they go in first; every ini below can then override one by
    // name or through `overrideAndReplace`, which is exactly how the shipped extractor/tank/turret inis
    // take over their built-in counterparts.
    registerBuiltinUnits(*this);
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
            // CustomUnitConfigParser.java:1220 - ${...} is resolved on the merged file before any key is
            // read, so a numeric expression never reaches a getter as text.
            {
                std::vector<std::string> warn;
                varsub::substitute(ini, path, warn);
                for (size_t w = 0; w < warn.size() && w < 20; ++w) printf("varsub: %s\n", warn[w].c_str());
            }
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
    // RWX does not gate production on isBuilding, it gates it on the option: an option naming a building
    // becomes a PlaceBuildingAction, whose isQueuable() is false (:149), while one naming a moving unit
    // becomes a QueueUnitAction (:86) that any hull can run, because CustomUnit.update:1866 ticks
    // FactoryQueueManager for every unit and :249 multiplies the queued unit's buildSpeed by the
    // producer's nanoFactorySpeed. That is how a nautilus or an experimental carrier builds ships and
    // planes without being a building -- so the flag means "has something to queue", not "is a factory".
    for (auto& np : order) {
        auto nit = byName.find(np);
        if (nit == byName.end()) continue;
        UnitTemplate& f = nit->second;
        bool queues = false;
        for (auto& b : f.canBuild) {
            auto it = byName.find(ToLower(b.unitName));
            if (it != byName.end() && !it->second.isBuilding) { queues = true; break; }
        }
        f.isFactory = queues;
    }
    // re-sync alias copies with (possibly extended) real templates
    for (auto& a : aliases) {
        auto rit = byName.find(a.second);
        if (rit != byName.end()) byName[a.first] = rit->second;
    }
}

TemplateStore g_templates;
