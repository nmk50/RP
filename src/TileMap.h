// Map loading: parses Tiled .tmx (inline + external .tsx tilesets), decodes base64+gzip layers,
// classifies terrain by tile properties, and reads unit spawns from the Units layer.
#pragma once
#include <string>
#include <vector>
#include "Common.h"

enum TileColor { TC_LAND, TC_GRASS, TC_DIRT, TC_SAND, TC_SNOW, TC_ICE, TC_MOUNTAIN, TC_SHALLOW, TC_WATER, TC_DEEP, TC_LAVA, TC_VOID };

struct MapTile {
    uint8_t color = TC_VOID;
    bool water = false;    // blocks land movement
    bool lava = false;     // blocks all ground
    bool blocked = false;  // cliff / large rock: blocks land
    bool cost = false;     // small rock: passable, higher cost
    bool resource = false; // res_pool: extractor can be placed
    bool tree = false;
    bool hasUnit = false;  // occupied by static object (from Units layer handled separately)
};

struct SpawnPoint {
    std::string unit;
    int team = 0;
    int tx = 0, ty = 0;
};

class TileMap {
public:
    int w = 0, h = 0;
    std::vector<MapTile> tiles; // w*h
    std::vector<int> buildOcc;  // per-tile count of buildings occupying it (RW: cost -1)
    std::vector<SpawnPoint> spawns;
    std::string name;

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
    bool walkableWater(int x, int y) const {
        if (!inBounds(x, y)) return false;
        return tiles[y * w + x].water;
    }
    bool canPlaceBuilding(int x, int y) const {
        if (!inBounds(x, y)) return false;
        const MapTile& t = tiles[y * w + x];
        return !(t.water || t.lava || t.blocked) && !occupied(x, y);
    }

    bool loadTmx(const std::string& path, std::string* err = nullptr);
};
