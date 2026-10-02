// Grid A* pathfinding with movement-type aware traversability.
#pragma once
#include <vector>
#include "Common.h"
#include "TileMap.h"
#include "UnitTemplate.h"

// Returns world-space waypoints (tile centers) from start tile to goal tile.
// Empty result = no path. Air units fly straight (ignoring terrain).
std::vector<Vec2> findPath(const TileMap& map, int sx, int sy, int gx, int gy, MoveType mt);
