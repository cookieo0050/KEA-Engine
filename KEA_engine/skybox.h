#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include "shader.h"

class Skybox {
public:
    void init(const std::string& imagePath);
    void draw(const glm::mat4& view, const glm::mat4& projection);

private:
    GLuint m_VAO = 0, m_VBO = 0;
    GLuint m_cubemapTexture = 0;
    Shader* m_shader = nullptr;
};