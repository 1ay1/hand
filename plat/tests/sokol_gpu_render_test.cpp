// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Headless proof that plat::SokolGpu drives REAL pixels through the
// type-theoretic API: create an offscreen GL context, sg_setup, make a
// SokolGpu, then via the LINEAR Frame draw a coloured quad into an FBO and read
// the pixels back. Verifies (a) the quad's colour lands where expected and (b)
// the clear colour lands elsewhere — i.e. the whole begin_frame → draw →
// auto-present path works on hardware, not a mock.

#include "plat/sokol_gpu.hpp"

#include <cstdio>
#include <vector>

#include "hand/platform/sokol_gl.hpp" // pulls <GL/gl.h> with GL_GLEXT_PROTOTYPES
#include "hand/platform/testing.hpp"

using namespace plat;

static int fails = 0;
static void ck(bool ok, const char *n) {
    if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
}

int main() {
    if (!hand::platform::make_offscreen_context_current(toe::PixelSize{64, 64})) {
        std::fprintf(stderr, "skip: no offscreen GL context\n");
        return 77;
    }
    hand::platform::sokolgl::setup();

    constexpr int W = 200, H = 120;

    // An offscreen FBO to render into + read back.
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    SokolGpu gpu;
    gpu.set_target(PassTarget{fbo, Size{W, H}});
    ck(gpu.size() == Size{W, H}, "gpu reports its target size");

    // One green quad covering the rect (40,30)-(120,90), on a dark clear.
    QuadInstance q{};
    q.x = 40; q.y = 30; q.w = 80; q.h = 60;
    q.u0 = 0; q.v0 = 0; q.u1 = 1; q.v1 = 1;
    q.r = 40; q.g = 200; q.b = 90; q.a = 255;
    QuadInstance arr[1] = {q};

    // The type-theoretic render: begin_frame yields the linear Frame; draw
    // through it; the Frame's dtor ends the pass. sokol commits below.
    {
        auto f = gpu.begin_frame(Color{18, 18, 24, 255});
        f.draw(std::span<const QuadInstance>{arr}, ResourceId{}); // null tex => solid
    }
    sg_commit();
    glFinish();

    std::vector<unsigned char> px(static_cast<std::size_t>(W) * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

    auto at = [&](int x, int y) {
        // glReadPixels is bottom-up; flip y to sample in top-down coords.
        const std::size_t i = (static_cast<std::size_t>(H - 1 - y) * W + x) * 4;
        return std::array<int, 3>{px[i], px[i + 1], px[i + 2]};
    };

    // Centre of the quad (80,60) must be greenish.
    auto c = at(80, 60);
    ck(c[1] > 150 && c[0] < 90 && c[2] < 130, "quad centre is the drawn green");
    // A corner (5,5) must be the clear colour, not green.
    auto bg = at(5, 5);
    ck(bg[1] < 60, "outside the quad is the clear colour");
    std::printf("quad rgb=(%d,%d,%d) bg rgb=(%d,%d,%d)\n", c[0], c[1], c[2], bg[0], bg[1], bg[2]);

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);

    std::printf(fails ? "%d SOKOL GPU RENDER TEST(S) FAILED\n" : "SOKOL GPU RENDER TEST PASS\n",
                fails);
    return fails ? 1 : 0;
}
