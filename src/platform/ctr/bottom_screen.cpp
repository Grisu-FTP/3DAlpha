// See bottom_screen.hpp.

#include "platform/ctr/bottom_screen.hpp"

#include <sys/iosupport.h>

#include <cstring>

namespace mc::ctr::bottom {
namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr u32 kBytes = u32(kWidth) * kHeight * sizeof(u16);

// 150 KB in .bss. Aligned to a cache line so the copy and the flush both start
// on one.
alignas(32) u16 gPixels[kWidth * kHeight];

bool gChanged = false;
bool gGpuOwned = false;
u32 gCopyMicros = 0;

// **The tap on stdout.** `consoleInit` installs libctru's own devoptab for
// stdout; this is a copy of it whose write marks the picture changed and then
// hands the bytes on unchanged. Reinstalled after every `consoleInit`, which
// puts libctru's back.
devoptab_t gTap;
ssize_t (*gConsoleWrite)(struct _reent*, void*, const char*, size_t) = nullptr;

ssize_t tapWrite(struct _reent* r, void* fd, const char* ptr, size_t len)
{
    gChanged = true;
    return gConsoleWrite(r, fd, ptr, len);
}

}  // namespace

PrintConsole* initConsole()
{
    PrintConsole* console = consoleInit(GFX_BOTTOM, nullptr);

    // consoleInit has just cleared the real framebuffer to black; the picture
    // is cleared to match, so callers that reprint after a re-init see the
    // same blank screen they always did.
    std::memset(gPixels, 0, sizeof(gPixels));
    if (console != nullptr) {
        console->frameBuffer = gPixels;
    }

    const devoptab_t* installed = devoptab_list[STD_OUT];
    if (installed != nullptr && installed != &gTap && installed->write_r != nullptr) {
        gTap = *installed;
        gConsoleWrite = installed->write_r;
        gTap.write_r = tapWrite;
        devoptab_list[STD_OUT] = &gTap;
    }

    gChanged = false;
    return console;
}

u16* pixels() { return gPixels; }

void changed() { gChanged = true; }

void flush()
{
    if (gGpuOwned) {
        // Kept, not dropped: it goes across when the screen comes back.
        gChanged = true;
        return;
    }

    u16 width = 0;
    u16 height = 0;
    u8* framebuffer = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &width, &height);
    // Checked rather than assumed, as hud::bottomSurface checked it before
    // this: 240 down a column, 320 columns, two bytes a pixel.
    if (framebuffer == nullptr || width != kHeight || height != kWidth
        || gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES) {
        return;
    }

    // The picture is whole before this runs, so however the scanout lines up
    // with the copy, every pixel on the glass is either last frame's or this
    // one's.
    const u64 before = svcGetSystemTick();
    std::memcpy(framebuffer, gPixels, kBytes);
    GSPGPU_FlushDataCache(framebuffer, kBytes);
    gCopyMicros = u32((svcGetSystemTick() - before) * 1000000ull / u64(SYSCLOCK_ARM11));
    gChanged = false;
}

void presentIfChanged()
{
    if (gChanged) {
        flush();
    }
}

void setGpuOwned(bool owned) { gGpuOwned = owned; }

u32 lastCopyMicros() { return gCopyMicros; }

}  // namespace mc::ctr::bottom
