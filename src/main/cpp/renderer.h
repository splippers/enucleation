#pragma once
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <vector>
#include "eye_controller.h"
#include "text_overlay.h"

struct EyeFramebuffer {
    GLuint fbo    = 0;
    GLuint depth  = 0;  // depth renderbuffer
    GLuint width  = 0;
    GLuint height = 0;
};

class Renderer {
public:
    bool init(EGLDisplay display, EGLContext share_context);
    void destroy();

    // Build one FBO per swapchain image for a given eye.
    // 'textures' are the GL texture IDs from xrEnumerateSwapchainImages.
    void create_eye_framebuffers(int eye, const std::vector<GLuint>& textures,
                                 uint32_t width, uint32_t height);

    // Render into the acquired swapchain image for this eye.
    // When passthrough_active is true the active eye clears with alpha=0 so
    // the passthrough camera layer shows through; the inactive eye stays black.
    void render_eye(int eye, uint32_t image_index, EyeMode mode,
                    bool passthrough_active = false) const;

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;  // pbuffer surface — no window needed

private:
    std::vector<EyeFramebuffer> eye_fbos_[2];
    TextOverlay overlay_;
};
