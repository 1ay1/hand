// SPDX-License-Identifier: LGPL-2.0-or-later
//
// plat/sokol_gpu.cpp — SokolGpu impl: the one TU that includes sokol_gfx.h.
//
// A minimal instanced-quad pipeline: a unit quad (triangle strip) drawn once
// per QuadInstance, positioned/sized from the instance's pixel rect against a
// screen-size uniform, tinted by the instance colour, and multiplied by a
// sampled texel (a 1x1 white image stands in for null-texture solid draws).
// This is deliberately simpler than toe's cell shader (no SDF/rounded-corner
// branch yet) — enough to prove the plat::Gpu contract drives real pixels.

#include "plat/sokol_gpu.hpp"

#include <cstring>
#include <utility>
#include <vector>

#include "sokol/sokol_gfx.h"

namespace plat {

namespace {

// GLSL 4.10 core (matches toe's glcore backend). Vertex expands the unit quad
// to the instance's pixel rect and maps to clip space via uScreen; fragment
// tints by colour * sampled texel.
constexpr const char *kVS = R"(#version 410
layout(location=0) in vec2 aCorner;   // unit quad corner in [0,1]
layout(location=1) in vec4 aRect;     // x,y,w,h in pixels
layout(location=2) in vec4 aUV;       // u0,v0,u1,v1
layout(location=3) in vec4 aColor;    // rgba normalized
uniform vec2 uScreen;                 // framebuffer size in px
out vec2 vUV;
out vec4 vColor;
void main() {
    vec2 px = aRect.xy + aCorner * aRect.zw;
    vec2 ndc = vec2(px.x / uScreen.x * 2.0 - 1.0, 1.0 - px.y / uScreen.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = mix(aUV.xy, aUV.zw, aCorner);
    vColor = aColor;
}
)";

constexpr const char *kFS = R"(#version 410
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 frag;
void main() {
    frag = vColor * texture(uTex, vUV);
}
)";

} // namespace

// --- resource deleters plat declared -----------------------------------
void TextureDeleter::operator()(ResourceId id) const noexcept {
    if (id.valid()) sg_destroy_image(sg_image{id.value});
}
void PipelineDeleter::operator()(ResourceId id) const noexcept {
    if (id.valid()) sg_destroy_pipeline(sg_pipeline{id.value});
}

SokolGpu::~SokolGpu() {
    if (pip_) sg_destroy_pipeline(sg_pipeline{pip_});
    if (shd_) sg_destroy_shader(sg_shader{shd_});
    if (quad_vbuf_) sg_destroy_buffer(sg_buffer{quad_vbuf_});
    if (inst_vbuf_) sg_destroy_buffer(sg_buffer{inst_vbuf_});
    if (smp_) sg_destroy_sampler(sg_sampler{smp_});
    if (white_view_) sg_destroy_view(sg_view{white_view_});
    if (white_) sg_destroy_image(sg_image{white_});
}

SokolGpu::SokolGpu(SokolGpu &&o) noexcept { *this = std::move(o); }
SokolGpu &SokolGpu::operator=(SokolGpu &&o) noexcept {
    if (this != &o) {
        target_ = o.target_;
        pip_ = std::exchange(o.pip_, 0);
        shd_ = std::exchange(o.shd_, 0);
        quad_vbuf_ = std::exchange(o.quad_vbuf_, 0);
        inst_vbuf_ = std::exchange(o.inst_vbuf_, 0);
        smp_ = std::exchange(o.smp_, 0);
        white_ = std::exchange(o.white_, 0);
        white_view_ = std::exchange(o.white_view_, 0);
        pass_open_ = o.pass_open_;
    }
    return *this;
}

void SokolGpu::ensure_pipeline() {
    if (pip_) return;

    // Unit quad as a triangle strip: (0,0)(1,0)(0,1)(1,1).
    const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    sg_buffer_desc qb = {};
    qb.data = SG_RANGE(quad);
    quad_vbuf_ = sg_make_buffer(&qb).id;

    sg_buffer_desc ib = {};
    ib.size = std::size_t{64000} * sizeof(QuadInstance);
    ib.usage.stream_update = true;
    inst_vbuf_ = sg_make_buffer(&ib).id;

    sg_sampler_desc sd = {};
    sd.min_filter = SG_FILTER_LINEAR;
    sd.mag_filter = SG_FILTER_LINEAR;
    sd.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    smp_ = sg_make_sampler(&sd).id;

    // 1x1 white image so null-texture draws multiply by 1.
    const std::uint8_t white_px[4] = {255, 255, 255, 255};
    sg_image_desc wd = {};
    wd.width = 1;
    wd.height = 1;
    wd.pixel_format = SG_PIXELFORMAT_RGBA8;
    sg_image_data wdat = {};
    wdat.mip_levels[0] = {white_px, sizeof white_px};
    wd.data = wdat;
    white_ = sg_make_image(&wd).id;
    sg_view_desc wv = {};
    wv.texture.image = sg_image{white_};
    white_view_ = sg_make_view(&wv).id;

    sg_shader_desc shd = {};
    shd.vertex_func.source = kVS;
    shd.fragment_func.source = kFS;
    // Uniform block 0 (vs): uScreen vec2.
    shd.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    shd.uniform_blocks[0].size = sizeof(float) * 2;
    shd.uniform_blocks[0].glsl_uniforms[0].glsl_name = "uScreen";
    shd.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_FLOAT2;
    // Sampled texture + sampler (fs).
    shd.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    shd.views[0].texture.image_type = SG_IMAGETYPE_2D;
    shd.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    shd.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    shd.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    shd.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    shd.texture_sampler_pairs[0].view_slot = 0;
    shd.texture_sampler_pairs[0].sampler_slot = 0;
    shd.texture_sampler_pairs[0].glsl_name = "uTex";
    shd_ = sg_make_shader(&shd).id;

    sg_pipeline_desc pd = {};
    pd.shader = sg_shader{shd_};
    pd.layout.buffers[0].step_func = SG_VERTEXSTEP_PER_VERTEX;
    pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
    pd.layout.buffers[1].stride = sizeof(QuadInstance);
    pd.layout.attrs[0] = {0, 0, SG_VERTEXFORMAT_FLOAT2};   // aCorner
    pd.layout.attrs[1] = {1, 0, SG_VERTEXFORMAT_FLOAT4};   // aRect
    pd.layout.attrs[2] = {1, 16, SG_VERTEXFORMAT_FLOAT4};  // aUV
    pd.layout.attrs[3] = {1, 32, SG_VERTEXFORMAT_UBYTE4N}; // aColor
    pd.primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    pd.colors[0].blend.enabled = true;
    pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
    pip_ = sg_make_pipeline(&pd).id;
}

Texture SokolGpu::create_texture(Size s, std::span<const std::uint8_t> rgba) {
    if (s.w <= 0 || s.h <= 0) return {};
    sg_image_desc d = {};
    d.width = s.w;
    d.height = s.h;
    d.pixel_format = SG_PIXELFORMAT_RGBA8;
    d.usage.dynamic_update = true;
    sg_image img = sg_make_image(&d);
    if (img.id != SG_INVALID_ID && !rgba.empty()) {
        sg_image_data data = {};
        data.mip_levels[0] = {rgba.data(), rgba.size()};
        sg_update_image(img, &data);
    }
    // Wrap the image AND a view in one ResourceId space: we store the image id
    // and make a view on demand at draw time. For simplicity here, the Texture
    // owns the image; the view is created lazily and cached alongside — but to
    // keep the handle a single id, we make the view now and store the image in
    // the high bits is overkill. Instead: store the image id; draw() makes a
    // transient view. (sokol views are cheap.) The deleter frees the image.
    return Texture{ResourceId{img.id}};
}

void SokolGpu::update_texture(ResourceId id, Size s, std::span<const std::uint8_t> rgba) {
    if (!id.valid() || rgba.empty()) return;
    (void)s;
    sg_image_data data = {};
    data.mip_levels[0] = {rgba.data(), rgba.size()};
    sg_update_image(sg_image{id.value}, &data);
}

Frame<SokolGpu::Ops> SokolGpu::begin_frame(Color clear) {
    ensure_pipeline();
    sg_pass pass = {};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = {clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f,
                                         clear.a / 255.0f};
    // Offscreen FBO vs default swapchain. For the headless path the host binds
    // its own GL FBO and we render into the "swapchain" (sokol_gl glue in tests
    // handles the FBO bind); here we begin a swapchain pass sized to target_.
    sg_swapchain sw = {};
    sw.width = target_.size.w;
    sw.height = target_.size.h;
    sw.color_format = SG_PIXELFORMAT_RGBA8;
    sw.depth_format = SG_PIXELFORMAT_NONE;
    sw.sample_count = 1;
    sw.gl.framebuffer = target_.fbo;
    pass.swapchain = sw;
    sg_begin_pass(&pass);
    pass_open_ = true;
    return Frame<Ops>{Ops{this}};
}

void SokolGpu::Ops::draw(std::span<const QuadInstance> instances, ResourceId tex) {
    if (!dev || instances.empty()) return;
    const int off = sg_append_buffer(sg_buffer{dev->inst_vbuf_},
                                     sg_range{instances.data(), instances.size_bytes()});
    sg_apply_pipeline(sg_pipeline{dev->pip_});

    // Texture: a valid id → make a transient view; else the 1x1 white view.
    std::uint32_t view = dev->white_view_;
    sg_view transient = {};
    if (tex.valid()) {
        sg_view_desc vd = {};
        vd.texture.image = sg_image{tex.value};
        transient = sg_make_view(&vd);
        view = transient.id;
    }

    sg_bindings bind = {};
    bind.vertex_buffers[0] = sg_buffer{dev->quad_vbuf_};
    bind.vertex_buffers[1] = sg_buffer{dev->inst_vbuf_};
    bind.vertex_buffer_offsets[1] = off;
    bind.views[0] = sg_view{view};
    bind.samplers[0] = sg_sampler{dev->smp_};
    sg_apply_bindings(&bind);

    const float uScreen[2] = {static_cast<float>(dev->target_.size.w),
                              static_cast<float>(dev->target_.size.h)};
    sg_apply_uniforms(0, sg_range{uScreen, sizeof uScreen});
    sg_draw(0, 4, static_cast<int>(instances.size()));

    if (transient.id != SG_INVALID_ID) sg_destroy_view(transient);
}

void SokolGpu::Ops::finish() {
    if (dev && dev->pass_open_) {
        sg_end_pass();
        dev->pass_open_ = false;
    }
}

} // namespace plat
