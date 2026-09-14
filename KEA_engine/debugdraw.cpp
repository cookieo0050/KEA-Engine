// ============================================================================
// debugdraw.cpp - Simple 3D Debug Lines / Points / Triangles
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// A tiny helper for drawing one-off lines, dots and triangles in the 3D world
// (e.g. visualising a raycast or a collision normal). It uploads the few points
// to the GPU and draws them with a flat-colour shader, ignoring depth so debug
// shapes show through walls.
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - init(): creates the shader and one VAO/VBO used by all draw functions.
// - drawLine/drawPoint/drawTriangle: all three do the same pattern:
//     1. use the shader
//     2. upload the coordinates into the VBO (glBufferData, DYNAMIC because the
//        buffer is re-filled every call)
//     3. disable depth test, draw, re-enable depth test
//   The GLSL shaders are at the top of the file (debugVertSrc / debugFragSrc).
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - This is "immediate mode" debugging: it discards state each call, so it is
//   simple but not efficient - only use it for a few shapes per frame.
// - drawPoint sets gl_PointSize = 8 pixels in the vertex shader, overridable
//   with the `size` parameter.
// ============================================================================
#include "debugdraw.h"

static const char* debugVertSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 view;
uniform mat4 projection;
void main() {
    gl_Position = projection * view * vec4(aPos, 1.0);
    gl_PointSize = 8.0;
}
)";

static const char* debugFragSrc = R"(
#version 330 core
out vec4 FragColor;
uniform vec3 color;
void main() {
    FragColor = vec4(color, 1.0);
}
)";

void DebugDraw::init() {
    m_shader = new Shader(debugVertSrc, debugFragSrc);
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
}

void DebugDraw::drawLine(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color,
    const glm::mat4& view, const glm::mat4& projection) {
    float verts[6] = { from.x, from.y, from.z, to.x, to.y, to.z };
    m_shader->use();
    m_shader->setMat4("view", view);
    m_shader->setMat4("projection", projection);
    m_shader->setVec3("color", color);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_LINES, 0, 2);
    glEnable(GL_DEPTH_TEST);
}

void DebugDraw::drawPoint(const glm::vec3& pos, const glm::vec3& color, float size,
    const glm::mat4& view, const glm::mat4& projection) {
    float verts[3] = { pos.x, pos.y, pos.z };
    m_shader->use();
    m_shader->setMat4("view", view);
    m_shader->setMat4("projection", projection);
    m_shader->setVec3("color", color);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glPointSize(size);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_POINTS, 0, 1);
    glEnable(GL_DEPTH_TEST);
}

void DebugDraw::drawTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& color,
    const glm::mat4& view, const glm::mat4& projection) {
    float verts[9] = { a.x,a.y,a.z, b.x,b.y,b.z, c.x,c.y,c.z };
    m_shader->use();
    m_shader->setMat4("view", view);
    m_shader->setMat4("projection", projection);
    m_shader->setVec3("color", color);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEnable(GL_DEPTH_TEST);
}