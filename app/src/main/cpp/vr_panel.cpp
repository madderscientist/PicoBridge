#include "vr_panel.h"

#include <android/bitmap.h>
#include <android/log.h>

#include <mutex>

#include "renderer.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PicoBridge/panel", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PicoBridge/panel", __VA_ARGS__)

namespace {
constexpr int kTexW = 1024;
constexpr int kTexH = 768;

std::mutex g_urlMutex;
std::string g_pendingUrl;
jobject g_bitmap = nullptr;

// 世界坐标点变换到面板局部坐标（四元数共轭旋转）
XrVector3f toLocal(const XrPosef &pose, const XrVector3f &p) {
    const XrVector3f d{p.x - pose.position.x, p.y - pose.position.y, p.z - pose.position.z};
    const XrQuaternionf &q = pose.orientation;
    const float w = -q.w;  // 共轭 = 反向旋转
    const float uvx = q.y * d.z - q.z * d.y;
    const float uvy = q.z * d.x - q.x * d.z;
    const float uvz = q.x * d.y - q.y * d.x;
    const float uuvx = q.y * uvz - q.z * uvy;
    const float uuvy = q.z * uvx - q.x * uvz;
    const float uuvz = q.x * uvy - q.y * uvx;
    return {d.x + 2.0f * (w * uvx + uuvx), d.y + 2.0f * (w * uvy + uuvy),
            d.z + 2.0f * (w * uvz + uuvz)};
}

void JNICALL onConnect(JNIEnv *env, jclass, jstring url) {
    const char *s = env->GetStringUTFChars(url, nullptr);
    {
        std::lock_guard<std::mutex> lock(g_urlMutex);
        g_pendingUrl = s;
    }
    LOGI("connect requested: %s", s);
    env->ReleaseStringUTFChars(url, s);
}
}  // namespace

bool VrPanel::init(android_app *app, XrSpace, const std::string &initialHost, Renderer *renderer) {
    renderer_ = renderer;
    if (app->activity->vm->AttachCurrentThread(&env_, nullptr) != JNI_OK) {
        LOGE("attach thread failed");
        return false;
    }

    // NativeActivity 的 ClassLoader 才能看到 APK 里的类；FindClass 在 native 线程上找不到
    jclass actCls = env_->GetObjectClass(app->activity->clazz);
    jmethodID getCl = env_->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject cl = env_->CallObjectMethod(app->activity->clazz, getCl);
    jclass clCls = env_->GetObjectClass(cl);
    jmethodID loadClass =
            env_->GetMethodID(clCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env_->NewStringUTF("com.madderscientist.picobridge.VrPanel");
    auto cls = (jclass)env_->CallObjectMethod(cl, loadClass, name);
    env_->DeleteLocalRef(name);
    if (env_->ExceptionCheck() || cls == nullptr) {
        env_->ExceptionClear();
        LOGE("VrPanel class not found");
        return false;
    }
    panelClass_ = (jclass)env_->NewGlobalRef(cls);

    // NativeActivity 是 dlopen 加载这个 .so 的，没走 System.loadLibrary，
    // 所以 JNI 动态查找找不到符号（UnsatisfiedLinkError），必须显式注册
    const JNINativeMethod methods[] = {
            {"nativeOnConnect", "(Ljava/lang/String;)V", reinterpret_cast<void *>(onConnect)}};
    if (env_->RegisterNatives(panelClass_, methods, 1) != JNI_OK) {
        env_->ExceptionClear();
        LOGE("RegisterNatives failed");
        return false;
    }

    jmethodID mCreate = env_->GetStaticMethodID(panelClass_, "create",
                                                "(IILjava/lang/String;)Landroid/graphics/Bitmap;");
    mPointer_ = env_->GetStaticMethodID(panelClass_, "pointer", "(FFI)V");
    mRender_ = env_->GetStaticMethodID(panelClass_, "render", "(Z)V");
    if (mCreate == nullptr || mPointer_ == nullptr || mRender_ == nullptr) {
        env_->ExceptionClear();
        LOGE("VrPanel methods not found");
        return false;
    }

    jstring host = env_->NewStringUTF(initialHost.c_str());
    jobject bmp = env_->CallStaticObjectMethod(panelClass_, mCreate, kTexW, kTexH, host);
    env_->DeleteLocalRef(host);
    if (env_->ExceptionCheck() || bmp == nullptr) {
        env_->ExceptionClear();
        LOGE("panel create failed");
        return false;
    }
    g_bitmap = env_->NewGlobalRef(bmp);

    pose_ = {{0, 0, 0, 1}, {0, 1.2f, -1.5f}};
    renderer_->setPanelQuad(pose_, sizeW_, sizeH_);
    renderer_->showPanel(visible_);
    ready_ = true;
    LOGI("panel ready (bitmap %dx%d)", kTexW, kTexH);
    return true;
}

void VrPanel::setVisible(bool v) {
    visible_ = v;
    dirty_ = true;
    primed_ = false;
    lastPressed_ = false;
    if (renderer_ != nullptr) renderer_->showPanel(v);
}

void VrPanel::update(const XrPosef &aim, bool aimValid, bool pressed, bool wsConnected) {
    if (!ready_ || !visible_) return;

    if (!primed_) {
        if (pressed) return;
        primed_ = true;
    }

    if (aimValid) {
        // 射线起点/方向变到面板局部坐标，与 z=0 平面求交
        const XrVector3f o = toLocal(pose_, aim.position);
        const XrQuaternionf &q = aim.orientation;
        // 手柄 -z 为前方
        const XrVector3f fwd{-2.0f * (q.x * q.z + q.w * q.y),
                             -2.0f * (q.y * q.z - q.w * q.x),
                             -(1.0f - 2.0f * (q.x * q.x + q.y * q.y))};
        const XrVector3f d = toLocal(pose_, {aim.position.x + fwd.x, aim.position.y + fwd.y,
                                             aim.position.z + fwd.z});
        const XrVector3f dir{d.x - o.x, d.y - o.y, d.z - o.z};
        if (dir.z < -1e-4f || dir.z > 1e-4f) {
            const float t = -o.z / dir.z;
            if (t > 0) {
                const float lx = o.x + dir.x * t, ly = o.y + dir.y * t;
                const float u = (lx / sizeW_) + 0.5f, v = 0.5f - (ly / sizeH_);
                if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
                    const float px = u * kTexW, py = v * kTexH;
                    if (pressed && !lastPressed_) {
                        env_->CallStaticVoidMethod(panelClass_, mPointer_, px, py, 0);
                        dirty_ = true;
                    } else if (!pressed && lastPressed_) {
                        env_->CallStaticVoidMethod(panelClass_, mPointer_, px, py, 2);
                        dirty_ = true;
                    }
                    if (env_->ExceptionCheck()) {
                        env_->ExceptionDescribe();
                        env_->ExceptionClear();
                    }
                }
            }
        }
    }
    lastPressed_ = pressed;

    if (wsConnected != lastConnected_) {
        lastConnected_ = wsConnected;
        dirty_ = true;
    }
    if (!dirty_) return;
    dirty_ = false;
    redraw(wsConnected);
}

void VrPanel::redraw(bool wsConnected) {
    env_->CallStaticVoidMethod(panelClass_, mRender_, wsConnected ? JNI_TRUE : JNI_FALSE);
    if (env_->ExceptionCheck()) {
        env_->ExceptionClear();
        return;
    }
    void *pixels = nullptr;
    if (AndroidBitmap_lockPixels(env_, g_bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) {
        LOGE("lockPixels failed");
        return;
    }
    renderer_->uploadPanel(pixels, kTexW, kTexH);
    AndroidBitmap_unlockPixels(env_, g_bitmap);
}

void VrPanel::destroy() {
    if (env_ == nullptr) return;
    if (g_bitmap != nullptr) env_->DeleteGlobalRef(g_bitmap);
    if (panelClass_ != nullptr) env_->DeleteGlobalRef(panelClass_);
    g_bitmap = nullptr;
    panelClass_ = nullptr;
    ready_ = false;
}

bool VrPanel::consumeUrl(std::string *out) {
    std::lock_guard<std::mutex> lock(g_urlMutex);
    if (g_pendingUrl.empty()) return false;
    *out = g_pendingUrl;
    g_pendingUrl.clear();
    return true;
}
