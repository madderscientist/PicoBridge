// PICO 4 Ultra 全量 XR 数据桥接：
//   OpenXR -> WebSocket -> PC 上的 server.py
//
// 采集内容：头显位姿、双眼视图、双手柄位姿与全部按键/摇杆/触摸/电量、
//           双手 26 关节手势、全身 24 关节动捕。
// 反向通道：服务端 POST /haptic 下发的震动指令会经同一条 WebSocket 回到这里。
//
// 运行时可覆盖的系统属性（改完重启应用即可，不用重新打包）：
//   adb shell setprop debug.pico.bridge_url ws://192.168.137.63:8000/ws/device
//   adb shell setprop debug.pico.bridge_ar  0     # 0=不透视(黑底) 1=AR透视(默认)

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <sys/system_properties.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "ws_client.h"
#include "renderer.h"
#include "vr_panel.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PicoBridge", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "PicoBridge", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PicoBridge", __VA_ARGS__)

#define XR_CHECK(expr)                                          \
    do {                                                        \
        XrResult _r = (expr);                                   \
        if (XR_FAILED(_r)) {                                    \
            LOGE("%s failed: %d", #expr, static_cast<int>(_r)); \
            return false;                                       \
        }                                                       \
    } while (0)

namespace {

constexpr int kLeft = 0;
constexpr int kRight = 1;

const char *const kBodyJointNames[XR_BODY_JOINT_COUNT_BD] = {
    "PELVIS",         "LEFT_HIP",     "RIGHT_HIP",     "SPINE1",      "LEFT_KNEE",
    "RIGHT_KNEE",     "SPINE2",       "LEFT_ANKLE",    "RIGHT_ANKLE", "SPINE3",
    "LEFT_FOOT",      "RIGHT_FOOT",   "NECK",          "LEFT_COLLAR", "RIGHT_COLLAR",
    "HEAD",           "LEFT_SHOULDER","RIGHT_SHOULDER","LEFT_ELBOW",  "RIGHT_ELBOW",
    "LEFT_WRIST",     "RIGHT_WRIST",  "LEFT_HAND",     "RIGHT_HAND"};

const char *const kHandJointNames[XR_HAND_JOINT_COUNT_EXT] = {
    "PALM",            "WRIST",
    "THUMB_METACARPAL","THUMB_PROXIMAL",   "THUMB_DISTAL",       "THUMB_TIP",
    "INDEX_METACARPAL","INDEX_PROXIMAL",   "INDEX_INTERMEDIATE", "INDEX_DISTAL",  "INDEX_TIP",
    "MIDDLE_METACARPAL","MIDDLE_PROXIMAL", "MIDDLE_INTERMEDIATE","MIDDLE_DISTAL", "MIDDLE_TIP",
    "RING_METACARPAL", "RING_PROXIMAL",    "RING_INTERMEDIATE",  "RING_DISTAL",   "RING_TIP",
    "LITTLE_METACARPAL","LITTLE_PROXIMAL", "LITTLE_INTERMEDIATE","LITTLE_DISTAL", "LITTLE_TIP"};

// SMPL 运动树：每个关节的父关节，用来连骨架线（-1 表示根）
const int kBodyParent[XR_BODY_JOINT_COUNT_BD] = {-1, 0, 0,  3,  1,  2,  6,  4,
                                                 5,  9, 7,  8,  9,  9,  9, 12,
                                                 13, 14, 16, 17, 18, 19, 20, 21};

std::string systemProp(const char *key, const std::string &fallback) {
    char buf[PROP_VALUE_MAX] = {};
    int n = __system_property_get(key, buf);
    return (n > 0) ? std::string(buf, static_cast<size_t>(n)) : fallback;
}

// 地址优先级：2D 配置页写的文件 > 系统属性 > 编译期默认值
std::string serverUrl(android_app *app) {
    if (app->activity->internalDataPath != nullptr) {
        const std::string path = std::string(app->activity->internalDataPath) + "/server_url.txt";
        if (FILE *f = std::fopen(path.c_str(), "rb")) {
            char buf[256] = {};
            const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            std::string url(buf, n);
            while (!url.empty() && (url.back() == '\n' || url.back() == '\r' || url.back() == ' ')) {
                url.pop_back();
            }
            if (!url.empty()) return url;
        }
    }
    return systemProp("debug.pico.bridge_url", "ws://192.168.137.63:8000/ws/device");
}

void saveServerUrl(const std::string &dataPath, const std::string &url) {
    if (dataPath.empty()) return;
    const std::string path = dataPath + "/server_url.txt";
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(url.data(), 1, url.size(), f);
        std::fclose(f);
    }
}

void appendPose(std::string &out, const XrPosef &pose, XrSpaceLocationFlags flags) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"position\":[%.6f,%.6f,%.6f],\"orientation\":[%.6f,%.6f,%.6f,%.6f],"
                  "\"position_valid\":%s,\"orientation_valid\":%s}",
                  pose.position.x, pose.position.y, pose.position.z, pose.orientation.x,
                  pose.orientation.y, pose.orientation.z, pose.orientation.w,
                  (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? "true" : "false",
                  (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ? "true" : "false");
    out += buf;
}

// 只够解析 {"type":"haptic","hand":"right","intensity":0.6,"duration":80} 这种扁平消息
bool jsonString(const std::string &s, const char *key, std::string &out) {
    const std::string pat = "\"" + std::string(key) + "\"";
    size_t k = s.find(pat);
    if (k == std::string::npos) return false;
    size_t q1 = s.find('"', s.find(':', k + pat.size()) + 1);
    if (q1 == std::string::npos) return false;
    size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    out = s.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

bool jsonNumber(const std::string &s, const char *key, double &out) {
    const std::string pat = "\"" + std::string(key) + "\"";
    size_t k = s.find(pat);
    if (k == std::string::npos) return false;
    size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    out = std::strtod(s.c_str() + colon + 1, nullptr);
    return true;
}

struct AppState {
    bool resumed = false;
};

void handleAppCmd(android_app *app, int32_t cmd) {
    auto *state = static_cast<AppState *>(app->userData);
    if (cmd == APP_CMD_RESUME) state->resumed = true;
    if (cmd == APP_CMD_PAUSE) state->resumed = false;
}

class Bridge {
public:
    bool init(android_app *app);
    void shutdown();
    void pollXrEvents(bool *exitLoop);
    void tick();
    bool sessionRunning() const { return sessionRunning_; }

private:
    bool initEgl();
    bool createInstance(android_app *app);
    bool createSession();
    void createPassthrough();
    bool createActions();
    void applyPendingHaptic();
    void onServerMessage(const std::string &text);

    void appendControllers(std::string &json, XrTime t);
    void appendHands(std::string &json, XrTime t);
    void appendBody(std::string &json, XrTime t);
    void appendViews(std::string &json);
    void buildFrameJson(const XrFrameState &frameState, std::string &out);

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrReferenceSpaceType refSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
    const char *refSpaceName() const {
        switch (refSpaceType_) {
            case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR: return "LOCAL_FLOOR";
            case XR_REFERENCE_SPACE_TYPE_STAGE: return "STAGE";
            case XR_REFERENCE_SPACE_TYPE_LOCAL: return "LOCAL";
            default: return "OTHER";
        }
    }
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    bool focused_ = false;

    XrEnvironmentBlendMode blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    uint32_t viewCount_ = 0;

    // AR 透视（PICO 实现的是 Meta 那套 XR_FB_passthrough）
    bool wantAr_ = true;
    bool hasPassthroughExt_ = false;
    bool supportsPassthrough_ = false;
    XrPassthroughFB passthrough_ = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer_ = XR_NULL_HANDLE;
    PFN_xrCreatePassthroughFB pfnCreatePassthrough_ = nullptr;
    PFN_xrDestroyPassthroughFB pfnDestroyPassthrough_ = nullptr;
    PFN_xrPassthroughStartFB pfnPassthroughStart_ = nullptr;
    PFN_xrCreatePassthroughLayerFB pfnCreatePassthroughLayer_ = nullptr;
    PFN_xrDestroyPassthroughLayerFB pfnDestroyPassthroughLayer_ = nullptr;
    PFN_xrPassthroughLayerResumeFB pfnPassthroughLayerResume_ = nullptr;

    // 输入
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrPath handPath_[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace gripSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aimSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrAction aGripPose_ = XR_NULL_HANDLE, aAimPose_ = XR_NULL_HANDLE;
    XrAction aTrigger_ = XR_NULL_HANDLE, aTriggerClick_ = XR_NULL_HANDLE, aTriggerTouch_ = XR_NULL_HANDLE;
    XrAction aSqueeze_ = XR_NULL_HANDLE, aSqueezeClick_ = XR_NULL_HANDLE;
    XrAction aStick_ = XR_NULL_HANDLE, aStickClick_ = XR_NULL_HANDLE, aStickTouch_ = XR_NULL_HANDLE;
    XrAction aPrimary_ = XR_NULL_HANDLE, aPrimaryTouch_ = XR_NULL_HANDLE;
    XrAction aSecondary_ = XR_NULL_HANDLE, aSecondaryTouch_ = XR_NULL_HANDLE;
    XrAction aMenu_ = XR_NULL_HANDLE, aBattery_ = XR_NULL_HANDLE, aHaptic_ = XR_NULL_HANDLE;

    // 眼动：注视射线同样走 action 系统，但不带 subaction path
    bool hasEyeGazeExt_ = false;
    bool supportsEyeGaze_ = false;
    XrAction aEyeGaze_ = XR_NULL_HANDLE;
    XrSpace eyeGazeSpace_ = XR_NULL_HANDLE;

    // 手势
    bool hasHandTracking_ = false;
    XrHandTrackerEXT handTracker_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    PFN_xrCreateHandTrackerEXT pfnCreateHandTracker_ = nullptr;
    PFN_xrDestroyHandTrackerEXT pfnDestroyHandTracker_ = nullptr;
    PFN_xrLocateHandJointsEXT pfnLocateHandJoints_ = nullptr;

    // 全身动捕
    XrBodyTrackerBD bodyTracker_ = XR_NULL_HANDLE;
    PFN_xrCreateBodyTrackerBD pfnCreateBodyTracker_ = nullptr;
    PFN_xrDestroyBodyTrackerBD pfnDestroyBodyTracker_ = nullptr;
    PFN_xrLocateBodyJointsBD pfnLocateBodyJoints_ = nullptr;
    PFN_xrGetBodyTrackingStatePICO pfnGetBodyTrackingState_ = nullptr;
    PFN_xrStartBodyTrackingCalibrationAppPICO pfnStartCalibApp_ = nullptr;
    bool hasBodyTracking2_ = false;
    bool supportsBodyTracking_ = false;
    bool calibrationRequested_ = false;
    int bodyStatus_ = -1;
    int bodyMessage_ = -1;
    int stateProbeCountdown_ = 0;

    // EGL：只为满足建会话时的图形绑定要求
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLConfig eglConfig_ = nullptr;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;

    WsClient ws_;
    uint64_t seq_ = 0;
    std::string jsonBuf_;  // 复用缓冲区，避免每帧 48KB 的堆分配

    Renderer renderer_;
    bool wantDraw_ = true;
    std::vector<XrView> views_;
    bool viewsValid_ = false;

    VrPanel panel_;
    bool lastMenu_ = false;
    bool awaitingConnect_ = false;
    std::string dataPath_;

    std::mutex hapticMtx_;
    bool hapticPending_ = false;
    int hapticHand_ = kRight;
    float hapticAmplitude_ = 0.6f;
    float hapticDurationMs_ = 80.0f;
};

bool Bridge::initEgl() {
    eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay_ == EGL_NO_DISPLAY || !eglInitialize(eglDisplay_, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        return false;
    }
    const EGLint cfgAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                 EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
                                 EGL_RED_SIZE,        8,
                                 EGL_GREEN_SIZE,      8,
                                 EGL_BLUE_SIZE,       8,
                                 EGL_ALPHA_SIZE,      8,
                                 EGL_NONE};
    EGLint numConfigs = 0;
    if (!eglChooseConfig(eglDisplay_, cfgAttribs, &eglConfig_, 1, &numConfigs) || numConfigs < 1) {
        LOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, ctxAttribs);
    if (eglContext_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return false;
    }
    const EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbAttribs);
    if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
        LOGE("eglMakeCurrent failed");
        return false;
    }
    return true;
}

bool Bridge::createInstance(android_app *app) {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                           reinterpret_cast<PFN_xrVoidFunction *>(&initializeLoader)))) {
        XrLoaderInitInfoAndroidKHR loaderInit{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInit.applicationVM = app->activity->vm;
        loaderInit.applicationContext = app->activity->clazz;
        initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loaderInit));
    }

    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> available(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, available.data());
    auto has = [&available](const char *name) {
        for (const auto &e : available) {
            if (std::strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };

    std::vector<const char *> enabled{XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                      XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
    if (!has(XR_BD_BODY_TRACKING_EXTENSION_NAME)) {
        LOGE("runtime does not expose %s", XR_BD_BODY_TRACKING_EXTENSION_NAME);
        return false;
    }
    enabled.push_back(XR_BD_BODY_TRACKING_EXTENSION_NAME);
    hasBodyTracking2_ = has(XR_PICO_BODY_TRACKING2_EXTENSION_NAME);
    if (hasBodyTracking2_) enabled.push_back(XR_PICO_BODY_TRACKING2_EXTENSION_NAME);
    hasHandTracking_ = has(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if (hasHandTracking_) enabled.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    hasEyeGazeExt_ = has(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    if (hasEyeGazeExt_) enabled.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    // LOCAL_FLOOR 在 OpenXR 1.0 下需要这个扩展；它等价于 WebXR 的 local-floor
    if (has(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME)) {
        enabled.push_back(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
    }
    if (has(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME)) {
        enabled.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
    }
    wantAr_ = systemProp("debug.pico.bridge_ar", "1") != "0";
    hasPassthroughExt_ = has(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (wantAr_ && hasPassthroughExt_) enabled.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    LOGI("ext: body_tracking2=%d hand_tracking=%d passthrough=%d eye_gaze=%d",
         static_cast<int>(hasBodyTracking2_), static_cast<int>(hasHandTracking_),
         static_cast<int>(hasPassthroughExt_), static_cast<int>(hasEyeGazeExt_));

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = app->activity->vm;
    androidInfo.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    createInfo.enabledExtensionNames = enabled.data();
    std::strcpy(createInfo.applicationInfo.applicationName, "PicoBridge");
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XR_CHECK(xrCreateInstance(&createInfo, &instance_));

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(instance_, &systemInfo, &systemId_));

    XrSystemPassthroughPropertiesFB passthroughProps{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB};
    XrSystemEyeGazeInteractionPropertiesEXT eyeProps{
        XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    passthroughProps.next = &eyeProps;
    XrSystemBodyTrackingPropertiesBD bodyProps{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_BD};
    bodyProps.next = &passthroughProps;
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    props.next = &bodyProps;
    XR_CHECK(xrGetSystemProperties(instance_, systemId_, &props));
    supportsBodyTracking_ = bodyProps.supportsBodyTracking == XR_TRUE;
    supportsPassthrough_ = passthroughProps.supportsPassthrough == XR_TRUE;
    supportsEyeGaze_ = hasEyeGazeExt_ && eyeProps.supportsEyeGazeInteraction == XR_TRUE;
    LOGI("system '%s', supportsBodyTracking=%d supportsPassthrough=%d supportsEyeGaze=%d",
         props.systemName, static_cast<int>(supportsBodyTracking_),
         static_cast<int>(supportsPassthrough_), static_cast<int>(supportsEyeGaze_));

    // 用 XR_FB_passthrough 时画面来自透视图层，混合模式保持 OPAQUE；
    // 拿不到该扩展才退而试 ALPHA_BLEND。
    uint32_t blendCount = 0;
    xrEnumerateEnvironmentBlendModes(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                     0, &blendCount, nullptr);
    std::vector<XrEnvironmentBlendMode> blendModes(blendCount);
    xrEnumerateEnvironmentBlendModes(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                     blendCount, &blendCount, blendModes.data());
    for (auto m : blendModes) {
        LOGI("environment blend mode available: %d", static_cast<int>(m));
        if (wantAr_ && !hasPassthroughExt_ && m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
            blendMode_ = m;
        }
    }
    LOGI("blend mode %d, passthrough layer %s", static_cast<int>(blendMode_),
         (wantAr_ && hasPassthroughExt_) ? "enabled" : "disabled");

    xrEnumerateViewConfigurationViews(instance_, systemId_,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount_,
                                      nullptr);

    xrGetInstanceProcAddr(instance_, "xrCreateBodyTrackerBD",
                          reinterpret_cast<PFN_xrVoidFunction *>(&pfnCreateBodyTracker_));
    xrGetInstanceProcAddr(instance_, "xrDestroyBodyTrackerBD",
                          reinterpret_cast<PFN_xrVoidFunction *>(&pfnDestroyBodyTracker_));
    xrGetInstanceProcAddr(instance_, "xrLocateBodyJointsBD",
                          reinterpret_cast<PFN_xrVoidFunction *>(&pfnLocateBodyJoints_));
    if (hasBodyTracking2_) {
        xrGetInstanceProcAddr(instance_, "xrGetBodyTrackingStatePICO",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetBodyTrackingState_));
        xrGetInstanceProcAddr(instance_, "xrStartBodyTrackingCalibrationAppPICO",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnStartCalibApp_));
    }
    if (hasHandTracking_) {
        xrGetInstanceProcAddr(instance_, "xrCreateHandTrackerEXT",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnCreateHandTracker_));
        xrGetInstanceProcAddr(instance_, "xrDestroyHandTrackerEXT",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnDestroyHandTracker_));
        xrGetInstanceProcAddr(instance_, "xrLocateHandJointsEXT",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnLocateHandJoints_));
        hasHandTracking_ = pfnCreateHandTracker_ != nullptr && pfnLocateHandJoints_ != nullptr;
    }
    if (pfnCreateBodyTracker_ == nullptr || pfnLocateBodyJoints_ == nullptr) {
        LOGE("body tracking entry points missing");
        return false;
    }
    if (wantAr_ && hasPassthroughExt_) {
        xrGetInstanceProcAddr(instance_, "xrCreatePassthroughFB",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnCreatePassthrough_));
        xrGetInstanceProcAddr(instance_, "xrDestroyPassthroughFB",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnDestroyPassthrough_));
        xrGetInstanceProcAddr(instance_, "xrPassthroughStartFB",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnPassthroughStart_));
        xrGetInstanceProcAddr(instance_, "xrCreatePassthroughLayerFB",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnCreatePassthroughLayer_));
        xrGetInstanceProcAddr(instance_, "xrDestroyPassthroughLayerFB",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnDestroyPassthroughLayer_));
        xrGetInstanceProcAddr(instance_, "xrPassthroughLayerResumeFB",
                              reinterpret_cast<PFN_xrVoidFunction *>(&pfnPassthroughLayerResume_));
    }
    return true;
}

bool Bridge::createActions() {
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "gameplay");
    std::strcpy(asci.localizedActionSetName, "Gameplay");
    XR_CHECK(xrCreateActionSet(instance_, &asci, &actionSet_));

    xrStringToPath(instance_, "/user/hand/left", &handPath_[kLeft]);
    xrStringToPath(instance_, "/user/hand/right", &handPath_[kRight]);

    auto makeAction = [&](const char *name, XrActionType type, XrAction *out) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = type;
        std::strcpy(aci.actionName, name);
        std::strcpy(aci.localizedActionName, name);
        aci.countSubactionPaths = 2;
        aci.subactionPaths = handPath_;
        return XR_SUCCEEDED(xrCreateAction(actionSet_, &aci, out));
    };

    makeAction("grip_pose", XR_ACTION_TYPE_POSE_INPUT, &aGripPose_);
    makeAction("aim_pose", XR_ACTION_TYPE_POSE_INPUT, &aAimPose_);
    makeAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT, &aTrigger_);
    makeAction("trigger_click", XR_ACTION_TYPE_BOOLEAN_INPUT, &aTriggerClick_);
    makeAction("trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &aTriggerTouch_);
    makeAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, &aSqueeze_);
    makeAction("squeeze_click", XR_ACTION_TYPE_BOOLEAN_INPUT, &aSqueezeClick_);
    makeAction("thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, &aStick_);
    makeAction("thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, &aStickClick_);
    makeAction("thumbstick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &aStickTouch_);
    makeAction("primary_button", XR_ACTION_TYPE_BOOLEAN_INPUT, &aPrimary_);
    makeAction("primary_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &aPrimaryTouch_);
    makeAction("secondary_button", XR_ACTION_TYPE_BOOLEAN_INPUT, &aSecondary_);
    makeAction("secondary_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &aSecondaryTouch_);
    makeAction("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, &aMenu_);
    makeAction("battery", XR_ACTION_TYPE_FLOAT_INPUT, &aBattery_);
    makeAction("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, &aHaptic_);

    // 注视射线是全局的，不能带 subaction path，否则 xrCreateAction 会失败
    if (supportsEyeGaze_) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
        std::strcpy(aci.actionName, "eye_gaze");
        std::strcpy(aci.localizedActionName, "Eye Gaze");
        if (XR_FAILED(xrCreateAction(actionSet_, &aci, &aEyeGaze_))) {
            LOGW("eye gaze action creation failed");
            aEyeGaze_ = XR_NULL_HANDLE;
        }
    }

    // 整套建议绑定是原子生效的：只要有一条路径该 profile 不认，整包都会被拒。
    // 所以先试全量，失败再退到核心集合。
    auto suggest = [&](const char *profile, bool full) {
        std::vector<XrActionSuggestedBinding> bindings;
        auto bind = [&](XrAction action, const char *path) {
            XrPath p = XR_NULL_PATH;
            if (XR_SUCCEEDED(xrStringToPath(instance_, path, &p))) bindings.push_back({action, p});
        };
        for (const char *side : {"left", "right"}) {
            const std::string s = std::string("/user/hand/") + side;
            bind(aGripPose_, (s + "/input/grip/pose").c_str());
            bind(aAimPose_, (s + "/input/aim/pose").c_str());
            bind(aTrigger_, (s + "/input/trigger/value").c_str());
            bind(aSqueeze_, (s + "/input/squeeze/value").c_str());
            bind(aStick_, (s + "/input/thumbstick").c_str());
            bind(aStickClick_, (s + "/input/thumbstick/click").c_str());
            bind(aHaptic_, (s + "/output/haptic").c_str());
            if (full) {
                bind(aTriggerClick_, (s + "/input/trigger/click").c_str());
                bind(aTriggerTouch_, (s + "/input/trigger/touch").c_str());
                bind(aSqueezeClick_, (s + "/input/squeeze/click").c_str());
                bind(aStickTouch_, (s + "/input/thumbstick/touch").c_str());
                bind(aBattery_, (s + "/input/battery/value").c_str());
            }
        }
        // A/B 只在右手，X/Y 只在左手
        bind(aPrimary_, "/user/hand/left/input/x/click");
        bind(aSecondary_, "/user/hand/left/input/y/click");
        bind(aPrimary_, "/user/hand/right/input/a/click");
        bind(aSecondary_, "/user/hand/right/input/b/click");
        bind(aMenu_, "/user/hand/left/input/menu/click");
        if (full) {
            bind(aPrimaryTouch_, "/user/hand/left/input/x/touch");
            bind(aSecondaryTouch_, "/user/hand/left/input/y/touch");
            bind(aPrimaryTouch_, "/user/hand/right/input/a/touch");
            bind(aSecondaryTouch_, "/user/hand/right/input/b/touch");
        }

        XrPath profilePath = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(instance_, profile, &profilePath))) return false;
        XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestion.interactionProfile = profilePath;
        suggestion.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggestion.suggestedBindings = bindings.data();
        return XR_SUCCEEDED(xrSuggestInteractionProfileBindings(instance_, &suggestion));
    };

    for (const char *profile : {"/interaction_profiles/bytedance/pico4s_controller",
                                "/interaction_profiles/bytedance/pico4_controller",
                                "/interaction_profiles/bytedance/pico_neo3_controller"}) {
        const bool ok = suggest(profile, true) || suggest(profile, false);
        LOGI("suggest bindings %s -> %d", profile, static_cast<int>(ok));
    }

    if (aEyeGaze_ != XR_NULL_HANDLE) {
        XrPath profilePath = XR_NULL_PATH, gazePath = XR_NULL_PATH;
        xrStringToPath(instance_, "/interaction_profiles/ext/eye_gaze_interaction", &profilePath);
        xrStringToPath(instance_, "/user/eyes_ext/input/gaze_ext/pose", &gazePath);
        XrActionSuggestedBinding binding{aEyeGaze_, gazePath};
        XrInteractionProfileSuggestedBinding suggestion{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestion.interactionProfile = profilePath;
        suggestion.countSuggestedBindings = 1;
        suggestion.suggestedBindings = &binding;
        const bool ok = XR_SUCCEEDED(xrSuggestInteractionProfileBindings(instance_, &suggestion));
        LOGI("suggest bindings eye_gaze_interaction -> %d", static_cast<int>(ok));
        if (!ok) aEyeGaze_ = XR_NULL_HANDLE;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    XR_CHECK(xrAttachSessionActionSets(session_, &attach));

    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asi.poseInActionSpace.orientation.w = 1.0f;
        asi.subactionPath = handPath_[i];
        asi.action = aGripPose_;
        xrCreateActionSpace(session_, &asi, &gripSpace_[i]);
        asi.action = aAimPose_;
        xrCreateActionSpace(session_, &asi, &aimSpace_[i]);
    }

    if (aEyeGaze_ != XR_NULL_HANDLE) {
        XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asi.poseInActionSpace.orientation.w = 1.0f;
        asi.action = aEyeGaze_;
        if (XR_FAILED(xrCreateActionSpace(session_, &asi, &eyeGazeSpace_))) {
            LOGW("eye gaze action space creation failed");
            eyeGazeSpace_ = XR_NULL_HANDLE;
        }
    }
    return true;
}

bool Bridge::createSession() {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGraphicsRequirements = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
                                   reinterpret_cast<PFN_xrVoidFunction *>(&pfnGraphicsRequirements)));
    XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK(pfnGraphicsRequirements(instance_, systemId_, &graphicsRequirements));

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = eglDisplay_;
    binding.config = eglConfig_;
    binding.context = eglContext_;

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId_;
    XR_CHECK(xrCreateSession(instance_, &sessionInfo, &session_));

    uint32_t spaceCount = 0;
    xrEnumerateReferenceSpaces(session_, 0, &spaceCount, nullptr);
    std::vector<XrReferenceSpaceType> spaces(spaceCount);
    xrEnumerateReferenceSpaces(session_, spaceCount, &spaceCount, spaces.data());
    auto hasSpace = [&spaces](XrReferenceSpaceType s) {
        for (auto v : spaces) {
            if (v == s) return true;
        }
        return false;
    };
    for (auto s : spaces) LOGI("reference space available: %d", static_cast<int>(s));

    // 默认用 LOCAL_FLOOR：地面高度原点，且长按 Home 重置视角时会跟着归位。
    // STAGE 的原点绑在安全区物理中心上，重置视角对它无效。
    const std::string want = systemProp("debug.pico.bridge_space", "local_floor");
    if (want == "stage" && hasSpace(XR_REFERENCE_SPACE_TYPE_STAGE)) {
        refSpaceType_ = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (want == "local") {
        refSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
    } else if (hasSpace(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR)) {
        refSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
    } else if (hasSpace(XR_REFERENCE_SPACE_TYPE_STAGE)) {
        refSpaceType_ = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else {
        refSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
    }
    LOGI("using reference space %d (%s)", static_cast<int>(refSpaceType_), refSpaceName());

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = refSpaceType_;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateReferenceSpace(session_, &spaceInfo, &appSpace_));
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(session_, &spaceInfo, &viewSpace_));

    createPassthrough();

    XrBodyTrackerCreateInfoBD trackerInfo{XR_TYPE_BODY_TRACKER_CREATE_INFO_BD};
    trackerInfo.jointSet = XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD;
    XR_CHECK(pfnCreateBodyTracker_(session_, &trackerInfo, &bodyTracker_));

    if (hasHandTracking_) {
        for (int i = 0; i < 2; ++i) {
            XrHandTrackerCreateInfoEXT hci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            hci.hand = (i == kLeft) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            hci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            if (XR_FAILED(pfnCreateHandTracker_(session_, &hci, &handTracker_[i]))) {
                LOGW("hand tracker %d creation failed", i);
                handTracker_[i] = XR_NULL_HANDLE;
            }
        }
    }

    return createActions();
}

void Bridge::createPassthrough() {
    if (pfnCreatePassthrough_ == nullptr || pfnCreatePassthroughLayer_ == nullptr) return;
    XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    pci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    // 这个调用在 PICO 的透视服务被拖死时会永不返回，只能重启头显恢复
    XrResult r = pfnCreatePassthrough_(session_, &pci, &passthrough_);
    if (XR_FAILED(r)) {
        LOGE("xrCreatePassthroughFB failed: %d", static_cast<int>(r));
        passthrough_ = XR_NULL_HANDLE;
        return;
    }
    XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    lci.passthrough = passthrough_;
    lci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    r = pfnCreatePassthroughLayer_(session_, &lci, &passthroughLayer_);
    if (XR_FAILED(r)) {
        LOGE("xrCreatePassthroughLayerFB failed: %d", static_cast<int>(r));
        passthroughLayer_ = XR_NULL_HANDLE;
    }
    if (pfnPassthroughStart_ != nullptr) pfnPassthroughStart_(passthrough_);
    if (pfnPassthroughLayerResume_ != nullptr && passthroughLayer_ != XR_NULL_HANDLE) {
        pfnPassthroughLayerResume_(passthroughLayer_);
    }
    LOGI("passthrough layer %s", passthroughLayer_ != XR_NULL_HANDLE ? "ready" : "unavailable");
}

bool Bridge::init(android_app *app) {
    if (!initEgl()) return false;
    if (!createInstance(app)) return false;
    if (!createSession()) return false;

    wantDraw_ = systemProp("debug.pico.bridge_draw", "1") != "0";
    if (wantDraw_ && !renderer_.init(instance_, systemId_, session_)) {
        LOGW("renderer init failed, continuing without overlay");
    }

    const std::string url = serverUrl(app);
    if (app->activity->internalDataPath != nullptr) dataPath_ = app->activity->internalDataPath;
    if (wantDraw_) {
        // 从 ws://host/path 里剥出 host 部分喂给面板
        std::string host = url;
        const std::string prefix = "ws://";
        if (host.compare(0, prefix.size(), prefix) == 0) host = host.substr(prefix.size());
        const size_t slash = host.find('/');
        if (slash != std::string::npos) host = host.substr(0, slash);
        if (!panel_.init(app, appSpace_, host, &renderer_)) {
            LOGW("vr panel init failed");
        }
    }

    ws_.setOnText([this](const std::string &text) { onServerMessage(text); });
    ws_.start(url);
    // 记住的地址能连上就自动收起面板；连不上则留在眼前等用户改
    awaitingConnect_ = true;
    return true;
}

void Bridge::shutdown() {
    ws_.stop();
    panel_.destroy();
    renderer_.destroy();
    if (passthroughLayer_ != XR_NULL_HANDLE && pfnDestroyPassthroughLayer_ != nullptr) {
        pfnDestroyPassthroughLayer_(passthroughLayer_);
    }
    if (passthrough_ != XR_NULL_HANDLE && pfnDestroyPassthrough_ != nullptr) {
        pfnDestroyPassthrough_(passthrough_);
    }
    for (int i = 0; i < 2; ++i) {
        if (handTracker_[i] != XR_NULL_HANDLE && pfnDestroyHandTracker_ != nullptr) {
            pfnDestroyHandTracker_(handTracker_[i]);
        }
        if (gripSpace_[i] != XR_NULL_HANDLE) xrDestroySpace(gripSpace_[i]);
        if (aimSpace_[i] != XR_NULL_HANDLE) xrDestroySpace(aimSpace_[i]);
    }
    if (bodyTracker_ != XR_NULL_HANDLE && pfnDestroyBodyTracker_ != nullptr) {
        pfnDestroyBodyTracker_(bodyTracker_);
    }
    if (actionSet_ != XR_NULL_HANDLE) xrDestroyActionSet(actionSet_);
    if (eyeGazeSpace_ != XR_NULL_HANDLE) xrDestroySpace(eyeGazeSpace_);
    if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
    if (appSpace_ != XR_NULL_HANDLE) xrDestroySpace(appSpace_);
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglSurface_ != EGL_NO_SURFACE) eglDestroySurface(eglDisplay_, eglSurface_);
        if (eglContext_ != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay_, eglContext_);
        eglTerminate(eglDisplay_);
    }
}

void Bridge::onServerMessage(const std::string &text) {
    std::string type;
    if (!jsonString(text, "type", type) || type != "haptic") return;
    std::string hand;
    jsonString(text, "hand", hand);
    double intensity = 0.6, duration = 80.0;
    jsonNumber(text, "intensity", intensity);
    jsonNumber(text, "duration", duration);

    std::lock_guard<std::mutex> lock(hapticMtx_);
    hapticPending_ = true;
    hapticHand_ = (hand == "left") ? kLeft : kRight;
    hapticAmplitude_ = static_cast<float>(intensity);
    hapticDurationMs_ = static_cast<float>(duration);
}

void Bridge::applyPendingHaptic() {
    int hand;
    float amplitude, durationMs;
    {
        std::lock_guard<std::mutex> lock(hapticMtx_);
        if (!hapticPending_) return;
        hapticPending_ = false;
        hand = hapticHand_;
        amplitude = hapticAmplitude_;
        durationMs = hapticDurationMs_;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = (amplitude < 0.0f) ? 0.0f : (amplitude > 1.0f ? 1.0f : amplitude);
    vibration.duration = static_cast<XrDuration>(durationMs * 1e6);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = aHaptic_;
    info.subactionPath = handPath_[hand];
    xrApplyHapticFeedback(session_, &info, reinterpret_cast<XrHapticBaseHeader *>(&vibration));
}

void Bridge::pollXrEvents(bool *exitLoop) {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (true) {
        event = {XR_TYPE_EVENT_DATA_BUFFER};
        if (xrPollEvent(instance_, &event) != XR_SUCCESS) break;

        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto &changed = *reinterpret_cast<XrEventDataSessionStateChanged *>(&event);
            sessionState_ = changed.state;
            focused_ = (sessionState_ == XR_SESSION_STATE_FOCUSED);
            LOGI("session state -> %d", static_cast<int>(sessionState_));
            if (sessionState_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(session_, &beginInfo))) sessionRunning_ = true;
            } else if (sessionState_ == XR_SESSION_STATE_STOPPING) {
                sessionRunning_ = false;
                xrEndSession(session_);
            } else if (sessionState_ == XR_SESSION_STATE_EXITING ||
                       sessionState_ == XR_SESSION_STATE_LOSS_PENDING) {
                sessionRunning_ = false;
                *exitLoop = true;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            *exitLoop = true;
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // 长按 Home 重置视角会走到这里；XrSpace 句柄继续有效，原点由运行时切换
            const auto &changed =
                *reinterpret_cast<XrEventDataReferenceSpaceChangePending *>(&event);
            LOGI("reference space %d recentered", static_cast<int>(changed.referenceSpaceType));
        } else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            for (int i = 0; i < 2; ++i) {
                XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session_, handPath_[i], &state))) {
                    char name[XR_MAX_PATH_LENGTH] = {};
                    uint32_t len = 0;
                    if (state.interactionProfile != XR_NULL_PATH &&
                        XR_SUCCEEDED(xrPathToString(instance_, state.interactionProfile,
                                                    sizeof(name), &len, name))) {
                        LOGI("hand %d interaction profile: %s", i, name);
                    }
                }
            }
        }
    }
}

void Bridge::appendControllers(std::string &json, XrTime t) {
    char buf[512];
    auto boolState = [&](XrAction action, int hand) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = action;
        gi.subactionPath = handPath_[hand];
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(session_, &gi, &st);
        return st.currentState == XR_TRUE;
    };
    auto floatState = [&](XrAction action, int hand) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = action;
        gi.subactionPath = handPath_[hand];
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        xrGetActionStateFloat(session_, &gi, &st);
        return st.currentState;
    };

    for (int hand = 0; hand < 2; ++hand) {
        const char *name = (hand == kLeft) ? "left" : "right";
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = aGripPose_;
        gi.subactionPath = handPath_[hand];
        XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
        xrGetActionStatePose(session_, &gi, &poseState);

        XrActionStateGetInfo si{XR_TYPE_ACTION_STATE_GET_INFO};
        si.action = aStick_;
        si.subactionPath = handPath_[hand];
        XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(session_, &si, &stick);

        std::snprintf(buf, sizeof(buf),
                      "\"%s\":{\"id\":\"%s\",\"handedness\":\"%s\","
                      "\"target_ray_mode\":\"tracked-pointer\",\"connected\":%s,\"grip\":",
                      name, name, name, poseState.isActive == XR_TRUE ? "true" : "false");
        json += buf;

        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(gripSpace_[hand], appSpace_, t, &loc))) {
            appendPose(json, loc.pose, loc.locationFlags);
            if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
                renderer_.addAxes(loc.pose, 0.12f);
            }
        } else {
            json += "null";
        }
        json += ",\"aim\":";
        loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(aimSpace_[hand], appSpace_, t, &loc))) {
            appendPose(json, loc.pose, loc.locationFlags);
        } else {
            json += "null";
        }

        std::snprintf(buf, sizeof(buf),
                      ",\"buttons\":{\"trigger\":%.4f,\"squeeze\":%.4f,\"trigger_click\":%s,"
                      "\"squeeze_click\":%s,\"thumbstick_pressed\":%s,\"a_x\":%s,\"b_y\":%s,"
                      "\"menu\":%s},\"touches\":{\"trigger\":%s,\"thumbstick\":%s,\"a_x\":%s,"
                      "\"b_y\":%s},\"thumbstick\":[%.4f,%.4f],\"battery\":%.3f}",
                      floatState(aTrigger_, hand), floatState(aSqueeze_, hand),
                      boolState(aTriggerClick_, hand) ? "true" : "false",
                      boolState(aSqueezeClick_, hand) ? "true" : "false",
                      boolState(aStickClick_, hand) ? "true" : "false",
                      boolState(aPrimary_, hand) ? "true" : "false",
                      boolState(aSecondary_, hand) ? "true" : "false",
                      boolState(aMenu_, hand) ? "true" : "false",
                      boolState(aTriggerTouch_, hand) ? "true" : "false",
                      boolState(aStickTouch_, hand) ? "true" : "false",
                      boolState(aPrimaryTouch_, hand) ? "true" : "false",
                      boolState(aSecondaryTouch_, hand) ? "true" : "false",
                      stick.currentState.x, stick.currentState.y, floatState(aBattery_, hand));
        json += buf;
        json += ',';
    }
}

void Bridge::appendHands(std::string &json, XrTime t) {
    json += "\"hands\":{";
    for (int hand = 0; hand < 2; ++hand) {
        if (hand > 0) json += ',';
        json += (hand == kLeft) ? "\"left\":" : "\"right\":";
        if (!hasHandTracking_ || handTracker_[hand] == XR_NULL_HANDLE) {
            json += "null";
            continue;
        }
        XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT] = {};
        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locations.jointLocations = joints;
        XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        info.baseSpace = appSpace_;
        info.time = t;
        if (XR_FAILED(pfnLocateHandJoints_(handTracker_[hand], &info, &locations))) {
            json += "null";
            continue;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "{\"active\":%s,\"joint_count\":%u,\"joints\":{",
                      locations.isActive == XR_TRUE ? "true" : "false", locations.jointCount);
        json += buf;
        // 手柄在用时 PICO 会关掉手势追踪，此时 26 个关节全是无效值，
        // 再序列化白白占掉一半带宽（约 7.8KB/帧）
        if (locations.isActive != XR_TRUE) {
            json += "}}";
            continue;
        }
        for (uint32_t i = 0; i < XR_HAND_JOINT_COUNT_EXT; ++i) {
            if (i > 0) json += ',';
            json += '"';
            json += kHandJointNames[i];
            json += "\":";
            appendPose(json, joints[i].pose, joints[i].locationFlags);
            json.pop_back();
            std::snprintf(buf, sizeof(buf), ",\"radius\":%.5f}", joints[i].radius);
            json += buf;
            if (joints[i].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
                renderer_.addCross(joints[i].pose.position, 0.008f, 1.0f, 0.85f, 0.2f);
            }
        }
        json += "}}";
    }
    json += "},";
}

void Bridge::appendBody(std::string &json, XrTime t) {
    XrBodyJointLocationBD joints[XR_BODY_JOINT_COUNT_BD] = {};

    // 注意：不要往 XrBodyJointLocationsBD.next 挂 XrBodyJointVelocitiesPICO，
    // PICO 运行时按 XrBodyTrackingPostureFlagsDataPICO 的布局写这个指针，会踩坏内存。
    XrBodyJointLocationsBD locations{XR_TYPE_BODY_JOINT_LOCATIONS_BD};
    locations.jointLocationCount = XR_BODY_JOINT_COUNT_BD;
    locations.jointLocations = joints;
    XrBodyJointsLocateInfoBD info{XR_TYPE_BODY_JOINTS_LOCATE_INFO_BD};
    info.baseSpace = appSpace_;
    info.time = t;
    const bool ok = XR_SUCCEEDED(pfnLocateBodyJoints_(bodyTracker_, &info, &locations));

    if (pfnGetBodyTrackingState_ != nullptr && --stateProbeCountdown_ <= 0) {
        stateProbeCountdown_ = 72;
        XrBodyTrackingStatePICO state{XR_TYPE_BODY_TRACKING_STATE_PICO};
        if (XR_SUCCEEDED(pfnGetBodyTrackingState_(session_, &state))) {
            bodyStatus_ = static_cast<int>(state.status);
            bodyMessage_ = static_cast<int>(state.message);
            if (state.message == XR_BODY_TRACKING_MESSAGE_TRACKER_NOT_CALIBRATED_PICO &&
                !calibrationRequested_ && pfnStartCalibApp_ != nullptr) {
                calibrationRequested_ = true;
                LOGW("trackers not calibrated, launching calibration app");
                pfnStartCalibApp_(session_);
            }
        }
    }

    json += "\"body\":";
    if (!ok) {
        json += "null,";
        return;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"joint_count\":%u,\"all_tracked\":%s,\"status\":%d,\"message\":%d,\"joints\":{",
                  locations.jointLocationCount,
                  locations.allJointPosesTracked == XR_TRUE ? "true" : "false", bodyStatus_,
                  bodyMessage_);
    json += buf;
    for (uint32_t i = 0; i < XR_BODY_JOINT_COUNT_BD; ++i) {
        if (i > 0) json += ',';
        json += '"';
        json += kBodyJointNames[i];
        json += "\":";
        appendPose(json, joints[i].pose, joints[i].locationFlags);

        if (joints[i].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
            renderer_.addAxes(joints[i].pose, 0.05f);
            const int parent = kBodyParent[i];
            if (parent >= 0 &&
                (joints[parent].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                renderer_.addLine(joints[parent].pose.position, joints[i].pose.position, 1.0f, 1.0f,
                                  1.0f);
            }
        }
    }
    json += "}},";
}

void Bridge::appendViews(std::string &json) {
    json += "\"views\":[";
    if (viewsValid_) {
        char buf[256];
        for (size_t i = 0; i < views_.size(); ++i) {
            if (i > 0) json += ',';
            appendPose(json, views_[i].pose, XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                                 XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
            json.pop_back();
            std::snprintf(buf, sizeof(buf),
                          ",\"fov\":{\"left\":%.6f,\"right\":%.6f,\"up\":%.6f,\"down\":%.6f}}",
                          views_[i].fov.angleLeft, views_[i].fov.angleRight, views_[i].fov.angleUp,
                          views_[i].fov.angleDown);
            json += buf;
        }
    }
    json += "],";
}

void Bridge::buildFrameJson(const XrFrameState &frameState, std::string &json) {
    const XrTime t = frameState.predictedDisplayTime;
    json.clear();

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"seq\":%" PRIu64 ",\"t\":%.6f,\"session_active\":true,\"session_state\":%d,"
                  "\"focused\":%s,\"should_render\":%s,\"blend_mode\":%d,"
                  "\"reference_space\":\"%s\",\"head\":",
                  seq_, static_cast<double>(t) / 1e9, static_cast<int>(sessionState_),
                  focused_ ? "true" : "false",
                  frameState.shouldRender == XR_TRUE ? "true" : "false",
                  static_cast<int>(blendMode_), refSpaceName());
    json += buf;

    XrSpaceLocation headLoc{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(viewSpace_, appSpace_, t, &headLoc))) {
        appendPose(json, headLoc.pose, headLoc.locationFlags);
    } else {
        json += "null";
    }
    json += ',';

    json += "\"eye_gaze\":";
    bool gazeWritten = false;
    if (eyeGazeSpace_ != XR_NULL_HANDLE) {
        XrEyeGazeSampleTimeEXT sampleTime{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
        XrSpaceLocation gazeLoc{XR_TYPE_SPACE_LOCATION};
        gazeLoc.next = &sampleTime;
        if (XR_SUCCEEDED(xrLocateSpace(eyeGazeSpace_, appSpace_, t, &gazeLoc)) &&
            (gazeLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            appendPose(json, gazeLoc.pose, gazeLoc.locationFlags);
            json.pop_back();
            std::snprintf(buf, sizeof(buf), ",\"sample_time\":%.6f}",
                          static_cast<double>(sampleTime.time) / 1e9);
            json += buf;
            renderer_.addRay(gazeLoc.pose, 2.0f, 0.2f, 1.0f, 1.0f);
            gazeWritten = true;
        }
    }
    if (!gazeWritten) json += "null";
    json += ',';

    appendViews(json);
    appendControllers(json, t);
    appendHands(json, t);
    appendBody(json, t);

    std::snprintf(buf, sizeof(buf),
                  "\"caps\":{\"body_tracking\":%s,\"hand_tracking\":%s,\"eye_gaze\":%s,"
                  "\"body_joint_count\":%d}}",
                  supportsBodyTracking_ ? "true" : "false",
                  hasHandTracking_ ? "true" : "false",
                  eyeGazeSpace_ != XR_NULL_HANDLE ? "true" : "false", XR_BODY_JOINT_COUNT_BD);
    json += buf;
}

void Bridge::tick() {
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(session_, &waitInfo, &frameState))) return;

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(session_, &beginInfo);

    // 必须先于下面任何 add* 调用，它会清空上一帧的顶点
    renderer_.beginFrame();
    if (renderer_.ready()) {
        const XrPosef origin{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        renderer_.addAxes(origin, 0.5f);                    // +X 红 / +Y 绿 / +Z 蓝
        renderer_.addRay(origin, 1.0f, 1.0f, 0.9f, 0.15f);  // -Z 黄：正前方
    }

    if (focused_) {
        XrActiveActionSet active{actionSet_, XR_NULL_PATH};
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &active;
        xrSyncActions(session_, &syncInfo);
        applyPendingHaptic();

        // 菜单键切换配置面板
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = aMenu_;
        gi.subactionPath = handPath_[kLeft];
        XrActionStateBoolean menu{XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(session_, &gi, &menu);
        const bool menuNow = menu.currentState == XR_TRUE;
        if (menuNow && !lastMenu_) panel_.toggle();
        lastMenu_ = menuNow;

        // 右手射线操作面板
        if (panel_.visible()) {
            gi.action = aTrigger_;
            gi.subactionPath = handPath_[kRight];
            XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
            xrGetActionStateFloat(session_, &gi, &trigger);

            XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
            const bool ok =
                XR_SUCCEEDED(xrLocateSpace(aimSpace_[kRight], appSpace_,
                                           frameState.predictedDisplayTime, &aim)) &&
                (aim.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
            panel_.update(aim.pose, ok, trigger.currentState > 0.6f, ws_.connected());
            if (ok) renderer_.addRay(aim.pose, 3.0f, 0.2f, 1.0f, 0.6f);
        }
    }

    // 用户在面板上确认了新地址就重连
    std::string newUrl;
    if (VrPanel::consumeUrl(&newUrl)) {
        LOGI("switching to %s", newUrl.c_str());
        ws_.setUrl(newUrl);
        saveServerUrl(dataPath_, newUrl);
        awaitingConnect_ = true;
    }
    // 连上了再收面板，否则用户看不到连接结果
    if (awaitingConnect_ && ws_.connected()) {
        awaitingConnect_ = false;
        panel_.setVisible(false);
    }

    // 视图位姿每帧只定位一次，JSON 与叠加层渲染共用
    views_.assign(viewCount_, {XR_TYPE_VIEW});
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo viewLocate{XR_TYPE_VIEW_LOCATE_INFO};
    viewLocate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    viewLocate.displayTime = frameState.predictedDisplayTime;
    viewLocate.space = appSpace_;
    uint32_t located = 0;
    viewsValid_ = viewCount_ > 0 &&
                  XR_SUCCEEDED(xrLocateViews(session_, &viewLocate, &viewState, viewCount_,
                                             &located, views_.data())) &&
                  (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
                  (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

    // 即使 shouldRender 为假也照发：位姿数据依然有效，消费端不该断流
    buildFrameJson(frameState, jsonBuf_);
    ws_.send(jsonBuf_);
    ++seq_;

    // shouldRender 为假时规范要求不提交任何合成层，也不要碰 swapchain
    const bool render = frameState.shouldRender == XR_TRUE;
    const bool overlay =
        render && renderer_.ready() && viewsValid_ && renderer_.render(views_);

    XrCompositionLayerPassthroughFB passthroughLayerInfo{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    passthroughLayerInfo.layerHandle = passthroughLayer_;
    passthroughLayerInfo.space = XR_NULL_HANDLE;

    XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                 XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    projectionLayer.space = appSpace_;
    projectionLayer.viewCount = static_cast<uint32_t>(renderer_.projectionViews().size());
    projectionLayer.views = renderer_.projectionViews().data();

    // 透视层在底，线框+配置面板都画在投影层里
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t layerCount = 0;
    if (render && passthroughLayer_ != XR_NULL_HANDLE) {
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&passthroughLayerInfo);
    }
    if (overlay) {
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projectionLayer);
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = blendMode_;
    endInfo.layerCount = layerCount;
    endInfo.layers = (layerCount > 0) ? layers : nullptr;
    xrEndFrame(session_, &endInfo);
}

}  // namespace

void android_main(android_app *app) {
    JNIEnv *env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    AppState appState;
    app->userData = &appState;
    app->onAppCmd = handleAppCmd;

    Bridge bridge;
    if (!bridge.init(app)) {
        LOGE("initialization failed, exiting");
        ANativeActivity_finish(app->activity);
    }

    bool exitLoop = false;
    while (app->destroyRequested == 0 && !exitLoop) {
        for (;;) {
            int events = 0;
            android_poll_source *source = nullptr;
            const int timeout =
                (!appState.resumed && !bridge.sessionRunning() && app->destroyRequested == 0) ? -1 : 0;
            if (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void **>(&source)) < 0) {
                break;
            }
            if (source != nullptr) source->process(app, source);
        }

        bridge.pollXrEvents(&exitLoop);
        if (bridge.sessionRunning()) bridge.tick();
    }

    bridge.shutdown();
    app->activity->vm->DetachCurrentThread();
}
