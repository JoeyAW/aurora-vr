#pragma once

#include "aurora/aurora.h"
#include "webgpu/gpu.hpp"

#include <SDL3/SDL_events.h>
#include <aurora/rmlui.hpp>
#include <dawn/webgpu_cpp.h>

namespace aurora::rmlui {

struct RecordedFrame {
  wgpu::BindGroup bindGroup;
  bool overlay = false;
};

void initialize(const AuroraWindowSize& size) noexcept;
void handle_event(SDL_Event& event) noexcept;
RecordedFrame record_frame(const webgpu::Viewport& presentViewport) noexcept;
void shutdown() noexcept;

// Read-only accessor for VR's menu billboard (src/dusk/vr/vr_stereo_render.hpp)
// -- returns whatever record_frame() last rendered into s_renderTarget, or a
// default-constructed TextureWithSampler (view == null) if RmlUi hasn't
// rendered a frame yet this session. Does NOT render anything itself.
//
// One frame of latency versus what's currently on screen is inherent, not a
// bug: record_frame() is only ever called from aurora::end_frame(), which
// (per src/m_Do/m_Do_main.cpp's call order) always runs AFTER
// dusk::vr::tick() has already finished its own per-eye rendering for that
// same frame -- so s_renderTarget is always a fully-finished PREVIOUS frame
// throughout the whole VR eye loop, never a torn/in-progress one. No
// double-buffering needed as a result.
const webgpu::TextureWithSampler& get_render_target() noexcept;

// Forces record_frame() to skip the backdrop-blur background (as if
// context_has_visible_backdrop_filter() had returned false) regardless of
// whether a document actually has one, for as long as `force` is true.
//
// EXISTS TO BREAK A REAL FEEDBACK LOOP found 2026-08-16 by VR's menu
// billboard (src/dusk/vr/vr_stereo_render.hpp): when a document's backdrop
// filter is visible, record_frame() passes webgpu::present_source() into
// RenderInterface::BeginFrame() as the "scene" content that gets baked
// into s_renderTarget for the blur to sample. But present_source() is ALSO
// what the VR desktop-mirror feature (aurora::gfx::set_present_source_mirror(),
// gfx.hpp) points at the just-rendered VR eye texture -- and that same eye
// texture, this same frame, was drawn INCLUDING the menu billboard, which
// itself samples s_renderTarget from the frame before. Left unbroken, this
// is a genuine real-time video feedback loop (visually: "my view is
// looping in the window", one more generation of nesting every frame) --
// not a cosmetic bug, an actual unbounded circular dependency between
// RmlUi's own backdrop content and VR's billboard capture of RmlUi's
// output. Call with `force=true` whenever VR's desktop mirror is actually
// active this frame (see vr_main.cpp's tick(), right where
// set_present_source_mirror()/clear_present_source_mirror() are already
// called) -- the menu simply renders without its blurred-background effect
// while that's the case, a minor, acceptable visual tradeoff against an
// actual infinite-recursion artifact.
void set_force_no_backdrop(bool force) noexcept;

} // namespace aurora::rmlui
