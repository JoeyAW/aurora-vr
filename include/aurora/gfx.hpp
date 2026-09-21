#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx {

inline constexpr size_t InlineDrawPayloadSize = 128;
inline constexpr size_t MaxColorAttachments = 8;
inline constexpr uint32_t SceneColorAttachmentIndex = 0;

enum class ColorAttachmentSemantic : uint8_t {
  SceneColor,
  Normal,
  Auxiliary,
};

struct ColorAttachmentLayout {
  ColorAttachmentSemantic semantic = ColorAttachmentSemantic::Auxiliary;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct RenderTargetLayout {
  uint64_t key = 0;
  uint32_t colorAttachmentCount = 0;
  std::array<ColorAttachmentLayout, MaxColorAttachments> colorAttachments{};
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  uint32_t sampleCount = 1;
};

/// Generational handle: 0 is never valid, and IDs are not reused after unregister_draw_type.
using DrawTypeId = uint64_t;
inline constexpr DrawTypeId InvalidDrawType = 0;

struct Range {
  uint32_t offset = 0;
  uint32_t size = 0;

  bool operator==(const Range& rhs) const { return offset == rhs.offset && size == rhs.size; }
  bool operator!=(const Range& rhs) const { return !(*this == rhs); }
};

struct DrawContext {
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer storageBuffer;
  RenderTargetLayout layout;
};

/// Invoked on the render worker thread while replaying the pass the draw was
/// recorded into. The encoder's pipeline/bind-group/viewport/scissor state is
/// restored after the callback returns. Handles in the context are borrowed and
/// valid only for the duration of the call. targetLayout and target dimensions
/// describe the containing pass.
using DrawCallback = void (*)(const DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* payload,
                              size_t payloadSize, void* userdata);

struct DrawTypeDescriptor {
  const char* label = nullptr;
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
};

wgpu::Device device() noexcept;
wgpu::Queue queue() noexcept;
wgpu::TextureFormat color_format() noexcept;
wgpu::TextureFormat depth_format() noexcept;
uint32_t sample_count() noexcept;
RenderTargetLayout scene_render_target_layout() noexcept;
bool uses_reversed_z() noexcept;

DrawTypeId register_draw_type(const DrawTypeDescriptor& desc);
void unregister_draw_type(DrawTypeId type) noexcept;
/// Records an inline custom draw into the currently open render pass at the
/// current position in the command stream. Payload (<= InlineDrawPayloadSize)
/// is copied. Returns false (with a warning) outside an active render pass.
bool push_custom_draw(DrawTypeId type, const void* payload, size_t payloadSize);

/// Generational handle: 0 is never valid, and IDs are not reused after unregister_encoder_task_type.
using EncoderTaskId = uint64_t;
inline constexpr EncoderTaskId InvalidEncoderTask = 0;

struct EncoderTaskContext {
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer storageBuffer;
};

struct EncoderTaskCompletionContext {
  wgpu::Device device;
  wgpu::Queue queue;
};

/// Invoked on the render worker thread with the frame's command encoder,
/// positioned between two render passes. The callback may begin/end compute
/// passes and record copies on the encoder; it must leave no pass open when it
/// returns and must not Finish the encoder. Handles in the context are borrowed
/// and valid only for the duration of the call. Data appended to the streaming
/// buffers before the task was pushed is GPU-visible inside it.
using EncoderTaskCallback = void (*)(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd,
                                     const void* payload, size_t payloadSize, void* userdata);

/// Invoked on the render worker after the frame command buffer containing the
/// encoder task has been submitted. This can be used to e.g. call present on
/// an externally managed surface rendered to by the task.
using EncoderTaskCompletionCallback = void (*)(const EncoderTaskCompletionContext& ctx, const void* payload,
                                               size_t payloadSize, void* userdata);

struct EncoderTaskDescriptor {
  const char* label = nullptr;
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  EncoderTaskCompletionCallback afterSubmit = nullptr;
};

EncoderTaskId register_encoder_task_type(const EncoderTaskDescriptor& desc);
void unregister_encoder_task_type(EncoderTaskId type) noexcept;
/// Seals the current EFB pass, records an encoder task to execute on the frame
/// encoder at this point, and resumes rendering on a pass that loads the
/// existing contents. Payload semantics match push_custom_draw. Returns false
/// (with a warning) outside an active render pass or while an offscreen pass
/// is open.
bool push_encoder_task(EncoderTaskId type, const void* payload, size_t payloadSize);

/// Append transient data to the shared per-frame streaming buffers. Returned
/// ranges are valid for the current frame only. Returns an empty Range (with a
/// warning) outside an active recording frame.
Range push_verts(const uint8_t* data, size_t length, size_t alignment);
Range push_indices(const uint8_t* data, size_t length, size_t alignment);
Range push_uniform(const uint8_t* data, size_t length);
Range push_storage(const uint8_t* data, size_t length);

struct ResolveDesc {
  bool color = true;
  bool depth = false;
  bool normal = false;
};

struct ResolvedTargets {
  wgpu::TextureView color;  // single-sample snapshot; null if not requested
  wgpu::Texture colorTexture; // single-sample snapshot texture backing `color`; null if not requested
  wgpu::TextureView depth;  // single-sample R32Float depth snapshot; null if not requested
  wgpu::TextureView normal; // RGB10A2Unorm snapshot; null when not requested or unavailable
  wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
};

/// Snapshots the current pass targets into pooled textures (valid for the
/// current frame), then: on the EFB, continues rendering on a fresh EFB pass
/// (GXCopyTex semantics); in an offscreen pass created by create_pass, ends it
/// and restores the suspended EFB pass (GXRestoreFrameBuffer semantics).
/// Requesting no attachments is a plain pass break (or offscreen close,
/// discarding its output). Depth is left null when unsupported by the device.
/// Returns false (with a warning) outside an active render pass. Scene pipelines
/// must rebuild based on layout.key, which can change at runtime.
/// 
/// **Normals:**
/// The first resolve with normals will enable the normal attachment at the start
/// of the next frame and stay enabled until shutdown. Before the normal buffer
/// gets enabled, the resolved normal buffer will be null. Resolved normals will
/// also be null in offscreen passes and when unsupported by the device.
bool resolve_pass(const ResolveDesc& desc, ResolvedTargets& out);

// NEW this session (VR_MOD_HANDOFF_10 follow-up): 0 when there's no active
// pass; otherwise a unique id identifying the current RenderPass object,
// stable across ordinary rendering but NOT preserved if something silently
// substitutes a different pass in (see resolve_pass_checked() below for why
// that matters and when it happens). A caller that opens an offscreen pass
// via create_pass() and needs to be sure the SAME pass is still current
// later (not just that *some* offscreen pass is open -- is_offscreen()
// alone can't tell the two apart) should capture this right after
// create_pass() succeeds.
uint64_t current_pass_id() noexcept;

// Companion to current_pass_id() (NEW this session): the current pass's
// colorView, or null outside a pass. resolve_pass_into() always carries the
// same colorView/depthStencilView forward onto the new pass object it
// substitutes in (new id, same underlying render target) -- so a caller
// whose expected pass id (from current_pass_id()) no longer matches can
// still check whether its real render target is the current one via this,
// rather than assuming an id mismatch always means the target is gone. See
// resolve_pass_checked() below.
wgpu::TextureView current_pass_color_view() noexcept;

// Same contract as resolve_pass(), except it first verifies the current
// pass is still the exact one identified by expectedPassId (from
// current_pass_id(), captured right after this caller's own create_pass()
// call). Confirmed this session: resolve_pass_into() (the internal path
// behind ordinary in-game GXCopyTex draining -- shadows, HUD, menu
// overlays, used constantly by normal gameplay) seals whatever pass is
// current and substitutes a new one in its place, entirely independent of
// is_offscreen()'s state -- it doesn't check that flag at all. If that
// happens while this caller's own offscreen pass is still supposed to be
// open (e.g. during the scene draw between create_pass()/resolve_pass()),
// is_offscreen() stays true but the pass object underneath has changed.
// resolve_pass_checked() catches that: on mismatch it logs a warning
// identifying this as a foreign-substitution case and returns false
// WITHOUT resolving/snapshotting the wrong pass. Treat a false return the
// same as resolve_pass() returning false -- skip whatever this frame would
// have done with the (now-absent) targets, rather than proceeding with a
// wrong-sized or otherwise-corrupt result.
// resolve_pass_checked() catches that: on an id mismatch it now (this
// session) also checks expectedColorView (if given, from
// current_pass_color_view() captured alongside expectedPassId) against the
// current pass's colorView -- resolve_pass_into()'s substitution always
// carries the same colorView forward, so a still-matching view means the
// real render target survived and this resolves normally instead of
// refusing. Only when the id AND the colorView both no longer match (the
// real target is genuinely gone, not just re-wrapped) does it log the
// foreign-substitution warning and return false WITHOUT resolving/
// snapshotting the wrong pass. expectedColorView defaults to null for
// callers that don't have one to give (id-only check, prior behavior).
// Treat a false return the same as resolve_pass() returning false -- skip
// whatever this frame would have done with the (now-absent) targets,
// rather than proceeding with a wrong-sized or otherwise-corrupt result.
bool resolve_pass_checked(const ResolveDesc& desc, ResolvedTargets& out, uint64_t expectedPassId,
                          wgpu::TextureView expectedColorView = nullptr);

/// Desktop mirror support: makes the game's desktop window show `source`
/// (e.g. a VR eye's just-resolved ResolvedTargets) instead of whatever the
/// normal flatscreen draw would otherwise put there. Internally just points
/// aurora's own present-resample pass (which samples SOME source and
/// scales/letterboxes it into the real OS window every frame regardless of
/// caller) at a different, already-rendered texture -- no extra render pass,
/// no CPU readback, effectively free. No-ops if `source.color`/
/// `source.colorTexture` are null (e.g. a skipped/foreign-substituted eye
/// this frame -- see resolve_pass_checked()'s own comment for when that
/// happens); the previous override (or none) is left as-is in that case.
/// `source` must be a snapshot texture that outlives this call (true for
/// anything resolve_pass()/resolve_pass_checked() hands back -- pooled for
/// the current frame). Call clear_present_source_mirror() once nothing
/// should override the desktop window anymore (e.g. VR session ended, or
/// mirroring turned off in settings) -- otherwise the last-set source keeps
/// showing indefinitely, including stale content from a since-ended session.
void set_present_source_mirror(const ResolvedTargets& source) noexcept;
void clear_present_source_mirror() noexcept;

/// Suppresses acquiring/presenting the OS window surface at end_frame()
/// (the frame's GPU work is still submitted exactly as before -- only the
/// final blit into the window's swapchain image and the Present() are
/// skipped). Added 2026-09-19 for standalone VR on Android (Quest): while an
/// OpenXR session owns the display, the Activity's own surface is never
/// visible, yet GetCurrentTexture() on it still blocked the render worker
/// for 6-15ms every frame (measured on a Quest 3) waiting on its buffer
/// queue -- and every caller of synchronize() waited behind that. Set true
/// for the duration of a VR session on a platform with no visible window;
/// set false again when the session ends so the window resumes normally.
/// Plain flag, read by the render worker at end_frame -- takes effect from
/// the next frame boundary.
void set_surface_present_suppressed(bool suppressed) noexcept;
bool surface_present_suppressed() noexcept;

/// Opens an offscreen render pass (GXCreateFrameBuffer semantics): cleared
/// single-sample color+depth at (width, height) with full-target
/// viewport/scissor. Subsequent draws target it until resolve_pass restores the
/// EFB. Nesting is unsupported: returns false (with a warning) outside an
/// active render pass or while any offscreen pass is already open.
bool create_pass(uint32_t width, uint32_t height);

/// A caller-owned color target for create_pass_external(): `view` must be a
/// view of `texture` in `format`, and `format` must be color_format() so the
/// same pipelines apply. The texture needs RenderAttachment (drawn into),
/// CopySrc + TextureBinding (in-pass GXCopyTex captures read the pass's
/// color) usages.
struct ExternalPassTarget {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
};

/// create_pass() with the caller's own color target instead of a pooled
/// offscreen texture (depth is still pooled). The pass counts as consumed
/// (its output IS the external texture), so resolve it with `.color =
/// false` -- no snapshot copy is made, the caller already holds the result.
/// VR uses this to render straight into the XR swapchain's shared image,
/// skipping the snapshot copy and the whole hand-off chain.
bool create_pass_external(const ExternalPassTarget& target);

/// True while an offscreen pass (create_pass or GXCreateFrameBuffer) is open.
bool is_offscreen() noexcept;
/// True while an offscreen pass is open NESTED inside the protected
/// offscreen pass (see set_protected_offscreen_pass()) -- i.e. the protected
/// pass is suspended and a different, mono target is current. Used by
/// aurora::gx::stereo_active() so single-pass stereo instancing never
/// applies to such a nested capture.
bool is_nested_in_protected_offscreen() noexcept;
/// Flags the current render pass for single-pass stereo replay (see
/// gx::StereoState). Called by the GX draw path for every draw recorded
/// while gx::stereo_active().
void mark_current_pass_stereo() noexcept;

/// Controls what aurora::gx::logical_fb_size() reports while an offscreen
/// pass is open, which sets the scale factor GXSetViewport/GXSetScissor
/// calls get mapped through (see gx.cpp's map_logical_viewport/
/// map_logical_scissor). Off by default: offscreen passes report their own
/// target size as "logical", so a viewport call already sized to match the
/// pass's own target (e.g. a bloom/shadow/DOF downsample pass) maps 1:1, as
/// existing offscreen effects expect.
///
/// Some callers (VR eye rendering) instead replay the SAME full-scene draw
/// path used for normal flatscreen rendering -- which sets its viewport
/// using the configured native resolution (vi::configured_fb_size(), e.g.
/// FB_WIDTH/FB_HEIGHT), with no awareness that it's currently targeting a
/// differently-sized offscreen texture. Without this enabled, that call
/// lands as literal pixel coordinates on the offscreen target: correct
/// content confined to a native-resolution-sized corner of the larger
/// target, the rest left at the pass's clear color. Enabling this makes
/// logical_fb_size() report the native configured size instead (matching
/// the non-offscreen case), so the native-resolution viewport call is
/// correctly scaled up to fill the actual (e.g. VR eye) target.
///
/// Scoped to the caller's own offscreen pass: set true before opening it,
/// false before/after closing it. Leaving it enabled outside of that
/// wrongly rescales unrelated offscreen effects that run afterward.
void set_offscreen_uses_native_logical_size(bool enabled) noexcept;
bool offscreen_uses_native_logical_size() noexcept;

/// Marks the CURRENTLY-OPEN offscreen pass (identified by passId, e.g. from
/// current_pass_id()) as protected: resolve_pass_into() (the internal path
/// behind ordinary in-game GXCopyTex draining -- shadows, HUD, menu/dialogue
/// backdrops, minimaps, screenshots, more) will refuse to substitute over it
/// no matter which system requests the copy, instead silently dropping that
/// specific copy (its destination texture keeps whatever content it already
/// had). Root-caused for VR: create_pass()'s offscreen eye pass has no other
/// way to survive an ordinary mid-scene GXCopyTex, since that substitution
/// happens unconditionally and independently of create_pass/resolve_pass's
/// own g_inOffscreen nesting guard. Callers whose captures land during a
/// protected pass may render with stale/no data for that effect -- same
/// tradeoff already accepted per-call-site elsewhere, just enforced
/// centrally so no call site (found or not) can corrupt the protected pass.
///
/// Scoped like set_offscreen_uses_native_logical_size(): set right after
/// opening the pass to protect, clear right after resolving/closing it.
void set_protected_offscreen_pass(uint64_t passId) noexcept;
void clear_protected_offscreen_pass() noexcept;

/// Diagnostic accessor for the current render target's pixel size (the
/// active offscreen pass's target, or the onscreen target if none is open).
/// Plain-pointer signature so callers don't need Vec2/math.hpp.
void get_current_render_target_size(uint32_t* width, uint32_t* height) noexcept;

/// Increments on aurora_end_frame.
uint32_t current_frame() noexcept;

/// Blocks until the render worker has drained its queue. After this returns,
/// no draw callback is executing or queued to execute; used before unloading
/// code that registered draw types. Callable from the game thread only.
void synchronize();

/// Render-worker timing of the most recently completed frame (ms): time
/// spent encoding render passes / encoder tasks, in CommandEncoder::Finish,
/// in Queue::Submit, and the worker's wall time from its begin-frame task to
/// the end of submit. Diagnostic; written by the worker, read from anywhere.
struct WorkerFrameStats {
  float encodeMs = 0.f;
  float finishMs = 0.f;
  float submitMs = 0.f;
  float wallMs = 0.f;
  uint32_t drawCalls = 0;
  uint32_t mergedDrawCalls = 0;
  uint32_t renderPasses = 0; // non-discarded render passes encoded this frame
  uint32_t maxPassDraws = 0; // draw commands in the biggest pass (the scene pass)
};
WorkerFrameStats worker_frame_stats() noexcept;

} // namespace aurora::gfx
