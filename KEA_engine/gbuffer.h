#pragma once
#include <glad/glad.h>

class GBuffer {
public:
    void init(int width, int height);
    void bindForWriting();

    GLuint positionTex() const { return m_gPosition; }
    GLuint normalTex() const { return m_gNormal; }
    GLuint albedoSpecTex() const { return m_gAlbedoSpec; }
    GLuint getFBO() const { return m_fbo; }
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    GLuint m_fbo = 0;
    GLuint m_gPosition = 0, m_gNormal = 0, m_gAlbedoSpec = 0;
    GLuint m_rbo = 0;
    int m_width = 0, m_height = 0;
};