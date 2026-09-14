// ============================================================================
// shadow.cpp - Directional (Sun) Shadow Mapping
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// Creates the depth texture used for the sun's directional shadows. It stores a
// depth-only framebuffer, renders the map from the sun's viewpoint, and gives
// the main.cpp pass #6 the matrix that converts world positions into sun-view
// coordinates (lightSpaceMatrix) so it can compare depths and detect shadows.
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - init(): makes an FBO with a DEPTH_COMPONENT texture attached. No colour
//   attachment - the only thing we care about is "how far is this from the sun?"
//   (that's all a shadow map is). Clamped to border colour 1.0 (max depth =
//   "no geometry here" = not shadowed).
// - getLightSpaceMatrix(): places a camera where the sun is, looking at the
//   scene centre, with an orthographic projection that covers the whole scene
//   (radius = sceneRadius). Returns proj * view.
// - beginRender()/endRender(): switch into/out of the depth framebuffer.
//   main.cpp draws every chunk with depthShader while this is active (pass #1).
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - The depth shaders (depthVertSrc/depthFragSrc) are trivial: write depth only.
//   The fragment shader body is literally empty `{}` because depth is written
//   automatically before the fragment shader runs.
// - Front-face culling is used in main.cpp during this pass to avoid "shadow
//   acne" (self-shadowing due to depth precision).
// - Directional light = "from infinitely far away" = orthographic projection.
//   Point lights instead use cube maps (see pointShadowCubemaps in main.cpp).
// ============================================================================
#include "shadow.h"
#include <glm/gtc/matrix_transform.hpp>

static const char* depthVertSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 lightSpaceMatrix;
uniform mat4 model;
void main() {
    gl_Position = lightSpaceMatrix * model * vec4(aPos, 1.0);
}
)";

static const char* depthFragSrc = R"(
#version 330 core
void main() { }
)";

void ShadowMap::init(unsigned int resolution) {
    m_resolution = resolution;
    m_depthShader = new Shader(depthVertSrc, depthFragSrc);

    glGenFramebuffers(1, &m_fbo);

    glGenTextures(1, &m_depthTex);
    glBindTexture(GL_TEXTURE_2D, m_depthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, m_resolution, m_resolution,
        0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_depthTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

glm::mat4 ShadowMap::getLightSpaceMatrix(const glm::vec3& lightDir, const glm::vec3& sceneCenter, float sceneRadius) const {
    glm::vec3 lightPos = sceneCenter - glm::normalize(lightDir) * sceneRadius * 2.0f;
    glm::mat4 lightView = glm::lookAt(lightPos, sceneCenter, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 lightProj = glm::ortho(-sceneRadius, sceneRadius, -sceneRadius, sceneRadius,
        0.1f, sceneRadius * 4.0f);
    return lightProj * lightView;
}

void ShadowMap::beginRender() {
    glViewport(0, 0, m_resolution, m_resolution);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void ShadowMap::endRender(int screenWidth, int screenHeight) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screenWidth, screenHeight);
}