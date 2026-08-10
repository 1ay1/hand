// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Positive test for the GPU resource algebra + linear Frame token: exercises
// the LEGAL protocol end-to-end against a mock backend that records calls, and
// asserts the RAII/affine semantics (move transfers ownership, dtor frees once,
// frame auto-finishes). The NEGATIVE space — code that must NOT compile — is
// covered by the compile-fail cases in CMake (see PLAT_COMPILE_FAIL_* below and
// the target in CMakeLists.txt).

#include "plat/frame.hpp"
#include "plat/gpu.hpp"

#include <cstdio>
#include <span>
#include <vector>

using namespace plat;

static int fails = 0;
static void ck(bool ok, const char *n) {
    if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
}

// A record of backend effects, so the test can assert them.
struct Log {
    int textures_freed = 0;
    int draws = 0;
    int finishes = 0;
};
static Log g_log;

// The deleters plat declares must be defined by "the backend"; here the test is
// the backend.
void plat::TextureDeleter::operator()(ResourceId) const noexcept { ++g_log.textures_freed; }
void plat::PipelineDeleter::operator()(ResourceId) const noexcept {}

// Mock FrameOps: records draws + the single finish.
struct MockFrameOps {
    void draw(std::span<const QuadInstance>, ResourceId) { ++g_log.draws; }
    void finish() { ++g_log.finishes; }
};
static_assert(FrameOps<MockFrameOps>, "MockFrameOps must model FrameOps");

// Mock Gpu: satisfies the Gpu concept; hands out MockFrameOps-backed Frames.
struct MockGpu {
    Size size_{800, 600};
    std::uint32_t next_id = 1;

    [[nodiscard]] Size size() const noexcept { return size_; }
    Texture create_texture(Size, std::span<const std::uint8_t>) {
        return Texture{ResourceId{next_id++}};
    }
    void update_texture(ResourceId, Size, std::span<const std::uint8_t>) {}
    Frame<MockFrameOps> begin_frame(Color) { return Frame<MockFrameOps>{MockFrameOps{}}; }
};
static_assert(Gpu<MockGpu>, "MockGpu must model Gpu");

int main() {
    // --- affine Texture: move transfers ownership; dtor frees exactly once ---
    g_log = {};
    {
        MockGpu gpu;
        const std::vector<std::uint8_t> px(4, 0);
        Texture t = gpu.create_texture(Size{1, 1}, px);
        ck(t.valid(), "texture handle valid after create");
        Texture t2 = std::move(t);           // ownership moves
        ck(!t.valid(), "moved-from texture is null");
        ck(t2.valid(), "moved-to texture owns the id");
        // t (null) + t2 (owning) leave scope: only ONE free must happen.
    }
    ck(g_log.textures_freed == 1, "exactly one free for a moved-then-dropped texture");

    // release() relinquishes without freeing.
    g_log = {};
    {
        MockGpu gpu;
        const std::vector<std::uint8_t> px(4, 0);
        Texture t = gpu.create_texture(Size{1, 1}, px);
        ResourceId raw = t.release();
        ck(raw.valid() && !t.valid(), "release hands out the id and nulls the handle");
    }
    ck(g_log.textures_freed == 0, "released texture is NOT auto-freed");

    // --- linear Frame: begin -> draw -> auto-finish at scope end -------------
    g_log = {};
    {
        MockGpu gpu;
        std::vector<QuadInstance> cells(3);
        {
            auto f = gpu.begin_frame(Color{20, 20, 26});
            f.draw(cells, ResourceId{1});
            f.draw(cells);            // bg-only batch (null texture)
            // no explicit finish: the destructor must end the pass.
        }
        ck(g_log.draws == 2, "two draw batches issued inside the frame");
        ck(g_log.finishes == 1, "frame auto-finished exactly once at scope end");
    }

    // explicit finish is idempotent (no double-commit).
    g_log = {};
    {
        MockGpu gpu;
        auto f = gpu.begin_frame(Color{});
        f.finish();
        f.finish(); // no-op
    }
    ck(g_log.finishes == 1, "explicit finish + dtor => still exactly one finish");

    std::printf(fails ? "%d GPU ALGEBRA TEST(S) FAILED\n" : "ALL GPU ALGEBRA TESTS PASS\n", fails);
    return fails ? 1 : 0;
}
