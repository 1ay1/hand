// SPDX-License-Identifier: LGPL-2.0-or-later
//
// plat/any_platform.hpp — the ONE type-erasure seam.
//
// Concepts give zero-cost static dispatch but force the whole program generic
// on the backend and fix it at compile time. A terminal wants one binary that
// picks Wayland OR X11 at startup. The type-theoretic way to erase is NOT
// hand-rolled virtuals sprinkled everywhere — it's a SINGLE, concept-GATED
// wrapper:
//
//   * The constructor is `requires Platform<P>` — you physically cannot erase a
//     type that doesn't satisfy the contract. The concept guarantee survives
//     the erasure boundary.
//   * Erasure happens ONCE, at the top (select the backend → wrap it). Below the
//     seam, inside each backend, everything is concrete and inlined.
//   * Only COARSE operations cross the vtable (poll a frame of events, present,
//     window actions) — never per-cell or per-byte work, which lives in
//     toe-core with no abstraction at all. So the virtual call cost is noise.
//
// AnyPlatform itself MODELS Platform (it forwards every required op), so host
// code written against `Platform auto&` works identically whether it holds a
// concrete backend or the erased one — the seam is transparent to callers.

#ifndef PLAT_ANY_PLATFORM_HPP
#define PLAT_ANY_PLATFORM_HPP

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "plat/event.hpp"
#include "plat/gpu.hpp"
#include "plat/platform.hpp"

namespace plat {

// The erased frame: a concrete backend's Frame is non-movable, so we cannot
// store it. Instead AnyPlatform exposes a scoped render callback: you hand it a
// draw function and it opens/uses/closes the frame internally on the concrete
// backend, preserving the linear-Frame guarantee behind the seam.
//
//   any.render(clear, [&](AnyFrame& f){ f.draw(cells, atlas); });
//
// AnyFrame is the erased draw surface passed to that callback.
class AnyFrame {
public:
    virtual ~AnyFrame() = default;
    virtual void draw(std::span<const QuadInstance> instances, ResourceId tex) = 0;
};

class AnyPlatform {
public:
    // Concept-GATED erasure: only a real Platform can be wrapped. This is the
    // single point where static dispatch becomes dynamic; the guarantee that the
    // wrapped thing is a complete Platform is enforced here by the constraint.
    template <class P>
        requires Platform<std::remove_cvref_t<P>> && (!std::same_as<std::remove_cvref_t<P>, AnyPlatform>)
    explicit AnyPlatform(P &&p)
        : self_(std::make_unique<Model<std::remove_cvref_t<P>>>(std::forward<P>(p))) {}

    AnyPlatform(AnyPlatform &&) noexcept = default;
    AnyPlatform &operator=(AnyPlatform &&) noexcept = default;

    // --- Window ---
    [[nodiscard]] Size size() const { return self_->size(); }
    [[nodiscard]] float scale() const { return self_->scale(); }
    [[nodiscard]] bool should_close() const { return self_->should_close(); }
    void set_title(std::string_view t) { self_->set_title(t); }
    void set_decorations(Decorations d) { self_->set_decorations(d); }
    void window_action(WinAction a) { self_->window_action(a); }

    // --- Input ---
    void poll_events(const std::function<void(const Event &)> &sink) { self_->poll_events(sink); }

    // --- Waker ---
    Woke wait(std::span<const int> fds, Deadline d) { return self_->wait(fds, d); }

    // --- Gpu (coarse: texture management + scoped render) ---
    [[nodiscard]] Size gpu_size() const { return self_->gpu_size(); }
    [[nodiscard]] Texture create_texture(Size s, std::span<const std::uint8_t> px) {
        return self_->create_texture(s, px);
    }
    void update_texture(ResourceId t, Size s, std::span<const std::uint8_t> px) {
        self_->update_texture(t, s, px);
    }
    // Open a frame, run `body` against the erased draw surface, auto-present.
    // The concrete backend's linear Frame lives and dies entirely inside here.
    void render(Color clear, const std::function<void(AnyFrame &)> &body) {
        self_->render(clear, body);
    }

    // --- optional refinements, queried at runtime (nullopt/no-op if absent) ---
    [[nodiscard]] bool has_clipboard() const { return self_->has_clipboard(); }
    [[nodiscard]] std::string get_clipboard() { return self_->get_clipboard(); }
    void set_clipboard(std::string_view s) { self_->set_clipboard(s); }
    [[nodiscard]] std::string get_primary() { return self_->get_primary(); }
    void set_primary(std::string_view s) { self_->set_primary(s); }
    void open_url(std::string_view u) { self_->open_url(u); }
    void notify(std::string_view title, std::string_view body) { self_->notify(title, body); }

private:
    // The erased interface (the small vtable). Coarse ops only.
    struct Concept {
        virtual ~Concept() = default;
        virtual Size size() const = 0;
        virtual float scale() const = 0;
        virtual bool should_close() const = 0;
        virtual void set_title(std::string_view) = 0;
        virtual void set_decorations(Decorations) = 0;
        virtual void window_action(WinAction) = 0;
        virtual void poll_events(const std::function<void(const Event &)> &) = 0;
        virtual Woke wait(std::span<const int>, Deadline) = 0;
        virtual Size gpu_size() const = 0;
        virtual Texture create_texture(Size, std::span<const std::uint8_t>) = 0;
        virtual void update_texture(ResourceId, Size, std::span<const std::uint8_t>) = 0;
        virtual void render(Color, const std::function<void(AnyFrame &)> &) = 0;
        virtual bool has_clipboard() const = 0;
        virtual std::string get_clipboard() = 0;
        virtual void set_clipboard(std::string_view) = 0;
        virtual std::string get_primary() = 0;
        virtual void set_primary(std::string_view) = 0;
        virtual void open_url(std::string_view) = 0;
        virtual void notify(std::string_view, std::string_view) = 0;
    };

    // The concrete adapter. `if constexpr` folds the optional refinements: a
    // backend without Clipboard gets a no-op implementation, so AnyPlatform
    // always presents the full surface while backends only implement what they
    // can — the refinement query becomes a runtime bool.
    template <class P>
    struct Model final : Concept {
        P p;
        explicit Model(P v) : p(std::move(v)) {}

        Size size() const override { return p.size(); }
        float scale() const override { return p.scale(); }
        bool should_close() const override { return p.should_close(); }
        void set_title(std::string_view t) override { p.set_title(t); }
        void set_decorations(Decorations d) override { p.set_decorations(d); }
        void window_action(WinAction a) override { p.window_action(a); }
        void poll_events(const std::function<void(const Event &)> &sink) override {
            p.poll_events(sink);
        }
        Woke wait(std::span<const int> fds, Deadline d) override { return p.wait(fds, d); }
        Size gpu_size() const override { return p.size(); }
        Texture create_texture(Size s, std::span<const std::uint8_t> px) override {
            return p.gpu().create_texture(s, px);
        }
        void update_texture(ResourceId t, Size s, std::span<const std::uint8_t> px) override {
            p.gpu().update_texture(t, s, px);
        }
        void render(Color clear, const std::function<void(AnyFrame &)> &body) override {
            auto f = p.begin_frame(clear); // concrete linear Frame — stays local
            FrameAdapter<decltype(f)> af{f};
            body(af); // caller draws through the erased surface
            // f's destructor ends+presents the pass here.
        }
        bool has_clipboard() const override { return Clipboard<P>; }
        std::string get_clipboard() override {
            if constexpr (Clipboard<P>) return p.get_clipboard();
            else return {};
        }
        void set_clipboard(std::string_view s) override {
            if constexpr (Clipboard<P>) p.set_clipboard(s);
        }
        std::string get_primary() override {
            if constexpr (Clipboard<P>) return p.get_primary();
            else return {};
        }
        void set_primary(std::string_view s) override {
            if constexpr (Clipboard<P>) p.set_primary(s);
        }
        void open_url(std::string_view u) override {
            if constexpr (Sys<P>) p.open_url(u);
        }
        void notify(std::string_view t, std::string_view b) override {
            if constexpr (Sys<P>) p.notify(t, b);
        }
    };

    // Wraps a concrete (non-movable) Frame so the erased AnyFrame::draw forwards
    // to it. Lives only for the duration of one render() callback.
    template <class ConcreteFrame>
    struct FrameAdapter final : AnyFrame {
        ConcreteFrame &f;
        explicit FrameAdapter(ConcreteFrame &fr) : f(fr) {}
        void draw(std::span<const QuadInstance> instances, ResourceId tex) override {
            f.draw(instances, tex);
        }
    };

    std::unique_ptr<Concept> self_;
};

} // namespace plat

#endif // PLAT_ANY_PLATFORM_HPP
