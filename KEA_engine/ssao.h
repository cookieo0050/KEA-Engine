#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include "shader.h"

class SSAO {
public:
    void init(int width, int height);
    void renderSSAO(GLuint gPositionTex, GLuint gNormalTex,
        const glm::mat4& view, const glm::mat4& projection, GLuint quadVAO);
    // Bilateral blur needs the G-Buffer position + normal to avoid bleeding the
    // occlusion across surface edges (corners, depth discontinuities).
    void blur(GLuint quadVAO, GLuint gPositionTex, GLuint gNormalTex);

    GLuint resultTexture() const { return m_ssaoBlurTex; }
    GLuint rawTexture() const { return m_ssaoTex; }

private:
    GLuint m_ssaoFBO = 0, m_ssaoTex = 0;
    GLuint m_ssaoBlurFBO = 0, m_ssaoBlurTex = 0;
    GLuint m_noiseTex = 0;
    std::vector<glm::vec3> m_kernel;
    Shader* m_ssaoShader = nullptr;
    Shader* m_blurShader = nullptr;
    int m_width = 0, m_height = 0;
};