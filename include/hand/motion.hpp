#pragma once
// hand::motion — the cursor's movement and the little shows around it.
//
//   movement   none · glide (eased, short trail) · spring (overshoots, settles)
//              · smear (the leading edge jumps, the tail stretches after it)
//   effects    ripple · sonicboom · wireframe · pixiedust · railgun · torpedo
//              · fireworks (on Enter) -- a tiny particle system, drawn as nodes
//   blink      hard · fade · breathe
//
// Pure model + step + draw; the app owns a State, steps it on frame ticks,
// emits on typed cursor moves, and draws it over the grid.

#include "config.hpp"

#include <mayag/mayag.hpp>

#include <cstdint>
#include <vector>

namespace hand::motion {

struct Particle {
    float x = 0, y = 0, vx = 0, vy = 0, g = 0;   ///< px, px/s, px/s²
    float life = 0, max = 1;                     ///< s
    float size = 2, w = 0, h = 0;
    std::uint8_t kind = 0;                       ///< 0 dot, 1 ring, 2 disc, 3 frame
    std::uint32_t rgb = 0xFFFFFF;
};

struct State {
    std::vector<Particle> ps;
    std::uint32_t seed = 0x9E3779B9u;
    int  last_col = -1, last_row = -1;
    double typed_at = -10.0, enter_at = -10.0;   ///< app clock
    float vx = 0.0f, vy = 0.0f;                  ///< spring velocity, cells/s
    float tx = 0.0f, ty = 0.0f;                  ///< the tail: lags the head by `trail`, cells
    bool  tail_known = false;
    [[nodiscard]] bool busy() const noexcept { return !ps.empty(); }
};

/// The cursor moved from (fx,fy) to (tx,ty) cells after typing: emit `fx`.
/// (cx,cy) = the new cell's centre in window px.
void emit(State& s, CursorFx fx, float cx, float cy, float dirx, float diry, float cw, float ch, bool enter,
          const Palette& pal);
/// While the drawn cursor travels (torpedo): a puff where it is now.
void travel(State& s, CursorFx fx, float cx, float cy, float ch, const Palette& pal);
/// Advance every particle by dt seconds.
void step(State& s, float dt);
/// The particles as absolute-positioned nodes.
[[nodiscard]] std::vector<mayag::Node> draw(const State& s);
/// The smear: tapered copies of the cursor from (lx,ly) back to the cell (tx,ty), px.
[[nodiscard]] std::vector<mayag::Node> smear(float lx, float ly, float tx, float ty, float cw, float ch, std::uint32_t rgb);

/// Advance the drawn cursor (x,y cells) toward its cell (tx,ty) by dt.
void move_cursor(State& s, CursorAnim anim, float& x, float& y, float tx, float ty, float dt);
/// Advance the tail toward the head (hx,hy cells); `length` 0 = no tail.
void move_tail(State& s, float hx, float hy, float length, float dt);
[[nodiscard]] bool tail_settled(const State& s, float hx, float hy);
/// The comet: fading, shrinking ghosts from the head (hx,hy) back to the tail, px.
[[nodiscard]] std::vector<mayag::Node> comet(float hx, float hy, float tx, float ty, float cw, float ch, std::uint32_t rgb,
                                             float strength);
[[nodiscard]] bool cursor_settled(const State& s, CursorAnim anim, float x, float y, float tx, float ty);

}  // namespace hand::motion
