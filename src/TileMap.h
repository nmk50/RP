// Map loading: parses Tiled .tmx (inline + external .tsx tilesets), decodes base64+gzip layers,
// classifies terrain by tile properties, and reads unit spawns from the Units layer.
#pragma once
#include <string>
#include <unordered_set>
#include <vector>
#include "Common.h"

enum TileColor { TC_LAND, TC_GRASS, TC_DIRT, TC_SAND, TC_SNOW, TC_ICE, TC_MOUNTAIN, TC_SHALLOW, TC_WATER, TC_DEEP, TC_LAVA, TC_VOID };

struct MapTile {
    uint8_t color = TC_VOID;
    // Mirrors RWX MapTile's flags (MapTile.createTile:209-249). `blocked` is the derived "a LAND unit
    // cannot enter" answer used by the walkability helpers; the individual flags are kept because the
    // other movement types read them separately (PathCostMap.b:320-343).
    bool water = false;       // isWater
    bool waterBridge = false; // isWaterBridge: crossable by ships, not a bridge for tanks
    bool lava = false;        // isLava
    bool cliff = false;       // isCliff -- cliff / cliff-soft / lava-cliff
    bool largeObject = false; // hasLargeObject -- large-cliff / trees
    bool blocked = false;     // movementBlockLevel -1, or cliff / large object / lava
    bool cost = false;        // movementBlockLevel 40 (small-rock): passable but slow
    bool noBuild = false;     // blocksBuildingPlacement
    bool resource = false;    // res_pool: extractor can be placed
    bool tree = false;
    bool hasUnit = false;  // occupied by static object (from Units layer handled separately)
};

struct SpawnPoint {
    std::string unit;
    // The tileset's `type` property: RWX Tree.a_(String) parses "<atlas>.<subType>" out of it, which is
    // how the misc tileset offers a hand-picked tree row (1.0 .. 1.4) next to the plain `unit=tree` roll.
    std::string type;
    int team = 0;
    int tx = 0, ty = 0;
};

// RWX Tileset: image is resolved by basename from tilesets/bitmaps/<imageKey>; the atlas is a grid of
// tileWidth x tileHeight cells with stride = tile + spacing. columns is derived from the loaded bitmap
// width at draw time (Tileset.initTextureMetrics), not from the .tsx columns attribute.
struct TilesetRender {
    int firstgid = 1;
    std::string imageKey;
    int tileWidth = 20, tileHeight = 20;
    int strideX = 20, strideY = 20;
    // Tiled's `margin`: the blank border around the atlas (units.tsx uses margin=1 spacing=1).
    int marginX = 0, marginY = 0;
};

// RWX MapLayer: raw per-tile global ids kept for rendering, in map layer order. The Units layer holds
// spawn markers (MapTile.createTile returns null for them) so it is not drawn as terrain.
struct MapRenderLayer {
    std::string name;
    bool ground = false;
    bool skip = false;
    float opacity = 1.0f;
    std::vector<uint32_t> gids; // w*h, 0 = empty
};

class TileMap {
public:
    int w = 0, h = 0;
    std::vector<MapTile> tiles; // w*h
    std::vector<int> buildOcc;  // per-tile count of buildings occupying it (RW: cost -1)
    std::vector<SpawnPoint> spawns;
    std::string name;

    // Terrain imagery (RWX Tileset / MapLayer). Empty for procedural maps -> renderer falls back to
    // flat terrain colors. Gameplay classification above is independent of these.
    std::vector<TilesetRender> tilesets;
    std::vector<MapRenderLayer> renderLayers;
    // Tileset cells whose only job is to carry a `unit` property (the editor paints a red "?" there).
    // RWX turns such a tile into a spawn and draws nothing, so the renderer has to skip these gids.
    std::unordered_set<uint32_t> unitMarkerGids;
    bool isUnitMarker(uint32_t gid) const { return unitMarkerGids.count(gid) != 0; }

    MapTile& at(int x, int y) { return tiles[y * w + x]; }
    const MapTile& at(int x, int y) const { return tiles[y * w + x]; }
    bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }

    void setSize(int nw, int nh) { w = nw; h = nh; tiles.assign((size_t)w * h, MapTile{}); buildOcc.assign((size_t)w * h, 0); }
    void markOcc(int x, int y, int delta) { if (inBounds(x, y) && (size_t)y * w + x < buildOcc.size()) buildOcc[(size_t)y * w + x] += delta; }
    bool occupied(int x, int y) const {
        if (!inBounds(x, y) || (size_t)y * w + x >= buildOcc.size()) return false;
        return buildOcc[(size_t)y * w + x] > 0;
    }

    float worldW() const { return (float)w * TILE_SIZE; }
    float worldH() const { return (float)h * TILE_SIZE; }

    bool walkableLand(int x, int y) const {
        if (!inBounds(x, y)) return false;
        const MapTile& t = tiles[y * w + x];
        return !(t.water || t.lava || t.blocked) && !occupied(x, y);
    }
    bool walkableAir(int x, int y) const { return inBounds(x, y); }
    // RWX PathCostMap.d:320-337 -- OVER_CLIFF_WATER crosses water, cliff and large rock (z, z2, z3 all
    // true); only lava and building occupancy still stop it.
    bool walkableAmphibious(int x, int y) const {
        if (!inBounds(x, y)) return false;
        const MapTile& t = tiles[y * w + x];
        return !t.lava && !occupied(x, y);
    }
    bool walkableWater(int x, int y) const {
        if (!inBounds(x, y)) return false;
        const MapTile& t = tiles[y * w + x];
        return t.water || t.waterBridge;
    }
    bool canPlaceBuilding(int x, int y) const {
        if (!inBounds(x, y)) return false;
        const MapTile& t = tiles[y * w + x];
        return !(t.water || t.lava || t.blocked || t.noBuild) && !occupied(x, y);
    }
    // RWX BaseBuilding: seaFactory / water-movement buildings must sit on water
    bool canPlaceWaterBuilding(int x, int y) const {
        if (!inBounds(x, y)) return false;
        const MapTile& t = tiles[y * w + x];
        return (t.water || t.waterBridge) && !t.lava && !t.noBuild && !occupied(x, y);
    }

    bool loadTmx(const std::string& path, std::string* err = nullptr);
};
