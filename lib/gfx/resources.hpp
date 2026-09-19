#pragma once

#include "types.hpp"

namespace aurora::gfx {
inline constexpr bool UseTextureBuffer = true;
inline constexpr uint64_t UniformBufferSize = 25165824; // 24 MiB
// DOUBLED (dusklight VR crash: opening a door in Lakebed Temple hit
// abort() inside a fixed-capacity staging buffer append). dusklight's VR
// mod wraps a single aurora_begin_frame()/aurora_end_frame() pair around
// BOTH eyes' full scene draws, so VR needs roughly double a flatscreen
// frame's vertex data within the same one-frame budget; a geometry-heavy
// room streaming in fresh vertex data was enough to push combined stereo
// submission past the old 5 MiB ceiling. IndexBufferSize didn't need a
// matching VR-motivated bump -- already large enough here.
inline constexpr uint64_t VertexBufferSize = 10485760;  // 10 MiB (was 5 MiB)
inline constexpr uint64_t IndexBufferSize = 2097152;    // 2 MiB
inline constexpr uint64_t StorageBufferSize = 8388608;  // 8 MiB
inline constexpr uint64_t TextureUploadSize = 25165824; // 24 MiB

namespace detail {
struct Resources {
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer storageBuffer;
  wgpu::BindGroupLayout staticBindGroupLayout;
  wgpu::BindGroup staticBindGroup;
  wgpu::BindGroupLayout uniformBindGroupLayout;
  wgpu::BindGroup uniformBindGroup;
  wgpu::Limits limits;
  AuroraStats stats{};
};

Resources& resources() noexcept;
} // namespace detail
} // namespace aurora::gfx
