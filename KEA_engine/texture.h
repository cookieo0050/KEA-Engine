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

    // True once load() actually decoded an image (a failed load leaves the
    // object with id != 0 but zero dimensions).
    bool valid() const { return m_id != 0 && m_width > 0; }

    static glm::ivec2 getImageSize(const std::string& path);

private:
    GLuint m_id = 0;
    int m_width = 0, m_height = 0;
};