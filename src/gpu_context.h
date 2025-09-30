#ifndef GG_DIRECT_GPU_CONTEXT_H
#define GG_DIRECT_GPU_CONTEXT_H

#include <memory>
#include <deque>

#include <drm_fourcc.h>
#include <EGL/egl.h>

#include "display.h"

struct gbm_device;
struct gbm_surface;
struct gbm_bo;

namespace gpu {

struct PendingFrame {
    gbm_bo* bo;
    std::shared_ptr<display::frameBuffer> framebuffer;
};

class Context {
public:
    Context();
    ~Context();

    bool initialize(display::device& device, const display::mode& mode, uint32_t drmFormat = DRM_FORMAT_XRGB8888);
    void cleanup(display::device& device);

    bool makeCurrent() const;
    void beginFrame();
    PendingFrame swapBuffers(display::device& device);

    void onPageFlipComplete(display::device& device);
    void releaseFrame(display::device& device, const PendingFrame& frame);

    EGLDisplay getEglDisplay() const { return eglDisplay; }
    EGLContext getEglContext() const { return eglContext; }
    EGLSurface getEglSurface() const { return eglSurface; }
    gbm_surface* getSurface() const { return surface; }
    uint32_t getFormat() const { return pixelFormat; }
    uint32_t getWidth() const { return width; }
    uint32_t getHeight() const { return height; }

private:
    display::device* drmDevice;
    gbm_device* gbmDevice;
    gbm_surface* surface;

    EGLDisplay eglDisplay;
    EGLContext eglContext;
    EGLConfig eglConfig;
    EGLSurface eglSurface;

    uint32_t pixelFormat;
    uint32_t width;
    uint32_t height;

    std::deque<PendingFrame> pendingFrames;
};

} // namespace gpu

#endif // GG_DIRECT_GPU_CONTEXT_H
