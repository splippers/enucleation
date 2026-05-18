#include "xr_context.h"
#include <android/log.h>
#include <cstring>
#include <stdexcept>

#define LOG_TAG "MonoView/XR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

#define XR_CHECK(expr) do { \
    XrResult _r = (expr); \
    if (XR_FAILED(_r)) { LOGE("XR_FAILED(%d) at %s:%d", _r, __FILE__, __LINE__); return false; } \
} while(0)

// ---------------------------------------------------------------------------
// Helper: load a single extension function pointer by name, warn on failure.
// ---------------------------------------------------------------------------
template<typename T>
static void load_pfn(XrInstance inst, const char* name, T& out_pfn) {
    XrResult r = xrGetInstanceProcAddr(inst, name,
                     reinterpret_cast<PFN_xrVoidFunction*>(&out_pfn));
    if (XR_FAILED(r)) {
        LOGW("xrGetInstanceProcAddr(\"%s\") failed: %d", name, r);
        out_pfn = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
bool XrContext::create_instance(android_app* app) {
    // Android loader initialisation — must happen before xrCreateInstance.
    PFN_xrInitializeLoaderKHR init_loader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&init_loader));

    XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader_info.applicationVM       = app->activity->vm;
    loader_info.applicationContext  = app->activity->clazz;
    XR_CHECK(init_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info)));

    // Required + optional extensions.  Foveation pair is non-fatal if absent.
    const char* extensions[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_FB_PASSTHROUGH_EXTENSION_NAME,
        "XR_FB_passthrough_color_adjust",  // XrPassthroughBrightnessContrastSaturationFB
        XR_FB_FOVEATION_EXTENSION_NAME,
        XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME,
    };

    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM       = app->activity->vm;
    android_info.applicationActivity = app->activity->clazz;

    XrApplicationInfo app_info{};
    std::strncpy(app_info.applicationName, "MonoView", XR_MAX_APPLICATION_NAME_SIZE);
    app_info.applicationVersion = 1;
    std::strncpy(app_info.engineName, "none", XR_MAX_ENGINE_NAME_SIZE);
    app_info.engineVersion  = 0;
    app_info.apiVersion     = XR_CURRENT_API_VERSION;

    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    info.next                    = &android_info;
    info.applicationInfo         = app_info;
    info.enabledExtensionCount   = 6;
    info.enabledExtensionNames   = extensions;

    XR_CHECK(xrCreateInstance(&info, &instance));

    // Get the HMD system
    XrSystemGetInfo sys_info{XR_TYPE_SYSTEM_GET_INFO};
    sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(instance, &sys_info, &system));

    LOGI("Instance and system ready");
    return true;
}

// ---------------------------------------------------------------------------
// Session (GLES3 binding)
// ---------------------------------------------------------------------------
bool XrContext::create_session() {
    // Query the GLES requirements — mandatory call before xrCreateSession.
    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_reqs = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_reqs));
    XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK(get_reqs(instance, system, &reqs));

    // Create a minimal EGL context so the runtime can share it with us.
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(egl_display, nullptr, nullptr);

    const EGLint config_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n;
    eglChooseConfig(egl_display, config_attrs, &cfg, 1, &n);

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    egl_context = eglCreateContext(egl_display, cfg, EGL_NO_CONTEXT, ctx_attrs);

    const EGLint pb_attrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(egl_display, cfg, pb_attrs);
    eglMakeCurrent(egl_display, surface, surface, egl_context);

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = egl_display;
    binding.config  = cfg;
    binding.context = egl_context;

    XrSessionCreateInfo sess_info{XR_TYPE_SESSION_CREATE_INFO};
    sess_info.next     = &binding;
    sess_info.systemId = system;
    XR_CHECK(xrCreateSession(instance, &sess_info, &session));

    // Create reference spaces
    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.poseInReferenceSpace = {{0,0,0,1}, {0,0,0}};

    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    XR_CHECK(xrCreateReferenceSpace(session, &space_info, &stage_space));

    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(session, &space_info, &view_space));

    LOGI("Session created");
    return true;
}

// ---------------------------------------------------------------------------
// Swapchains — one per eye
// ---------------------------------------------------------------------------
bool XrContext::create_swapchains() {
    // Enumerate view configurations to get recommended resolution.
    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(instance, system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);

    std::vector<XrViewConfigurationView> cfg_views(view_count,
        {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance, system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        view_count, &view_count, cfg_views.data());

    // Pick a supported swapchain format — prefer SRGB.
    uint32_t fmt_count = 0;
    xrEnumerateSwapchainFormats(session, 0, &fmt_count, nullptr);
    std::vector<int64_t> formats(fmt_count);
    xrEnumerateSwapchainFormats(session, fmt_count, &fmt_count, formats.data());

    int64_t chosen_format = formats[0];
    for (int64_t f : formats) {
        if (f == GL_SRGB8_ALPHA8) { chosen_format = f; break; }
    }

    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto& cv = cfg_views[eye];
        swapchains[eye].width  = cv.recommendedImageRectWidth;
        swapchains[eye].height = cv.recommendedImageRectHeight;

        XrSwapchainCreateInfo sc_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc_info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sc_info.format      = chosen_format;
        sc_info.sampleCount = 1;
        sc_info.width       = cv.recommendedImageRectWidth;
        sc_info.height      = cv.recommendedImageRectHeight;
        sc_info.faceCount   = 1;
        sc_info.arraySize   = 1;
        sc_info.mipCount    = 1;
        XR_CHECK(xrCreateSwapchain(session, &sc_info, &swapchains[eye].handle));

        uint32_t img_count = 0;
        xrEnumerateSwapchainImages(swapchains[eye].handle, 0, &img_count, nullptr);
        std::vector<XrSwapchainImageOpenGLESKHR> xr_images(img_count,
            {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(swapchains[eye].handle, img_count, &img_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(xr_images.data()));

        swapchains[eye].images.resize(img_count);
        for (uint32_t i = 0; i < img_count; ++i)
            swapchains[eye].images[i] = xr_images[i].image;

        LOGI("Swapchain eye %u: %dx%d, %u images",
             eye, cv.recommendedImageRectWidth, cv.recommendedImageRectHeight, img_count);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Input — A button cycles eye mode
// ---------------------------------------------------------------------------
bool XrContext::create_input_actions() {
    XrActionSetCreateInfo as_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(as_info.actionSetName,          "monoview", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(as_info.localizedActionSetName, "MonoView", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    as_info.priority = 0;
    XR_CHECK(xrCreateActionSet(instance, &as_info, &action_set));

    xrStringToPath(instance, "/user/hand/left",  &hand_paths[0]);
    xrStringToPath(instance, "/user/hand/right", &hand_paths[1]);

    XrActionCreateInfo ac_info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(ac_info.actionName,          "cycle_eye_mode", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(ac_info.localizedActionName, "Cycle Eye Mode", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    ac_info.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
    ac_info.countSubactionPaths = 2;
    ac_info.subactionPaths      = hand_paths;
    XR_CHECK(xrCreateAction(action_set, &ac_info, &cycle_action));

    // Haptic output action — confirmation buzz when mode changes.
    XrActionCreateInfo hap_info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(hap_info.actionName,          "confirm_haptic", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(hap_info.localizedActionName, "Mode Change Confirm", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    hap_info.actionType          = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    hap_info.countSubactionPaths = 2;
    hap_info.subactionPaths      = hand_paths;
    XR_CHECK(xrCreateAction(action_set, &hap_info, &haptic_action));

    // Left thumbstick axis — passthrough opacity control.
    XrActionCreateInfo stick_info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(stick_info.actionName,          "left_stick", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(stick_info.localizedActionName, "Left Thumbstick", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    stick_info.actionType          = XR_ACTION_TYPE_VECTOR2F_INPUT;
    stick_info.countSubactionPaths = 1;
    stick_info.subactionPaths      = &hand_paths[0]; // left only
    XR_CHECK(xrCreateAction(action_set, &stick_info, &stick_action));

    // Y (left) / B (right) — toggle edge enhancement filter.
    XrActionCreateInfo edge_info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(edge_info.actionName,          "edge_enhance", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(edge_info.localizedActionName, "Edge Enhance", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    edge_info.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
    edge_info.countSubactionPaths = 2;
    edge_info.subactionPaths      = hand_paths;
    XR_CHECK(xrCreateAction(action_set, &edge_info, &edge_action));

    // Right thumbstick axis — passthrough brightness control.
    XrActionCreateInfo rstick_info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(rstick_info.actionName,          "right_stick", XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(rstick_info.localizedActionName, "Right Thumbstick", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    rstick_info.actionType          = XR_ACTION_TYPE_VECTOR2F_INPUT;
    rstick_info.countSubactionPaths = 1;
    rstick_info.subactionPaths      = &hand_paths[1];  // right only
    XR_CHECK(xrCreateAction(action_set, &rstick_info, &right_stick_action));

    return true;
}

bool XrContext::attach_action_sets() {
    // Suggest bindings: A (right) and X (left) both cycle mode.
    XrPath a_click, x_click;
    xrStringToPath(instance, "/user/hand/right/input/a/click", &a_click);
    xrStringToPath(instance, "/user/hand/left/input/x/click",  &x_click);

    XrPath left_haptic, right_haptic, left_stick, right_stick;
    xrStringToPath(instance, "/user/hand/left/output/haptic",   &left_haptic);
    xrStringToPath(instance, "/user/hand/right/output/haptic",  &right_haptic);
    xrStringToPath(instance, "/user/hand/left/input/thumbstick",  &left_stick);
    xrStringToPath(instance, "/user/hand/right/input/thumbstick", &right_stick);

    // Y (left) and B (right) toggle edge enhance.
    XrPath y_click, b_click;
    xrStringToPath(instance, "/user/hand/left/input/y/click",  &y_click);
    xrStringToPath(instance, "/user/hand/right/input/b/click", &b_click);

    XrActionSuggestedBinding bindings[] = {
        {cycle_action,       a_click},
        {cycle_action,       x_click},
        {haptic_action,      left_haptic},
        {haptic_action,      right_haptic},
        {stick_action,       left_stick},
        {edge_action,        y_click},
        {edge_action,        b_click},
        {right_stick_action, right_stick},
    };

    XrPath oculus_profile;
    xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &oculus_profile);

    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile     = oculus_profile;
    suggested.suggestedBindings      = bindings;
    suggested.countSuggestedBindings = 8;
    XR_CHECK(xrSuggestInteractionProfileBindings(instance, &suggested));

    XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach_info.actionSets      = &action_set;
    attach_info.countActionSets = 1;
    XR_CHECK(xrAttachSessionActionSets(session, &attach_info));

    return true;
}

// ---------------------------------------------------------------------------
// XR_FB_passthrough — create handles and load function pointers
// ---------------------------------------------------------------------------
bool XrContext::create_passthrough() {
    // Load all required extension function pointers.
    load_pfn(instance, "xrCreatePassthroughFB",       pfn_xrCreatePassthroughFB);
    load_pfn(instance, "xrDestroyPassthroughFB",      pfn_xrDestroyPassthroughFB);
    load_pfn(instance, "xrPassthroughStartFB",        pfn_xrPassthroughStartFB);
    load_pfn(instance, "xrPassthroughPauseFB",        pfn_xrPassthroughPauseFB);
    load_pfn(instance, "xrCreatePassthroughLayerFB",  pfn_xrCreatePassthroughLayerFB);
    load_pfn(instance, "xrDestroyPassthroughLayerFB", pfn_xrDestroyPassthroughLayerFB);
    load_pfn(instance, "xrPassthroughLayerPauseFB",    pfn_xrPassthroughLayerPauseFB);
    load_pfn(instance, "xrPassthroughLayerResumeFB",  pfn_xrPassthroughLayerResumeFB);
    load_pfn(instance, "xrPassthroughLayerSetStyleFB", pfn_xrPassthroughLayerSetStyleFB);

    if (!pfn_xrCreatePassthroughFB || !pfn_xrCreatePassthroughLayerFB) {
        LOGE("XR_FB_passthrough function pointers unavailable — passthrough disabled");
        return false;
    }

    // Create the top-level passthrough object.
    XrPassthroughCreateInfoFB pt_info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    pt_info.flags = 0;
    XrResult r = pfn_xrCreatePassthroughFB(session, &pt_info, &passthrough_handle);
    if (XR_FAILED(r)) {
        LOGE("xrCreatePassthroughFB failed: %d", r);
        return false;
    }

    // Create a reconstruction layer (full-screen camera passthrough).
    XrPassthroughLayerCreateInfoFB layer_info{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    layer_info.passthrough = passthrough_handle;
    layer_info.purpose     = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    layer_info.flags       = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    r = pfn_xrCreatePassthroughLayerFB(session, &layer_info, &passthrough_layer);
    if (XR_FAILED(r)) {
        LOGE("xrCreatePassthroughLayerFB failed: %d", r);
        return false;
    }

    LOGI("XR_FB_passthrough handles created");
    return true;
}

void XrContext::start_passthrough() {
    if (passthrough_active) return;
    if (!pfn_xrPassthroughStartFB || passthrough_handle == XR_NULL_HANDLE) return;

    XrResult r = pfn_xrPassthroughStartFB(passthrough_handle);
    if (XR_FAILED(r)) {
        LOGE("xrPassthroughStartFB failed: %d", r);
        return;
    }

    // Resume the layer in case it was previously paused.
    if (pfn_xrPassthroughLayerResumeFB && passthrough_layer != XR_NULL_HANDLE) {
        pfn_xrPassthroughLayerResumeFB(passthrough_layer);
    }

    passthrough_active = true;
    LOGI("Passthrough started");
}

void XrContext::stop_passthrough() {
    if (!passthrough_active) return;
    if (!pfn_xrPassthroughPauseFB || passthrough_handle == XR_NULL_HANDLE) return;

    // Pause the layer first, then pause the passthrough subsystem.
    if (pfn_xrPassthroughLayerPauseFB && passthrough_layer != XR_NULL_HANDLE) {
        pfn_xrPassthroughLayerPauseFB(passthrough_layer);
    }

    XrResult r = pfn_xrPassthroughPauseFB(passthrough_handle);
    if (XR_FAILED(r)) {
        LOGE("xrPassthroughPauseFB failed: %d", r);
    }

    passthrough_active = false;
    LOGI("Passthrough stopped");
}

// ---------------------------------------------------------------------------
// XR_FB_foveation — Fixed Foveated Rendering
//
// Applies HIGH-level fixed foveation to both swapchains after they are
// created.  The Quest runtime renders the peripheral area at reduced
// resolution and the foveal area at full resolution, saving ~20-30%
// GPU time with minimal perceived quality loss for stationary content.
// ---------------------------------------------------------------------------
bool XrContext::create_foveation() {
    load_pfn(instance, "xrCreateFoveationProfileFB",  pfn_xrCreateFoveationProfileFB);
    load_pfn(instance, "xrDestroyFoveationProfileFB", pfn_xrDestroyFoveationProfileFB);
    load_pfn(instance, "xrUpdateSwapchainFB",         pfn_xrUpdateSwapchainFB);

    if (!pfn_xrCreateFoveationProfileFB || !pfn_xrUpdateSwapchainFB) {
        LOGW("XR_FB_foveation unavailable — FFR disabled");
        return false;
    }

    // Create a HIGH fixed-foveation profile.
    XrFoveationLevelProfileCreateInfoFB level_info{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    level_info.level   = XR_FOVEATION_LEVEL_HIGH_FB;
    level_info.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;  // fixed, not dynamic

    XrFoveationProfileCreateInfoFB profile_info{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
    profile_info.next = &level_info;

    XrResult r = pfn_xrCreateFoveationProfileFB(session, &profile_info, &foveation_profile);
    if (XR_FAILED(r)) {
        LOGE("xrCreateFoveationProfileFB failed: %d", r);
        return false;
    }

    // Apply the profile to both eye swapchains.
    XrSwapchainStateFoveationFB fov_state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
    fov_state.flags   = 0;
    fov_state.profile = foveation_profile;

    for (auto& sc : swapchains) {
        if (sc.handle == XR_NULL_HANDLE) continue;
        r = pfn_xrUpdateSwapchainFB(sc.handle,
                reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&fov_state));
        if (XR_FAILED(r))
            LOGW("xrUpdateSwapchainFB failed: %d (non-fatal)", r);
    }

    LOGI("Fixed Foveated Rendering: HIGH");
    return true;
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------
bool XrContext::poll_events() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* state_event =
                    reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                session_state = state_event->state;
                LOGI("Session state → %d", session_state);

                if (session_state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    xrBeginSession(session, &begin);
                    session_running = true;
                }
                if (session_state == XR_SESSION_STATE_STOPPING) {
                    // Tear down passthrough before ending the session.
                    if (passthrough_active) stop_passthrough();
                    xrEndSession(session);
                    session_running = false;
                }
                if (session_state == XR_SESSION_STATE_EXITING ||
                    session_state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                quit = true;
                break;
            default:
                break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
bool XrContext::begin_frame(XrFrameState& out_state) {
    out_state = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(xrWaitFrame(session, &wait_info, &out_state))) return false;

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(session, &begin_info);
    return true;
}

bool XrContext::locate_views(XrTime time, XrView out_views[2]) {
    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime           = time;
    locate_info.space                 = stage_space;

    XrViewState view_state{XR_TYPE_VIEW_STATE};
    uint32_t view_count = 2;
    out_views[0] = {XR_TYPE_VIEW};
    out_views[1] = {XR_TYPE_VIEW};
    XR_CHECK(xrLocateViews(session, &locate_info, &view_state, 2, &view_count, out_views));
    return true;
}

void XrContext::end_frame(XrTime display_time,
                          XrCompositionLayerProjectionView submitted_views[2],
                          bool should_render) {
    // Build the projection layer (the GL-rendered eye content).
    XrCompositionLayerProjection proj_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    proj_layer.space     = stage_space;
    proj_layer.viewCount = 2;
    proj_layer.views     = submitted_views;

    // When passthrough is active the projection layer must have
    // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT set so that
    // pixels with alpha=0 let the passthrough camera show through.
    if (passthrough_active) {
        proj_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    }

    if (passthrough_active && passthrough_layer != XR_NULL_HANDLE) {
        // Passthrough composition layer — sits below the projection layer.
        XrCompositionLayerPassthroughFB pt_comp_layer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        pt_comp_layer.flags       = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        pt_comp_layer.space       = XR_NULL_HANDLE;  // passthrough layers don't use a space
        pt_comp_layer.layerHandle = passthrough_layer;

        // Two-layer stack: passthrough first (bottom), projection on top.
        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pt_comp_layer),
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj_layer),
        };

        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime          = display_time;
        // Use ADDITIVE blend so the passthrough fills the background fully.
        // ALPHA_BLEND is not universally supported; OPAQUE with a passthrough
        // layer beneath is the correct Meta Quest pattern.
        end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end_info.layerCount           = should_render ? 2 : 1;
        end_info.layers               = layers;

        xrEndFrame(session, &end_info);
    } else {
        // No passthrough — submit only the projection layer.
        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj_layer)
        };

        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime          = display_time;
        end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end_info.layerCount           = should_render ? 1 : 0;
        end_info.layers               = should_render ? layers : nullptr;

        xrEndFrame(session, &end_info);
    }
}

void XrContext::apply_haptic_confirm() {
    if (haptic_action == XR_NULL_HANDLE || session == XR_NULL_HANDLE) return;

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = 0.7f;
    vibration.duration  = 80'000'000;  // 80 ms in nanoseconds
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

    for (int hand = 0; hand < 2; ++hand) {
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action        = haptic_action;
        info.subactionPath = hand_paths[hand];
        xrApplyHapticFeedback(session, &info,
            reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
    }
}

ButtonPress XrContext::poll_cycle_button(float dt) {
    XrActiveActionSet active{};
    active.actionSet = action_set;

    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.activeActionSets      = &active;
    sync.countActiveActionSets = 1;
    xrSyncActions(session, &sync);

    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = cycle_action;
    xrGetActionStateBoolean(session, &get_info, &state);

    if (!state.isActive) { _btn_hold_time = 0.0f; return ButtonPress::None; }

    if (state.currentState) {
        // Button held down — accumulate time, wait for release
        _btn_hold_time += dt;
        return ButtonPress::None;
    }

    // Button just released
    if (state.changedSinceLastSync) {
        float held      = _btn_hold_time;
        _btn_hold_time  = 0.0f;
        if (held > 0.04f) // ignore phantom sub-40ms events
            return held >= 1.2f ? ButtonPress::Long : ButtonPress::Short;
    } else {
        _btn_hold_time = 0.0f;
    }
    return ButtonPress::None;
}

float XrContext::poll_left_stick_y() const {
    if (stick_action == XR_NULL_HANDLE) return 0.0f;
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    XrActionStateGetInfo  info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action        = stick_action;
    info.subactionPath = hand_paths[0]; // left hand
    xrGetActionStateVector2f(session, &info, &state);
    return state.isActive ? state.currentState.y : 0.0f;
}

float XrContext::poll_right_stick_y() const {
    if (right_stick_action == XR_NULL_HANDLE) return 0.0f;
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    XrActionStateGetInfo  info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action        = right_stick_action;
    info.subactionPath = hand_paths[1]; // right hand
    xrGetActionStateVector2f(session, &info, &state);
    return state.isActive ? state.currentState.y : 0.0f;
}

bool XrContext::poll_edge_button() const {
    if (edge_action == XR_NULL_HANDLE) return false;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = edge_action;
    xrGetActionStateBoolean(session, &get_info, &state);
    // Rising-edge only: button pressed this frame (not held from last frame)
    return state.isActive && state.changedSinceLastSync && state.currentState;
}

void XrContext::apply_passthrough_style(float opacity, float brightness, bool edge_on) {
    if (!pfn_xrPassthroughLayerSetStyleFB || passthrough_layer == XR_NULL_HANDLE) return;
    if (opacity    < 0.1f)  opacity    = 0.1f;
    if (opacity    > 1.0f)  opacity    = 1.0f;
    if (brightness < -0.5f) brightness = -0.5f;
    if (brightness >  0.5f) brightness =  0.5f;
    passthrough_opacity = opacity;

    XrPassthroughBrightnessContrastSaturationFB bcs{
        XR_TYPE_PASSTHROUGH_BRIGHTNESS_CONTRAST_SATURATION_FB};
    bcs.brightness = brightness;
    bcs.contrast   = edge_on ? 1.6f : 1.0f;
    bcs.saturation = 1.0f;

    XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
    style.next                 = &bcs;
    style.textureOpacityFactor = opacity;
    pfn_xrPassthroughLayerSetStyleFB(passthrough_layer, &style);
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------
void XrContext::destroy() {
    // Foveation profile
    if (foveation_profile != XR_NULL_HANDLE && pfn_xrDestroyFoveationProfileFB) {
        pfn_xrDestroyFoveationProfileFB(foveation_profile);
        foveation_profile = XR_NULL_HANDLE;
    }

    // Destroy passthrough objects before destroying the session.
    if (passthrough_active) stop_passthrough();

    if (passthrough_layer != XR_NULL_HANDLE && pfn_xrDestroyPassthroughLayerFB) {
        pfn_xrDestroyPassthroughLayerFB(passthrough_layer);
        passthrough_layer = XR_NULL_HANDLE;
    }
    if (passthrough_handle != XR_NULL_HANDLE && pfn_xrDestroyPassthroughFB) {
        pfn_xrDestroyPassthroughFB(passthrough_handle);
        passthrough_handle = XR_NULL_HANDLE;
    }

    for (auto& sc : swapchains)
        if (sc.handle != XR_NULL_HANDLE) xrDestroySwapchain(sc.handle);

    if (view_space  != XR_NULL_HANDLE) xrDestroySpace(view_space);
    if (stage_space != XR_NULL_HANDLE) xrDestroySpace(stage_space);

    if (action_set != XR_NULL_HANDLE) xrDestroyActionSet(action_set);

    if (session  != XR_NULL_HANDLE) xrDestroySession(session);
    if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);

    if (egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
        eglTerminate(egl_display);
    }
}
