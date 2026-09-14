// EditorCore.cpp
#include "EditorCore.h"
#include <iostream>

EditorCore::EditorCore() = default;

EditorCore::~EditorCore() {
    shutdown();
}

bool EditorCore::init(int width, int height, const char* title) {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!m_window) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); // VSync

    glfwGetWindowSize(m_window, &m_width, &m_height);
    glfwGetFramebufferSize(m_window, &m_fbWidth, &m_fbHeight);

    glfwSetWindowUserPointer(m_window, this);

    glfwSetKeyCallback(m_window, s_keyCallback);
    glfwSetMouseButtonCallback(m_window, s_mouseButtonCallback);
    glfwSetCursorPosCallback(m_window, s_cursorPosCallback);
    glfwSetScrollCallback(m_window, s_scrollCallback);
    glfwSetFramebufferSizeCallback(m_window, s_framebufferSizeCallback);

    m_lastTime = glfwGetTime();
    return true;
}

void EditorCore::shutdown() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
}

void EditorCore::pollEvents() {
    // Update previous frame state
    for (int i = 0; i <= GLFW_KEY_LAST; ++i) m_prevKeys[i] = m_keys[i];
    for (int i = 0; i <= GLFW_MOUSE_BUTTON_LAST; ++i) m_prevMouseButtons[i] = m_mouseButtons[i];
    m_prevMousePos = m_mousePos;
    m_scrollY = 0.0f;

    double now = glfwGetTime();
    m_dt = (float)(now - m_lastTime);
    m_time = now;
    m_lastTime = now;

    glfwPollEvents();
}

void EditorCore::swapBuffers() {
    glfwSwapBuffers(m_window);
}

bool EditorCore::shouldClose() const {
    return glfwWindowShouldClose(m_window);
}

void EditorCore::setShouldClose(bool v) {
    glfwSetWindowShouldClose(m_window, v);
}

void EditorCore::setCursorMode(int mode) {
    glfwSetInputMode(m_window, GLFW_CURSOR, mode);
}

void EditorCore::getCursorPos(double* x, double* y) const {
    glfwGetCursorPos(m_window, x, y);
}

// Static callbacks
void EditorCore::s_keyCallback(GLFWwindow* win, int key, int scancode, int action, int mods) {
    EditorCore* self = static_cast<EditorCore*>(glfwGetWindowUserPointer(win));
    if (self) {
        if (key >= 0 && key <= GLFW_KEY_LAST) {
            if (action == GLFW_PRESS || action == GLFW_REPEAT) self->m_keys[key] = true;
            else if (action == GLFW_RELEASE) self->m_keys[key] = false;
        }
        if (self->m_keyCallback) self->m_keyCallback(key, scancode, action, mods);
    }
}

void EditorCore::s_mouseButtonCallback(GLFWwindow* win, int button, int action, int mods) {
    EditorCore* self = static_cast<EditorCore*>(glfwGetWindowUserPointer(win));
    if (self) {
        if (button >= 0 && button <= GLFW_MOUSE_BUTTON_LAST) {
            if (action == GLFW_PRESS) self->m_mouseButtons[button] = true;
            else if (action == GLFW_RELEASE) self->m_mouseButtons[button] = false;
        }
        if (self->m_mouseButtonCallback) self->m_mouseButtonCallback(button, action, mods);
    }
}

void EditorCore::s_cursorPosCallback(GLFWwindow* win, double x, double y) {
    EditorCore* self = static_cast<EditorCore*>(glfwGetWindowUserPointer(win));
    if (self) {
        self->m_mousePos = { (float)x, (float)y };
        if (self->m_firstMouse) {
            self->m_prevMousePos = self->m_mousePos;
            self->m_firstMouse = false;
        }
        self->m_mouseDelta = self->m_mousePos - self->m_prevMousePos;
        self->m_prevMousePos = self->m_mousePos;
        if (self->m_cursorPosCallback) self->m_cursorPosCallback(x, y);
    }
}

void EditorCore::s_scrollCallback(GLFWwindow* win, double x, double y) {
    EditorCore* self = static_cast<EditorCore*>(glfwGetWindowUserPointer(win));
    if (self) {
        self->m_scrollY = (float)y;
        if (self->m_scrollCallback) self->m_scrollCallback(x, y);
    }
}

void EditorCore::s_framebufferSizeCallback(GLFWwindow* win, int w, int h) {
    EditorCore* self = static_cast<EditorCore*>(glfwGetWindowUserPointer(win));
    if (self) {
        self->m_fbWidth = w;
        self->m_fbHeight = h;
        if (self->m_fbSizeCallback) self->m_fbSizeCallback(w, h);
    }
}