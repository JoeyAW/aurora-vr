#pragma once

#include <aurora/aurora.h>
#include <aurora/math.hpp>

#include "wgpu.hpp"

#include <array>
#include <cstdint>

struct SDL_Window;

namespace aurora::webgpu {
inline constexpr wgpu::TextureFormat NormalBufferFormat = wgpu::TextureFormat::RGB10A2Unorm;

struct GraphicsConfig {
  wgpu::SurfaceConfiguration surfaceConfiguration;
  wgpu::TextureFormat depthFormat;
  uint32_t msaaSamples;
  uint16_t textureAnisotropy;
  bool normalBuffer = false;
};
struct TextureWithSampler {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Extent3D size;
  wgpu::TextureFormat format;
  wgpu::Sampler sampler;
};
struct Viewport {
  float left;
  float top;
  float width;
  float height;
  float znear;
  float zfar;

  bool operator==(const Viewport& rhs) const {
    return left == rhs.left && top == rhs.top && width == rhs.width && height == rhs.height && znear == rhs.znear &&
           zfar == rhs.zfar;
  }
  bool operator!=(const Viewport& rhs) const { return !(*this == rhs); }
};

extern wgpu::Device g_device;
extern wgpu::Queue g_queue;
extern wgpu::Surface g_surface;
extern wgpu::BackendType g_backendType;
extern GraphicsConfig g_graphicsConfig;
extern TextureWithSampler g_frameBuffer;
extern TextureWithSampler g_frameBufferResolved;
extern TextureWithSampler g_depthBuffer;
extern TextureWithSampler g_normalBuffer;
extern wgpu::RenderPipeline g_CopyPipeline;
extern wgpu::RenderPipeline g_CopyPremultipliedAlphaPipeline;
extern wgpu::BindGroup g_CopyBindGroup;
extern wgpu::Instance g_instance;
extern wgpu::AdapterInfo g_adapterInfo;
extern bool g_hasCoreFeatures;
extern bool g_bcTexturesSupported;
extern bool g_astcTexturesSupported;
extern bool g_textureComponentSwizzleSupported;
extern bool g_sharedFenceDxgiSupported;
extern bool g_sharedTextureMemoryD3D12Supported;
// Android/Vulkan equivalent of g_sharedTextureMemoryD3D12Supported above --
// gates dusk::vr::Session's shared-image GPU-direct swapchain-copy path
// (vr_xr_submit.hpp): true when the adapter supports importing an opaque-fd
// VkImage (SharedTextureMemoryOpaqueFD) AND exporting a Vulkan shared fence
// (SharedFenceSyncFD or SharedFenceVkSemaphoreOpaqueFD, needed by EndAccess).
// Set in initialize(); see gpu.cpp's definition comment for history.
extern bool g_vulkanSharedImageExportSupported;

bool initialize(AuroraBackend backend, bool allowCpu);
void shutdown();
void release_surface() noexcept;
bool refresh_surface(bool recreate = true);
void resize_swapchain(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight, bool force = false);
TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled);
bool enable_normal_buffer();
const TextureWithSampler& present_source() noexcept;
// Desktop mirror support: while set, present_source() returns this instead
// of the normal internal framebuffer, so whatever aurora's existing
// present-resample pass (resample_present_source(), runs every frame
// regardless) samples from can be swapped out for something already
// rendered elsewhere -- e.g. a VR eye -- with no extra render work beyond
// that already-scheduled resample. `source`'s view/texture/sampler must
// stay alive for as long as the override is set (ref-counted wgpu handles,
// so holding a copy here is sufficient -- no separate lifetime tracking
// needed). Plain field writes, no direct GPU/queue calls -- safe to call
// from the same thread that calls aurora_begin_frame()/aurora_end_frame(),
// unlike e.g. resample_present_source()'s g_queue.WriteBuffer which must
// run on the render worker thread specifically.
void set_present_source_override(const TextureWithSampler& source) noexcept;
void clear_present_source_override() noexcept;
wgpu::BindGroup create_copy_bind_group(const TextureWithSampler& source);
void set_resampler(AuroraSampler sampler) noexcept;
AuroraSampler get_resampler() noexcept;
Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept;
const TextureWithSampler& resample_present_source(const wgpu::CommandEncoder& encoder, const Viewport& viewport);
void draw_clear(const wgpu::RenderPassEncoder& pass, bool clearColor, bool clearAlpha, bool clearDepth,
                const Vec4<float>& clearColorValue, float clearDepthValue);

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void* userdata);
void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void* userdata);
void cache_prune();
void cache_shutdown();

} // namespace aurora::webgpu
