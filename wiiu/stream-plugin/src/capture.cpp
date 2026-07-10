// TV framebuffer capture via a GX2CopyColorBufferToScanBuffer hook
// (PLAN.md §4A.1, the StreamingPluginWiiU approach).
//
// On every TV scan-out we may issue a GPU blit (GX2CopySurface) of the
// game's color buffer into one of two linear "capture" surfaces the CPU
// can read. The blit issued during scan-out N is consumed at scan-out
// N+1 — one full frame period later — so the GPU is guaranteed done
// without ever stalling the render thread with GX2DrawDone().
//
// Surface lifecycle (guarded by gCaptureMutex):
//   FREE → (blit issued) COPY_PENDING → (next hook) ENCODING → FREE
//
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#include <cstring>

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/mutex.h>
#include <gx2/enum.h>
#include <gx2/mem.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <wups.h>

#include "stream_state.h"
#include "wsu_wiiu.h"

namespace wsu {
namespace {

enum SurfaceState { FREE = 0, COPY_PENDING = 1, ENCODING = 2 };

struct CaptureSlot {
    GX2Surface surface{};
    void *image = nullptr;
    SurfaceState state = FREE;
    uint32_t issuedAtMs = 0;
};

OSMutex gCaptureMutex;
CaptureSlot gSlots[2];
CapturedSurface gCaptured[2];
uint32_t gLastCaptureMs = 0;
bool gInitialized = false;
bool gFormatWarned = false;

bool formatSupported(uint32_t format) {
    return format == GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8 ||
           format == GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8;
}

void freeSlot(CaptureSlot &slot) {
    if (slot.image != nullptr) {
        MEMFreeToDefaultHeap(slot.image);
        slot.image = nullptr;
    }
    slot.state = FREE;
}

// (Re)allocates `slot` as a linear surface matching the source dims.
// Called with gCaptureMutex held.
bool prepareSlot(CaptureSlot &slot, const GX2Surface &src) {
    if (slot.image != nullptr && slot.surface.width == src.width &&
        slot.surface.height == src.height &&
        slot.surface.format == src.format) {
        return true;
    }
    freeSlot(slot);

    std::memset(&slot.surface, 0, sizeof(slot.surface));
    slot.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    slot.surface.width = src.width;
    slot.surface.height = src.height;
    slot.surface.depth = 1;
    slot.surface.mipLevels = 1;
    slot.surface.format = src.format;
    slot.surface.aa = GX2_AA_MODE1X;
    slot.surface.use = GX2_SURFACE_USE_TEXTURE;
    slot.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    slot.surface.swizzle = 0;
    GX2CalcSurfaceSizeAndAlignment(&slot.surface);

    slot.image = MEMAllocFromDefaultHeapEx(slot.surface.imageSize,
                                           slot.surface.alignment);
    if (slot.image == nullptr) {
        WSU_LOG("stream: capture surface alloc failed (%u bytes)",
                slot.surface.imageSize);
        return false;
    }
    slot.surface.image = slot.image;
    // Make sure no dirty CPU cache lines alias the GPU's target memory.
    DCFlushRange(slot.image, slot.surface.imageSize);
    WSU_LOG("stream: capture surface %ux%u pitch=%u (%u bytes)", src.width,
            src.height, slot.surface.pitch, slot.surface.imageSize);
    return true;
}

void onTvScanout(const GX2ColorBuffer *colorBuffer) {
    if (!gInitialized || colorBuffer == nullptr) return;
    if (!netHostActive()) return;

    const GX2Surface &src = colorBuffer->surface;
    if (src.aa != GX2_AA_MODE1X) return; // needs an AA resolve; skip
    if (!formatSupported(src.format)) {
        if (!gFormatWarned) {
            WSU_LOG("stream: unsupported scan-out format 0x%x, not capturing",
                    static_cast<unsigned>(src.format));
            gFormatWarned = true;
        }
        return;
    }

    OSLockMutex(&gCaptureMutex);

    uint32_t now = nowMs();

    // 1. Hand any blit issued last scan-out to the encoder — the GPU has
    //    had a full frame period to retire it.
    for (int i = 0; i < 2; i++) {
        CaptureSlot &slot = gSlots[i];
        if (slot.state == COPY_PENDING && slot.issuedAtMs != now) {
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU, slot.image,
                          slot.surface.imageSize);
            gCaptured[i].pixels = static_cast<const uint8_t *>(slot.image);
            gCaptured[i].width = slot.surface.width;
            gCaptured[i].height = slot.surface.height;
            gCaptured[i].pitch = slot.surface.pitch;
            gCaptured[i].format = slot.surface.format;
            gCaptured[i].timestampMs = slot.issuedAtMs;
            slot.state = ENCODING;
            encoderSubmit(i);
        }
    }

    // 2. Rate-cap new captures and issue the next blit into a free slot.
    const uint32_t intervalMs =
        gConfig.fps > 0 ? 1000u / static_cast<uint32_t>(gConfig.fps) : 50u;
    if (ageMs(now, gLastCaptureMs) >= intervalMs) {
        for (int i = 0; i < 2; i++) {
            CaptureSlot &slot = gSlots[i];
            if (slot.state != FREE) continue;
            if (!prepareSlot(slot, src)) break;
            GX2CopySurface(&src, 0, 0, &slot.surface, 0, 0);
            slot.state = COPY_PENDING;
            slot.issuedAtMs = now;
            gLastCaptureMs = now;
            break;
        }
    }

    OSUnlockMutex(&gCaptureMutex);
}

} // namespace

const CapturedSurface *captureGetSurface(int surfaceIndex) {
    if (surfaceIndex < 0 || surfaceIndex > 1) return nullptr;
    return &gCaptured[surfaceIndex];
}

void captureRelease(int surfaceIndex) {
    if (surfaceIndex < 0 || surfaceIndex > 1) return;
    OSLockMutex(&gCaptureMutex);
    gSlots[surfaceIndex].state = FREE;
    OSUnlockMutex(&gCaptureMutex);
}

bool captureInit() {
    OSInitMutex(&gCaptureMutex);
    gLastCaptureMs = 0;
    gFormatWarned = false;
    gInitialized = true;
    return true;
}

void captureShutdown() {
    OSLockMutex(&gCaptureMutex);
    gInitialized = false;
    for (auto &slot : gSlots) freeSlot(slot);
    OSUnlockMutex(&gCaptureMutex);
}

} // namespace wsu

// ------------------------------------------------------------------
// The hook itself.
// ------------------------------------------------------------------

DECL_FUNCTION(void, GX2CopyColorBufferToScanBuffer,
              const GX2ColorBuffer *colorBuffer, GX2ScanTarget scanTarget);

void my_GX2CopyColorBufferToScanBuffer(const GX2ColorBuffer *colorBuffer,
                                       GX2ScanTarget scanTarget) {
    if (scanTarget == GX2_SCAN_TARGET_TV) {
        wsu::onTvScanout(colorBuffer);
    }
    real_GX2CopyColorBufferToScanBuffer(colorBuffer, scanTarget);
}

WUPS_MUST_REPLACE(GX2CopyColorBufferToScanBuffer, WUPS_LOADER_LIBRARY_GX2,
                  GX2CopyColorBufferToScanBuffer);
