#pragma once

#include "frame_packet.hpp"

namespace aurora::gfx {

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass);

namespace detail {
// Which eye the render worker is currently encoding a stereo-replay pass
// for (0/1; 0 outside such a pass). Read by gx::render() to fill
// DrawImmediateData::stereoEye.
uint32_t current_stereo_eye() noexcept;
// Worker-thread timing accumulators (see gfx::worker_frame_stats()).
void worker_stats_begin_frame() noexcept;
void worker_stats_add_encode(double ms) noexcept;
void worker_stats_finish_submit(double finishMs, double submitMs, uint32_t drawCalls, uint32_t mergedDrawCalls) noexcept;
void worker_stats_add_pass(uint32_t drawCommands) noexcept;
void encode_op(wgpu::CommandEncoder& encoder, FramePacket& frame, const FrameOp& op);
}

} // namespace aurora::gfx
