// EditorCamera.h - Free-fly + orbit camera with smooth interpolation
#pragma once
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

class EditorCore;

class EditorCamera {
public:
    enum class Mode {
        FreeFly,    // WASD + mouse look
        Orbit       // Orbit around target
    };

    EditorCamera();
    ~EditorCamera() = default;

    void update(EditorCore* core, float dt);
    void processInput(EditorCore* core, float dt);

    void setMode(Mode m) { m_mode = m; }
    Mode mode() const { return m_mode; }

    void setPosition(const glm::vec3& pos) { m_position = pos; }
    void setTarget(const glm::vec3& target) { m_target = target; }
    void setYawPitch(float yaw, float pitch) { m_yaw = yaw; m_pitch = pitch; clampPitch(); }

    const glm::vec3& position() const { return m_position; }
    const glm::vec3& front() const { return m_front; }
    const glm::vec3& up() const { return m_up; }
    const glm::vec3& right() const { return m_right; }
    const glm::vec3& target() const { return m_target; }

    float yaw() const { return m_yaw; }
    float pitch() const { return m_pitch; }
    float distance() const { return m_distance; }
    void setDistance(float d) { m_distance = glm::clamp(d, 1.0f, 500.0f); }

    float fov() const { return m_fov; }
    void setFov(float f) { m_fov = glm::clamp(f, 30.0f, 120.0f); }

    float nearPlane() const { return m_near; }
    float farPlane() const { return m_far; }
    void setNearFar(float n, float f) { m_near = n; m_far = f; }

    glm::mat4 view() const;
    glm::mat4 projection(float aspect) const;
    glm::mat4 viewProjection(float aspect) const;

    void lookAt(const glm::vec3& eye, const glm::vec3& center, const glm::vec3& up = {0,1,0});
    void frameBounds(const glm::vec3& min, const glm::vec3& max);
    void focusOnPoint(const glm::vec3& point);

    float speed() const { return m_speed; }
    void setSpeed(float s) { m_speed = s; }
    float sensitivity() const { return m_sensitivity; }
    void setSensitivity(float s) { m_sensitivity = s; }

private:
    void updateVectors();
    void clampPitch();
    void orbitUpdate(EditorCore* core, float dt);
    void freeFlyUpdate(EditorCore* core, float dt);

    Mode m_mode = Mode::FreeFly;

    // Free-fly state
    glm::vec3 m_position = {0, 8, 16};
    glm::vec3 m_front = {0, 0, -1};
    glm::vec3 m_up = {0, 1, 0};
    glm::vec3 m_right = {1, 0, 0};
    float m_yaw = -90.0f;
    float m_pitch = -20.0f;
    float m_speed = 15.0f;
    float m_sensitivity = 0.1f;
    float m_fov = 75.0f;
    float m_near = 0.1f;
    float m_far = 500.0f;

    // Orbit state
    glm::vec3 m_target = {0, 0, 0};
    float m_distance = 20.0f;

    bool m_rightMouseDown = false;
    bool m_middleMouseDown = false;
    double m_lastX = 0, m_lastY = 0;
    bool m_firstMouse = true;
};