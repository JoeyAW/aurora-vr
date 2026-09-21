#pragma once

#include "pipeline_cache.hpp"
#include "types.hpp"
#include "tex_palette_conv.hpp"
#include "texture.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace aurora::gfx::detail {

struct StagingHighWater {
  uint32_t verts = 0;
  uint32_t uniforms = 0;
  uint32_t indices = 0;
  uint32_t storage = 0;
  uint32_t textureUpload = 0;
  size_t textureUploadCount = 0;
};

struct CustomDrawCommand {
  DrawTypeId type = 0;
  uint32_t payloadSize = 0;
  alignas(std::max_align_t) std::array<std::byte, InlineDrawPayloadSize> payload{};
};

struct RenderPass;
using DrawEncoder = void (*)(void* payload, const wgpu::RenderPassEncoder& pass, const RenderPass& passInfo);

struct DrawCommand {
  DrawEncoder encoder = nullptr;
  alignas(std::max_align_t) std::array<std::byte, InlineDrawPayloadSize> payload{};
};

enum class CommandType {
  SetViewport,
  SetScissor,
  Draw,
  CustomDraw,
  DebugMarker,
};

struct Command {
  CommandType type;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif
  union Data {
    Viewport setViewport;
    ClipRect setScissor;
    DrawCommand draw;
    CustomDrawCommand customDraw;
    size_t debugMarkerIndex;
  } data;
};

using CommandList = std::vector<Command>;

struct RenderPass {
  // Unique per-instance id, self-assigned via a monotonic counter so EVERY
  // RenderPass anywhere gets one automatically. Exists so a caller that
  // opened an offscreen pass (create_pass) can later verify the "current"
  // pass is still the SAME pass object it opened, not a different one that
  // silently replaced it. resolve_pass_into() seals whatever pass is
  // current and substitutes a new pass object in its place, entirely
  // independent of create_pass()/resolve_pass()'s own inOffscreen nesting
  // guard (which resolve_pass_into doesn't check at all) -- if that
  // substitution happens while e.g. VR's offscreen pass is open, the
  // caller's own pass would otherwise be silently orphaned. See
  // current_pass_id()/resolve_pass_checked() in recording.cpp, which use
  // this field to detect exactly that and refuse instead of resolving the
  // wrong pass.
  uint64_t id = [] {
    static std::atomic<uint64_t> nextId{1};
    return nextId.fetch_add(1, std::memory_order_relaxed);
  }();

  struct ColorAttachment {
    ColorAttachmentSemantic semantic = ColorAttachmentSemantic::Auxiliary;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    wgpu::Extent3D size;
    wgpu::TextureView view;
    wgpu::TextureView resolveView;
    Vec4<float> clearValue{0.f, 0.f, 0.f, 0.f};
    wgpu::LoadOp loadOp = wgpu::LoadOp::Undefined;
    wgpu::StoreOp storeOp = wgpu::StoreOp::Store;
    bool clear = true;
  };

  std::string label;
  std::array<ColorAttachment, MaxColorAttachments> colorAttachments;
  uint32_t colorAttachmentCount = 0;
  wgpu::TextureView depthStencilView;
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  wgpu::Texture copySourceTexture;
  wgpu::TextureView copySourceView;
  wgpu::TextureView copySourceDepthView;
  wgpu::Texture copySourceNormalTexture;
  uint32_t msaaSamples = 1;

  TextureHandle resolveTarget;
  GXTexFmt resolveFormat = GX_TF_RGBA8;
  ClipRect resolveRect;
  Range resolveUniformRange;
  wgpu::Texture snapshotColorDst;
  wgpu::TextureView snapshotDepthDst;
  wgpu::Texture snapshotNormalDst;
  float clearDepthValue = 1.f;
  wgpu::LoadOp depthLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp depthStoreOp = wgpu::StoreOp::Store;
  wgpu::LoadOp stencilLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp stencilStoreOp = wgpu::StoreOp::Undefined;
  uint32_t stencilClearValue = 0;
  CommandList commands;
  bool clearDepth = true;
  bool hasDepth = true;
  bool hasStencil = false;
  bool hasDraws = false;
  bool discardable = false;
  bool captureDepthSnapshot = false;
  bool sealed = false;
  // Single-pass stereo (see gx::StereoState): encode this pass's commands
  // twice, viewport/scissor mapped into each half of the (double-wide)
  // target, DrawImmediateData::stereoEye = 0 then 1.
  bool stereoReplay = false;
  // The color target belongs to the caller (gfx::create_pass_external):
  // the pass's output is consumed by whoever owns that texture, so it is
  // never discardable for lack of a snapshot/resolve consumer.
  bool externalTarget = false;
  std::vector<tex_palette_conv::ConvRequest> paletteConvs;

  RenderTargetLayout target_layout() const noexcept;
  bool has_consumer() const {
    return externalTarget || resolveTarget || snapshotColorDst || snapshotDepthDst || snapshotNormalDst;
  }
  bool has_content() const {
    if (hasDraws || clearDepth) {
      return true;
    }
    for (uint32_t i = 0; i < colorAttachmentCount; ++i) {
      if (colorAttachments[i].clear) {
        return true;
      }
    }
    return false;
  }
};

inline void finalize_render_target_layout(RenderTargetLayout& layout) noexcept {
  Hasher hasher{};
  hasher.update(layout.colorAttachmentCount);
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    hasher.update(layout.colorAttachments[i].semantic);
    hasher.update(layout.colorAttachments[i].format);
  }
  hasher.update(static_cast<uint32_t>(layout.depthStencilFormat));
  hasher.update(layout.sampleCount);
  layout.key = hasher.digest();
}

inline RenderTargetLayout RenderPass::target_layout() const noexcept {
  RenderTargetLayout layout{
      .colorAttachmentCount = colorAttachmentCount,
      .depthStencilFormat = hasDepth || hasStencil ? depthStencilFormat : wgpu::TextureFormat::Undefined,
      .sampleCount = msaaSamples,
  };
  for (uint32_t i = 0; i < colorAttachmentCount; ++i) {
    layout.colorAttachments[i] = {
        .semantic = colorAttachments[i].semantic,
        .format = colorAttachments[i].format,
        .width = colorAttachments[i].size.width,
        .height = colorAttachments[i].size.height,
    };
  }
  finalize_render_target_layout(layout);
  return layout;
}

struct TextureCopy {
  wgpu::TexelCopyTextureInfo src;
  wgpu::TexelCopyTextureInfo dst;
  wgpu::Extent3D size;
};

struct EncoderTask {
  EncoderTaskId type = InvalidEncoderTask;
  std::array<uint8_t, InlineDrawPayloadSize> payload{};
  uint32_t payloadSize = 0;
};

enum class FrameOpType : uint8_t {
  RenderPass,
  TextureCopy,
  EncoderTask,
};

struct FrameOp {
  FrameOpType type = FrameOpType::RenderPass;
  uint32_t index = 0;
  RenderPass* renderPass = nullptr;
  TextureCopy* textureCopy = nullptr;
  EncoderTask* encoderTask = nullptr;
  StagingHighWater highWater;
  std::vector<const TextureUpload*> textureUploads;
};

using RenderPassList = std::deque<RenderPass>;

struct FramePacket {
  RenderPassList renderPasses;
  std::deque<TextureCopy> textureCopies;
  std::deque<EncoderTask> encoderTasks;
  std::deque<FrameOp> ops;
  std::deque<TextureUpload> textureUploads;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugMarkers;
#endif
  ByteBuffer verts;
  ByteBuffer uniforms;
  ByteBuffer indices;
  ByteBuffer storage;
  ByteBuffer textureUpload;
  wgpu::CommandEncoder encoder;
  std::vector<AfterSubmitCallback> afterSubmitCallbacks;
  uint64_t frameId = 0;
  uint32_t frameIndex = 0;
  size_t stagingBuffer = 0;
  StagingHighWater copied;
  AuroraStats stats{};
};

} // namespace aurora::gfx::detail
