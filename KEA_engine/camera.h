#pragma once
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

class Camera {
public:
    Camera(glm::vec3 position = glm::vec3(0.0f, 0.0f, 5.0f));

    glm::mat4 getViewMatrix() const;

    void processKeyboard(GLFWwindow* window, float deltaTime);
    void processMouseMovement(float xoffset, float yoffset);

    float getYaw() const { return m_yaw; }
    float getPitch() const { return m_pitch; }

    glm::vec3 position;
    glm::vec3 front;
    glm::vec3 up;

private:
    glm::vec3 m_worldUp;
    float m_yaw;
    float m_pitch;
    float m_speed;
    float m_sensitivity;

    void updateVectors();
};