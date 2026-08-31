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

    // 面板：Java 画到 Bitmap 后上传成纹理，当作带贴图的四边形画在投影层里
    void uploadPanel(const void *pixels, int w, int h);
    void setPanelQuad(const XrPosef &pose, float widthM, float heightM);
    void showPanel(bool v) { panelVisible_ = v; }

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

    GLuint panelProg_ = 0, panelVao_ = 0, panelVbo_ = 0, panelTex_ = 0;
    GLint uPanelMvp_ = -1, uPanelTex_ = -1;
    int panelW_ = 0, panelH_ = 0;
    bool panelVisible_ = false;
    bool panelQuadSet_ = false;
};
