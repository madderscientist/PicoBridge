#include "renderer.h"

#include <android/log.h>

#include <cmath>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PicoBridge/gl", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PicoBridge/gl", __VA_ARGS__)

namespace {

constexpr float kNear = 0.05f, kFar = 100.0f;
constexpr float kGridHalf = 2.5f, kGridStep = 0.5f, kDash = 0.01f, kGap = 0.01f;
constexpr float kGridY = -0.003f;  // 略低于 y=0，避开与坐标轴的深度争夺

const char *kVert = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
uniform mat4 uMvp;
out vec3 vColor;
void main() { vColor = aColor; gl_Position = uMvp * vec4(aPos, 1.0); }
)";

// alpha=1 让线条盖住透视画面；未绘制处 alpha=0，真实环境透上来
const char *kFrag = R"(#version 300 es
precision mediump float;
in vec3 vColor;
out vec4 outColor;
void main() { outColor = vec4(vColor, 1.0); }
)";

GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return s;
    char log[512] = {};
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    LOGE("shader: %s", log);
    glDeleteShader(s);
    return 0;
}

void setupAttribs(GLuint vao, GLuint vbo) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void *>(3 * sizeof(float)));
    glBindVertexArray(0);
}

void quatToMat3(const XrQuaternionf &q, float *m) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);
    m[3] = 2 * (x * y - z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z + x * w);
    m[6] = 2 * (x * z + y * w);     m[7] = 2 * (y * z - x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

XrVector3f apply(const XrPosef &p, const XrVector3f &v) {
    float r[9];
    quatToMat3(p.orientation, r);
    return {r[0] * v.x + r[3] * v.y + r[6] * v.z + p.position.x,
            r[1] * v.x + r[4] * v.y + r[7] * v.z + p.position.y,
            r[2] * v.x + r[5] * v.y + r[8] * v.z + p.position.z};
}

// 直接算 projection * inverse(pose)，省掉一次矩阵乘法与中间矩阵
void mvpFromView(const XrView &view, float *o) {
    float r[9];
    quatToMat3(view.pose.orientation, r);
    const float t[3] = {view.pose.position.x, view.pose.position.y, view.pose.position.z};

    float v[16];
    v[0] = r[0]; v[4] = r[1]; v[8]  = r[2];  v[12] = -(r[0] * t[0] + r[1] * t[1] + r[2] * t[2]);
    v[1] = r[3]; v[5] = r[4]; v[9]  = r[5];  v[13] = -(r[3] * t[0] + r[4] * t[1] + r[5] * t[2]);
    v[2] = r[6]; v[6] = r[7]; v[10] = r[8];  v[14] = -(r[6] * t[0] + r[7] * t[1] + r[8] * t[2]);
    v[3] = 0;    v[7] = 0;    v[11] = 0;     v[15] = 1;

    const float l = std::tan(view.fov.angleLeft), rr = std::tan(view.fov.angleRight);
    const float d = std::tan(view.fov.angleDown), u = std::tan(view.fov.angleUp);
    const float w = rr - l, h = u - d;

    float p[16] = {};
    p[0] = 2 / w;
    p[5] = 2 / h;
    p[8] = (rr + l) / w;
    p[9] = (u + d) / h;
    p[10] = -(kFar + kNear) / (kFar - kNear);
    p[11] = -1;
    p[14] = -(2 * kFar * kNear) / (kFar - kNear);

    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            o[c * 4 + row] = p[row] * v[c * 4] + p[4 + row] * v[c * 4 + 1] +
                             p[8 + row] * v[c * 4 + 2] + p[12 + row] * v[c * 4 + 3];
        }
    }
}

}  // namespace

void Renderer::addLine(const XrVector3f &a, const XrVector3f &b, float r, float g, float b2) {
    verts_.insert(verts_.end(), {a.x, a.y, a.z, r, g, b2, b.x, b.y, b.z, r, g, b2});
}

void Renderer::addAxes(const XrPosef &p, float len) {
    addLine(p.position, apply(p, {len, 0, 0}), 1.0f, 0.15f, 0.15f);
    addLine(p.position, apply(p, {0, len, 0}), 0.15f, 1.0f, 0.15f);
    addLine(p.position, apply(p, {0, 0, len}), 0.25f, 0.45f, 1.0f);
}

void Renderer::addCross(const XrVector3f &p, float s, float r, float g, float b2) {
    addLine({p.x - s, p.y, p.z}, {p.x + s, p.y, p.z}, r, g, b2);
    addLine({p.x, p.y - s, p.z}, {p.x, p.y + s, p.z}, r, g, b2);
    addLine({p.x, p.y, p.z - s}, {p.x, p.y, p.z + s}, r, g, b2);
}

void Renderer::addRay(const XrPosef &p, float len, float r, float g, float b2) {
    addLine(p.position, apply(p, {0, 0, -len}), r, g, b2);
}

void Renderer::buildGrid() {
    std::vector<float> g;
    const float c[3] = {0.22f, 0.24f, 0.30f};
    auto dashed = [&](float x0, float z0, float dx, float dz) {
        for (float t = 0.0f; t < 2 * kGridHalf; t += kDash + kGap) {
            const float t2 = std::fmin(t + kDash, 2 * kGridHalf);
            g.insert(g.end(), {x0 + dx * t,  kGridY, z0 + dz * t,  c[0], c[1], c[2],
                               x0 + dx * t2, kGridY, z0 + dz * t2, c[0], c[1], c[2]});
        }
    };
    for (float v = -kGridHalf; v <= kGridHalf + 1e-4f; v += kGridStep) {
        dashed(v, -kGridHalf, 0, 1);
        dashed(-kGridHalf, v, 1, 0);
    }

    glGenVertexArrays(1, &gridVao_);
    glGenBuffers(1, &gridVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(g.size() * sizeof(float)), g.data(),
                 GL_STATIC_DRAW);
    setupAttribs(gridVao_, gridVbo_);
    gridCount_ = static_cast<GLsizei>(g.size() / 6);
}

bool Renderer::initGl() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVert), fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (vs == 0 || fs == 0) return false;
    prog_ = glCreateProgram();
    glAttachShader(prog_, vs);
    glAttachShader(prog_, fs);
    glLinkProgram(prog_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[512] = {};
        glGetProgramInfoLog(prog_, sizeof(log), nullptr, log);
        LOGE("link: %s", log);
        return false;
    }
    uMvp_ = glGetUniformLocation(prog_, "uMvp");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    setupAttribs(vao_, vbo_);
    glGenFramebuffers(1, &fbo_);
    buildGrid();
    return true;
}

bool Renderer::init(XrInstance instance, XrSystemId systemId, XrSession session) {
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance, systemId,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                      nullptr);
    std::vector<XrViewConfigurationView> cfg(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (XR_FAILED(xrEnumerateViewConfigurationViews(instance, systemId,
                                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                    viewCount, &viewCount, cfg.data()))) {
        return false;
    }

    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
    int64_t chosen = 0;
    for (int64_t f : formats) {
        if (f == GL_RGBA8 || f == GL_SRGB8_ALPHA8) {
            chosen = f;
            if (f == GL_RGBA8) break;
        }
    }
    if (chosen == 0) {
        LOGE("no swapchain format with alpha");
        return false;
    }
    if (!initGl()) return false;

    targets_.resize(viewCount);
    pviews_.resize(viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    for (uint32_t i = 0; i < viewCount; ++i) {
        Target &t = targets_[i];
        t.width = cfg[i].recommendedImageRectWidth;
        t.height = cfg[i].recommendedImageRectHeight;

        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = chosen;
        ci.sampleCount = 1;
        ci.width = t.width;
        ci.height = t.height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(session, &ci, &t.swapchain))) return false;

        uint32_t n = 0;
        xrEnumerateSwapchainImages(t.swapchain, 0, &n, nullptr);
        t.images.resize(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(t.swapchain, n, &n,
                                   reinterpret_cast<XrSwapchainImageBaseHeader *>(t.images.data()));

        glGenRenderbuffers(1, &t.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(t.width),
                              static_cast<GLsizei>(t.height));
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        pviews_[i].subImage.swapchain = t.swapchain;
        pviews_[i].subImage.imageRect.extent = {static_cast<int32_t>(t.width),
                                                static_cast<int32_t>(t.height)};
    }

    ready_ = true;
    LOGI("renderer ready: %u views @ %ux%u, grid %d segments", viewCount, targets_[0].width,
         targets_[0].height, gridCount_ / 2);
    return true;
}

void Renderer::destroy() {
    for (auto &t : targets_) {
        if (t.depth != 0) glDeleteRenderbuffers(1, &t.depth);
        if (t.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(t.swapchain);
    }
    targets_.clear();
    if (fbo_ != 0) glDeleteFramebuffers(1, &fbo_);
    if (gridVbo_ != 0) glDeleteBuffers(1, &gridVbo_);
    if (gridVao_ != 0) glDeleteVertexArrays(1, &gridVao_);
    if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
    if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
    if (prog_ != 0) glDeleteProgram(prog_);
    ready_ = false;
}

bool Renderer::render(const std::vector<XrView> &views) {
    if (!ready_ || views.size() != targets_.size()) return false;

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts_.size() * sizeof(float)),
                 verts_.empty() ? nullptr : verts_.data(), GL_DYNAMIC_DRAW);
    const GLsizei dynCount = static_cast<GLsizei>(verts_.size() / 6);

    for (size_t i = 0; i < views.size(); ++i) {
        Target &t = targets_[i];

        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(t.swapchain, &ai, &index))) return false;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;  // 切勿用 XR_INFINITE_DURATION：头显未佩戴时会死锁
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (xrWaitSwapchainImage(t.swapchain, &wi) != XR_SUCCESS) {
            xrReleaseSwapchainImage(t.swapchain, &ri);
            return false;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               t.images[index].image, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);
        glViewport(0, 0, static_cast<GLsizei>(t.width), static_cast<GLsizei>(t.height));
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glClearColor(0, 0, 0, 0);  // 全透明底色，透出下层 AR 画面
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float mvp[16];
        mvpFromView(views[i], mvp);
        glUseProgram(prog_);
        glUniformMatrix4fv(uMvp_, 1, GL_FALSE, mvp);

        glLineWidth(1.0f);
        glBindVertexArray(gridVao_);
        glDrawArrays(GL_LINES, 0, gridCount_);

        if (dynCount > 0) {
            glLineWidth(4.0f);
            glBindVertexArray(vao_);
            glDrawArrays(GL_LINES, 0, dynCount);
        }
        glBindVertexArray(0);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (XR_FAILED(xrReleaseSwapchainImage(t.swapchain, &ri))) return false;

        pviews_[i].pose = views[i].pose;
        pviews_[i].fov = views[i].fov;
    }
    return true;
}
