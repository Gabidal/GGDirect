#include "gpu_context.h"

#include "logger.h"

#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <algorithm>

namespace gpu {

namespace {

const char* eglErrorToString(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "EGL_UNKNOWN_ERROR";
    }
}

} // namespace

Context::Context()
    : drmDevice(nullptr),
      gbmDevice(nullptr),
      surface(nullptr),
      eglDisplay(EGL_NO_DISPLAY),
      eglContext(EGL_NO_CONTEXT),
      eglConfig(nullptr),
      eglSurface(EGL_NO_SURFACE),
      pixelFormat(DRM_FORMAT_XRGB8888),
      width(0),
      height(0) {}

Context::~Context() {
    if (drmDevice) {
        cleanup(*drmDevice);
    }
}

bool Context::initialize(display::device& device, const display::mode& mode, uint32_t drmFormat) {
    drmDevice = &device;
    gbmDevice = device.getGbmDevice();
    if (!gbmDevice) {
        LOG_ERROR() << "GBM device not available; cannot initialize GPU context" << std::endl;
        return false;
    }

    width = mode.getWidth();
    height = mode.getHeight();
    pixelFormat = drmFormat;

    surface = gbm_surface_create(gbmDevice, width, height, pixelFormat,
                                 GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!surface) {
        LOG_ERROR() << "Failed to create GBM surface" << std::endl;
        return false;
    }

    eglDisplay = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbmDevice));
    if (eglDisplay == EGL_NO_DISPLAY) {
        LOG_ERROR() << "eglGetDisplay failed" << std::endl;
        cleanup(device);
        return false;
    }

    if (!eglInitialize(eglDisplay, nullptr, nullptr)) {
        LOG_ERROR() << "eglInitialize failed: " << eglErrorToString(eglGetError()) << std::endl;
        cleanup(device);
        return false;
    }

    static const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint numConfigs = 0;
    if (!eglChooseConfig(eglDisplay, configAttribs, &eglConfig, 1, &numConfigs) || numConfigs == 0) {
        LOG_ERROR() << "eglChooseConfig failed: " << eglErrorToString(eglGetError()) << std::endl;
        cleanup(device);
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        LOG_ERROR() << "eglBindAPI failed: " << eglErrorToString(eglGetError()) << std::endl;
        cleanup(device);
        return false;
    }

    static const EGLint contextAttribsES3[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttribsES3);
    if (eglContext == EGL_NO_CONTEXT) {
        static const EGLint contextAttribsES2[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttribsES2);
        if (eglContext == EGL_NO_CONTEXT) {
            LOG_ERROR() << "eglCreateContext failed: " << eglErrorToString(eglGetError()) << std::endl;
            cleanup(device);
            return false;
        }
    }

    eglSurface = eglCreateWindowSurface(eglDisplay, eglConfig,
                                        reinterpret_cast<EGLNativeWindowType>(surface), nullptr);
    if (eglSurface == EGL_NO_SURFACE) {
        LOG_ERROR() << "eglCreateWindowSurface failed: " << eglErrorToString(eglGetError()) << std::endl;
        cleanup(device);
        return false;
    }

    if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
        LOG_ERROR() << "eglMakeCurrent failed: " << eglErrorToString(eglGetError()) << std::endl;
        cleanup(device);
        return false;
    }

    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    return true;
}

void Context::cleanup(display::device& device) {
    while (!pendingFrames.empty()) {
        PendingFrame frame = pendingFrames.front();
        pendingFrames.pop_front();
        if (frame.framebuffer) {
            device.destroyFramebuffer(frame.framebuffer);
        }
        if (surface && frame.bo) {
            gbm_surface_release_buffer(surface, frame.bo);
        }
    }

    if (eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay, eglSurface);
            eglSurface = EGL_NO_SURFACE;
        }

        if (eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay, eglContext);
            eglContext = EGL_NO_CONTEXT;
        }

        eglTerminate(eglDisplay);
        eglDisplay = EGL_NO_DISPLAY;
    }

    if (surface) {
        gbm_surface_destroy(surface);
        surface = nullptr;
    }

    drmDevice = nullptr;
    gbmDevice = nullptr;
    width = 0;
    height = 0;
}

bool Context::makeCurrent() const {
    if (eglDisplay == EGL_NO_DISPLAY || eglSurface == EGL_NO_SURFACE || eglContext == EGL_NO_CONTEXT) {
        return false;
    }
    return eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);
}

void Context::beginFrame() {
    if (eglDisplay == EGL_NO_DISPLAY) {
        return;
    }
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glClear(GL_COLOR_BUFFER_BIT);
}

PendingFrame Context::swapBuffers(display::device& device) {
    PendingFrame result{nullptr, nullptr};

    if (eglDisplay == EGL_NO_DISPLAY || eglSurface == EGL_NO_SURFACE) {
        return result;
    }

    if (!eglSwapBuffers(eglDisplay, eglSurface)) {
        LOG_ERROR() << "eglSwapBuffers failed: " << eglErrorToString(eglGetError()) << std::endl;
        return result;
    }

    gbm_bo* bo = gbm_surface_lock_front_buffer(surface);
    if (!bo) {
        LOG_ERROR() << "Failed to lock GBM front buffer" << std::endl;
        return result;
    }

    auto fb = device.createFramebufferFromBo(bo, pixelFormat);
    if (!fb) {
        gbm_surface_release_buffer(surface, bo);
        return result;
    }

    result.bo = bo;
    result.framebuffer = fb;
    pendingFrames.push_back(result);
    return result;
}

void Context::onPageFlipComplete(display::device& device) {
    if (pendingFrames.empty()) {
        return;
    }

    PendingFrame frame = pendingFrames.front();
    pendingFrames.pop_front();

    if (frame.framebuffer) {
        device.destroyFramebuffer(frame.framebuffer);
    }

    if (surface && frame.bo) {
        gbm_surface_release_buffer(surface, frame.bo);
    }
}

void Context::releaseFrame(display::device& device, const PendingFrame& frame) {
    auto it = std::find_if(pendingFrames.begin(), pendingFrames.end(), [&](const PendingFrame& pending) {
        return pending.bo == frame.bo && pending.framebuffer == frame.framebuffer;
    });

    if (it != pendingFrames.end()) {
        if (it->framebuffer) {
            device.destroyFramebuffer(it->framebuffer);
        }
        if (surface && it->bo) {
            gbm_surface_release_buffer(surface, it->bo);
        }
        pendingFrames.erase(it);
    } else {
        if (frame.framebuffer) {
            device.destroyFramebuffer(frame.framebuffer);
        }
        if (surface && frame.bo) {
            gbm_surface_release_buffer(surface, frame.bo);
        }
    }
}

} // namespace gpu
