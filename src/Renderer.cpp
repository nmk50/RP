#include "Renderer.h"
#include "TileMap.h"
#include <raylib.h>
#include <cmath>

static Color terrainColor(int c) {
    switch (c) {
        case TC_GRASS:    return Color{ 96, 138, 74, 255 };
        case TC_LAND:     return Color{ 122, 128, 90, 255 };
        case TC_DIRT:     return Color{ 128, 106, 78, 255 };
        case TC_SAND:     return Color{ 196, 178, 120, 255 };
        case TC_SNOW:     return Color{ 228, 232, 238, 255 };
        case TC_ICE:      return Color{ 190, 214, 224, 255 };
        case TC_MOUNTAIN: return Color{ 110, 108, 104, 255 };
        case TC_SHALLOW:  return Color{ 70, 130, 180, 255 };
        case TC_WATER:    return Color{ 40, 96, 160, 255 };
        case TC_DEEP:     return Color{ 20, 60, 120, 255 };
        case TC_LAVA:     return Color{ 200, 70, 30, 255 };
        default:          return Color{ 40, 40, 40, 255 };
    }
}

static Color teamPal[8] = {
    { 90, 140, 240, 255 },   // blue (player)
    { 220, 70, 70, 255 },    // red
    { 80, 190, 90, 255 },    // green
    { 230, 200, 70, 255 },   // yellow
    { 200, 100, 220, 255 },  // purple
    { 240, 150, 60, 255 },   // orange
    { 70, 200, 210, 255 },   // cyan
    { 220, 120, 130, 255 },  // pink
};
Color Renderer::teamColor(int t) { return teamPal[((t % 8) + 8) % 8]; }

static std::string resolveImage(const UnitTemplate& t) {
    if (t.image.empty() || t.image == "NONE" || t.image == "AUTO") return "";
    std::string nm = t.image;
    auto strip = [&](const std::string& pfx) -> std::string {
        if (nm.rfind(pfx, 0) == 0) return nm.substr(pfx.size()); return nm;
    };
    std::vector<std::string> cands = {
        strip("SHARED:"),
    };
    std::string sh = strip("SHARED:");
    std::string paths[] = {
        t.sourceDir + "/" + nm,
        g_assetsRoot + "/drawable/" + sh,
        g_assetsRoot + "/drawable/" + nm,
        g_assetsRoot + "/units/shared/" + sh,
        t.sourceDir + "/" + sh,
    };
    for (auto& p : paths) if (FileExists(p.c_str())) return p;
    return "";
}

void Renderer::init() {
    Image im = GenImageColor(16, 16, MAGENTA);
    missing_ = LoadTextureFromImage(im);
    UnloadImage(im);
}
void Renderer::close() {
    for (auto& kv : cache_) UnloadTexture(kv.second);
    cache_.clear();
    UnloadTexture(missing_);
}

bool Renderer::loaded(const UnitTemplate& t, int which, Texture2D& out) {
    std::string nm;
    if (which == 0) nm = t.image;
    else if (which == 1) nm = t.imageTurret;
    else nm = t.imageWreak;
    if (nm.empty() || nm == "NONE" || nm == "AUTO") return false;
    std::string key = nm + "|" + t.sourceDir + "|" + std::to_string(which);
    auto it = cache_.find(key);
    if (it != cache_.end()) { out = it->second; return it->second.id != missing_.id; }
    // resolve
    UnitTemplate tmp = t;
    if (which == 1) { tmp.image = t.imageTurret; } else if (which == 2) { tmp.image = t.imageWreak; }
    std::string path = resolveImage(tmp);
    Texture2D tex = missing_;
    if (!path.empty()) { tex = LoadTexture(path.c_str()); }
    cache_[key] = tex;
    out = tex;
    return tex.id != missing_.id;
}

static void worldViewCull(const Camera2D& cam, float sw, float sh, float& x0, float& y0, float& x1, float& y1) {
    Vector2 tl = GetScreenToWorld2D({ 0.0f, 0.0f }, cam);
    Vector2 br = GetScreenToWorld2D({ sw, sh }, cam);
    x0 = tl.x; y0 = tl.y; x1 = br.x; y1 = br.y;
}

void Renderer::drawMap(Game& g, const Camera2D& cam) {
    float x0, y0, x1, y1; worldViewCull(cam, (float)GetScreenWidth(), (float)GetScreenHeight(), x0, y0, x1, y1);
    int tx0 = std::max(0, (int)(x0 / TILE_SIZE) - 1), ty0 = std::max(0, (int)(y0 / TILE_SIZE) - 1);
    int tx1 = std::min(g.map.w - 1, (int)(x1 / TILE_SIZE) + 1), ty1 = std::min(g.map.h - 1, (int)(y1 / TILE_SIZE) + 1);
    for (int y = ty0; y <= ty1; ++y)
        for (int x = tx0; x <= tx1; ++x) {
            const MapTile& t = g.map.at(x, y);
            Color c = terrainColor(t.color);
            if (t.blocked) c = Color{ (uint8_t)(c.r * 0.6f), (uint8_t)(c.g * 0.6f), (uint8_t)(c.b * 0.6f), 255 };
            DrawRectangle(x * TILE_SIZE, y * TILE_SIZE, TILE_SIZE, TILE_SIZE, c);
            if (t.resource) DrawRectangle(x * TILE_SIZE + 4, y * TILE_SIZE + 4, TILE_SIZE - 8, TILE_SIZE - 8, Color{ 150, 220, 255, 120 });
            if (t.tree) DrawCircle((x + 0.5f) * TILE_SIZE, (y + 0.5f) * TILE_SIZE, 5, Color{ 30, 80, 30, 200 });
        }
    // world border
    DrawRectangleLines(0, 0, g.map.worldW(), g.map.worldH(), BLACK);
}

// Draw one frame of a horizontal sprite-sheet (RWX total_frames) centered at pos, rotated rotDeg (clockwise).
static void drawSpriteFrame(const Texture2D& tex, int frames, int frame, Vector2 pos, float targetW, float rotDeg, Color col) {
    if (frames < 1) frames = 1;
    if (frame < 0 || frame >= frames) frame = 0;
    float fw = tex.width / (float)frames;
    Rectangle src{ frame * fw, 0, fw, (float)tex.height };
    float w = targetW, h = targetW * (tex.height / (fw > 0 ? fw : 1));
    Rectangle dst{ pos.x, pos.y, w, h };
    Vector2 org{ w / 2, h / 2 };
    DrawTexturePro(tex, src, dst, org, rotDeg, col);
}

// RWX sprites face up (north); screen angle a (y-down) maps to raylib clockwise rotation a_deg + 90.
static float spriteRot(float angleRad) { return angleRad * 180.0f / 3.14159265f + 90.0f; }

void Renderer::drawUnit(Game& g, const Unit& u, const Camera2D& cam) {
    if (!u.tpl) return;
    const UnitTemplate& t = *u.tpl;
    float r = std::max(6.0f, t.displayRadius);
    float scale = (r * 2.0f);
    Color col = WHITE;
    if (u.underConstruction && u.tpl->isBuilding) col = Color{ 255, 255, 255, 140 };
    if (t.isStatic()) col = Fade(col, 1.0f);

    int frames = std::max(1, t.totalFrames);
    int anim = frames > 1 ? (int)(GetTime() * 5.0f) % frames : 0;

    Texture2D tex;
    Vector2 pv{ u.pos.x, u.pos.y };
    if (loaded(t, 0, tex)) {
        int ef = (frames > 1 && tex.width % frames == 0) ? frames : 1;
        float rot = t.isStatic() ? 0 : spriteRot(u.facing);
        drawSpriteFrame(tex, ef, ef > 1 ? anim % ef : 0, pv, scale * t.imageScale, rot, col);
    } else {
        DrawCircleV(pv, r, Fade(teamColor(u.team), 0.8f));
    }
    // turret
    if (!t.imageTurret.empty() && t.imageTurret != "NONE") {
        Texture2D tur;
        if (loaded(t, 1, tur)) {
            int ef = (frames > 1 && tur.width % frames == 0) ? frames : 1;
            float rot = spriteRot(u.turretRot);
            drawSpriteFrame(tur, ef, ef > 1 ? anim % ef : 0, pv, scale * 0.7f * t.turretImageScale, rot, col);
        }
    }
    // team ring under selection
    if (u.selected) {
        DrawCircleLines((int)pv.x, (int)pv.y, r + 3, Color{ 60, 220, 90, 255 });
    }
    // hp bar
    if (u.hp < u.maxHp || u.selected || t.isBuilding) {
        float bw = r * 2.0f, bh = 4;
        float frac = std::max(0.0f, u.hp / u.maxHp);
        Vector2 p{ pv.x - r, pv.y - (r + 8) };
        DrawRectangle((int)p.x, (int)p.y, (int)bw, (int)bh, Color{ 30, 30, 30, 200 });
        Color hc = u.team == g.playerTeam() ? Color{ 70, 210, 90, 255 } : Color{ 220, 80, 70, 255 };
        DrawRectangle((int)p.x, (int)p.y, (int)(bw * frac), (int)bh, hc);
        if (t.isBuilding && u.underConstruction) {
            float bp = std::min(1.0f, u.buildProgress);
            DrawRectangle((int)p.x, (int)p.y + 5, (int)(bw * bp), 3, Color{ 240, 220, 90, 255 });
        }
    }
}

void Renderer::drawWorld(Game& g, const ViewState& view) {
    BeginMode2D(view.cam);
    drawMap(g, view.cam);

    // under-construction + normal units sorted so buildings draw first
    for (auto& u : g.units) if (u.tpl && u.tpl->isStatic()) drawUnit(g, u, view.cam);
    for (auto& u : g.units) if (u.tpl && !u.tpl->isStatic()) drawUnit(g, u, view.cam);

    // projectiles
    for (auto& p : g.projectiles) {
        float s = 3.0f * p.drawSize;
        Color pc = p.team == g.playerTeam() ? Color{ 255, 240, 120, 255 } : Color{ 255, 150, 90, 255 };
        DrawCircleV(Vector2{ p.pos.x, p.pos.y }, s, pc);
    }
    // effects
    for (auto& e : g.effects) {
        float prog = e.t / e.dur;
        float rad = e.radius * (0.5f + prog);
        Color ec = Color{ (uint8_t)(255), (uint8_t)(200 - 120 * prog), (uint8_t)(120 - 100 * prog), (uint8_t)(220 * (1 - prog)) };
        DrawCircleV(Vector2{ e.pos.x, e.pos.y }, rad, ec);
    }
    // command marker
    if (view.cmdMarkerOn) {
        float a = 1.0f - view.cmdMarkerT / 0.6f;
        DrawCircleLines((int)view.cmdMarker.x, (int)view.cmdMarker.y, 14 * (1 + (1 - a)), Fade(WHITE, std::max(0.0f, a)));
    }
    // placement ghost
    if (view.placing) {
        const UnitTemplate* t = g_templates.find(ToLower(view.placingName));
        if (t) {
            Color gc = view.placingValid ? Color{ 90, 220, 90, 120 } : Color{ 220, 80, 80, 120 };
            int fw = t->footprintTilesW(), fh = t->footprintTilesH();
            int cx = (int)(view.placingWorld.x / TILE_SIZE), cy = (int)(view.placingWorld.y / TILE_SIZE);
            int minx = cx + t->footL, miny = cy + t->footU;
            for (int y = miny; y < miny + fh; ++y) for (int x = minx; x < minx + fw; ++x)
                DrawRectangle(x * TILE_SIZE, y * TILE_SIZE, TILE_SIZE, TILE_SIZE, gc);
        }
    }
    EndMode2D();
}

bool Renderer::unitTexture(const std::string& name, int which, Texture2D& out) {
    const UnitTemplate* t = g_templates.find(ToLower(name));
    if (!t) return false;
    return loaded(*t, which, out);
}
