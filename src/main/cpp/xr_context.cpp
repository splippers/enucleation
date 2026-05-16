#include "xr_context.h"
#include <android/log.h>
#include <cstring>
#include <stdexcept>

#define LOG_TAG "MonoView/XR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define XR_CHECK(expr) do { \
    XrResult _r = (expr); \
    if (XR_FAILED(_r)) { LOGE("XR_FAILED(%d) at %s:%d", _r, __FILE__, __LINE__); return false; } \
} while(0)

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

    const char* extensions[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };

    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM      = app->activity->vm;
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
    info.enabledExtensionCount   = 2;
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

    return true;
}

bool XrContext::attach_action_sets() {
    // Suggest bindings: A (right) and X (left) both cycle mode.
    XrPath a_click, x_click;
    xrStringToPath(instance, "/user/hand/right/input/a/click", &a_click);
    xrStringToPath(instance, "/user/hand/left/input/x/click",  &x_click);

    XrActionSuggestedBinding bindings[] = {
        {cycle_action, a_click},
        {cycle_action, x_click},
    };

    XrPath oculus_profile;
    xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &oculus_profile);

    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile     = oculus_profile;
    suggested.suggestedBindings      = bindings;
    suggested.countSuggestedBindings = 2;
    XR_CHECK(xrSuggestInteractionProfileBindings(instance, &suggested));

    XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach_info.actionSets      = &action_set;
    attach_info.countActionSets = 1;
    XR_CHECK(xrAttachSessionActionSets(session, &attach_info));

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
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space     = stage_space;
    layer.viewCount = 2;
    layer.views     = submitted_views;

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)
    };

    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime          = display_time;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount           = should_render ? 1 : 0;
    end_info.layers               = should_render ? layers : nullptr;

    xrEndFrame(session, &end_info);
}

bool XrContext::poll_cycle_button() const {
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

    // changedSinceLastSync + currentState means rising edge
    return state.isActive && state.changedSinceLastSync && state.currentState;
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------
void XrContext::destroy() {
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
