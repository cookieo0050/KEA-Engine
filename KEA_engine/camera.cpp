// ============================================================================
// camera.cpp - First-Person Camera
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// Implements a classic first-person camera. It stores a position, a forward
// direction, yaw (left/right rotation) and pitch (up/down rotation), and builds
// the "view matrix" that every render pass uses to draw the world from the
// player's eyes.
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - getViewMatrix(): uses glm::lookAt(eye, eye+front, up) to produce the matrix
//   that transforms world coordinates into camera coordinates. Read this first.
// - processKeyboard(): WASD movement. Right vector = cross(front, up). Moving
//   "forward" means moving along `front`, "right" means along the right vector.
// - processMouseMovement(): mouse offset changes yaw/pitch; pitch is clamped to
//   +/-89 degrees so the camera can't flip over your head.
// - updateVectors(): converts yaw/pitch (in degrees) into a 3D direction using
//   spherical coordinates (sin/cos). This is THE math of looking around - if you
//   change it, look direction breaks, so be careful.
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - Yaw starts at -90.0 so the initial view faces -Z (OpenGL's "into screen").
// - m_speed (3.0) and m_sensitivity (0.1) are the two "feel" tunables.
// - NOTE: the actual player controller in player.cpp does its OWN WASD handling
//   and reads camera.front - the two files work together but are separate.
// ============================================================================
#include "camera.h"
#include <glm/gtc/matrix_transform.hpp>

Camera::Camera(glm::vec3 position)
    : position(position),
    front(glm::vec3(0.0f, 0.0f, -1.0f)),
    up(glm::vec3(0.0f, 1.0f, 0.0f)),
    m_worldUp(glm::vec3(0.0f, 1.0f, 0.0f)),
    m_yaw(-90.0f),
    m_pitch(0.0f),
    m_speed(3.0f),
    m_sensitivity(0.1f)
{
    updateVectors();
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(position, position + front, up);
}

void Camera::processKeyboard(GLFWwindow* window, float deltaTime) {
    float velocity = m_speed * deltaTime;
    glm::vec3 right = glm::normalize(glm::cross(front, m_worldUp));

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        position += front * velocity;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        position -= front * velocity;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        position -= right * velocity;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        position += right * velocity;
}

void Camera::processMouseMovement(float xoffset, float yoffset) {
    xoffset *= m_sensitivity;
    yoffset *= m_sensitivity;

    m_yaw += xoffset;
    m_pitch += yoffset;

    if (m_pitch > 89.0f) m_pitch = 89.0f;
    if (m_pitch < -89.0f) m_pitch = -89.0f;

    updateVectors();
}

void Camera::updateVectors() {
    glm::vec3 newFront;
    newFront.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    newFront.y = sin(glm::radians(m_pitch));
    newFront.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    front = glm::normalize(newFront);
}