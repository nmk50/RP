// Renderer: terrain, units, projectiles, effects, overlays. Owns a lazy texture cache.
#pragma once
#include <string>
#include <unordered_map>
#include <raylib.h>
#include "Game.h"

struct ViewState {
    Camera2D cam;
    bool placing = false;
    std::string placingName;
    Vec2 placingWorld;
    bool placingValid = false;
    float placeFacing = -3.14159265f / 2.0f;  // ghost rotation while placing (RWX buildingRotation)
    bool selActive = false;
    Vec2 selA, selB;
    Vec2 cmdMarker; bool cmdMarkerOn = false; float cmdMarkerT = 0;
};

class Renderer {
public:
    void init();
    void close();
    void drawWorld(Game& g, const ViewState& view);
    bool unitTexture(const std::string& name, int which, Texture2D& out);
    // Offscreen render of a unit template with every layer (body + turrets + legs),
    // used for command-bar icons. Cached by template name.
    bool unitIcon(const std::string& tplName, Texture2D& out);

private:
    std::unordered_map<std::string, Texture2D> cache_;
    Texture2D missing_;
    bool loaded(const UnitTemplate& t, int which, Texture2D& out); // which:0 body,1 turret,2 wreck
    // [turret_N] image: that barrel's own sprite, resolved like image_turret but by literal name
    bool byName(const UnitTemplate& t, const std::string& nm, Texture2D& out);
    // [graphics] image_shadow naming a file: resolved like any other image, but cached in its own slot
    // so a SHADOW:-blackened copy can never be handed to a turret that happens to name the same file
    // (the mechs do exactly that: hull image topGun.png, shadow SHADOW:topGun.png).
    bool shadowByName(const UnitTemplate& t, const std::string& nm, Texture2D& out);
    // RWX PlayerTeam.getTeamColorTextures():1421-1508 bakes ten variants of every sprite up front, slot 0
    // being the source image itself. RP bakes the same pixels on demand and keeps them here.
    bool teamed(const UnitTemplate& t, const std::string& nm, int which, int colorIdx, Texture2D& out);
    bool shared(const std::string& file, Texture2D& out);          // assets/drawable/<file>
    // RWX Tileset.initTextureMetrics: load tilesets/bitmaps/<imageKey>, columns = bitmapWidth / strideX.
    bool tilesetTexture(const TilesetRender& ts, Texture2D& out, int& columns);
    void drawMap(Game& g, const Camera2D& cam);
    // RWX MapLayer's per-tile fog tint + the fog atlas behind it: never-seen tiles are black, explored
    // ones are dimmed, tiles in sight get nothing. Drawn over the terrain and under every game object.
    void drawFog(Game& g, const Camera2D& cam);
    void drawUnitShadow(const Unit& u);
    void drawUnit(Game& g, const Unit& u, const Camera2D& cam);
    void drawLegs(Game& g, const Unit& u, int pass, const Camera2D& cam);
    void drawTree(const Unit& u);   // RWX Tree.c(): shared atlas sprite
    // RWX GameLogic.java:1915 / :1984 -- EffectManager.drawEffect(f, 1) runs before the game objects and
    // drawEffect(f, 2) after them, so `under` picks which of the two passes this is.
    void drawFx(Game& g, bool under);
    // RWX BuildPreview.drawAll: blue "pending site" markers shown after a build order, while the
    // builder walks over. Drawn after the terrain/fx but before the units.
    void drawBlueprints(Game& g, const Camera2D& cam);
    Color teamColor(int team);

private:
    std::unordered_map<std::string, Texture2D> teamCache_;
    std::unordered_map<std::string, Texture2D> tilesetCache_; // key: imageKey basename
    std::unordered_map<std::string, Texture2D> iconCache_;    // full unit previews for the command bar
    bool iconRender_ = false;    // true while drawing a command-bar icon (no HP/selection overlays)
    // Multiplied into the hull paint while a build preview ghost is drawn (BuildPreview.draw:318 draws
    // the real unit translucent instead of a coloured block). 1 = normal.
    float ghostAlpha_ = 1.0f;
    // The ghost turns red when the site cannot be built on: RWX marks it isUnitCapturable, which
    // getRenderPaint:4309-4312 answers with a red overlay filter.
    Color ghostTint_{ 255, 255, 255, 255 };
};
