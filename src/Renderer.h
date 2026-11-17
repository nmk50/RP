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

private:
    std::unordered_map<std::string, Texture2D> cache_;
    Texture2D missing_;
    bool loaded(const UnitTemplate& t, int which, Texture2D& out); // which:0 body,1 turret,2 wreck
    // [turret_N] image: that barrel's own sprite, resolved like image_turret but by literal name
    bool byName(const UnitTemplate& t, const std::string& nm, Texture2D& out);
    // RWX PlayerTeam.getTeamColorTextures():1421-1508 bakes ten variants of every sprite up front, slot 0
    // being the source image itself. RP bakes the same pixels on demand and keeps them here.
    bool teamed(const UnitTemplate& t, const std::string& nm, int which, int colorIdx, Texture2D& out);
    bool shared(const std::string& file, Texture2D& out);          // assets/drawable/<file>
    void drawMap(Game& g, const Camera2D& cam);
    void drawUnitShadow(const Unit& u);
    void drawUnit(Game& g, const Unit& u, const Camera2D& cam);
    void drawLegs(Game& g, const Unit& u, int pass, const Camera2D& cam);
    // RWX GameLogic.java:1915 / :1984 -- EffectManager.drawEffect(f, 1) runs before the game objects and
    // drawEffect(f, 2) after them, so `under` picks which of the two passes this is.
    void drawFx(Game& g, bool under);
    Color teamColor(int team);

private:
    std::unordered_map<std::string, Texture2D> teamCache_;
};
