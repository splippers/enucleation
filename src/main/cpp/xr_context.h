#pragma once
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android_native_app_glue.h>
#include <vector>
#include <string>

struct Swapchain {
    XrSwapchain handle  = XR_NULL_HANDLE;
    int32_t     width   = 0;
    int32_t     height  = 0;
    std::vector<GLuint> images;  // GL texture IDs, one per swapchain image
};

struct XrContext {
    // Core handles
    XrInstance  instance = XR_NULL_HANDLE;
    XrSystemId  system   = XR_NULL_SYSTEM_ID;
    XrSession   session  = XR_NULL_HANDLE;

    // Spaces
    XrSpace stage_space = XR_NULL_HANDLE;
    XrSpace view_space  = XR_NULL_HANDLE;

    // Swapchains — index 0 = left eye, 1 = right eye
    Swapchain swapchains[2];

    // Input
    XrActionSet action_set     = XR_NULL_HANDLE;
    XrAction    cycle_action   = XR_NULL_HANDLE;  // A button → cycle eye mode
    XrPath      hand_paths[2]  = {};              // left, right

    // EGL state shared with Renderer
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;

    // Session lifecycle state
    XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
    bool           session_running = false;
    bool           quit            = false;

    // -----------------------------------------------------------------------
    bool create_instance(android_app* app);
    bool create_session();
    bool create_swapchains();
    bool create_input_actions();
    bool attach_action_sets();

    // Returns true if the session state changed
    bool poll_events();

    // Returns false when it's time to quit
    bool begin_frame(XrFrameState& out_state);
    bool locate_views(XrTime time, XrView out_views[2]);

    void end_frame(XrTime display_time,
                   XrCompositionLayerProjectionView submitted_views[2],
                   bool should_render);

    bool poll_cycle_button() const;  // true on rising edge of cycle action

    void destroy();
};
