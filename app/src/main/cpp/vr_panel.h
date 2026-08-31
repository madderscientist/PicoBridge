#pragma once

#include <EGL/egl.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <openxr/openxr.h>

#include <string>

class Renderer;

// VR 内配置面板：Java 侧用 Canvas 画进 Bitmap，native 侧锁像素上传成 GL 纹理，
// 再由 Renderer 当作带贴图的四边形画进投影层。
// 之所以不用 XR_KHR_android_surface_swapchain：PICO 的实现对任何 format/usage 组合
// 都返回 XR_ERROR_VALIDATION_FAILURE。
class VrPanel {
public:
    bool init(android_app *app, XrSpace space, const std::string &initialHost, Renderer *renderer);
    void destroy();

    bool visible() const { return visible_; }
    void setVisible(bool v);
    void toggle() { setVisible(!visible_); }

    // 每帧调用：aim 为手柄瞄准位姿，pressed 为扳机是否按下
    void update(const XrPosef &aim, bool aimValid, bool pressed, bool wsConnected);

    static bool consumeUrl(std::string *out);

private:
    void redraw(bool wsConnected);

    JNIEnv *env_ = nullptr;
    jclass panelClass_ = nullptr;
    jmethodID mPointer_ = nullptr;
    jmethodID mRender_ = nullptr;
    Renderer *renderer_ = nullptr;

    XrPosef pose_{};
    float sizeW_ = 1.0f, sizeH_ = 0.75f;
    bool visible_ = true;
    bool ready_ = false;

    bool lastPressed_ = false;
    bool primed_ = false;  // 扇机得先被看到松开过，否则首帧会误判为一次点击
    bool lastConnected_ = false;
    bool dirty_ = true;
};
