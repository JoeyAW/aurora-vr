#include "encoding.hpp"

#include "frame.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "pipeline_cache.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "../gx/gx.hpp"
#include "../gx/pipeline.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

#include <atomic>
#include <chrono>

namespace aurora::gfx {
using namespace detail;
using webgpu::g_device;
using webgpu::g_queue;

namespace {
constexpr Module Log{"aurora::gfx"};
PipelineRef g_currentPipeline;
uint32_t g_stereoEye = 0;

// Stereo replay: map a viewport/scissor recorded against the full
// double-wide target into one eye's half.
Viewport stereo_viewport(const Viewport& vp, uint32_t eye, float halfWidth) {
  Viewport out = vp;
  out.left = vp.left * 0.5f + static_cast<float>(eye) * halfWidth;
  out.width = vp.width * 0.5f;
  return out;
}

ClipRect stereo_scissor(const ClipRect& sc, uint32_t eye, int32_t halfWidth) {
  ClipRect out = sc;
  out.x = sc.x / 2 + static_cast<int32_t>(eye) * halfWidth;
  out.width = (sc.x + sc.width) / 2 - sc.x / 2;
  return out;
}

void apply_viewport(const wgpu::RenderPassEncoder& pass, const Viewport& vp) {
  const float minDepth = gx::UseReversedZ ? 1.f - vp.zfar : vp.znear;
  const float maxDepth = gx::UseReversedZ ? 1.f - vp.znear : vp.zfar;
  pass.SetViewport(vp.left, vp.top, vp.width, vp.height, minDepth, maxDepth);
}

void apply_scissor(const wgpu::RenderPassEncoder& pass, const ClipRect& sc, const wgpu::Extent3D& size) {
  const auto x = std::clamp(static_cast<uint32_t>(sc.x), 0u, size.width);
  const auto y = std::clamp(static_cast<uint32_t>(sc.y), 0u, size.height);
  const auto w = std::clamp(static_cast<uint32_t>(sc.width), 0u, size.width - x);
  const auto h = std::clamp(static_cast<uint32_t>(sc.height), 0u, size.height - y);
  pass.SetScissorRect(x, y, w, h);
}

DrawContext make_draw_context(const RenderPass& passInfo) {
  auto& res = resources();
  return {
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = res.vertexBuffer,
      .indexBuffer = res.indexBuffer,
      .uniformBuffer = res.uniformBuffer,
      .storageBuffer = res.storageBuffer,
      .layout = passInfo.target_layout(),
  };
}

void render_custom_draw(const CustomDrawCommand& draw, const wgpu::RenderPassEncoder& pass,
                        const RenderPass& passInfo) {
  const auto drawType = find_runtime_draw_type(draw.type);
  if (!drawType) {
    // Unregistered between record and replay; the command is a no-op.
    return;
  }

  const auto context = make_draw_context(passInfo);
  drawType->draw(context, pass, draw.payload.data(), draw.payloadSize, drawType->userdata);
}

void execute_encoder_task(wgpu::CommandEncoder& cmd, FramePacket& frame, const EncoderTask& task) {
  const auto taskType = find_runtime_encoder_task_type(task.type);
  if (!taskType) {
    // Unregistered between record and encode; the task is a no-op.
    return;
  }

  auto& res = resources();
  const EncoderTaskContext context{
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = res.vertexBuffer,
      .indexBuffer = res.indexBuffer,
      .uniformBuffer = res.uniformBuffer,
      .storageBuffer = res.storageBuffer,
  };
  taskType->callback(context, cmd, task.payload.data(), task.payloadSize, taskType->userdata);
  if (taskType->afterSubmit != nullptr) {
    const auto payload = task.payload;
    const auto payloadSize = task.payloadSize;
    const auto callback = taskType->afterSubmit;
    const auto userdata = taskType->userdata;
    frame.afterSubmitCallbacks.emplace_back([payload, payloadSize, callback, userdata] {
      const EncoderTaskCompletionContext completionContext{
          .device = g_device,
          .queue = g_queue,
      };
      callback(completionContext, payload.data(), payloadSize, userdata);
    });
  }
}

void render_pass_commands(const wgpu::RenderPassEncoder& pass, FramePacket& frame, RenderPass& passInfo,
                          uint32_t stereoEye) {
  g_currentPipeline = UINTPTR_MAX;
  g_stereoEye = stereoEye;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif
  Viewport currentViewport{};
  ClipRect currentScissor{};
  bool hasViewport = false;
  bool hasScissor = false;
  const auto& targetSize = passInfo.colorAttachments[SceneColorAttachmentIndex].size;
  const bool stereo = passInfo.stereoReplay;
  const float halfWidthF = static_cast<float>(targetSize.width) * 0.5f;
  const int32_t halfWidthI = static_cast<int32_t>(targetSize.width / 2);

  // Bind bind group for the whole pass
  pass.SetBindGroup(0, resources().staticBindGroup);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);

  for (auto& cmd : passInfo.commands) {
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        pass.PopDebugGroup();
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        pass.PushDebugGroup(cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto vp = stereo ? stereo_viewport(cmd.data.setViewport, stereoEye, halfWidthF) : cmd.data.setViewport;
      apply_viewport(pass, vp);
      currentViewport = vp;
      hasViewport = true;
    } break;
    case CommandType::SetScissor: {
      const auto sc = stereo ? stereo_scissor(cmd.data.setScissor, stereoEye, halfWidthI) : cmd.data.setScissor;
      apply_scissor(pass, sc, targetSize);
      currentScissor = sc;
      hasScissor = true;
    } break;
    case CommandType::Draw: {
      auto& draw = cmd.data.draw;
      if (draw.encoder != nullptr) {
        draw.encoder(draw.payload.data(), pass, passInfo);
      }
    } break;
    case CommandType::CustomDraw: {
      render_custom_draw(cmd.data.customDraw, pass, passInfo);
      g_currentPipeline = UINTPTR_MAX;
      pass.SetBindGroup(0, resources().staticBindGroup);
      pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
      if (hasViewport) {
        apply_viewport(pass, currentViewport);
      }
      if (hasScissor) {
        apply_scissor(pass, currentScissor, passInfo.colorAttachments[SceneColorAttachmentIndex].size);
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      pass.InsertDebugMarker(wgpu::StringView(frame.debugMarkers[cmd.data.debugMarkerIndex]));
#endif
    } break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    pass.PopDebugGroup();
  }
#endif
  g_stereoEye = 0;
}

void render_pass(const wgpu::RenderPassEncoder& pass, FramePacket& frame, RenderPass& passInfo) {
  ZoneScoped;
  if (passInfo.stereoReplay) {
    // Single-pass stereo: the same command list once per half. Left then
    // right; the halves never overlap so ordering between them is free.
    render_pass_commands(pass, frame, passInfo, 0);
    render_pass_commands(pass, frame, passInfo, 1);
    return;
  }
  render_pass_commands(pass, frame, passInfo, 0);
}

void render(wgpu::CommandEncoder& cmd, FramePacket& frame, RenderPass& passInfo, uint32_t passIndex) {
  ZoneScoped;
  if (!passInfo.sealed) {
    return;
  }

  for (const auto& conv : passInfo.paletteConvs) {
    tex_palette_conv::run(cmd, conv);
  }
  if (passInfo.discardable) {
    // This pass has no effect and can be safely discarded (e.g. an empty EFB segment between two back-to-back pass
    // breaks, or an unresolved offscreen pass).
    return;
  }
  {
    uint32_t drawCommands = 0;
    for (const auto& c : passInfo.commands) {
      drawCommands += (c.type == CommandType::Draw || c.type == CommandType::CustomDraw) ? 1u : 0u;
    }
    worker_stats_add_pass(drawCommands);
    // TEMP DIAGNOSTIC (single-pass stereo perf, 2026-09-20): dump the pass
    // list of one frame every ~10s so the per-frame render-pass structure
    // on the tiled Quest GPU is visible in logcat.
    static uint32_t s_passDumpCounter = 0;
    if ((s_passDumpCounter++ / 64) % 12 == 0 && (s_passDumpCounter % 64) < 12) {
      const auto& c0 = passInfo.colorAttachments[SceneColorAttachmentIndex];
      Log.info("[passdump] frame#{} pass{} '{}' {}x{} draws={} colorLoad={} depthLoad={} stereo={} resolve={}",
               s_passDumpCounter / 64, passIndex, passInfo.label, c0.size.width, c0.size.height, drawCommands,
               static_cast<int>(c0.loadOp), static_cast<int>(passInfo.depthLoadOp), passInfo.stereoReplay,
               passInfo.resolveTarget ? 1 : 0);
    }
  }

  std::array<wgpu::RenderPassColorAttachment, MaxColorAttachments> attachments{};
  for (uint32_t i = 0; i < passInfo.colorAttachmentCount; ++i) {
    const auto& source = passInfo.colorAttachments[i];
    attachments[i] = {
        .view = source.view,
        .resolveTarget = source.resolveView,
        .loadOp = source.loadOp != wgpu::LoadOp::Undefined ? source.loadOp
                                                           : (source.clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load),
        .storeOp = source.storeOp,
        .clearValue =
            {
                .r = source.clearValue.x(),
                .g = source.clearValue.y(),
                .b = source.clearValue.z(),
                .a = source.clearValue.w(),
            },
    };
  }
  wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{};
  const wgpu::RenderPassDepthStencilAttachment* depthStencilAttachmentPtr = nullptr;
  if (passInfo.depthStencilView) {
    depthStencilAttachment = {
        .view = passInfo.depthStencilView,
        .depthLoadOp = passInfo.hasDepth ? (passInfo.depthLoadOp != wgpu::LoadOp::Undefined
                                                ? passInfo.depthLoadOp
                                                : (passInfo.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load))
                                         : wgpu::LoadOp::Undefined,
        .depthStoreOp = passInfo.hasDepth ? passInfo.depthStoreOp : wgpu::StoreOp::Undefined,
        .depthClearValue = passInfo.clearDepthValue,
        .stencilLoadOp = passInfo.hasStencil ? passInfo.stencilLoadOp : wgpu::LoadOp::Undefined,
        .stencilStoreOp = passInfo.hasStencil ? passInfo.stencilStoreOp : wgpu::StoreOp::Undefined,
        .stencilClearValue = passInfo.stencilClearValue,
    };
    depthStencilAttachmentPtr = &depthStencilAttachment;
  }
  const auto& sceneSize = passInfo.colorAttachments[SceneColorAttachmentIndex].size;
  const auto label = passInfo.label.empty()
                         ? fmt::format("Render pass {}", passIndex)
                         : fmt::format("{} {}x{}", passInfo.label, sceneSize.width, sceneSize.height);
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = label.c_str(),
      .colorAttachmentCount = passInfo.colorAttachmentCount,
      .colorAttachments = attachments.data(),
      .depthStencilAttachment = depthStencilAttachmentPtr,
      .timestampWrites = webgpu::gpu_prof::pass_writes(label),
  };

  auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
  render_pass(pass, frame, passInfo);
  pass.End();

  if (passInfo.captureDepthSnapshot) {
    depth_peek::encode_frame_snapshot(cmd, passInfo.copySourceDepthView,
                                      passInfo.colorAttachments[SceneColorAttachmentIndex].size, passInfo.msaaSamples);
  }

  if (passInfo.resolveTarget) {
    const auto& dstSize = passInfo.resolveTarget->size;
    const bool needsConversion = tex_copy_conv::needs_conversion(passInfo.resolveFormat);
    const bool needsScaling = dstSize.width != static_cast<uint32_t>(passInfo.resolveRect.width) ||
                              dstSize.height != static_cast<uint32_t>(passInfo.resolveRect.height);
    const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
    if (isDepth && passInfo.msaaSamples > 1) {
      Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
    }
    const tex_copy_conv::ConvRequest convReq{
        .fmt = passInfo.resolveFormat,
        .srcView = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView,
        .uniformRange = passInfo.resolveUniformRange,
        .dst = passInfo.resolveTarget,
        .sampleFilter = needsScaling ? tex_copy_conv::SampleFilter::Linear : tex_copy_conv::SampleFilter::Nearest,
    };
    if (needsConversion) {
      tex_copy_conv::run(cmd, convReq);
    } else if (needsScaling) {
      tex_copy_conv::blit(cmd, convReq);
    } else {
      const webgpu::gpu_prof::Zone zone{cmd, "EFB copy"};
      const wgpu::TexelCopyTextureInfo src{
          .texture = passInfo.copySourceTexture,
          .origin =
              wgpu::Origin3D{
                  .x = static_cast<uint32_t>(passInfo.resolveRect.x),
                  .y = static_cast<uint32_t>(passInfo.resolveRect.y),
              },
      };
      const wgpu::TexelCopyTextureInfo dst{
          .texture = passInfo.resolveTarget->texture,
      };
      const wgpu::Extent3D size{
          .width = static_cast<uint32_t>(passInfo.resolveRect.width),
          .height = static_cast<uint32_t>(passInfo.resolveRect.height),
          .depthOrArrayLayers = 1,
      };
      cmd.CopyTextureToTexture(&src, &dst, &size);
    }
  }

  if (passInfo.snapshotColorDst) {
    const webgpu::gpu_prof::Zone zone{cmd, "Pass snapshot"};
    const wgpu::TexelCopyTextureInfo src{
        .texture = passInfo.copySourceTexture,
    };
    const wgpu::TexelCopyTextureInfo dst{
        .texture = passInfo.snapshotColorDst,
    };
    const wgpu::Extent3D size{
        .width = passInfo.colorAttachments[SceneColorAttachmentIndex].size.width,
        .height = passInfo.colorAttachments[SceneColorAttachmentIndex].size.height,
        .depthOrArrayLayers = 1,
    };
    cmd.CopyTextureToTexture(&src, &dst, &size);
  }
  if (passInfo.snapshotDepthDst) {
    tex_copy_conv::snapshot_depth(cmd, passInfo.copySourceDepthView, passInfo.msaaSamples, passInfo.snapshotDepthDst);
  }
  if (passInfo.snapshotNormalDst) {
    const webgpu::gpu_prof::Zone zone{cmd, "Normal snapshot"};
    const wgpu::TexelCopyTextureInfo src{
        .texture = passInfo.copySourceNormalTexture,
    };
    const wgpu::TexelCopyTextureInfo dst{
        .texture = passInfo.snapshotNormalDst,
    };
    const wgpu::Extent3D size{
        .width = passInfo.colorAttachments[SceneColorAttachmentIndex].size.width,
        .height = passInfo.colorAttachments[SceneColorAttachmentIndex].size.height,
        .depthOrArrayLayers = 1,
    };
    cmd.CopyTextureToTexture(&src, &dst, &size);
  }
}

constexpr uint64_t VertexStagingOffset = 0;
constexpr uint64_t UniformStagingOffset = VertexStagingOffset + VertexBufferSize;
constexpr uint64_t IndexStagingOffset = UniformStagingOffset + UniformBufferSize;
constexpr uint64_t StorageStagingOffset = IndexStagingOffset + IndexBufferSize;
constexpr uint64_t TextureUploadStagingOffset = StorageStagingOffset + StorageBufferSize;

constexpr uint32_t align_down_copy_offset(uint32_t value) noexcept { return value & ~3u; }

void copy_staging_buffer_range(wgpu::CommandEncoder& cmd, const FramePacket& frame, uint32_t& copied,
                               uint32_t highWater, uint64_t stagingOffset, const wgpu::Buffer& dst) {
  if (highWater <= copied) {
    return;
  }
  const uint32_t copyStart = align_down_copy_offset(copied);
  const uint32_t copyEnd = AURORA_ALIGN(highWater, 4);
  cmd.CopyBufferToBuffer(staging_buffer(frame.stagingBuffer), stagingOffset + copyStart, dst, copyStart,
                         copyEnd - copyStart);
  copied = highWater;
}

bool needs_staging_copy(const FramePacket& frame, const FrameOp& op) {
  const auto& highWater = op.highWater;
  if (highWater.verts > frame.copied.verts || highWater.uniforms > frame.copied.uniforms ||
      highWater.indices > frame.copied.indices || highWater.storage > frame.copied.storage) {
    return true;
  }
  if constexpr (UseTextureBuffer) {
    return op.textureUploads.size() > frame.copied.textureUploadCount;
  }
  return false;
}

void copy_staging_to_high_water(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  if (!needs_staging_copy(frame, op)) {
    return;
  }
  const webgpu::gpu_prof::Zone zone{cmd, "Staging copies"};
  const auto& highWater = op.highWater;
  auto& res = resources();
  copy_staging_buffer_range(cmd, frame, frame.copied.verts, highWater.verts, VertexStagingOffset, res.vertexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.uniforms, highWater.uniforms, UniformStagingOffset,
                            res.uniformBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.indices, highWater.indices, IndexStagingOffset, res.indexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.storage, highWater.storage, StorageStagingOffset,
                            res.storageBuffer);

  if constexpr (UseTextureBuffer) {
    for (size_t i = frame.copied.textureUploadCount; i < op.textureUploads.size(); ++i) {
      const auto& item = *op.textureUploads[i];
      const wgpu::TexelCopyBufferInfo buf{
          .layout =
              wgpu::TexelCopyBufferLayout{
                  .offset = item.buffer ? item.layout.offset : item.layout.offset + TextureUploadStagingOffset,
                  .bytesPerRow = AURORA_ALIGN(item.layout.bytesPerRow, 256),
                  .rowsPerImage = item.layout.rowsPerImage,
              },
          .buffer = item.buffer ? item.buffer : staging_buffer(frame.stagingBuffer),
      };
      cmd.CopyBufferToTexture(&buf, &item.tex, &item.size);
    }
    frame.copied.textureUpload = highWater.textureUpload;
    frame.copied.textureUploadCount = op.textureUploads.size();
  }
}
} // namespace

namespace detail {
void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  const auto t0 = std::chrono::steady_clock::now();
  struct Timer {
    std::chrono::steady_clock::time_point t0;
    ~Timer() {
      worker_stats_add_encode(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
  } timer{t0};
  copy_staging_to_high_water(cmd, frame, op);
  switch (op.type) {
  case FrameOpType::RenderPass:
    if (op.renderPass != nullptr) {
      render(cmd, frame, *op.renderPass, op.index);
    }
    break;
  case FrameOpType::TextureCopy:
    if (op.textureCopy != nullptr) {
      const webgpu::gpu_prof::Zone zone{cmd, "Texture copy"};
      cmd.CopyTextureToTexture(&op.textureCopy->src, &op.textureCopy->dst, &op.textureCopy->size);
    }
    break;
  case FrameOpType::EncoderTask:
    if (op.encoderTask != nullptr) {
      execute_encoder_task(cmd, frame, *op.encoderTask);
    }
    break;
  }
}
} // namespace detail

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass) {
  if (ref == g_currentPipeline) {
    return true;
  }
  wgpu::RenderPipeline pipeline;
  if (!get_pipeline(ref, pipeline)) {
    return false;
  }
  pass.SetPipeline(pipeline);
  g_currentPipeline = ref;
  return true;
}
namespace detail {
uint32_t current_stereo_eye() noexcept { return g_stereoEye; }

namespace {
using StatsClock = std::chrono::steady_clock;
double g_wsEncodeMs = 0.0;
uint32_t g_wsPasses = 0;
uint32_t g_wsMaxPassDraws = 0;
StatsClock::time_point g_wsFrameStart;
std::atomic<WorkerFrameStats> g_wsLast{};
} // namespace

void worker_stats_begin_frame() noexcept {
  g_wsEncodeMs = 0.0;
  g_wsPasses = 0;
  g_wsMaxPassDraws = 0;
  g_wsFrameStart = StatsClock::now();
}
void worker_stats_add_encode(double ms) noexcept { g_wsEncodeMs += ms; }
void worker_stats_add_pass(uint32_t drawCommands) noexcept {
  ++g_wsPasses;
  g_wsMaxPassDraws = std::max(g_wsMaxPassDraws, drawCommands);
}
void worker_stats_finish_submit(double finishMs, double submitMs, uint32_t drawCalls, uint32_t mergedDrawCalls) noexcept {
  const double wall = std::chrono::duration<double, std::milli>(StatsClock::now() - g_wsFrameStart).count();
  g_wsLast.store(WorkerFrameStats{static_cast<float>(g_wsEncodeMs), static_cast<float>(finishMs),
                                  static_cast<float>(submitMs), static_cast<float>(wall), drawCalls, mergedDrawCalls,
                                  g_wsPasses, g_wsMaxPassDraws},
                 std::memory_order_relaxed);
}
} // namespace detail

WorkerFrameStats worker_frame_stats() noexcept { return detail::g_wsLast.load(std::memory_order_relaxed); }
} // namespace aurora::gfx
