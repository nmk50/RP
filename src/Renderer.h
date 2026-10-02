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
    void drawMap(Game& g, const Camera2D& cam);
    void drawUnit(Game& g, const Unit& u, const Camera2D& cam);
    Color teamColor(int team);
};
