#include "renderer.h"
#include <android/log.h>
#define LOG_TAG "MonoView/Renderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Clear colors that give obvious visual feedback per eye mode:
//   active eye  → tinted blue-purple
//   inactive eye → pure black (nothing rendered, no draw calls)
static constexpr float kActiveColor[4]   = {0.08f, 0.18f, 0.45f, 1.0f};
static constexpr float kInactiveColor[4] = {0.0f,  0.0f,  0.0f,  1.0f};

bool Renderer::init(EGLDisplay display, EGLContext share_context) {
    egl_display = display;

    const EGLint pbuffer_attribs[] = {
        EGL_WIDTH, 16, EGL_HEIGHT, 16,
        EGL_NONE
    };

    EGLint num_configs = 0;
    const EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLConfig config;
    eglChooseConfig(display, config_attribs, &config, 1, &num_configs);
    if (num_configs == 0) { LOGE("No EGL config"); return false; }

    egl_surface = eglCreatePbufferSurface(display, config, pbuffer_attribs);
    if (egl_surface == EGL_NO_SURFACE) { LOGE("No pbuffer surface"); return false; }

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    egl_context = eglCreateContext(display, config, share_context, ctx_attribs);
    if (egl_context == EGL_NO_CONTEXT) { LOGE("No EGL context"); return false; }

    eglMakeCurrent(display, egl_surface, egl_surface, egl_context);
    LOGI("GLES3 context ready");

    if (!overlay_.init()) {
        LOGE("TextOverlay init failed — overlay will be absent");
    }
    return true;
}

void Renderer::destroy() {
    for (int eye = 0; eye < 2; ++eye) {
        for (auto& fb : eye_fbos_[eye]) {
            if (fb.fbo)   glDeleteFramebuffers(1, &fb.fbo);
            if (fb.depth) glDeleteRenderbuffers(1, &fb.depth);
        }
        eye_fbos_[eye].clear();
    }
    overlay_.destroy();
    if (egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
        if (egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_surface);
    }
}

void Renderer::create_eye_framebuffers(int eye, const std::vector<GLuint>& textures,
                                        uint32_t width, uint32_t height) {
    eye_fbos_[eye].resize(textures.size());
    for (size_t i = 0; i < textures.size(); ++i) {
        auto& fb = eye_fbos_[eye][i];
        fb.width  = width;
        fb.height = height;

        glGenFramebuffers(1, &fb.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, textures[i], 0);

        glGenRenderbuffers(1, &fb.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, fb.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, fb.depth);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            LOGE("FBO incomplete for eye %d image %zu", eye, i);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void Renderer::render_eye(int eye, uint32_t image_index, EyeMode mode) const {
    const auto& fb = eye_fbos_[eye][image_index];
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
    glViewport(0, 0, fb.width, fb.height);

    if (eye_is_active(mode, eye)) {
        glClearColor(kActiveColor[0], kActiveColor[1], kActiveColor[2], kActiveColor[3]);
    } else {
        glClearColor(kInactiveColor[0], kInactiveColor[1], kInactiveColor[2], kInactiveColor[3]);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (eye_is_active(mode, eye)) {
        overlay_.render(fb.width, fb.height, mode);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
