// ============================================================================
// shader.cpp - GLSL Shader Compilation & Uniform Helpers
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// Compiles the GLSL shader programs used everywhere in the engine and wraps
// glUniform* calls into easy C++ methods. Every Shader object owns one GPU
// program (vertex + fragment shader linked together).
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - Constructor: takes two source strings (see the big strings in main.cpp),
//   creates/compiles a vertex shader, creates/compiles a fragment shader, then
//   links them into a single program with an ID.
// - checkCompileErrors(): if a shader fails to compile, prints the GPU error log
//   to the terminal. These messages are your best friend when a shader breaks.
// - fromFiles(): alternate constructor that reads .vert/.frag text files instead
//   of taking strings (the engine currently passes strings, so this is unused).
// - setBool/setInt/setFloat/setVec2/setVec3/setMat4: each one looks up the
//   uniform by name inside the program and uploads a value. Uniforms are the
//   way the CPU sends data to the GPU mid-frame (light positions, matrices, ...).
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - A "uniform" is a read-only variable in a shader you set before drawing.
// - glGetUniformLocation() can be slow in loops - caching it would be an
//   optimisation (not done here for simplicity).
// - Shader "programs" are the single most important GPU concept: geometry +
//   rendering recipe = a program.
// ============================================================================
#include "shader.h"
#include <glm/gtc/type_ptr.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
using namespace std;

Shader::Shader(const char* vertexSrc, const char* fragmentSrc) {
    GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vertexSrc, nullptr);
    glCompileShader(vertex);
    checkCompileErrors(vertex, "VERTEX");

    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fragmentSrc, nullptr);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT");

    ID = glCreateProgram();
    glAttachShader(ID, vertex);
    glAttachShader(ID, fragment);
    glLinkProgram(ID);
    checkCompileErrors(ID, "PROGRAM");

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

Shader Shader::fromFiles(const string& vertexPath, const string& fragmentPath) {
    string vertexCode, fragmentCode;
    ifstream vShaderFile, fShaderFile;
    vShaderFile.exceptions(ifstream::failbit | ifstream::badbit);
    fShaderFile.exceptions(ifstream::failbit | ifstream::badbit);
    try {
        vShaderFile.open(vertexPath);
        fShaderFile.open(fragmentPath);
        stringstream vShaderStream, fShaderStream;
        vShaderStream << vShaderFile.rdbuf();
        fShaderStream << fShaderFile.rdbuf();
        vShaderFile.close();
        fShaderFile.close();
        vertexCode = vShaderStream.str();
        fragmentCode = fShaderStream.str();
    }
    catch (ifstream::failure& e) {
        cerr << "ERROR::SHADER::FILE_NOT_SUCCESSFULLY_READ: " << e.what() << endl;
    }
    return Shader(vertexCode.c_str(), fragmentCode.c_str());
}

void Shader::use() const { glUseProgram(ID); }

void Shader::setBool(const string& name, bool value) const {
    glUniform1i(glGetUniformLocation(ID, name.c_str()), (int)value);
}
void Shader::setInt(const string& name, int value) const {
    glUniform1i(glGetUniformLocation(ID, name.c_str()), value);
}
void Shader::setFloat(const string& name, float value) const {
    glUniform1f(glGetUniformLocation(ID, name.c_str()), value);
}
void Shader::setVec2(const string& name, const glm::vec2& value) const {
    glUniform2fv(glGetUniformLocation(ID, name.c_str()), 1, glm::value_ptr(value));
}
void Shader::setVec3(const string& name, const glm::vec3& value) const {
    glUniform3fv(glGetUniformLocation(ID, name.c_str()), 1, glm::value_ptr(value));
}
void Shader::setMat4(const string& name, const glm::mat4& value) const {
    glUniformMatrix4fv(glGetUniformLocation(ID, name.c_str()), 1, GL_FALSE, glm::value_ptr(value));
}

void Shader::checkCompileErrors(GLuint shader, const string& type) {
    int success;
    char infoLog[1024];
    if (type != "PROGRAM") {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
            cerr << "ERROR::SHADER_COMPILATION_ERROR of type: " << type << "\n" << infoLog << "\n";
        }
    }
    else {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shader, 1024, nullptr, infoLog);
            cerr << "ERROR::PROGRAM_LINKING_ERROR of type: " << type << "\n" << infoLog << "\n";
        }
    }
}