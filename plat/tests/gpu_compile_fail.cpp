// SPDX-License-Identifier: LGPL-2.0-or-later
//
// COMPILE-FAIL cases for the GPU algebra. Each numbered case is guarded by a
// macro; CMake compiles this file once per case with -DPLAT_CF_CASE=N and
// asserts the build FAILS. This is how we prove the type theory actually forbids
// the illegal states — a green test suite that includes "this does not compile".
//
// Case 0 (no macro) is the control: it compiles cleanly.

#include "plat/frame.hpp"
#include "plat/gpu.hpp"

#include <span>
#include <utility>

using namespace plat;

// Minimal backend to construct the types under test.
void plat::TextureDeleter::operator()(ResourceId) const noexcept {}
void plat::PipelineDeleter::operator()(ResourceId) const noexcept {}
struct Ops {
    void draw(std::span<const QuadInstance>, ResourceId) {}
    void finish() {}
};

int main() {
#if PLAT_CF_CASE == 1
    // A Texture must not be COPYABLE (affine ownership). Copy-ctor is deleted.
    Texture a{ResourceId{1}};
    Texture b = a; // <-- must fail: use of deleted copy constructor
    (void)b;
#elif PLAT_CF_CASE == 2
    // A Texture must not be COPY-ASSIGNABLE.
    Texture a{ResourceId{1}};
    Texture b;
    b = a; // <-- must fail: deleted copy assignment
    (void)b;
#elif PLAT_CF_CASE == 3
    // A Frame must not be MOVABLE — it cannot escape its opening scope. This is
    // the linear-token guarantee.
    Frame<Ops> f{Ops{}};
    Frame<Ops> g = std::move(f); // <-- must fail: deleted move constructor
    (void)g;
#elif PLAT_CF_CASE == 4
    // A Frame must not be COPYABLE either.
    Frame<Ops> f{Ops{}};
    Frame<Ops> g = f; // <-- must fail: deleted copy constructor
    (void)g;
#elif PLAT_CF_CASE == 5
    // A distinct handle type: a Pipeline cannot be built from a Texture (they
    // are different Resource<Deleter> instantiations, not interconvertible).
    Texture t{ResourceId{1}};
    Pipeline p = std::move(t); // <-- must fail: no conversion Texture -> Pipeline
    (void)p;
#else
    // Case 0: control — everything legal compiles.
    Texture t{ResourceId{1}};
    Texture t2 = std::move(t); // moving IS allowed
    (void)t2;
    Frame<Ops> f{Ops{}};
    QuadInstance qi{};
    QuadInstance arr[1] = {qi};
    f.draw(std::span<const QuadInstance>{arr}, ResourceId{1});
#endif
    return 0;
}
