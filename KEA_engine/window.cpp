// ============================================================================
// window.cpp - GLFW Window Wrapper
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// A thin wrapper around GLFW (the library that creates OS windows and handles
// input). It hides all the raw GLFW calls so the rest of the engine can just
// do `Window window(800, 600, "KEA Engine")` and forget the details.
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - Constructor: init GLFW -> request an OpenGL 3.3 core context -> create the
//   window -> load GLAD (the OpenGL function loader, required before ANY OpenGL
//   call) -> set up a resize callback and depth testing.
// - framebuffer_size_callback: fires when the window is resized; updates the
//   viewport and records the new size.
// - swapBuffersAndPollEvents: the two calls that end every frame - draw what was
//   rendered (swap) and handle queued window/input events (poll).
// - setCursorDisabled: used to lock the mouse to the centre for first-person look.
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - GLFW creates the window, GLAD lets you actually use OpenGL functions.
//   window.cpp is where the two are glued together.
// - glfwSwapInterval(0) disables VSync -> no 60 FPS cap.
// - The Window object is attached to the GLFW window via UserPointer so the C
//   callback can reach back into the C++ object.
// ============================================================================
#include "window.h"
#include <iostream>
using namespace std;

static void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) self->onResize(width, height);
}

Window::Window(int width, int height, const char* title)
    : m_width(width), m_height(height)
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!m_window) {
        cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return;
    }

    glfwMakeContextCurrent(m_window);

    // TURN OFF V-SYNC: 0 disables the 60 FPS cap, allowing unconstrained frame rates
    glfwSwapInterval(0);

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebuffer_size_callback);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        cerr << "Failed to initialize GLAD! 😭\n";
        return;
    }

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
}

Window::~Window() {
    glfwTerminate();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_window);
}

void Window::swapBuffersAndPollEvents() {
    glfwSwapBuffers(m_window);
    glfwPollEvents();
}

void Window::setCursorDisabled(bool disabled) {
    glfwSetInputMode(m_window, GLFW_CURSOR, disabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void Window::setFullscreen(bool fullscreen) {
    if (fullscreen == m_fullscreen) return;

    if (fullscreen) {
        // Remember the current windowed position/size so windowed mode can be restored.
        glfwGetWindowPos(m_window, &m_windowedPosX, &m_windowedPosY);
        glfwGetWindowSize(m_window, &m_windowedWidth, &m_windowedHeight);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(m_window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    }
    else {
        glfwSetWindowMonitor(m_window, nullptr, m_windowedPosX, m_windowedPosY,
            m_windowedWidth, m_windowedHeight, 0);
    }

    m_fullscreen = fullscreen;
}

void Window::onResize(int width, int height) {
    m_width = width;
    m_height = height;
    if (onFramebufferResize) onFramebufferResize(width, height);
}