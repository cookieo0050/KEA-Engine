#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include "shader.h"

class SSGI {
public:
    void init(int width, int height);
    void render(GLuint gPositionTex, GLuint gNormalTex, GLuint gAlbedoTex,
        const glm::mat4& view, const glm::mat4& projection, GLuint quadVAO);
    void blur(GLuint quadVAO, GLuint gPositionTex, GLuint gNormalTex);

    GLuint resultTexture() const { return m_blurTex; }

private:
    GLuint m_fbo = 0, m_tex = 0;
    GLuint m_blurFBO = 0, m_blurTex = 0;
    GLuint m_noiseTex = 0;
    std::vector<glm::vec3> m_kernel;
    Shader* m_shader = nullptr;
    Shader* m_blurShader = nullptr;
    int m_width = 0, m_height = 0;
};