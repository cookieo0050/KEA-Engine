// EditorCamera.cpp
#include "EditorCamera.h"
#include "EditorCore.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>

EditorCamera::EditorCamera() {
    updateVectors();
}

void EditorCamera::update(EditorCore* core, float dt) {
    processInput(core, dt);

    if (m_mode == Mode::FreeFly) {
        freeFlyUpdate(core, dt);
    } else {
        orbitUpdate(core, dt);
    }
}

void EditorCamera::processInput(EditorCore* core, float dt) {
    // Mode toggle: F key
    if (core->isKeyJustPressed(GLFW_KEY_F)) {
        m_mode = (m_mode == Mode::FreeFly) ? Mode::Orbit : Mode::FreeFly;
        m_firstMouse = true;
    }

    // Right mouse - look around (free-fly) or orbit
    if (core->isMouseButtonJustPressed(GLFW_MOUSE_BUTTON_RIGHT)) {
        m_rightMouseDown = true;
        m_firstMouse = true;
    }
    if (core->isMouseButtonJustReleased(GLFW_MOUSE_BUTTON_RIGHT)) {
        m_rightMouseDown = false;
    }

    // Middle mouse - pan (orbit mode)
    if (core->isMouseButtonJustPressed(GLFW_MOUSE_BUTTON_MIDDLE)) {
        m_middleMouseDown = true;
        m_firstMouse = true;
    }
    if (core->isMouseButtonJustReleased(GLFW_MOUSE_BUTTON_MIDDLE)) {
        m_middleMouseDown = false;
    }

    // Scroll - zoom (orbit) or speed (free-fly)
    float scroll = core->scrollY();
    if (scroll != 0.0f) {
        if (m_mode == Mode::Orbit) {
            m_distance = glm::clamp(m_distance - scroll * m_distance * 0.1f, 1.0f, 500.0f);
        } else {
            m_speed = glm::clamp(m_speed + scroll * 5.0f, 1.0f, 100.0f);
        }
    }

    // Keyboard speed modifiers
    if (core->isKeyPressed(GLFW_KEY_LEFT_SHIFT)) m_speed *= 3.0f;
    if (core->isKeyPressed(GLFW_KEY_LEFT_CONTROL)) m_speed *= 0.3f;
}

void EditorCamera::freeFlyUpdate(EditorCore* core, float dt) {
    if (!m_rightMouseDown) {
        m_firstMouse = true;
        return;
    }

    if (m_firstMouse) {
        m_lastX = core->mousePos().x;
        m_lastY = core->mousePos().y;
        m_firstMouse = false;
    }

    float xoffset = (float)(core->mousePos().x - m_lastX);
    float yoffset = (float)(m_lastY - core->mousePos().y);
    m_lastX = core->mousePos().x;
    m_lastY = core->mousePos().y;

    m_yaw += xoffset * m_sensitivity;
    m_pitch += yoffset * m_sensitivity;
    clampPitch();

    updateVectors();

    // Movement
    float boost = core->isKeyPressed(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f;
    float s = m_speed * boost * dt;

    glm::vec3 flatFront = glm::normalize(glm::vec3(m_front.x, 0.0f, m_front.z));
    glm::vec3 right = m_right;

    if (core->isKeyPressed(GLFW_KEY_W)) m_position += flatFront * s;
    if (core->isKeyPressed(GLFW_KEY_S)) m_position -= flatFront * s;
    if (core->isKeyPressed(GLFW_KEY_D)) m_position += right * s;
    if (core->isKeyPressed(GLFW_KEY_A)) m_position -= right * s;
    if (core->isKeyPressed(GLFW_KEY_E)) m_position += m_up * s;
    if (core->isKeyPressed(GLFW_KEY_Q)) m_position -= m_up * s;
}

void EditorCamera::orbitUpdate(EditorCore* core, float dt) {
    if (m_rightMouseDown) {
        if (m_firstMouse) {
            m_lastX = core->mousePos().x;
            m_lastY = core->mousePos().y;
            m_firstMouse = false;
        }

        float xoffset = (float)(core->mousePos().x - m_lastX);
        float yoffset = (float)(core->mousePos().y - m_lastY);
        m_lastX = core->mousePos().x;
        m_lastY = core->mousePos().y;

        m_yaw += xoffset * m_sensitivity;
        m_pitch -= yoffset * m_sensitivity; // Inverted for orbit
        clampPitch();
    } else if (m_middleMouseDown) {
        if (m_firstMouse) {
            m_lastX = core->mousePos().x;
            m_lastY = core->mousePos().y;
            m_firstMouse = false;
        }

        float xoffset = (float)(core->mousePos().x - m_lastX);
        float yoffset = (float)(core->mousePos().y - m_lastY);
        m_lastX = core->mousePos().x;
        m_lastY = core->mousePos().y;

        // Pan target
        m_target -= m_right * xoffset * 0.01f * m_distance;
        m_target += m_up * yoffset * 0.01f * m_distance;
    } else {
        m_firstMouse = true;
    }

    updateVectors();

    // Position is derived from target + distance
    m_position = m_target - m_front * m_distance;
}

void EditorCamera::updateVectors() {
    m_front.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    m_front.y = sin(glm::radians(m_pitch));
    m_front.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    m_front = glm::normalize(m_front);

    m_right = glm::normalize(glm::cross(m_front, glm::vec3(0, 1, 0)));
    m_up = glm::normalize(glm::cross(m_right, m_front));
}

void EditorCamera::clampPitch() {
    m_pitch = glm::clamp(m_pitch, -89.0f, 89.0f);
}

glm::mat4 EditorCamera::view() const {
    return glm::lookAt(m_position, m_position + m_front, m_up);
}

glm::mat4 EditorCamera::projection(float aspect) const {
    return glm::perspective(glm::radians(m_fov), aspect, m_near, m_far);
}

glm::mat4 EditorCamera::viewProjection(float aspect) const {
    return projection(aspect) * view();
}

void EditorCamera::lookAt(const glm::vec3& eye, const glm::vec3& center, const glm::vec3& up) {
    m_position = eye;
    m_target = center;
    m_front = glm::normalize(center - eye);
    m_up = glm::normalize(up);
    m_right = glm::normalize(glm::cross(m_front, m_up));

    // Extract yaw/pitch from front
    m_yaw = glm::degrees(atan2(m_front.z, m_front.x));
    m_pitch = glm::degrees(asin(glm::clamp(m_front.y, -1.0f, 1.0f)));
    m_distance = glm::length(center - eye);
}

void EditorCamera::frameBounds(const glm::vec3& min, const glm::vec3& max) {
    glm::vec3 center = (min + max) * 0.5f;
    glm::vec3 size = max - min;
    float maxDim = glm::max(glm::max(size.x, size.y), size.z);
    m_distance = maxDim * 1.5f;
    m_target = center;
    m_yaw = -45.0f;
    m_pitch = -30.0f;
    clampPitch();
    updateVectors();
    m_position = m_target - m_front * m_distance;
    m_mode = Mode::Orbit;
}

void EditorCamera::focusOnPoint(const glm::vec3& point) {
    m_target = point;
    if (m_mode == Mode::Orbit) {
        m_position = m_target - m_front * m_distance;
    }
}