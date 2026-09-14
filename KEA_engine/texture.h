#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>

class Texture {
public:
    void load(const std::string& path);
    void bind(unsigned int unit = 0) const;

    GLuint id() const { return m_id; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    static glm::ivec2 getImageSize(const std::string& path);

private:
    GLuint m_id = 0;
    int m_width = 0, m_height = 0;
};