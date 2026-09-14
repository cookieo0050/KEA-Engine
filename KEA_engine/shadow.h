#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "shader.h"

class ShadowMap {
public:
    void init(unsigned int resolution = 2048);
    glm::mat4 getLightSpaceMatrix(const glm::vec3& lightDir, const glm::vec3& sceneCenter, float sceneRadius) const;
    void beginRender();
    void endRender(int screenWidth, int screenHeight);

    GLuint depthTexture() const { return m_depthTex; }
    Shader* depthShader() { return m_depthShader; }

private:
    GLuint m_fbo = 0, m_depthTex = 0;
    unsigned int m_resolution = 2048;
    Shader* m_depthShader = nullptr;
};