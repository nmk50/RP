#include "TileMap.h"
#include <tinyxml2.h>
#include <raylib.h>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unordered_map>

using namespace tinyxml2;

struct TileSetInfo {
    int firstgid = 1;
    std::string name;
    std::unordered_map<int, std::unordered_map<std::string, std::string>> props; // localId -> (propName->value)
};

static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

static std::string dirName(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

static void collectTiles(XMLNode* tilesetNode, TileSetInfo& ts) {
    for (XMLElement* tn = tilesetNode->FirstChildElement("tile"); tn; tn = tn->NextSiblingElement("tile")) {
        int id = 0; tn->QueryIntAttribute("id", &id);
        std::unordered_map<std::string, std::string> m;
        XMLElement* props = tn->FirstChildElement("properties");
        if (props) {
            for (XMLElement* pe = props->FirstChildElement("property"); pe; pe = pe->NextSiblingElement("property")) {
                const char* nm = pe->Attribute("name");
                const char* vl = pe->Attribute("value");
                if (nm) m[ToLower(nm)] = vl ? vl : "1";
            }
        }
        ts.props[id] = m;
    }
}

static TileColor colorFromName(const std::string& lname) {
    auto has = [&](const char* s) { return lname.find(s) != std::string::npos; };
    if (has("deep")) return TC_DEEP;
    if (has("shallow")) return TC_SHALLOW;
    if (has("water")) return TC_WATER;
    if (has("lava")) return TC_LAVA;
    if (has("snow")) return TC_SNOW;
    if (has("ice")) return TC_ICE;
    if (has("sand")) return TC_SAND;
    if (has("mountain")) return TC_MOUNTAIN;
    if (has("dirt")) return TC_DIRT;
    if (has("grass")) return TC_GRASS;
    return TC_LAND;
}

static void applyTile(TileMap& map, int x, int y, const TileSetInfo& ts, uint32_t gid, bool replace) {
    if (!map.inBounds(x, y)) return;
    int local = (int)(gid - (uint32_t)ts.firstgid);
    std::string ln = ToLower(ts.name);
    const auto& pm = ts.props.count(local) ? ts.props.at(local) : std::unordered_map<std::string, std::string>{};
    auto hasProp = [&](const char* k) { return pm.find(k) != pm.end(); };

    MapTile t;
    t.color = colorFromName(ln);
    if (t.color == TC_WATER || t.color == TC_DEEP || t.color == TC_SHALLOW) t.water = true;
    if (t.color == TC_LAVA) t.lava = true;
    if (hasProp("water")) t.water = true;
    if (hasProp("lava")) { t.lava = true; t.water = false; }
    if (hasProp("cliff") || hasProp("large-rock") || hasProp("largerock")) t.blocked = true;
    if (hasProp("small-rock") || hasProp("smallrock")) t.cost = true;
    if (hasProp("res_pool") || hasProp("resource") || hasProp("respool")) { t.resource = true; }
    if (hasProp("tree")) t.tree = true;

    auto unitIt = pm.find("unit");
    if (unitIt != pm.end() && !unitIt->second.empty()) {
        SpawnPoint sp; sp.unit = unitIt->second;
        auto teamIt = pm.find("team");
        sp.team = teamIt != pm.end() ? atoi(teamIt->second.c_str()) : 0;
        auto cu = pm.find("customunit");
        if (cu != pm.end() && !cu->second.empty()) sp.unit = cu->second;
        sp.tx = x; sp.ty = y;
        map.spawns.push_back(sp);
        return;
    }

    if (replace) { map.at(x, y) = t; }
    else {
        MapTile& d = map.at(x, y);
        if (d.color == TC_VOID) d.color = t.color;
        d.water = d.water || t.water;
        d.lava = d.lava || t.lava;
        d.blocked = d.blocked || t.blocked;
        d.cost = d.cost || t.cost;
        d.resource = d.resource || t.resource;
        d.tree = d.tree || t.tree;
    }
}

static const TileSetInfo* findTileset(const std::vector<TileSetInfo>& sets, uint32_t gid) {
    const TileSetInfo* best = nullptr;
    for (auto& s : sets) if (s.firstgid > 0 && (uint32_t)s.firstgid <= gid)
        if (!best || s.firstgid > best->firstgid) best = &s;
    return best;
}

static std::vector<uint32_t> decodeData(XMLElement* data, int w, int h) {
    std::vector<uint32_t> out((size_t)w * h, 0);
    if (!data) return out;
    const char* enc = data->Attribute("encoding");
    const char* comp = data->Attribute("compression");
    std::string text = data->GetText() ? data->GetText() : "";
    // gather all text nodes (tinyxml may split)
    std::string all; for (XMLNode* n = data->FirstChild(); n; n = n->NextSibling()) { if (XMLText* t = n->ToText()) all += t->Value(); }
    if (all.empty()) all = text;
    if (enc && std::string(enc) == "csv") {
        std::stringstream ss(all); std::string tok; int i = 0;
        while (std::getline(ss, tok, ',')) { tok = Trim(tok); if (!tok.empty() && i < (int)out.size()) out[i++] = (uint32_t)strtoul(tok.c_str(), nullptr, 10); }
        return out;
    }
    // base64 (possibly gzip)
    std::vector<uint8_t> bytes = Base64Decode(all);
    std::vector<uint8_t> raw;
    if (comp && ToLower(comp) == "gzip") raw = GzipInflate(bytes);
    else raw = bytes; // uncompressed base64 (concatenated uint32 LE)
    int count = (int)(raw.size() / 4);
    for (int i = 0; i < count && i < (int)out.size(); ++i) {
        uint32_t g = raw[i * 4] | (raw[i * 4 + 1] << 8) | (raw[i * 4 + 2] << 16) | ((uint32_t)raw[i * 4 + 3] << 24);
        out[i] = g & 0x0FFFFFFF; // strip flip flags
    }
    return out;
}

bool TileMap::loadTmx(const std::string& path, std::string* err) {
    std::string text = readFile(path);
    if (text.empty()) { if (err) *err = "cannot read " + path; return false; }
    XMLDocument doc;
    if (doc.Parse(text.c_str(), text.size()) != XML_SUCCESS) { if (err) *err = "xml parse error"; return false; }
    XMLElement* map = doc.FirstChildElement("map");
    if (!map) { if (err) *err = "no <map>"; return false; }
    map->QueryIntAttribute("width", &w);
    map->QueryIntAttribute("height", &h);
    {
        std::string bn = path.substr(path.find_last_of("/\\") + 1);
        size_t dot = bn.rfind('.');
        if (dot != std::string::npos) bn = bn.substr(0, dot);
        name = bn;
    }
    if (w <= 0 || h <= 0) { if (err) *err = "bad dims"; return false; }
    tiles.assign((size_t)w * h, MapTile{});
    buildOcc.assign((size_t)w * h, 0);

    std::string base = dirName(path);
    std::vector<TileSetInfo> sets;
    for (XMLElement* tsE = map->FirstChildElement("tileset"); tsE; tsE = tsE->NextSiblingElement("tileset")) {
        TileSetInfo ts;
        tsE->QueryIntAttribute("firstgid", &ts.firstgid);
        const char* src = tsE->Attribute("source");
        const char* nm = tsE->Attribute("name");
        if (src) {
            std::string lp = src;
            // resolve relative to map dir, else to assets/tilesets
            std::string cand1 = base + "/" + lp;
            std::string cand2 = g_assetsRoot + "/tilesets/" + lp;
            std::string use;
            if (!readFile(cand1).empty()) use = cand1; else if (!readFile(cand2).empty()) use = cand2;
            XMLDocument tdoc;
            if (!use.empty() && tdoc.LoadFile(use.c_str()) == XML_SUCCESS) {
                XMLElement* troot = tdoc.FirstChildElement("tileset");
                if (troot) {
                    const char* tn = troot->Attribute("name");
                    ts.name = nm ? nm : (tn ? tn : "unknown");
                    collectTiles(troot, ts);
                }
            } else if (nm) ts.name = nm;
        } else {
            ts.name = nm ? nm : "unknown";
            collectTiles(tsE, ts);
        }
        sets.push_back(ts);
    }
    std::sort(sets.begin(), sets.end(), [](const TileSetInfo& a, const TileSetInfo& b) { return a.firstgid < b.firstgid; });

    auto processLayer = [&](XMLElement* layer) {
        std::string L = ToLower(layer->Attribute("name") ? layer->Attribute("name") : "");
        bool replace = (L == "ground");
        XMLElement* data = layer->FirstChildElement("data");
        int lw = w, lh = h;
        layer->QueryIntAttribute("width", &lw); layer->QueryIntAttribute("height", &lh);
        std::vector<uint32_t> gids = decodeData(data, lw, lh);
        for (int y = 0; y < lh && y < h; ++y) for (int x = 0; x < lw && x < w; ++x) {
            uint32_t gid = gids[(size_t)y * lw + x];
            if (gid == 0) continue;
            const TileSetInfo* ts = findTileset(sets, gid);
            if (!ts) continue;
            applyTile(*this, x, y, *ts, gid, replace);
        }
    };

    for (XMLElement* layer = map->FirstChildElement("layer"); layer; layer = layer->NextSiblingElement("layer"))
        processLayer(layer);
    return true;
}
