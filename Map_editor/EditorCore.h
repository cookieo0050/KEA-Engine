// EditorCore.h - GLFW window, input, main loop
#pragma once
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <functional>
#include <vector>

class EditorCore {
public:
    using KeyCallback = std::function<void(int, int, int, int)>;
    using MouseButtonCallback = std::function<void(int, int, int)>;
    using CursorPosCallback = std::function<void(double, double)>;
    using ScrollCallback = std::function<void(double, double)>;
    using FramebufferSizeCallback = std::function<void(int, int)>;

    EditorCore();
    ~EditorCore();

    bool init(int width, int height, const char* title);
    void shutdown();

    void pollEvents();
    void swapBuffers();
    bool shouldClose() const;
    void setShouldClose(bool v);

    GLFWwindow* window() { return m_window; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    float aspectRatio() const { return (float)m_width / (float)m_height; }
    float framebufferWidth() const { return (float)m_fbWidth; }
    float framebufferHeight() const { return (float)m_fbHeight; }
    float aspectRatioFB() const { return (float)m_fbWidth / (float)m_fbHeight; }

    float deltaTime() const { return m_dt; }
    double time() const { return m_time; }

    void setKeyCallback(KeyCallback cb) { m_keyCallback = std::move(cb); }
    void setMouseButtonCallback(MouseButtonCallback cb) { m_mouseButtonCallback = std::move(cb); }
    void setCursorPosCallback(CursorPosCallback cb) { m_cursorPosCallback = std::move(cb); }
    void setScrollCallback(ScrollCallback cb) { m_scrollCallback = std::move(cb); }
    void setFramebufferSizeCallback(FramebufferSizeCallback cb) { m_fbSizeCallback = std::move(cb); }

    bool isKeyPressed(int key) const { return m_keys[key]; }
    bool isKeyJustPressed(int key) const { return m_keys[key] && !m_prevKeys[key]; }
    bool isKeyJustReleased(int key) const { return !m_keys[key] && m_prevKeys[key]; }

    bool isMouseButtonPressed(int button) const { return m_mouseButtons[button]; }
    bool isMouseButtonJustPressed(int button) const { return m_mouseButtons[button] && !m_prevMouseButtons[button]; }
    bool isMouseButtonJustReleased(int button) const { return !m_mouseButtons[button] && m_prevMouseButtons[button]; }

    glm::vec2 mousePos() const { return m_mousePos; }
    glm::vec2 mouseDelta() const { return m_mouseDelta; }
    float scrollY() const { return m_scrollY; }

    void setCursorMode(int mode);
    void getCursorPos(double* x, double* y) const;

private:
    static void s_keyCallback(GLFWwindow* win, int key, int scancode, int action, int mods);
    static void s_mouseButtonCallback(GLFWwindow* win, int button, int action, int mods);
    static void s_cursorPosCallback(GLFWwindow* win, double x, double y);
    static void s_scrollCallback(GLFWwindow* win, double x, double y);
    static void s_framebufferSizeCallback(GLFWwindow* win, int w, int h);

    GLFWwindow* m_window = nullptr;
    int m_width = 1280, m_height = 720;
    int m_fbWidth = 1280, m_fbHeight = 720;

    double m_time = 0.0;
    double m_lastTime = 0.0;
    float m_dt = 0.016f;

    bool m_keys[GLFW_KEY_LAST + 1] = {};
    bool m_prevKeys[GLFW_KEY_LAST + 1] = {};
    bool m_mouseButtons[GLFW_MOUSE_BUTTON_LAST + 1] = {};
    bool m_prevMouseButtons[GLFW_MOUSE_BUTTON_LAST + 1] = {};

    glm::vec2 m_mousePos = {0, 0};
    glm::vec2 m_prevMousePos = {0, 0};
    glm::vec2 m_mouseDelta = {0, 0};
    float m_scrollY = 0.0f;

    KeyCallback m_keyCallback;
    MouseButtonCallback m_mouseButtonCallback;
    CursorPosCallback m_cursorPosCallback;
    ScrollCallback m_scrollCallback;
    FramebufferSizeCallback m_fbSizeCallback;

    bool m_firstMouse = true;
};