#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>

// 线框调试渲染器：把坐标轴、骨架等叠在 AR 透视画面上。
// 地面网格是静态的，只在 init 时构建一次，不参与每帧上传。
class Renderer {
public:
    bool init(XrInstance instance, XrSystemId systemId, XrSession session);
    void destroy();
    bool ready() const { return ready_; }

    void beginFrame() { verts_.clear(); }
    void addLine(const XrVector3f &a, const XrVector3f &b, float r, float g, float b2);
    void addAxes(const XrPosef &pose, float len);
    void addCross(const XrVector3f &p, float size, float r, float g, float b2);
    void addRay(const XrPosef &pose, float len, float r, float g, float b2);

    bool render(const std::vector<XrView> &views);

    const std::vector<XrCompositionLayerProjectionView> &projectionViews() const { return pviews_; }

private:
    struct Target {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        uint32_t width = 0, height = 0;
        GLuint depth = 0;
    };

    bool initGl();
    void buildGrid();

    std::vector<Target> targets_;
    std::vector<XrCompositionLayerProjectionView> pviews_;
    std::vector<float> verts_;  // 每顶点 6 个 float：xyz + rgb

    GLuint prog_ = 0, vao_ = 0, vbo_ = 0, gridVao_ = 0, gridVbo_ = 0, fbo_ = 0;
    GLint uMvp_ = -1;
    GLsizei gridCount_ = 0;
    bool ready_ = false;
};
