#include "Renderer.h"
#include "TileMap.h"
#include "Debug.h"
#include <raylib.h>
#include <cmath>
#include <algorithm>
#include <vector>

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

// RWX PlayerTeam.i(int):1383-1391 hands out the ten colours of TeamColoring.h, addressed by the team's
// colour index -- which is why the green team is the one that sees the art exactly as drawn.
Color Renderer::teamColor(int t) {
    uint32_t a = kTeamColors[teamColorIndex(t)];
    return Color{ (uint8_t)((a >> 16) & 0xFF), (uint8_t)((a >> 8) & 0xFF), (uint8_t)(a & 0xFF),
                  (uint8_t)((a >> 24) & 0xFF) };
}

static std::string resolveImage(const UnitTemplate& t, bool& shadow) {
    shadow = false;
    if (t.image.empty() || t.image == "NONE" || t.image == "AUTO") return "";
    std::string nm = t.image;
    auto strip = [&](const std::string& pfx) {
        if (nm.rfind(pfx, 0) != 0) return false;
        nm = nm.substr(pfx.size());
        return true;
    };
    shadow = strip("SHADOW:");
    bool shared = strip("SHARED:");
    bool core = strip("CORE:");
    shadow = strip("SHADOW:") || shadow;
    if (core) {
        std::string path = g_assetsRoot + "/units/" + nm;
        return FileExists(path.c_str()) ? path : "";
    }
    std::string raw = shared ? "SHARED:" + nm : nm;
    std::string paths[] = {
        t.sourceDir + "/" + raw,
        g_assetsRoot + "/drawable/" + nm,
        g_assetsRoot + "/drawable/" + raw,
        g_assetsRoot + "/units/shared/" + nm,
        t.sourceDir + "/" + nm,
    };
    for (auto& p : paths) if (FileExists(p.c_str())) return p;
    return "";
}

static void blackenImage(Image& im) {
    ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Color* px = (Color*)im.data;
    for (int i = 0; i < im.width * im.height; ++i) px[i].r = px[i].g = px[i].b = 0;
}

static Texture2D loadResolvedTexture(const std::string& path, bool shadow) {
    if (!shadow) return LoadTexture(path.c_str());
    Image im = LoadImage(path.c_str());
    if (!im.data || im.width <= 0 || im.height <= 0) { UnloadImage(im); return {}; }
    blackenImage(im);
    Texture2D tex = LoadTextureFromImage(im);
    UnloadImage(im);
    return tex;
}

void Renderer::init() {
    Image im = GenImageColor(16, 16, MAGENTA);
    missing_ = LoadTextureFromImage(im);
    UnloadImage(im);
}
void Renderer::close() {
    for (auto& kv : cache_) if (kv.second.id != missing_.id) UnloadTexture(kv.second);
    cache_.clear();
    for (auto& kv : teamCache_) if (kv.second.id != missing_.id) UnloadTexture(kv.second);
    teamCache_.clear();
    for (auto& kv : tilesetCache_) if (kv.second.id != missing_.id) UnloadTexture(kv.second);
    tilesetCache_.clear();
    for (auto& kv : iconCache_) if (kv.second.id) UnloadTexture(kv.second);
    iconCache_.clear();
    UnloadTexture(missing_);
}

bool Renderer::loaded(const UnitTemplate& t, int which, Texture2D& out) {
    std::string nm;
    if (which == 0) nm = t.image;
    else if (which == 1) return byName(t, t.imageTurret, out);
    else nm = t.imageWreak;
    if (nm.empty() || nm == "NONE" || nm == "AUTO") return false;
    std::string key = nm + "|" + t.sourceDir + "|" + std::to_string(which);
    auto it = cache_.find(key);
    if (it != cache_.end()) { out = it->second; return it->second.id != missing_.id; }
    // resolve
    UnitTemplate tmp = t;
    if (which == 2) { tmp.image = t.imageWreak; }
    bool shadow;
    std::string path = resolveImage(tmp, shadow);
    Texture2D tex = path.empty() ? missing_ : loadResolvedTexture(path, shadow);
    if (tex.id == 0) tex = missing_;
    if (tex.id == missing_.id)
        TraceLog(LOG_WARNING, "missing image: %s (unit %s, which %d)", nm.c_str(), t.name.c_str(), which);
    cache_[key] = tex;
    out = tex;
    return tex.id != missing_.id;
}

// RWX CustomUnit.d(i):691 - a barrel with [turret_N] image draws that sprite, and barrels that name
// the same file share one load.
bool Renderer::byName(const UnitTemplate& t, const std::string& nm, Texture2D& out) {
    if (nm.empty() || nm == "NONE" || nm == "AUTO") return false;
    std::string key = nm + "|" + t.sourceDir + "|1";
    auto it = cache_.find(key);
    if (it != cache_.end()) { out = it->second; return it->second.id != missing_.id; }
    UnitTemplate tmp = t;
    tmp.image = nm;
    bool shadow;
    std::string path = resolveImage(tmp, shadow);
    Texture2D tex = path.empty() ? missing_ : loadResolvedTexture(path, shadow);
    if (tex.id == 0) tex = missing_;
    cache_[key] = tex;
    out = tex;
    return tex.id != missing_.id;
}

// RWX PlayerTeam.getTeamColorTextures(texture, mode, lazy):1414-1419 -> b():1445-1507: ten variants of one
// image, dyed per pixel by TeamColoring, with slot 0 left as the source texture itself (:1503) and a
// disabled mode giving ten raw copies (:1447). RP bakes the same pixels on demand.
bool Renderer::teamed(const UnitTemplate& t, const std::string& nm, int which, int colorIdx, Texture2D& out) {
    if (nm.empty() || nm == "NONE" || nm == "AUTO") return false;
    if (colorIdx <= 0 || colorIdx >= 10 || t.teamColoringMode == kTCModeDisabled)
        return which == 0 ? loaded(t, 0, out) : byName(t, nm, out);
    std::string key = nm + "|" + t.sourceDir + "|" + std::to_string(which)
                    + "|c" + std::to_string(colorIdx) + "|m" + std::to_string(t.teamColoringMode);
    auto it = teamCache_.find(key);
    if (it != teamCache_.end()) { out = it->second; return true; }
    UnitTemplate tmp = t;
    tmp.image = nm;
    bool shadow;
    std::string path = resolveImage(tmp, shadow);
    if (path.empty()) return false;
    Image im = LoadImage(path.c_str());
    if (!im.data || im.width <= 0 || im.height <= 0) { UnloadImage(im); return false; }
    if (shadow) blackenImage(im);
    else ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Color* px = (Color*)im.data;
    uint32_t dye = kTeamColors[colorIdx];
    for (int i = 0; i < im.width * im.height; ++i) {
        uint32_t argb = (uint32_t(px[i].a) << 24) | (uint32_t(px[i].r) << 16)
                      | (uint32_t(px[i].g) << 8) | uint32_t(px[i].b);
        argb = teamColourArgb(argb, t.teamColoringMode, dye);
        px[i].a = (uint8_t)((argb >> 24) & 0xFF); px[i].r = (uint8_t)((argb >> 16) & 0xFF);
        px[i].g = (uint8_t)((argb >> 8) & 0xFF);  px[i].b = (uint8_t)(argb & 0xFF);
    }
    Texture2D tex = LoadTextureFromImage(im);
    UnloadImage(im);
    teamCache_[key] = tex;
    out = tex;
    return true;
}

bool Renderer::shared(const std::string& file, Texture2D& out) {
    std::string key = "@drawable/" + file;
    auto it = cache_.find(key);
    if (it != cache_.end()) { out = it->second; return it->second.id != missing_.id; }
    std::string path = g_assetsRoot + "/drawable/" + file;
    Texture2D tex = missing_;
    if (FileExists(path.c_str())) tex = LoadTexture(path.c_str());
    cache_[key] = tex;
    out = tex;
    return tex.id != missing_.id;
}

bool Renderer::tilesetTexture(const TilesetRender& ts, Texture2D& out, int& columns) {
    auto it = tilesetCache_.find(ts.imageKey);
    if (it != tilesetCache_.end()) {
        out = it->second;
        columns = std::max(1, it->second.width / std::max(1, ts.strideX));
        return it->second.id != missing_.id;
    }
    std::string path = g_assetsRoot + "/tilesets/bitmaps/" + ts.imageKey;
    Texture2D tex = missing_;
    if (FileExists(path.c_str())) tex = LoadTexture(path.c_str());
    if (tex.id == 0) tex = missing_;
    tilesetCache_[ts.imageKey] = tex;
    out = tex;
    columns = std::max(1, tex.width / std::max(1, ts.strideX));
    return tex.id != missing_.id;
}

// RWX MapTile.createTile:251-300 + TileMap.getTileVariant:592-598 -- certain base tiles have a small
// variant strip; the decompiled selector always picks variant[0] when ((x*13+y*1313)%(count+1))-1 >= 0.
static int variantLocalIndex(const std::string& key, int local, int x, int y) {
    int count = 0, offset = 0;
    if (local == 0) {
        if (key == "shallowwater.png") count = 5;
        else if (key == "deepwater.png" || key == "water.png" || key == "snow.png") count = 2;
        else if (key == "longgrass.png" || key == "mountain.png") count = 3;
    } else if (local == 7 && key == "stone.png") { count = 4; offset = 23; }
    if (count > 0 && ((x * 13 + y * 1313) % (count + 1)) - 1 >= 0)
        return local + 1 + offset;
    return local;
}

static void worldViewCull(const Camera2D& cam, float sw, float sh, float& x0, float& y0, float& x1, float& y1) {
    Vector2 tl = GetScreenToWorld2D({ 0.0f, 0.0f }, cam);
    Vector2 br = GetScreenToWorld2D({ sw, sh }, cam);
    x0 = tl.x; y0 = tl.y; x1 = br.x; y1 = br.y;
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

// RWX CustomUnit.getRenderRotation:2338-2349 takes the turret's rendered angle (movementLevels[main
// TurretIndex].targetX) in place of the hull's rotation for every unit that sets
// lock_body_rotation_with_main_turret, and getRenderRotation(true) does the same for the shadow when
// lock_shadow_rotation_with_main_turret is on. No RP asset uses [attack] setMainTurretAs, so turret 0
// is the main turret the parser would pick (CustomUnitConfigParser.java:2201-2206).
static float unitRenderAngle(const Unit& u, bool shadow) {
    const UnitTemplate& t = *u.tpl;
    if ((shadow ? t.lockShadowRot : t.lockBodyRot) && !u.tur.empty()) return u.tur[0].angle;
    return u.facing;
}

static int submergedRenderAlpha(float z) {
    if (z < -0.3f) return std::max(45, (int)(((1.0f - ((-z) / 10.0f)) * 130.0f) + 45.0f));
    return 255;
}

static Color unitRenderPaint(const Unit& u) {
    Color col{ 255, 255, 255, (uint8_t)submergedRenderAlpha(u.posZ) };
    if (u.underConstruction && u.buildProgress < 1.0f)
        col = Color{ 140, 255, 140, (uint8_t)(20.0f + 220.0f * std::max(0.0f, u.buildProgress)) };
    return col;
}

void Renderer::drawMap(Game& g, const Camera2D& cam) {
    float x0, y0, x1, y1; worldViewCull(cam, (float)GetScreenWidth(), (float)GetScreenHeight(), x0, y0, x1, y1);
    int tx0 = std::max(0, (int)(x0 / TILE_SIZE) - 1), ty0 = std::max(0, (int)(y0 / TILE_SIZE) - 1);
    int tx1 = std::min(g.map.w - 1, (int)(x1 / TILE_SIZE) + 1), ty1 = std::min(g.map.h - 1, (int)(y1 / TILE_SIZE) + 1);

    bool realTerrain = !g.map.renderLayers.empty() && !g.map.tilesets.empty();
    if (realTerrain) {
        // RWX renders each MapLayer in order: opaque Ground first, then Items / decoration over it.
        DrawRectangle(0, 0, (int)g.map.worldW(), (int)g.map.worldH(), Color{ 24, 24, 24, 255 });
        std::vector<int> firsts;
        firsts.reserve(g.map.tilesets.size());
        for (auto& ts : g.map.tilesets) firsts.push_back(ts.firstgid);
        auto findTs = [&](uint32_t gid) -> const TilesetRender* {
            int idx = (int)(std::upper_bound(firsts.begin(), firsts.end(), (int)gid) - firsts.begin()) - 1;
            if (idx < 0) return nullptr;
            return &g.map.tilesets[idx];
        };
        for (const auto& layer : g.map.renderLayers) {
            if (layer.skip || layer.gids.empty()) continue;
            Color tint{ 255, 255, 255, (uint8_t)(std::max(0.0f, std::min(1.0f, layer.opacity)) * 255.0f) };
            for (int y = ty0; y <= ty1; ++y)
                for (int x = tx0; x <= tx1; ++x) {
                    uint32_t gid = layer.gids[(size_t)y * g.map.w + x];
                    if (gid == 0) continue;
                    if (g.map.isUnitMarker(gid)) continue;   // spawn marker, the unit itself is the art
                    const TilesetRender* ts = findTs(gid);
                    if (!ts || ts->imageKey.empty()) continue;
                    Texture2D tex; int columns;
                    if (!tilesetTexture(*ts, tex, columns)) continue;
                    int local = variantLocalIndex(ts->imageKey, (int)gid - ts->firstgid, x, y);
                    int col = local % columns, row = local / columns;
                    Rectangle src{ (float)(col * ts->strideX), (float)(row * ts->strideY), (float)ts->tileWidth, (float)ts->tileHeight };
                    Rectangle dst{ (float)(x * TILE_SIZE), (float)(y * TILE_SIZE), (float)TILE_SIZE, (float)TILE_SIZE };
                    DrawTexturePro(tex, src, dst, Vector2{ 0, 0 }, 0.0f, tint);
                }
        }
    } else {
        for (int y = ty0; y <= ty1; ++y)
            for (int x = tx0; x <= tx1; ++x) {
                const MapTile& t = g.map.at(x, y);
                Color c = terrainColor(t.color);
                if (t.blocked) c = Color{ (uint8_t)(c.r * 0.6f), (uint8_t)(c.g * 0.6f), (uint8_t)(c.b * 0.6f), 255 };
                DrawRectangle(x * TILE_SIZE, y * TILE_SIZE, TILE_SIZE, TILE_SIZE, c);
                if (t.resource) DrawRectangle(x * TILE_SIZE + 4, y * TILE_SIZE + 4, TILE_SIZE - 8, TILE_SIZE - 8, Color{ 150, 220, 255, 120 });
                if (t.tree) DrawCircle((x + 0.5f) * TILE_SIZE, (y + 0.5f) * TILE_SIZE, 5, Color{ 30, 80, 30, 200 });
            }
    }
    // RWX ScorchMark: permanent ground decals, drawn on the terrain layer below every unit
    Texture2D sc;
    if (!g.scorches.empty() && shared("blood_mark.png", sc)) {
        int cells = std::max(1, sc.width / 50);
        for (auto& s : g.scorches)
            drawSpriteFrame(sc, cells, s.variant % cells, Vector2{ s.pos.x, s.pos.y }, 50.0f, 0, WHITE);
    }
    // world border
    DrawRectangleLines(0, 0, g.map.worldW(), g.map.worldH(), BLACK);
}

// RWX image_shadow: AUTO = the body frame with RGB forced to black, shifted by shadowOffsetX/Y world px.
// Drawn in its own pass so one unit's shadow never stains a neighbour's sprite.
// RWX Tree.b(int,int):133-169 -- a tree is a unit, but its sprite is one cell of a shared atlas.
// Type 0 is palm_tree.png (27x41 cells with a 1 px margin on both axes, five sway frames in a row);
// types 1/2 are trees.png / trees_snow.png (25x30 cells on a 26 px horizontal pitch, five sub types
// stacked down the rows). Tree(boolean) itself calls b(1, -1), so every `unit=tree` in a tileset is a
// type 1 whose sub type and scale come out of the object id, and the angle out of the position.
void Renderer::drawTree(const Unit& u) {
    unsigned h = (unsigned)u.id * 2654435761u;
    const int type = u.treeType;
    // Utility.getDeterministicRandomIntInRange(0, 4, objectId) when the tileset leaves the sub type open
    const int sub = u.treeSub >= 0 ? (u.treeSub > 4 ? 4 : u.treeSub) : (int)((h >> 7) % 5u);
    Texture2D tex{};
    int cw, ch, ox, oy, shadowOff;
    float scale;
    if (type == 0) {
        // palm_tree.png: 27x41 cells, a 1 px margin on both axes, five sway frames in one row (:136-142)
        if (!shared("palm_tree.png", tex)) return;
        cw = 27; ch = 41; ox = 1; oy = 1; shadowOff = 0;
        scale = 1.0f;                       // b() only rolls a scale for the trees/trees_snow atlases
    } else {
        if (!shared(type == 2 ? "trees_snow.png" : "trees.png", tex)) return;
        // 25x30 cells on a 26 px horizontal pitch, five sub types stacked down the rows (:151-164)
        cw = 25; ch = 30; ox = 0; oy = ch * sub; shadowOff = cw;
        // Utility.getRandomIntInRange(1.0f, 2.0f, objectId + 1) -- sub type 0 only rolls 1.0 .. 1.2
        scale = 1.0f + ((h >> 3) % 101u) / 100.0f * (sub == 0 ? 0.2f : 1.0f);
    }
    float w = cw * scale, ht = ch * scale;
    // Tree.n():373 -- the rotation is a hash of the position, which is what gives a forest its
    // natural scatter. normalizeAngle(x, false) folds it back into -180 .. 180 degrees.
    float rot = u.pos.y * 5.0f + u.pos.x * 3.0f;
    rot -= 360.0f * std::floor((rot + 180.0f) / 360.0f);
    Vector2 c{ u.pos.x, u.pos.y - u.posZ };
    // Tree.c():212-228 -- the destination is getUnitBounds(), i.e. a cell-sized box centred on the unit,
    // scaled and rotated about its own centre. The black silhouette one cell to the right is drawn first
    // as the drop shadow, then the canopy on top.
    Rectangle dst{ c.x - w / 2, c.y - ht / 2, w, ht };
    Vector2 org{ w / 2, ht / 2 };
    if (shadowOff)
        DrawTexturePro(tex, Rectangle{ (float)(ox + shadowOff), (float)oy, (float)cw, (float)ch },
                       dst, org, rot, WHITE);
    DrawTexturePro(tex, Rectangle{ (float)ox, (float)oy, (float)cw, (float)ch }, dst, org, rot, WHITE);
}

void Renderer::drawUnitShadow(const Unit& u) {
    if (!u.tpl) return;
    const UnitTemplate& t = *u.tpl;
    if (ToLower(t.imageShadow) != "auto") return;
    Texture2D tex;
    if (!loaded(t, 0, tex)) return;
    int frames = std::max(1, t.totalFrames);
    int ef = (frames > 1 && tex.width % frames == 0) ? frames : 1;
    int anim = ef > 1 ? u.animFrame % ef : 0;
    float rot = t.isStatic() ? 0 : spriteRot(unitRenderAngle(u, true));
    // RWX OrderableUnit.drawShadow:4425-4438 builds the same es/et frame rect the hull uses and scales
    // it by getRenderScale(), so the shadow is the body frame at its native pixel size.
    drawSpriteFrame(tex, ef, anim, Vector2{ u.pos.x + t.shadowOffX, u.pos.y + t.shadowOffY },
        (tex.width / (float)ef) * t.imageScale * u.animScale, rot, Color{ 0, 0, 0, 128 });
}

void Renderer::drawUnit(Game& g, const Unit& u, const Camera2D& cam) {
    if (!u.tpl) return;
    const UnitTemplate& t = *u.tpl;
    // RWX UnitTypeEnum.tree is a NaturalUnit with its own sprite path (Tree.c()).
    if (u.tplKey == "tree") { drawTree(u); return; }
    float r = std::max(6.0f, t.displayRadius);
    Color col = WHITE;
    if (u.underConstruction && u.tpl->isBuilding)
        col = Color{ 140, 255, 140, (uint8_t)(40 + 200 * std::max(0.0f, std::min(1.0f, u.buildProgress))) };

    // The hull pose the animation controller wrote this tick (CustomUnit.animationFrameIndex /
    // .currentFrameTime, CustomUnitAnimationController.b():276-279). With no animation running these are
    // back at [graphics] default_frame and scale 1.
    int frames = std::max(1, t.totalFrames);
    int anim = std::max(0, u.animFrame);

    Texture2D tex;
    // RWX draws the hull at posY - posZ (CustomUnit.java:2416), while its shadow stays flat on the
    // ground: OrderableUnit.drawShadow:4414-4425 deliberately does not subtract the height.
    Vector2 pv{ u.pos.x, u.pos.y - u.posZ };
    // A non-slowDeathFall hull that dies above the ground has already been given image_wreak as its
    // base texture (CustomUnit.e():946-951), so what falls is the burnt wreck sprite, turrets, shield
    // and health bar all gone from the frame it died (:2414-2424 draws a wreck as the bare texture).
    Texture2D wt;
    if (u.dead && u.corpseWreck && loaded(t, 2, wt)) {
        drawSpriteFrame(wt, 1, 0, pv, wt.width * t.imageScale,
                        t.isStatic() ? 0 : spriteRot(u.facing), WHITE);
        return;
    }
    // CustomUnit.d():671-677 -- a dead hull draws the raw wreck, a living one the baked variant of its own
    // team colour slot. The shadow (k():680) never joins in and stays raw.
    drawLegs(g, u, 0, cam);
    int colorIdx = teamColorIndex(u.team);
    // Only the living hull sprite rides the getUnitBounds() translate (:2834-2840 -> the rect c():2426 draws
    // into). The wreck above and the shadow, shield and turrets below all stay on the plain hull position.
    Vector2 bv{ pv.x + t.bodyOffsetDrawX(), pv.y + t.bodyOffsetDrawY() };
    if (teamed(t, t.image, 0, colorIdx, tex)) {
        int ef = (frames > 1 && tex.width % frames == 0) ? frames : 1;
        float rot = t.isStatic() ? 0 : spriteRot(unitRenderAngle(u, false));
        // RWX sizes the hull from es/et, which CustomUnit:1046-1052 sets to the config's frameWidth x
        // frameHeight -- the sprite's own pixels -- and then scales that rect by getRenderScale():2327 =
        // imageScale * currentFrameTime. displayRadius is a HUD/collision radius, never a sprite size.
        drawSpriteFrame(tex, ef, ef > 1 ? anim % ef : 0, bv,
                        (tex.width / (float)ef) * t.imageScale * u.animScale, rot, col);
    } else {
        DrawCircleV(pv, r, Fade(teamColor(u.team), 0.8f));
    }
    // RWX GameViewUtils.a(OrderableUnit):154-194 - every [turret_N] is drawn at its own pivot F(i),
    // rotated by its own aim, with its own sprite and recoil; a barrel with no image of its own
    // falls back to [graphics] image_turret.
    if (!t.turrets.empty()) {
        for (size_t i = 0; i < t.turrets.size(); ++i) {
            const TurretTpl& tt = t.turrets[i];
            if (tt.invisible) continue;
            const TurretState* s = (i < u.tur.size()) ? &u.tur[i] : nullptr;
            float a = s ? s->angle : u.facing;
            Vec2 aim{ std::cos(a), std::sin(a) }, right{ -aim.y, aim.x };
            Vec2 tp = turretPivot(u, (int)i);   // recoilOffset is already inside it, as in RWX's F(i)
            // GameViewUtils:192 draws the sprite's centre at pivot - (h(i), i(i)) and only then rotates
            // and scales about the pivot, so image_drawOffsetX/Y ride the aim and turretImageScale.
            if (tt.drawOffsetX != 0 || tt.drawOffsetY != 0)
                tp = tp + (aim * tt.drawOffsetY - right * tt.drawOffsetX) * t.turretImageScale;
            Texture2D tur;
            // RWX CustomUnit.d(i):691-706 walks four slots: the barrel's own baked array, its raw image, the
            // [graphics] image_turret array -- only baked when teamColorsOnTurret is on (:1407) -- then the
            // raw image_turret. So a barrel that names its own sprite ignores the hull-wide flag only if
            // image_applyTeamColors says so (TurretConfig.java:551-554).
            bool ownImage = !tt.image.empty();
            std::string turName = ownImage ? tt.image : t.imageTurret;
            bool dyeTurret = (ownImage && tt.applyTeamColors >= 0)
                ? tt.applyTeamColors == 1 : t.teamColorsOnTurret;
            if (!(dyeTurret ? teamed(t, turName, 1, colorIdx, tur) : byName(t, turName, tur))) continue;
            // GameViewUtils:192 hands the whole texture to GraphicsEngine.b(texture,x,y,paint), which
            // KoolGraphicsEngine:441-449 draws over fullSourceRect() -- a turret has no frame strip of its
            // own (TurretConfig carries no total_frames), and p(i):2332 is the only scale it gets.
            drawSpriteFrame(tur, 1, 0, Vector2{ tp.x, tp.y - u.posZ },
                            tur.width * t.turretImageScale, spriteRot(a), col);
        }
        // RWX shoot_flame is not part of the barrel's drawing at all: CustomUnit.java:1593-1595 hands the
        // muzzle point of D(i) to EffectTemplate.a(), which spawns an Effect the effect layer draws later,
        // so an invisible barrel (turret_antiair_t3's missile pods) or one with no sprite still flashes.
        // effectTemplates[3] = drawable/flame.png, 20x25 frames, centred on that point (EffectManager:603
        // leaves an=true), and its own paint: the built-in flame carries color=-1, never the hull's team or
        // under-construction tint. small plays frames 0..3 at 0.5/frame (8 ticks), large/medium 1..4 at
        // 0.4 (10 ticks), and medium is the only one scaled, to 0.75 (EffectTemplate.java:107-111).
        Texture2D fl;
        if (shared("flame.png", fl)) {
            int ff = std::max(1, fl.width / 20);
            for (size_t i = 0; i < t.turrets.size() && i < u.tur.size(); ++i) {
                const TurretState& s = u.tur[i];
                if (s.flameType <= 0 || s.flameT <= 0) continue;
                int total = s.flameType == 1 ? 8 : 10;
                float elapsed = (float)total - s.flameT;
                int frame = s.flameType == 1 ? (int)(elapsed * 0.5f) : 1 + (int)(elapsed * 0.4f);
                drawSpriteFrame(fl, ff, std::min(ff - 1, frame),
                    Vector2{ pv.x + s.flameOff.x, pv.y + s.flameOff.y },
                    20.0f * (s.flameType == 2 ? 0.75f : 1.0f), spriteRot(s.flameAng), WHITE);
            }
        }
        // CustomUnit:2482-2489 -> GameViewUtils.a(unit,texture,f,i):163-173 -- chargeEffectImage is drawn
        // centred on the muzzle D(i), scaled by movementLevels[i].speed / warmup on both axes, unrotated
        // and with a null paint: no team dye, no under-construction tint. It shows only while the barrel
        // holds a charge, so it grows down the barrel as the warmup ticks up and bleeds away with
        // warmupCallDownRate when the aim is interrupted.
        if (!u.dead) {
            for (size_t i = 0; i < t.turrets.size() && i < u.tur.size(); ++i) {
                const TurretTpl& tt = t.turrets[i];
                const TurretState& s = u.tur[i];
                if (tt.chargeEffectImage.empty() || tt.warmup <= 0.0f || s.warm <= 0.0f) continue;
                Texture2D ch;
                if (!byName(t, tt.chargeEffectImage, ch)) continue;
                float f = s.warm / tt.warmup;
                Vec2 mz = muzzlePoint(u, (int)i);
                drawSpriteFrame(ch, 1, 0, Vector2{ mz.x, mz.y - u.posZ }, ch.width * f, 0, WHITE);
            }
        }
    }
    drawLegs(g, u, 1, cam);
    // RWX CustomUnit.c():2491-2521 - the bubble is [graphics] image_shield, or the shared
    // drawable/shield_mid.png, drawn 1.25x over shieldRenderRadius on top of the whole unit. Its
    // alpha is how full the pool is plus how recently it soaked a hit, so a shielded unit glows
    // brighter the moment it is shot at.
    if (u.shield > 0.0f && t.maxShield > 0 && !iconRender_) {
        Texture2D sh;
        bool custom = !t.imageShield.empty() && byName(t, t.imageShield, sh);
        if (custom || shared("shield_mid.png", sh)) {
            float flash = std::min(u.shieldFlash, 50.0f);
            float a = t.shieldOnlyDeflectionFlash
                ? (flash / 50.0f) * 0.5f + (std::min(u.shieldFlash, 5.0f) / 5.0f) * 0.2f
                : 0.09f + (u.shield / u.shieldMax) * 0.4f + (flash / 50.0f) * 0.5f;
            if (a > 1.0f) a = 1.0f;
            if (a > 0.0f) {
                float f = (t.shieldRenderRadius * 2.0f) / (custom ? (float)sh.width : 87.0f) * 1.25f;
                drawSpriteFrame(sh, 1, 0, pv, sh.width * f, t.isStatic() ? 0 : spriteRot(u.facing),
                    Color{ 255, 255, 255, (uint8_t)(a * 255.0f) });
            }
        }
    }
    // team ring under selection
    if (u.selected && !iconRender_) {
        DrawCircleLines((int)pv.x, (int)pv.y, r + 3, Color{ 60, 220, 90, 255 });
    }
    // hp bar
    bool energyBar = !iconRender_ && t.maxEnergy > 0 && t.showEnergyBar && u.energy < t.maxEnergy;
    if (!iconRender_ && (u.hp < u.maxHp || u.selected || t.isBuilding || energyBar ||
        (t.maxShield > 0 && t.showShieldBar && u.shield < u.shieldMax))) {
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
        // RWX BaseUnit.java:1146-1163 -- a row of slot cells under the health bar, only for a selected
        // carrier (the same `isSelected || settingsEngine.showHp` gate the health bar uses) and only while
        // [graphics] showTransportBar (CustomUnit.java:532/541 returns -1 to hide it). The weight drawn is
        // dI(), i.e. the *slots* taken, so a nine-slot dropship shows a single heavy hull as five cells.
        bool transportBar = u.selected && t.canTransportUnits() && t.showTransportBar;
        int slotsUsed = transportBar ? g.transportSlotsUsed(u) : 0;
        if (transportBar) {
            float tw = r * 2.0f + (t.maxTransportingUnits > 10 ? 20.0f : 0.0f);
            float cell = (float)(int)(tw / t.maxTransportingUnits + 0.5f);
            float x0 = pv.x - tw / 2.0f;
            for (int i = 1; i <= t.maxTransportingUnits; ++i) {
                float cx = x0 + (i - 1) * cell;
                Rectangle box{ cx, p.y + 3, std::max(1.0f, cell - 2.0f), 3.0f };
                if (slotsUsed >= i) DrawRectangleRec(box, Color{ 0, 0, 255, 255 });
                DrawRectangleLinesEx(box, 1.0f, Color{ 0, 0, 210, 110 });
            }
        }
        // RWX BaseUnit.java:1166-1183: one secondary bar sits under the health bar, and
        // getSecondaryBarProgress (CustomUnit.java:3479-3495) fills that slot with the energy pool
        // *before* the bubble -- a unit with both only ever shows its charge. While the
        // recharge-to-full latch holds the barrels the bar is drawn in the dim grey of :1170-1174.
        if (energyBar || (t.maxShield > 0 && t.showShieldBar && u.shield < u.shieldMax)) {
            // :1163 -- f8 becomes 3+5 once the cells are drawn, so the secondary bar drops 5 px for them
            float by = p.y + 5 + (u.underConstruction ? 5 : 0) + (transportBar ? 5 : 0);
            float sf = energyBar ? std::max(0.0f, u.energy / t.maxEnergy)
                                 : std::max(0.0f, u.shield / u.shieldMax);
            Color fill = u.energyRecharging ? Color{ 103, 117, 119, 118 } : Color{ 23, 179, 207, 200 };
            DrawRectangle((int)p.x, (int)by, (int)bw, 2, Color{ 20, 20, 20, 160 });
            DrawRectangle((int)p.x, (int)by, (int)(bw * sf), 2, fill);
        }
        // RWX production bar: yellow progress + queue count while a factory is building a unit
        if (t.isFactory && u.operational() && !u.prodQueue.empty()) {
            Vector2 p{ pv.x - r, pv.y - (r + 13) };
            DrawRectangle((int)p.x, (int)p.y, (int)bw, 3, Color{ 30, 30, 30, 200 });
            DrawRectangle((int)p.x, (int)p.y, (int)(bw * std::min(1.0f, u.prodProgress)), 3, Color{ 250, 190, 40, 255 });
            if (u.prodQueue.size() > 1)
                DrawText(std::to_string(u.prodQueue.size()).c_str(), (int)(pv.x + r + 3), (int)p.y - 3, 12, Color{ 250, 190, 40, 255 });
        }
    }
    // upgrade-in-progress bar (RWX self action queue)
    if (u.upgradeIdx >= 0 && !iconRender_) {
        float bw = r * 2.0f;
        Vector2 p{ pv.x - r, pv.y - (r + 13) };
        DrawRectangle((int)p.x, (int)p.y, (int)bw, 3, Color{ 30, 30, 30, 200 });
        DrawRectangle((int)p.x, (int)p.y, (int)(bw * std::min(1.0f, u.upgradeProgress)), 3, Color{ 60, 200, 210, 255 });
    }
}

void Renderer::drawLegs(Game& g, const Unit& u, int pass, const Camera2D& cam) {
    if (!u.tpl || u.legs.empty() || u.dead) return;
    const UnitTemplate& t = *u.tpl;
    bool details = cam.zoom >= 0.45f;
    bool underAll = pass == 2, overBody = pass == 1;
    float scale = t.imageScale * u.animScale;
    int colorIdx = teamColorIndex(u.team);
    Color basePaint = unitRenderPaint(u);
    float dir = (t.lockLegRot && !u.tur.empty()) ? u.tur[0].angle : u.facing;
    Vec2 aim{ std::cos(dir), std::sin(dir) }, right{ -aim.y, aim.x };
    for (size_t i = 0; i < u.legs.size() && i < t.legs.size(); ++i) {
        const LegTpl& C = t.legs[i];
        const LegState& S = u.legs[i];
        if (C.hidden || C.drawUnderAllUnits != underAll || S.alpha <= 0.0f) continue;
        bool drawLimb = C.drawOverBody == overBody;
        float height = S.z + u.posZ;
        bool drawShadow = !overBody && cam.zoom >= 0.3f && height > 0.0f && !C.imageShadow.empty();
        if (!drawLimb && !drawShadow) continue;
        Vec2 fw = u.pos + S.foot;
        Vector2 fp{ fw.x, fw.y - S.z - u.posZ };
        float rot = spriteRot(S.angle) + S.spin + C.drawDirOffset;
        Texture2D ft, lt, sh;
        if (drawShadow && byName(t, C.imageShadow, sh))
            drawSpriteFrame(sh, 1, 0, Vector2{ fp.x, fp.y + height * scale }, sh.width * scale,
                            rot, Color{ 255, 255, 255, 128 });
        if (!drawLimb) continue;
        float alpha = height < -0.3f ? submergedRenderAlpha(height) * 0.003921569f : 1.0f;
        if (S.alpha < 1.0f) alpha *= S.alpha;
        Color col = basePaint;
        // Limb fading caps the base paint alpha, preserving construction opacity.
        if (alpha < 1.0f) col.a = (uint8_t)std::min((int)col.a, (int)(255.0f * alpha));
        bool hasFoot = (details || C.drawFootZoomedOut) && (C.footTeamColors
            ? teamed(t, C.imageFoot, 1, colorIdx, ft) : byName(t, C.imageFoot, ft));
        bool hasLeg = (details || C.drawLegZoomedOut) && (C.legTeamColors
            ? teamed(t, C.imageLeg, 1, colorIdx, lt) : byName(t, C.imageLeg, lt));
        if (hasFoot && !C.drawFootOnTop)
            drawSpriteFrame(ft, 1, 0, fp, ft.width * scale, rot, col);
        if (hasLeg) {
            Vec2 d = aim * C.attachY + right * C.attachX - S.foot;
            float half = lt.height * 0.5f;
            float len = half;
            float d2 = d.x * d.x + d.y * d.y;
            if (d2 < (half - 2.0f) * (half - 2.0f))
                len = std::round(std::sqrt((float)(int)d2));
            if (len > 0.0f) {
                float ang = std::atan2(d.y, d.x) * 180.0f / 3.14159265f + 90.0f;
                int top = (int)(half - len), bottom = (int)(half + len);
                float halfWidth = (float)(lt.width / 2) * scale;
                Rectangle src{ 0, (float)top, (float)lt.width, (float)(bottom - top) };
                Rectangle dst{ fp.x, fp.y, 2.0f * halfWidth, 2.0f * len * scale };
                DrawTexturePro(lt, src, dst, Vector2{ halfWidth, len * scale }, ang, col);
            }
        }
        if (hasFoot && C.drawFootOnTop)
            drawSpriteFrame(ft, 1, 0, fp, ft.width * scale, rot, col);
    }
}

// Effect.update(GameEngine,false):403-637, the draw half of one [effect_*] instance. Everything RWX keeps in
// Effect is already stepped by Game::updateFx; what is left is the sheet cell, the anchor, the scale/alpha
// curves and the rotation.
void Renderer::drawFx(Game& g, bool under) {
    const int want = under ? 1 : 2;
    for (const Fx& f : g.fx) {
        if (f.dead || f.delay > 0.0f || f.spec.layer != want) continue;   // :410-412
        const EffectTpl& s = f.spec;
        Texture2D tex;
        Rectangle src;
        if (!s.image.empty()) {
            // :419-423 -- a custom image replaces the built-in strip entirely, and its SpriteSheet was built
            // by EffectTemplate.a(config,ini,section):487-505 off the texture's own size.
            if (!f.src || !byName(*f.src, s.image, tex)) continue;
            int tw = tex.width, th = tex.height;
            int cw = s.frameW > 0 ? s.frameW : tw / std::max(1, s.totalFrames);
            int ch = s.frameH > 0 ? s.frameH : th;
            if (cw <= 0 || ch <= 0 || tw <= 0 || th <= 0) continue;
            if (s.totalFrames == 1 && cw >= tw) {
                src = Rectangle{ 0, 0, (float)tw, (float)th };            // SpriteSheet.k
            } else {
                // :494-501 -- h (columns) only leaves Integer.MAX_VALUE when the cell is shorter than the
                // texture, which is what makes a sheet wrap onto a second row.
                int cols = ch < th ? std::max(1, tw / cw) : 0;
                int row = cols > 0 ? f.frame / cols : 0;
                int col = cols > 0 ? f.frame % cols : f.frame;
                src = Rectangle{ (float)(col * cw), (float)(row * ch), (float)cw, (float)ch };
            }
        } else {
            if (s.strip < 0 || s.strip >= 20) continue;
            const FxSheetSpec& sh = kFxSheets[s.strip];
            if (!shared(sh.file, tex)) continue;
            // Effect.getEffectSprite:388-401 -- origin + frame * step, single row for all twenty sheets.
            src = sh.whole ? Rectangle{ 0, 0, (float)tex.width, (float)tex.height }
                           : Rectangle{ (float)(sh.ox + f.frame * sh.sx), (float)sh.oy,
                                        (float)sh.cw, (float)sh.ch };
        }
        // RWX hands the backend a rect that may run off the sheet (animateFrameEnd == total_frames lets ap
        // reach one past the last cell); raylib would sample wrapped pixels, so the rect is clipped instead.
        if (src.x >= tex.width || src.y >= tex.height) continue;
        if (src.x + src.width > tex.width) src.width = tex.width - src.x;
        if (src.y + src.height > tex.height) src.height = tex.height - src.y;
        if (src.width <= 0 || src.height <= 0) continue;

        // :430-433 createPointWithOffset(I,J,K) = (I, J-K), then :460-466 adds the attached GameObject's live
        // position back -- a hull, or the round an effectOnCreate entry is riding. One that is already gone
        // keeps the last snapshot updateFx took.
        Vec2 pt{ f.ij.x, f.ij.y - f.k };
        if (f.unitId >= 0 || f.projId >= 0) { pt.x += f.hull.x; pt.y += f.hull.y - f.hullZ; }
        // :436-442 -- the lerp only runs when the template asked for a size change at all, and its progress
        // is 1 - V/W, not 1 - V/Vmax: the hit flashes carry W=25 under V=35.
        float sc = 1.0f;
        if (s.scaleFrom != 1.0f || s.scaleTo != 1.0f) {
            float prog = f.w > 0.0f ? 1.0f - f.life / f.w : 1.0f;
            sc = s.scaleFrom + (s.scaleTo - s.scaleFrom) * prog;
        }
        float w = src.width * sc, h = src.height * sc;
        // :451-459 -- an=true is set by every creator, so the cell is always centred; imageAnchorY then
        // slides it along its own height. That shift also moves the rotation pivot (:541).
        float anchor = s.imageAnchorY != 0.5f ? h * (0.5f - s.imageAnchorY) : 0.0f;
        // :482-533 -- f = W - V is the age, and the two fades share the one boundary t. With W under V the
        // age starts negative, which holds the alpha at E until V drops below W (the hit-flash look).
        float age = f.w - f.life;
        float ta = 1.0f, tr = 1.0f, tg = 1.0f, tb = 1.0f;
        if (s.color != 0) {                       // x == -1 in RWX, which RP writes as 0
            // parseRgba packs RRGGBBAA, the same layout the projectile tint at :571 reads back out.
            tr = ((s.color >> 24) & 0xFF) / 255.0f;
            tg = ((s.color >> 16) & 0xFF) / 255.0f;
            tb = ((s.color >> 8) & 0xFF) / 255.0f;
            ta = (s.color & 0xFF) / 255.0f;
        }
        float a;
        if (s.fadeOut && age >= s.fadeIn)
            a = ta * (f.life / std::max(0.001f, f.w - s.fadeIn)) * s.alpha;
        else if (s.fadeIn > 0.0f && age < s.fadeIn)
            a = ta * (age / s.fadeIn) * s.alpha;
        else a = ta * s.alpha;
        if (a > 1.0f) a = 1.0f;
        if (a <= 0.0f) continue;
        // :536-542 -- rotation is applied ONLY when Y is non-zero, and then at Y + 90. An effect pinned at
        // zero (the shockwave, the light) is drawn in the sheet's own orientation, not turned a quarter.
        float rot = f.dir != 0.0f ? f.dir + 90.0f : 0.0f;
        DrawTexturePro(tex, src, Rectangle{ pt.x, pt.y + anchor, w, h }, Vector2{ w * 0.5f, h * 0.5f }, rot,
                       Color{ (uint8_t)(tr * 255.0f), (uint8_t)(tg * 255.0f), (uint8_t)(tb * 255.0f),
                              (uint8_t)(a * 255.0f) });
    }
}

void Renderer::drawWorld(Game& g, const ViewState& view) {
    BeginMode2D(view.cam);
    drawMap(g, view.cam);
    debugDrawGrid(g, view.cam);

    // RWX CustomUnit.java:946-951,2414-2424: a wreck is the whole image_wreak texture drawn at the
    // unit's last rotation, with no turret, shadow layering or health bar.
    for (auto& w : g.wrecks) {
        if (!w.tpl) continue;
        Texture2D wt;
        if (!loaded(*w.tpl, 2, wt)) continue;
        float wr = wt.width * w.tpl->imageScale;
        drawSpriteFrame(wt, 1, 0, Vector2{ w.pos.x, w.pos.y }, wr, spriteRot(w.facing), WHITE);
    }
    // RWX GameLogic.java:1915 -- EffectManager.drawEffect(f, 1) is the ground pass, ahead of every game
    // object, so a drawUnderUnits effect ends up beneath the hulls and their shadows.
    drawFx(g, true);
    // under-construction + normal units sorted so buildings draw first. A hull in someone's hold is not
    // drawn at all (CustomUnit.a(GameEngine):4656) -- it reappears where b():718-809 puts it.
    for (auto& u : g.units) if (!u.riding()) drawUnitShadow(u);
    for (auto& u : g.units) if (!u.riding()) drawLegs(g, u, 2, view.cam);
    for (auto& u : g.units) if (u.tpl && u.tpl->isStatic() && !u.riding()) drawUnit(g, u, view.cam);
    for (auto& u : g.units) if (u.tpl && !u.tpl->isStatic() && !u.riding()) drawUnit(g, u, view.cam);

    // RWX projectile trail (Projectile.update:921-939): effects.png cell 0 is a 25x25 grey puff taken
    // from the sheet at x=1+26*cell, y=1 (EffectManager.java:665-678). It fades in over 5 ticks to
    // alpha E=0.5, then fades out linearly to tick 70 (r=true, t=5 -> alpha = 0.5*V/(W-t)), while its
    // size lerps G=0.5 -> F=1.2 times the 25px cell (Effect.java:436-441,495-526). Drawn under the
    // shots, which is RWX's layer order for these layer-2 effects.
    {
        Texture2D puffs;
        bool any = false;
        for (auto& e : g.effects) if (e.trail) { any = true; break; }
        if (any && shared("effects.png", puffs)) {
            for (auto& e : g.effects) {
                if (!e.trail) continue;
                float el = e.t - e.delay;
                if (el < 0) continue;
                // Effect.java:521-526: E=0.5, fading in over t ticks and then out over the remaining
                // W-t; :437 scales lerp(G, F, 1 - V/W).
                float in = e.puffFadeIn;
                float a = (in > 0.0f && el < in) ? 0.5f * (el / in)
                    : 0.5f * std::max(0.0f, (e.life - el) / std::max(1.0f, e.life - in));
                float sz = 25.0f * (e.puffFrom + (e.puffTo - e.puffFrom) * std::min(1.0f, el / e.life));
                DrawTexturePro(puffs, Rectangle{ 1, 1, 25, 25 },
                               Rectangle{ e.pos.x, e.pos.y, sz, sz }, Vector2{ sz / 2, sz / 2 },
                               e.rot * 57.29578f + 90.0f,   // Effect.java:541 draws at Y + 90
                               Color{ 255, 255, 255, (uint8_t)(a * 255.0f) });
            }
        }
    }
    // A falling hull's fire puffs (CustomUnit.java:1702-1714): EffectEmitter.b picks aq=7, which is
    // fire.png's 20px row (EffectManager.java:735-744), and a random one of its four frames. E stays at
    // its default 1 and s=true/t=10 fade it in over the first 10 ticks, r=true then fades the remaining
    // 40 (Effect.java:521-526), while R=0.2 turns the sprite (:366) -- here as a random start angle.
    {
        Texture2D ftex;
        bool any = false;
        for (auto& e : g.effects) if (e.fire) { any = true; break; }
        if (any && shared("fire.png", ftex)) {
            float cell = (float)ftex.height;
            int frames = std::max(1, ftex.width / (int)cell);
            for (auto& e : g.effects) {
                if (!e.fire) continue;
                float el = e.t - e.delay;
                if (el < 0) continue;
                float a = el < 10.0f ? el / 10.0f
                    : std::max(0.0f, (e.life - el) / std::max(1.0f, e.life - 10.0f));
                int f = std::min(frames - 1, e.frame);
                DrawTexturePro(ftex, Rectangle{ f * cell, 0, cell, cell },
                               Rectangle{ e.pos.x, e.pos.y, cell, cell }, Vector2{ cell / 2, cell / 2 },
                               e.rot * 57.29578f, Color{ 255, 255, 255, (uint8_t)(a * 255.0f) });
            }
        }
    }

    // projectiles: RWX draws the [projectile_N] frame from a single-row sheet — projectiles.png has
    // 20px cells, drawType 1 = projectiles_large.png (60px), 2 = projectiles2.png — rotated by the
    // travel angle + 90 and scaled to cell*drawSize (Projectile.java:1688-1711 + GameViewUtils.java:100).
    // frame : -1 keeps the plain tinted dot instead.
    for (auto& p : g.projectiles) {
        // RWX clears shouldDraw once an ordinary shot lands (:1038-1040), but a beam keeps it and stays
        // drawn until its life runs out, so a landed beam is still rendered here.
        if (p.hit && !p.isBeam) continue;
        Color tint{ (uint8_t)(p.color >> 24), (uint8_t)((p.color >> 16) & 0xFF),
                    (uint8_t)((p.color >> 8) & 0xFF), (uint8_t)p.color };
        if (p.isBeam) {
            // Projectile.c():1552-1683 -- the beam runs from the round's own pos (the muzzle, which
            // moveWithParent keeps riding the hull) to the live target, or the frozen aim point once the
            // target is gone. Both ends are lifted by their own posZ.
            Vector2 a{ p.pos.x, p.pos.y - p.posZ };
            const Unit* bt = p.targetId >= 0 ? g.get(p.targetId) : nullptr;
            Vector2 b = (bt && !bt->dead) ? Vector2{ bt->pos.x, bt->pos.y - bt->posZ }
                                          : Vector2{ p.impactPos.x, p.impactPos.y - p.impactZ };
            float dx = b.x - a.x, dy = b.y - a.y;
            float length = std::sqrt(dx * dx + dy * dy);
            if (length < 0.001f) { b.x = a.x + 0.001f; dx = 0.001f; length = 0.001f; }
            float ux = dx / length, uy = dy / length;
            float angDeg = std::atan2(dy, dx) * 57.29578f + 90.0f;
            Texture2D beamTex;
            if (!p.beamImage.empty() && p.from && byName(*p.from, p.beamImage, beamTex)) {
                // :1581-1622 -- beamImage is tiled muzzle->target (width = texture width, length = the
                // gap), rotated angle+90, with beamImageOffsetRate scrolling the tile phase by age.
                float texH = (float)beamTex.height;
                float phase = std::fmod(p.beamImageOffsetRate * p.ticks, texH);
                if (phase < 0) phase += texH;
                for (float s = -phase; s < length; s += texH) {
                    float a0 = std::max(0.0f, s), a1 = std::min(length, s + texH);
                    if (a1 <= a0) continue;
                    float seg = a1 - a0;
                    Vector2 c{ a.x + ux * (a0 + a1) * 0.5f, a.y + uy * (a0 + a1) * 0.5f };
                    Rectangle src{ 0, a0 - s, (float)beamTex.width, seg };
                    Rectangle dst{ c.x, c.y, (float)beamTex.width, seg };
                    DrawTexturePro(beamTex, src, dst, Vector2{ beamTex.width * 0.5f, seg * 0.5f }, angDeg, tint);
                }
                Texture2D cap;
                if (!p.beamImageStart.empty() && byName(*p.from, p.beamImageStart, cap))
                    drawSpriteFrame(cap, 1, 0, a, (float)cap.width, p.beamStartRotated ? angDeg : 0.0f, tint);
                if (!p.beamImageEnd.empty() && byName(*p.from, p.beamImageEnd, cap))
                    drawSpriteFrame(cap, 1, 0, b, (float)cap.width, p.beamEndRotated ? angDeg : 0.0f, tint);
            } else if (p.lightingEffect) {
                // :1645-1683 -- a 20-node polyline whose perpendicular jitter (+-10px) is re-rolled every
                // 4 ticks; under 200px the node count drops by 5. RP derives the jitter from the tick
                // bucket so it stays stable for those 4 ticks without carrying RWX's O[20]/N state.
                int nodes = 20;
                if (length < 200.0f) nodes = std::max(0, nodes - 5);
                else if (length < 100.0f) nodes = std::max(0, nodes - 10);
                if (nodes < 2) nodes = 2;
                float seg = length / (nodes - 1);
                int bucket = p.ticks / 4;
                BeginBlendMode(BLEND_ADDITIVE);
                Vector2 prev = a;
                for (int i = 1; i < nodes; ++i) {
                    float j = (i == nodes - 1) ? 0.0f
                        : std::sin((bucket * 131 + i * 17) * 12.9898f) * 43758.5453f;
                    j = (j - std::floor(j)) * 20.0f - 10.0f;
                    Vector2 cur{ a.x + ux * i * seg - uy * j, a.y + uy * i * seg + ux * j };
                    DrawLineEx(prev, cur, 2.0f, tint);
                    prev = cur;
                }
                EndBlendMode();
            } else {
                // :1623-1643 -- laserEffect: a wide faint glow line under a narrow bright core, both
                // muzzle->target, plus a small hit circle at the far end.
                BeginBlendMode(BLEND_ADDITIVE);
                Color glow = tint; glow.a = (uint8_t)(tint.a * 0.5f);
                DrawLineEx(a, b, 6.0f, glow);
                DrawLineEx(a, b, 3.0f, tint);
                DrawCircleV(b, 5.0f, tint);
                EndBlendMode();
            }
            continue;
        }
        // RWX [projectile_N] lightColor: EffectManager.createLightEffect attaches an additive
        // light_50.png glow (alpha 0.3, size = texture * lightSize) that follows the projectile.
        if (p.lightColor) {
            Texture2D glow;
            if (shared("light_50.png", glow)) {
                float gs = glow.width * p.lightSize;
                Color gc{ (uint8_t)(p.lightColor >> 24), (uint8_t)((p.lightColor >> 16) & 0xFF),
                          (uint8_t)((p.lightColor >> 8) & 0xFF), (uint8_t)(0.3f * (p.lightColor & 0xFF)) };
                BeginBlendMode(BLEND_ADDITIVE);
                DrawTexturePro(glow, Rectangle{ 0, 0, (float)glow.width, (float)glow.height },
                               Rectangle{ p.pos.x, p.pos.y - p.posZ, gs, gs }, Vector2{ gs * 0.5f, gs * 0.5f }, 0, gc);
                EndBlendMode();
            }
        }
        const char* sheet = p.drawType == 1 ? "projectiles_large.png"
                          : (p.drawType == 2 ? "projectiles2.png" : "projectiles.png");
        Texture2D tex;
        if (p.frame < 0 || !shared(sheet, tex)) {
            // unframed: RWX draws a filled dot of radius drawSize, its shadow left on the ground and the
            // dot itself lifted posZ (:1714-1718).
            if (p.posZ > 0.0f && p.shadowFrame >= 0)
                DrawCircleV(Vector2{ p.pos.x, p.pos.y }, std::max(1.0f, p.drawSize), Color{ 0, 0, 0, 128 });
            DrawCircleV(Vector2{ p.pos.x, p.pos.y - p.posZ }, std::max(1.0f, p.drawSize), tint);
            continue;
        }
        int cell = std::max(1, tex.height);
        int frames = std::max(1, tex.width / cell);
        auto clampFrame = [&](int f) { return f < 0 ? 0 : std::min(frames - 1, f); };
        // Projectile.draw:1702-1711 passes this.aT, the angle a shot's nose is dragged around at
        // 12 deg/tick (update:1421) and which an arcing shell holds straight up or straight down.
        float ang = spriteRot(p.drawAngle);
        // Projectile.java:1715-1721 -- the shadow stays on the ground and only appears once the shot is
        // off it; the sprite itself is drawn posZ px higher.
        Vector2 pos{ p.pos.x, p.pos.y };
        if (p.shadowFrame >= 0 && p.posZ > 0.0f)
            drawSpriteFrame(tex, frames, clampFrame(p.shadowFrame), pos, cell * p.drawSize, ang, Color{ 0, 0, 0, 128 });
        drawSpriteFrame(tex, frames, clampFrame(p.frame), Vector2{ p.pos.x, p.pos.y - p.posZ }, cell * p.drawSize, ang, tint);
    }
    // RWX explosion: explode_big.png strip (13 frames), alpha 0.7 fading over the last 10 ticks
    Texture2D ex;
    if (!g.effects.empty() && shared("explode_big.png", ex)) {
        const int kFrames = 13;
        float fw = ex.width / (float)kFrames;
        for (auto& e : g.effects) {
            if (e.trail || e.fire) continue;
            float el = e.t - e.delay;
            if (el < 0) continue;
            float a = 0.7f * std::max(0.0f, std::min(1.0f, (e.life - el) / 10.0f));
            drawSpriteFrame(ex, kFrames, std::min(kFrames - 1, (int)(el * e.frameSpeed)),
                Vector2{ e.pos.x, e.pos.y }, fw * e.scale, e.rot * 57.29578f,
                Color{ 255, 255, 255, (uint8_t)(a * 255.0f) });
        }
    }
    // RWX GameLogic.java:1984 -- drawEffect(f, 2) is the air pass, after the units and the shots. The muzzle
    // list lands here, which is what puts a shockwave or a spark cloud over the barrel that made it.
    drawFx(g, false);
    // nano build particles (green flecks flying builder -> building, RW effects.png frame 6)
    for (auto& p : g.particles) {
        float prog = p.dur > 0 ? std::min(1.0f, p.t / p.dur) : 1.0f;
        float rad = 2.5f + 10.0f * prog;
        uint8_t a = (uint8_t)(200 * (1 - prog));
        Color pc = p.reclaim ? Color{ 200, 40, 40, a } : Color{ 80, 200, 40, a };
        DrawCircleV(Vector2{ p.pos.x, p.pos.y }, rad, pc);
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
    debugDrawWorld(g, view.cam);
    EndMode2D();

    // selection drag box (screen space)
    if (view.selActive) {
        float x0 = std::min(view.selA.x, view.selB.x), y0 = std::min(view.selA.y, view.selB.y);
        float w = std::fabs(view.selA.x - view.selB.x), h = std::fabs(view.selA.y - view.selB.y);
        DrawRectangle((int)x0, (int)y0, (int)w, (int)h, Color{ 90, 220, 90, 40 });
        DrawRectangleLines((int)x0, (int)y0, (int)w, (int)h, Color{ 90, 220, 90, 255 });
    }
}

// RWX UnitTypeEnum.drawUnit() builds a command-bar icon by drawing the full
// template (hull + every turret + its leg rig) into an offscreen buffer. RP
// renders that once per template into a cached texture instead of using only
// the body sprite.
bool Renderer::unitIcon(const std::string& tplName, Texture2D& out) {
    std::string key = ToLower(tplName);
    auto it = iconCache_.find(key);
    if (it != iconCache_.end()) { out = it->second; return it->second.id != missing_.id; }
    const UnitTemplate* t = g_templates.find(key);
    if (!t) return false;

    // A stand-in unit posed as RWX's preview: team 0 (undied art), turrets
    // aimed north-west, hull at frame 0, legs reset at their attach points.
    Unit u;
    u.tpl = t;
    u.tplKey = key;
    u.team = 0;
    u.pos = Vec2{ 0, 0 };
    u.hp = u.maxHp = std::max(1.0f, t->maxHp);
    u.facing = -3.14159265f / 2.0f;   // north, same as RWX's icon preview
    u.animFrame = t->defaultFrame;
    u.animScale = 1.0f;
    u.tur.assign(t->turrets.size(), TurretState{});
    for (auto& s : u.tur) s.angle = u.facing;
    u.legs.assign(t->legs.size(), LegState{});
    for (size_t i = 0; i < u.legs.size(); ++i) {
        const LegTpl& C = t->legs[i];
        u.legs[i].alpha = C.alpha;
        u.legs[i].foot = Vec2{ C.attachX, C.attachY };
        u.legs[i].angle = u.facing;
        u.legs[i].z = 0;
    }

    // Frame the sprite: its natural size plus room for turrets/legs/shield.
    Texture2D body{};
    float bodyW = loaded(*t, 0, body) ? (body.width / (float)std::max(1, t->totalFrames)) * t->imageScale : 2 * t->displayRadius;
    float bodyH = loaded(*t, 0, body) ? (float)body.height * t->imageScale : 2 * t->displayRadius;
    float extent = std::max({ bodyW, bodyH, 2.0f * t->displayRadius, t->shieldRenderRadius * 2.0f });
    for (const TurretTpl& tt : t->turrets) {
        Texture2D bar{};
        bool got = tt.image.empty() ? loaded(*t, 1, bar) : byName(*t, tt.image, bar);
        float reach = 2.0f * (std::abs(tt.x) + std::abs(tt.y) + std::abs(tt.drawOffsetX) + std::abs(tt.drawOffsetY))
                    + (got ? (float)bar.width * t->turretImageScale : 40.0f * t->turretImageScale);
        extent = std::max(extent, reach);
    }
    const int SZ = 128;
    // Sprite sheets carry a transparent margin, so framing on the raw extent leaves the preview looking
    // lost in its box; pulling the extent in and allowing a little more zoom fills the button.
    float zoom = std::min(4.5f, (SZ - 4) / std::max(14.0f, extent * 0.85f));
    Camera2D cam{};
    cam.zoom = zoom;
    cam.offset = Vector2{ SZ / 2.0f, SZ / 2.0f };
    cam.target = Vector2{ 0, 0 };

    RenderTexture2D rt = LoadRenderTexture(SZ, SZ);
    BeginTextureMode(rt);
    ClearBackground(Color{ 0, 0, 0, 0 });
    BeginMode2D(cam);
    // The real Game is only needed for the HUD bars / transport readout, which
    // don't belong on an icon; an empty stand-in is enough for drawUnit's passes.
    static Game dummy;
    iconRender_ = true;
    drawLegs(dummy, u, 2, cam);             // under-all legs
    drawUnit(dummy, u, cam);                // hull + turrets + over-body legs
    iconRender_ = false;
    EndMode2D();
    EndTextureMode();
    iconCache_[key] = rt.texture;
    out = rt.texture;
    return true;
}

bool Renderer::unitTexture(const std::string& name, int which, Texture2D& out) {
    const UnitTemplate* t = g_templates.find(ToLower(name));
    if (!t) return false;
    return loaded(*t, which, out);
}
