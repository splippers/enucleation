#include <android_native_app_glue.h>
#include <android/log.h>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <cmath>
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

// Preferred mode: the single-eye mode the user has settled on (auto-learned after
// 30 continuous seconds of use, persisted across restarts).  Long-press jumps here
// rather than always resetting to Both, so a user who prefers LeftOnly always gets
// back to their preferred state in one gesture instead of two.
static void save_preferred_mode(const char* dir, EyeMode mode) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/preferred_mode.txt", dir);
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "%d\n", static_cast<int>(mode));
    std::fclose(f);
}

static EyeMode load_preferred_mode(const char* dir) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/preferred_mode.txt", dir);
    FILE* f = std::fopen(path, "r");
    if (!f) return EyeMode::Both;
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
    EyeMode    eye_mode       = load_eye_mode(internal_dir);
    EyeMode    preferred_mode = load_preferred_mode(internal_dir);
    bool       initialized    = false;

    // How long the user has been continuously in the current single-eye mode.
    // After 30s we promote that mode to preferred and give a confirming buzz.
    float      time_in_mode   = 0.0f;
    bool       prefer_locked  = (preferred_mode != EyeMode::Both);
    static constexpr float kAutoPreferSecs = 30.0f;

    LOGI("MonoView starting — restored eye mode: %s  preferred: %s",
         mode_name(eye_mode), mode_name(preferred_mode));

    auto prev_tp = std::chrono::steady_clock::now();

    while (!app->destroyRequested && !xr.quit) {
        // ---- Frame delta time ----
        auto  now_tp = std::chrono::steady_clock::now();
        float dt     = std::chrono::duration<float>(now_tp - prev_tp).count();
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.100f) dt = 0.100f;  // cap at 100 ms to avoid large jumps on resume
        prev_tp = now_tp;

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

            // Enable Fixed Foveated Rendering — non-fatal.
            if (!xr.create_foveation()) {
                LOGI("FFR unavailable — running at full resolution");
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
        auto btn = xr.poll_cycle_button(dt);

        if (btn == ButtonPress::Short) {
            eye_mode     = cycle_mode(eye_mode);
            time_in_mode = 0.0f;  // reset timer — user is still exploring
            LOGI("Eye mode → %s", mode_name(eye_mode));
            apply_eye_mode_passthrough(xr, eye_mode);
            xr.apply_haptic_confirm();
            save_eye_mode(internal_dir, eye_mode);
        } else if (btn == ButtonPress::Long) {
            // Long press: jump to preferred single-eye mode (or Both if none set yet)
            EyeMode target = (preferred_mode != EyeMode::Both) ? preferred_mode : EyeMode::Both;
            if (eye_mode != target) {
                eye_mode     = target;
                time_in_mode = 0.0f;
                LOGI("Long press — jump to preferred mode: %s", mode_name(eye_mode));
                apply_eye_mode_passthrough(xr, eye_mode);
                xr.apply_haptic_confirm();
                save_eye_mode(internal_dir, eye_mode);
            }
        }

        // Auto-prefer: after 30 continuous seconds in a single-eye mode, record it
        // as the preferred mode with a subtle confirming buzz.
        if (eye_mode != EyeMode::Both) {
            time_in_mode += dt;
            if (time_in_mode >= kAutoPreferSecs && preferred_mode != eye_mode) {
                preferred_mode = eye_mode;
                prefer_locked  = true;
                save_preferred_mode(internal_dir, preferred_mode);
                xr.apply_haptic_confirm();  // one short buzz — "got it"
                LOGI("Auto-preferred mode: %s", mode_name(preferred_mode));
            }
        } else {
            time_in_mode = 0.0f;
        }

        // Passthrough opacity: left thumbstick Y with 0.15 dead zone
        if (xr.passthrough_active) {
            float stick_y = xr.poll_left_stick_y();
            if (fabsf(stick_y) > 0.15f)
                xr.set_passthrough_opacity(xr.passthrough_opacity + stick_y * dt * 0.8f);
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

                bool eye_preferred = (preferred_mode == eye_mode &&
                                      eye_mode != EyeMode::Both);
                renderer.render_eye(eye, image_index, eye_mode,
                                    xr.passthrough_active,
                                    xr.passthrough_opacity,
                                    eye_preferred,
                                    /*edge_on=*/false);

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
