#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "shader.h"

class DebugDraw {
public:
    void init();
    void drawLine(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color,
        const glm::mat4& view, const glm::mat4& projection);
    void drawPoint(const glm::vec3& pos, const glm::vec3& color, float size,
        const glm::mat4& view, const glm::mat4& projection);
    void drawTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& color,
        const glm::mat4& view, const glm::mat4& projection);

private:
    Shader* m_shader = nullptr;
    GLuint m_VAO = 0, m_VBO = 0;
};