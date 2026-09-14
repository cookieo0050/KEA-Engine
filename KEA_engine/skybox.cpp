// ============================================================================
// skybox.cpp - Cubemap Sky Background
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// Draws the sky. It loads one image and applies it to all 6 faces of a cube map
// texture, then draws a giant cube around the camera so you always see the sky
// in every direction. (The source image is a 360 panorama-style picture.)
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - skyboxVertices: the 36 vertices (12 triangles) of a 1x1x1 cube centred at
//   the origin. Its position IS the texture coordinate (TexCoords = aPos), which
//   is exactly what cubemap sampling needs - a direction, not a 2D uv.
// - init(): builds the cube mesh, loads the image, then calls glTexImage2D six
//   times - once for each cube face (POSITIVE_X, NEGATIVE_X, ... ) - reusing the
//   same pixel data for every face. A real game would use 6 different images.
// - draw(): renders with glDepthFunc(GL_LEQUAL) and positions the cube at the
//   camera with the translation stripped out (mat3(mat3(view))) so the skybox
//   "follows" you - you can never walk to the edge of it. pos.xyww forces the
//   depth to the far plane so it always renders behind everything.
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - The skybox cube is drawn with depth test LEQUAL and depth exactly at 1.0, so
//   it only shows where nothing else was drawn.
// - If you want a prettier sky, replace this with 6 face images or an equirect
//   converter - the structure stays the same.
// ============================================================================
#include "skybox.h"
#include "stb_image.h"
#include <iostream>
using namespace std;

static const char* skyboxVertSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
out vec3 TexCoords;
uniform mat4 view;
uniform mat4 projection;
void main() {
    TexCoords = aPos;
    vec4 pos = projection * mat4(mat3(view)) * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
)";

static const char* skyboxFragSrc = R"(
#version 330 core
out vec4 FragColor;
in vec3 TexCoords;
uniform samplerCube skybox;
void main() {
    FragColor = texture(skybox, TexCoords);
}
)";

static float skyboxVertices[] = {
    -1.0f,  1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,   1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,  -1.0f, -1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
     1.0f, -1.0f, -1.0f,   1.0f, -1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f,  1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f, -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
    -1.0f,  1.0f, -1.0f,   1.0f,  1.0f, -1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f
};

void Skybox::init(const string& imagePath) {
    m_shader = new Shader(skyboxVertSrc, skyboxFragSrc);

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), skyboxVertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glGenTextures(1, &m_cubemapTexture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_cubemapTexture);

    int width, height, channels;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* data = stbi_load(imagePath.c_str(), &width, &height, &channels, 0);

    if (!data) {
        cerr << "Skybox failed to load image: " << imagePath << " (" << stbi_failure_reason() << ")\n";
        return;
    }

    GLenum format = (channels == 4) ? GL_RGBA : GL_RGB;
    for (unsigned int i = 0; i < 6; i++) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, format,
            width, height, 0, format, GL_UNSIGNED_BYTE, data);
    }
    stbi_image_free(data);

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
}

void Skybox::draw(const glm::mat4& view, const glm::mat4& projection) {
    glDepthFunc(GL_LEQUAL);
    m_shader->use();
    m_shader->setMat4("view", view);
    m_shader->setMat4("projection", projection);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_cubemapTexture);
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glDepthFunc(GL_LESS);
}