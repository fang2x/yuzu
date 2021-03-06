// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"

struct ScreenInfo;

namespace VideoCore {

class RasterizerInterface {
public:
    virtual ~RasterizerInterface() {}

    /// Draw the current batch of vertex arrays
    virtual void DrawArrays() = 0;

    /// Notify rasterizer that the specified Maxwell register has been changed
    virtual void NotifyMaxwellRegisterChanged(u32 method) = 0;

    /// Notify rasterizer that all caches should be flushed to Switch memory
    virtual void FlushAll() = 0;

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    virtual void FlushRegion(Tegra::GPUVAddr addr, u64 size) = 0;

    /// Notify rasterizer that any caches of the specified region should be invalidated
    virtual void InvalidateRegion(Tegra::GPUVAddr addr, u64 size) = 0;

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    /// and invalidated
    virtual void FlushAndInvalidateRegion(Tegra::GPUVAddr addr, u64 size) = 0;

    /// Attempt to use a faster method to perform a display transfer with is_texture_copy = 0
    virtual bool AccelerateDisplayTransfer(const void* config) {
        return false;
    }

    /// Attempt to use a faster method to perform a display transfer with is_texture_copy = 1
    virtual bool AccelerateTextureCopy(const void* config) {
        return false;
    }

    /// Attempt to use a faster method to fill a region
    virtual bool AccelerateFill(const void* config) {
        return false;
    }

    /// Attempt to use a faster method to display the framebuffer to screen
    virtual bool AccelerateDisplay(const Tegra::FramebufferConfig& framebuffer,
                                   VAddr framebuffer_addr, u32 pixel_stride,
                                   ScreenInfo& screen_info) {
        return false;
    }

    virtual bool AccelerateDrawBatch(bool is_indexed) {
        return false;
    }
};
} // namespace VideoCore
