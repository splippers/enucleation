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

// XR_FB_passthrough PFN_ typedefs are provided by openxr.h (SDK >= 1.0.22).
// No manual redeclaration needed.

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
    // XR_FB_passthrough state
    // -----------------------------------------------------------------------
    XrPassthroughFB      passthrough_handle = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthrough_layer  = XR_NULL_HANDLE;
    bool                 passthrough_active = false;

    // Runtime-loaded passthrough function pointers
    PFN_xrCreatePassthroughFB       pfn_xrCreatePassthroughFB       = nullptr;
    PFN_xrDestroyPassthroughFB      pfn_xrDestroyPassthroughFB      = nullptr;
    PFN_xrPassthroughStartFB        pfn_xrPassthroughStartFB        = nullptr;
    PFN_xrPassthroughPauseFB        pfn_xrPassthroughPauseFB        = nullptr;
    PFN_xrCreatePassthroughLayerFB  pfn_xrCreatePassthroughLayerFB  = nullptr;
    PFN_xrDestroyPassthroughLayerFB pfn_xrDestroyPassthroughLayerFB = nullptr;
    PFN_xrPassthroughLayerPauseFB   pfn_xrPassthroughLayerPauseFB   = nullptr;
    PFN_xrPassthroughLayerResumeFB  pfn_xrPassthroughLayerResumeFB  = nullptr;

    // -----------------------------------------------------------------------
    bool create_instance(android_app* app);
    bool create_session();
    bool create_swapchains();
    bool create_input_actions();
    bool attach_action_sets();

    // Load XR_FB_passthrough function pointers and create handles.
    // Call once after create_session() succeeds.
    bool create_passthrough();

    // Start/stop the passthrough camera feed.
    void start_passthrough();
    void stop_passthrough();

    // Returns true if the session state changed
    bool poll_events();

    // Returns false when it's time to quit
    bool begin_frame(XrFrameState& out_state);
    bool locate_views(XrTime time, XrView out_views[2]);

    // When passthrough_active is true, the passthrough layer is prepended
    // to the composition layer list so the camera appears behind the GL scene.
    void end_frame(XrTime display_time,
                   XrCompositionLayerProjectionView submitted_views[2],
                   bool should_render);

    bool poll_cycle_button() const;  // true on rising edge of cycle action

    void destroy();
};
