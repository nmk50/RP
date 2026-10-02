#include "Pathfinding.h"
#include <queue>
#include <unordered_map>
#include <functional>

struct Node {
    int idx; float f;
    bool operator>(const Node& o) const { return f > o.f; }
};

static inline int tileOf(const TileMap& m, int x, int y) { return y * m.w + x; }

bool isWalkable(const TileMap& map, int x, int y, MoveType mt) {
    if (!map.inBounds(x, y)) return false;
    switch (mt) {
        case MoveType::AIR:
        case MoveType::HOVER: return true;
        case MoveType::WATER: return map.walkableWater(x, y);
        default: return map.walkableLand(x, y);
    }
}

std::vector<Vec2> findPath(const TileMap& map, int sx, int sy, int gx, int gy, MoveType mt) {
    std::vector<Vec2> result;
    if (!map.inBounds(sx, sy)) return result;
    // clamp goal to nearest walkable tile
    if (!isWalkable(map, gx, gy, mt)) {
        int best = -1; int bx = gx, by = gy;
        for (int r = 0; r <= 3 && best < 0; ++r)
            for (int oy = -r; oy <= r && best < 0; ++oy)
                for (int ox = -r; ox <= r && best < 0; ++ox)
                    if (map.inBounds(gx + ox, gy + oy) && isWalkable(map, gx + ox, gy + oy, mt)) { best = 1; bx = gx + ox; by = gy + oy; }
        if (best < 0) return result;
        gx = bx; gy = by;
    }
    if (mt == MoveType::AIR || mt == MoveType::HOVER) { // straight line
        result.push_back({ (sx + 0.5f) * TILE_SIZE, (sy + 0.5f) * TILE_SIZE });
        result.push_back({ (gx + 0.5f) * TILE_SIZE, (gy + 0.5f) * TILE_SIZE });
        return result;
    }

    const int W = map.w, H = map.h;
    auto octile = [&](int x, int y) {
        int dx = std::abs(x - gx), dy = std::abs(y - gy);
        return (float)(dx + dy) + (0.4142f - 2.0f) * std::min(dx, dy);
    };
    std::vector<float> gcost((size_t)W * H, 1e18f);
    std::vector<int> came((size_t)W * H, -1);
    std::vector<char> closed((size_t)W * H, 0);
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    int sIdx = tileOf(map, sx, sy), gIdx = tileOf(map, gx, gy);
    gcost[sIdx] = 0;
    open.push({ sIdx, octile(sx, sy) });

    const int dxs[8] = { 1,-1,0,0,1,1,-1,-1 };
    const int dys[8] = { 0,0,1,-1,1,-1,1,-1 };

    int found = -1;
    while (!open.empty()) {
        Node cur = open.top(); open.pop();
        int ci = cur.idx;
        if (closed[ci]) continue;
        closed[ci] = 1;
        if (ci == gIdx) { found = ci; break; }
        int cx = ci % W, cy = ci / W;
        for (int k = 0; k < 8; ++k) {
            int nx = cx + dxs[k], ny = cy + dys[k];
            if (!map.inBounds(nx, ny)) continue;
            if (!isWalkable(map, nx, ny, mt)) continue;
            // no corner-cutting through blocked diagonal
            if (k >= 4) {
                if (!isWalkable(map, cx + dxs[k], cy, mt) || !isWalkable(map, cx, cy + dys[k], mt)) continue;
            }
            int ni = tileOf(map, nx, ny);
            if (closed[ni]) continue;
            float step = (k >= 4) ? 1.4142f : 1.0f;
            if (map.at(nx, ny).cost) step += 0.6f;
            float ng = gcost[ci] + step;
            if (ng < gcost[ni]) {
                gcost[ni] = ng;
                came[ni] = ci;
                open.push({ ni, ng + octile(nx, ny) });
            }
        }
    }
    if (found < 0) return result;

    std::vector<int> path;
    for (int i = found; i != -1; i = came[i]) path.push_back(i);
    std::reverse(path.begin(), path.end());
    for (int i : path) {
        int x = i % W, y = i / W;
        result.push_back({ (x + 0.5f) * TILE_SIZE, (y + 0.5f) * TILE_SIZE });
    }
    // drop the first if equal to start
    if (result.size() > 1) result.erase(result.begin());
    return result;
}
