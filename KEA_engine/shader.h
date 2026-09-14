#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>

class Shader {
public:
    GLuint ID;

    Shader(const char* vertexSrc, const char* fragmentSrc);
    static Shader fromFiles(const std::string& vertexPath, const std::string& fragmentPath);

    void use() const;

    void setBool(const std::string& name, bool value) const;
    void setInt(const std::string& name, int value) const;
    void setFloat(const std::string& name, float value) const;
    void setVec2(const std::string& name, const glm::vec2& value) const;
    void setVec3(const std::string& name, const glm::vec3& value) const;
    void setMat4(const std::string& name, const glm::mat4& value) const;

private:
    void checkCompileErrors(GLuint shader, const std::string& type);
};