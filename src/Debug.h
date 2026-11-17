// In-game debug toolkit: stats/inspector panels, world overlays, time control, cheats, unit spawner.
#pragma once
#include <string>
#include <raylib.h>
#include "Game.h"

struct DebugState {
    bool stats = false;      // F1
    bool inspector = false;  // F2
    bool world = false;      // F3
    bool grid = false;       // F4
    bool paused = false;     // P
    float speed = 1.0f;      // [ / ]
    bool step = false;       // . while paused
    // F10 spawn picker: type to filter, click an entry to arm, click the map to place
    bool spawnOpen = false;
    bool spawnArmed = false;
    std::string spawnFilter;
    std::string spawnName;
    int spawnScroll = 0;
};
extern DebugState g_debug;

// Toggles, time control, cheats and the picker's text input. Call once per frame before game input.
void debugHandleKeys(Game& g);
// Left click: picker rows / armed placement. Returns true when the click was consumed.
bool debugMouseLeft(Game& g, Vector2 mouse, Vec2 world, bool overUI);
// Right click: cancels armed placement / closes the picker. Returns true when consumed.
bool debugMouseRight();
// Tile grid + terrain flags, drawn right above the map.
void debugDrawGrid(Game& g, const Camera2D& cam);
// Radii, ranges, paths, target lines, turret aims, projectile vectors. Inside BeginMode2D.
void debugDrawWorld(Game& g, const Camera2D& cam);
// Screen-space panels: stats, inspector, spawn picker, time indicator.
void debugDrawHUD(Game& g, const Camera2D& cam);