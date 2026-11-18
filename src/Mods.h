// Mod discovery / selection, mirroring RWX's ModManager (gameFramework/mod/ModManager.java) and
// ModInfo (gameFramework/mod/ModInfo.java).
//
// RWX treats three folders as mod roots and loads custom units from each *entry* in them:
//   assets/units                      -- the core data, always on, not a toggleable mod (:998 of
//                                        CustomUnitConfigParser.loadAllCustomUnitsAndMods)
//   {game}/mods/units                 -- user mods, enabled by default (getDefaultUserModsFolder
//                                        :3399-3407, loadModsFromDir(dir, true, false) :455)
//   assets/builtin_mods               -- optional built-in mods, disabled by default (:461)
//   assets/builtin_mods_enabled       -- built-in mods that ship enabled (:467)
// One entry is either a sub-directory or a bare .ini file (:404 `isDirectory || endsWith(".ini")`),
// and its display title comes from <entry>/mod-info.txt ([mod] title / description / minVersion /
// thumbnail), read by ModInfo.refreshData():467-530.
//
// The selection is persisted the same way ModManager.saveModSelection():215-230 does it -- one
// "<id>|<state>" record per mod -- but as a readable text file instead of a settings string, because
// RP has no SettingsEngine. RWX's id is Utility.truncate(name) = sha256 hex (Utility:708-710); RP
// uses FNV-1a 64 instead of pulling in a hash library. The id only has to be stable across runs and
// independent of the directory name's punctuation, which FNV-1a is.
#pragma once
#include <string>
#include <vector>

enum class ModKind {
    Core,           // assets/units -- always loaded, never toggleable
    User,           // assets/mods/units/<entry>
    Builtin,        // assets/builtin_mods/<entry> -- off unless the selection says otherwise
    BuiltinEnabled  // assets/builtin_mods_enabled/<entry> -- on unless the selection says otherwise
};

struct ModEntry {
    std::string dirName;   // directory or .ini file name, RWX ModInfo.dirName
    std::string uuid;      // stable id, RWX ModInfo.uuid
    std::string path;      // full path that custom units are loaded from
    std::string relPath;   // path relative to the assets root, for logs
    std::string title, description, minVersion, thumbnail; // from mod-info.txt
    ModKind kind = ModKind::User;
    bool enabled = true;
    int iniCount = 0;      // .ini files found under the entry (recursive)
    std::string error;     // non-empty = the entry could not be used
    bool canToggle() const { return kind != ModKind::Core; }
    const std::string& displayTitle() const { return title.empty() ? dirName : title; }
    const char* kindLabel() const {
        switch (kind) {
            case ModKind::Core: return "Shipped";
            case ModKind::User: return "Storage";
            case ModKind::Builtin: return "Built-in";
            default: return "Built-in";
        }
    }
};

class ModRegistry {
public:
    // Walks the four roots, reads every mod-info.txt and counts .ini files. Clears any previous scan.
    void scan(const std::string& assetsRoot);
    // Applies assets/mods_enabled.txt over the defaults left by scan(). A missing file means "first
    // run": the defaults are kept and written out, which is what makes builtin_mods stay off.
    void loadSelection();
    bool saveSelection() const;

    void setEnabled(size_t index, bool on);
    void disableAll();

    // Absolute paths to hand to TemplateStore: the core root first, then every enabled mod in scan
    // order, so a later mod overrides a unit name an earlier one already defined.
    std::vector<std::string> loadRoots() const;

    const std::vector<ModEntry>& all() const { return mods_; }
    std::vector<ModEntry>& mutableAll() { return mods_; }
    int enabledCount() const;
    int toggleableCount() const;
    const std::string& assetsRoot() const { return assetsRoot_; }
    const std::string& selectionPath() const { return selPath_; }

private:
    void scanRoot(const std::string& dir, ModKind kind, bool enabledByDefault);
    std::string assetsRoot_, selPath_;
    std::vector<ModEntry> mods_;
};

extern ModRegistry g_mods;

// FNV-1a 64 as 16 lowercase hex digits -- the stable stand-in for RWX's sha256(dirName).
std::string ModStableId(const std::string& text);
