// Mod discovery / selection. See Mods.h for the RWX sources this mirrors.
#include "Mods.h"
#include "Common.h"
#include "Ini.h"
#include "raylib.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

ModRegistry g_mods;

std::string ModStableId(const std::string& text) {
    uint64_t h = 0xcbf29ce484222325ull;             // FNV-1a 64 offset basis
    for (unsigned char c : text) {
        h ^= (uint64_t)c;
        h *= 0x100000001b3ull;                       // FNV prime
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf);
}

namespace {

namespace fs = std::filesystem;

bool HasModInfo(const std::string& dir) {
    return FileExists((dir + "/mod-info.txt").c_str());
}

// A mod entry is a *sub-directory* or a bare .ini file, so this has to list directories too -- and
// raylib's LoadDirectoryFilesEx only ever returns files (its ScanDirectoryFiles skips directories),
// which is why this goes through <filesystem> instead.
std::vector<std::string> ListDir(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) out.push_back(e.path().string());
    std::sort(out.begin(), out.end());
    return out;
}

// ModInfo.fixSourceFolder():375-396 -- a mod that was extracted with one extra wrapper folder still
// has to work: when <entry>/mod-info.txt is missing but the entry holds exactly one sub-directory
// that has one, the sub-directory becomes the source folder.
std::string FixSourceFolder(const std::string& entry) {
    if (HasModInfo(entry)) return entry;
    std::vector<std::string> subs;
    for (const std::string& p : ListDir(entry)) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) subs.push_back(p);
    }
    if (subs.size() == 1 && HasModInfo(subs[0])) return subs[0];
    return entry;
}

int CountIni(const std::string& path) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) return EndsWith(ToLower(path), ".ini") ? 1 : 0;
    int n = 0;
    for (fs::recursive_directory_iterator it(path, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && EndsWith(ToLower(it->path().string()), ".ini")) ++n;
    }
    return n;
}

// ModInfo.refreshData():467-515 -- [mod] title / description / minVersion / thumbnail. A description
// may carry literal "\n" escapes, which :477-479 turns into real newlines.
void ReadModInfo(const std::string& entry, ModEntry& e) {
    IniFile ini;
    if (!ParseIni(entry + "/mod-info.txt", ini)) return;
    IniSection* m = ini.find("mod");
    if (!m) return;
    e.title = m->get("title");
    e.description = m->get("description");
    for (size_t at = 0; (at = e.description.find("\\n", at)) != std::string::npos; at += 1)
        e.description.replace(at, 2, "\n");
    e.minVersion = m->get("minversion");
    std::string thumb = Trim(m->get("thumbnail"));
    if (!thumb.empty()) e.thumbnail = entry + "/" + thumb;
}

std::string RelToAssets(const std::string& root, const std::string& path) {
    if (path.size() > root.size() && path.compare(0, root.size(), root) == 0) {
        std::string r = path.substr(root.size());
        while (!r.empty() && (r[0] == '/' || r[0] == '\\')) r.erase(0, 1);
        return r;
    }
    // <filesystem> hands back a normalised path while the assets root may still carry "out/../assets",
    // so fall back to the tail that actually matters for a log line.
    size_t at = path.find_last_of("/\\");
    return (at == std::string::npos) ? path : path.substr(at + 1);
}

} // namespace

void ModRegistry::scanRoot(const std::string& dir, ModKind kind, bool enabledByDefault) {
    for (const std::string& p : ListDir(dir)) {
        std::error_code ec;
        bool isDir = fs::is_directory(p, ec);
        if (!isDir && !EndsWith(ToLower(p), ".ini")) continue;   // ModManager:404
        size_t slash = p.find_last_of("/\\");
        ModEntry e;
        e.dirName = (slash == std::string::npos) ? p : p.substr(slash + 1);
        e.uuid = ModStableId(e.dirName);
        e.path = isDir ? FixSourceFolder(p) : p;
        e.relPath = RelToAssets(assetsRoot_, e.path);
        e.kind = kind;
        e.enabled = enabledByDefault;
        e.iniCount = CountIni(e.path);
        if (isDir) ReadModInfo(e.path, e);
        if (e.iniCount == 0) e.error = "no .ini files";
        mods_.push_back(e);
    }
}

void ModRegistry::scan(const std::string& assetsRoot) {
    assetsRoot_ = assetsRoot;
    selPath_ = assetsRoot + "/mods_enabled.txt";
    mods_.clear();
    // Core first: it is the data every other root may override (CustomUnitConfigParser:998).
    ModEntry core;
    core.dirName = "units";
    core.uuid = ModStableId("units");
    core.path = assetsRoot + "/units";
    core.relPath = "units";
    core.kind = ModKind::Core;
    core.enabled = true;
    core.title = "Core Units";
    core.description = "Shipped units, buildings and the built-in hulls. Always loaded.";
    core.iniCount = CountIni(core.path);
    mods_.push_back(core);
    // Then the three mod roots in RWX's own load order (:455/:461/:467), each entry on its default
    // state until loadSelection() overrides it.
    scanRoot(assetsRoot + "/mods/units", ModKind::User, true);
    scanRoot(assetsRoot + "/builtin_mods", ModKind::Builtin, false);
    scanRoot(assetsRoot + "/builtin_mods_enabled", ModKind::BuiltinEnabled, true);
}

void ModRegistry::loadSelection() {
    // ModManager.loadModSelection():233-269 -- every mod starts from its default, then the saved
    // records are applied on top, and a mod that is new to the file keeps the default.
    std::ifstream f(selPath_);
    if (!f) {
        // First run: freeze the defaults so builtin_mods stays off next time (the job
        // ModManager:483-489 does when settingsEngine.lastModCount is -1).
        saveSelection();
        return;
    }
    std::vector<std::pair<std::string, bool>> saved;   // id, enabled
    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        // "<id>|<dirName>|<enabled>" -- the name is only there to make the file readable.
        std::vector<std::string> parts;
        size_t at = 0;
        while (at <= line.size()) {
            size_t bar = line.find('|', at);
            parts.push_back(Trim(line.substr(at, bar == std::string::npos ? std::string::npos : bar - at)));
            if (bar == std::string::npos) break;
            at = bar + 1;
        }
        if (parts.size() < 2) continue;
        const std::string& state = ToLower(parts.back());
        if (state != "enabled" && state != "disabled") continue;
        saved.push_back({ parts[0], state == "enabled" });
    }
    for (ModEntry& e : mods_) {
        if (!e.canToggle()) continue;                  // core is not in the file by design
        for (auto& s : saved) {
            if (s.first == e.uuid || s.first == e.dirName) { e.enabled = s.second; break; }
        }
    }
}

bool ModRegistry::saveSelection() const {
    std::ofstream f(selPath_, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "# RP mod selection -- written by the Mods screen.\n";
    f << "# <id>|<folder>|<enabled|disabled>\n";
    for (const ModEntry& e : mods_) {
        if (!e.canToggle()) continue;
        f << e.uuid << '|' << e.dirName << '|' << (e.enabled ? "enabled" : "disabled") << '\n';
    }
    return (bool)f;
}

void ModRegistry::setEnabled(size_t index, bool on) {
    if (index >= mods_.size()) return;
    if (!mods_[index].canToggle()) return;
    mods_[index].enabled = on;
}

void ModRegistry::disableAll() {
    for (ModEntry& e : mods_) if (e.canToggle()) e.enabled = false;
}

std::vector<std::string> ModRegistry::loadRoots() const {
    std::vector<std::string> out;
    for (const ModEntry& e : mods_) {
        if (e.kind == ModKind::Core || (e.enabled && e.error.empty())) out.push_back(e.path);
    }
    return out;
}

int ModRegistry::enabledCount() const {
    int n = 0;
    for (const ModEntry& e : mods_) if (e.enabled && e.error.empty()) ++n;
    return n;
}

int ModRegistry::toggleableCount() const {
    int n = 0;
    for (const ModEntry& e : mods_) if (e.canToggle()) ++n;
    return n;
}
