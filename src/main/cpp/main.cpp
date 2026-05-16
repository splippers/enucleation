#include <android_native_app_glue.h>
#include <android/log.h>
#include <cstdio>
#include <cerrno>
#include "xr_context.h"
#include "renderer.h"
#include "eye_controller.h"

#define LOG_TAG "MonoView"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Android event callback — keeps the native glue pump alive.
// All real logic lives in the main loop below.
// ---------------------------------------------------------------------------
static void handle_android_cmd(android_app* app, int32_t cmd) {
    (void)app; (void)cmd;
}

// ---------------------------------------------------------------------------
// Apply the current eye mode to passthrough state.
// Passthrough runs whenever exactly one eye is active (LeftOnly / RightOnly).
// When both eyes are rendering normally, passthrough is paused.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Persist the chosen eye mode across app restarts.
// Uses a plain text file in the app's internal-storage dir.
// The path is passed in from android_app::activity at startup.
// ---------------------------------------------------------------------------
static void save_eye_mode(const char* dir, EyeMode mode) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/eyemode.txt", dir);
    FILE* f = std::fopen(path, "w");
    if (!f) { LOGI("Could not save eye mode: %s", std::strerror(errno)); return; }
    std::fprintf(f, "%d\n", static_cast<int>(mode));
    std::fclose(f);
}

static EyeMode load_eye_mode(const char* dir) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/eyemode.txt", dir);
    FILE* f = std::fopen(path, "r");
    if (!f) return EyeMode::Both;  // default on first run
    int v = 0;
    std::fscanf(f, "%d", &v);
    std::fclose(f);
    if (v < 0 || v > 2) return EyeMode::Both;
    return static_cast<EyeMode>(v);
}

static void apply_eye_mode_passthrough(XrContext& xr, EyeMode mode) {
    if (mode == EyeMode::Both) {
        xr.stop_passthrough();
    } else {
        // LeftOnly or RightOnly — enable camera passthrough in active eye.
        xr.start_passthrough();
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
void android_main(android_app* app) {
    app->onAppCmd = handle_android_cmd;

    XrContext  xr{};
    Renderer   renderer{};
    const char* internal_dir = app->activity->internalDataPath;
    EyeMode    eye_mode = load_eye_mode(internal_dir);
    bool       initialized = false;

    LOGI("MonoView starting — restored eye mode: %s", mode_name(eye_mode));

    while (!app->destroyRequested && !xr.quit) {
        // ---- Android event pump ----
        int events;
        android_poll_source* source;
        while (ALooper_pollOnce(0, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(app, source);
        }

        // ---- One-time init (needs the app window to have appeared) ----
        if (!initialized) {
            if (!xr.create_instance(app))      continue;
            if (!xr.create_session())          continue;
            if (!xr.create_swapchains())       continue;
            if (!xr.create_input_actions())    continue;
            if (!xr.attach_action_sets())      continue;

            // Set up passthrough — non-fatal if the device doesn't support it.
            if (!xr.create_passthrough()) {
                LOGI("Passthrough unavailable — will render without camera feed");
            }

            // Renderer shares the EGL context created by XrContext.
            if (!renderer.init(xr.egl_display, xr.egl_context)) continue;

            for (int eye = 0; eye < 2; ++eye) {
                renderer.create_eye_framebuffers(
                    eye,
                    xr.swapchains[eye].images,
                    xr.swapchains[eye].width,
                    xr.swapchains[eye].height);
            }

            initialized = true;
            LOGI("Initialized — press A or X to cycle eye mode");
        }

        // ---- OpenXR event pump ----
        xr.poll_events();
        if (!xr.session_running) continue;

        // ---- Input ----
        if (xr.poll_cycle_button()) {
            eye_mode = cycle_mode(eye_mode);
            LOGI("Eye mode → %s", mode_name(eye_mode));
            apply_eye_mode_passthrough(xr, eye_mode);
            xr.apply_haptic_confirm();
            save_eye_mode(internal_dir, eye_mode);
        }

        // ---- Frame ----
        XrFrameState frame_state{};
        if (!xr.begin_frame(frame_state)) continue;

        XrView views[2];
        XrCompositionLayerProjectionView submitted_views[2]{};

        if (frame_state.shouldRender) {
            xr.locate_views(frame_state.predictedDisplayTime, views);

            for (int eye = 0; eye < 2; ++eye) {
                auto& sc = xr.swapchains[eye];

                uint32_t image_index = 0;
                XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrAcquireSwapchainImage(sc.handle, &acquire, &image_index);

                XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(sc.handle, &wait);

                renderer.render_eye(eye, image_index, eye_mode,
                                    xr.passthrough_active);

                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(sc.handle, &release);

                submitted_views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                submitted_views[eye].pose    = views[eye].pose;
                submitted_views[eye].fov     = views[eye].fov;
                submitted_views[eye].subImage.swapchain = sc.handle;
                submitted_views[eye].subImage.imageRect = {
                    {0, 0}, {sc.width, sc.height}
                };
            }
        }

        xr.end_frame(frame_state.predictedDisplayTime, submitted_views,
                     frame_state.shouldRender);
    }

    renderer.destroy();
    xr.destroy();
    LOGI("MonoView exit");
}
