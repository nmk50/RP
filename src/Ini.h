// RWX-style .ini parser (header-only). Grammar: [section] headers, key:value / key=value,
// '#' comments, and dynamic sections like [turret_1]. Values keep original case.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include "Common.h"

struct IniSection {
    std::string name;
    std::vector<std::pair<std::string, std::string>> kv; // preserves order, allows override tracking
    std::map<std::string, std::string> idx;              // lowercased key -> value

    void set(const std::string& k, const std::string& v) {
        std::string lk = ToLower(k);
        idx[lk] = v;
        // keep ordered list too
        for (auto& p : kv) if (ToLower(p.first) == lk) { p.second = v; return; }
        kv.push_back({ k, v });
    }
    bool has(const std::string& k) const { return idx.count(ToLower(k)) > 0; }
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = idx.find(ToLower(k)); return it == idx.end() ? def : it->second;
    }
    double getNum(const std::string& k, double def = 0) const {
        auto it = idx.find(ToLower(k)); if (it == idx.end()) return def;
        try { return std::stod(it->second); } catch (...) { return def; }
    }
    bool getBool(const std::string& k, bool def = false) const {
        auto it = idx.find(ToLower(k)); if (it == idx.end()) return def;
        std::string v = ToLower(Trim(it->second));
        return v == "true" || v == "1" || v == "yes";
    }
};

struct IniFile {
    std::vector<IniSection> sections;
    IniSection* find(const std::string& name) {
        std::string ln = ToLower(name);
        for (auto& s : sections) if (ToLower(s.name) == ln) return &s;
        return nullptr;
    }
    IniSection& getOrCreate(const std::string& name) {
        if (IniSection* s = find(name)) return *s;
        sections.push_back({}); sections.back().name = name; return sections.back();
    }
    // All sections whose name starts with prefix (case-insensitive), e.g. "turret_".
    std::vector<IniSection*> startingWith(const std::string& prefix) {
        std::vector<IniSection*> out; std::string lp = ToLower(prefix);
        for (auto& s : sections) if (ToLower(s.name).rfind(lp, 0) == 0) out.push_back(&s);
        return out;
    }
};

inline bool ParseIni(const std::string& path, IniFile& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    std::string text = ss.str();
    // strip UTF-8 BOM
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text = text.substr(3);
    std::istringstream in(text);
    std::string line;
    IniSection* cur = nullptr;
    bool inTriple = false;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (!inTriple) {
            if (line.empty()) continue;
            if (line[0] == '#') continue;
            if (line.rfind("\"\"\"", 0) == 0) { inTriple = true; continue; }
            if (line[0] == '[' && line.back() == ']') {
                std::string nm = Trim(line.substr(1, line.size() - 2));
                cur = &out.getOrCreate(nm);
                continue;
            }
            size_t sep = line.find(':');
            size_t eq = line.find('=');
            size_t pos = std::string::npos;
            if (sep != std::string::npos && (eq == std::string::npos || sep < eq)) pos = sep;
            else if (eq != std::string::npos) pos = eq;
            if (pos == std::string::npos) continue;
            std::string k = Trim(line.substr(0, pos));
            std::string v = Trim(line.substr(pos + 1));
            // strip trailing inline comment only if v not numeric-ish with #; keep #color values
            if (!k.empty() && cur) cur->set(k, v);
        } else {
            if (line.rfind("\"\"\"", 0) == 0) inTriple = false;
        }
    }
    return true;
}
