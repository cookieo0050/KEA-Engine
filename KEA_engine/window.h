#pragma once
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <functional>

class Window {
public:
    Window(int width, int height, const char* title);
    ~Window();

    bool shouldClose() const;
    void swapBuffersAndPollEvents();
    void setCursorDisabled(bool disabled);

    GLFWwindow* handle() const { return m_window; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    void setFullscreen(bool fullscreen);
    bool isFullscreen() const { return m_fullscreen; }

    void onResize(int width, int height);

    // Called by the framebuffer-size callback whenever the window resizes so the
    // engine can rebuild its size-dependent render targets (set from main()).
    std::function<void(int, int)> onFramebufferResize;

private:
    GLFWwindow* m_window = nullptr;
    int m_width, m_height;
    bool m_fullscreen = false;
    int m_windowedPosX = 0, m_windowedPosY = 0;
    int m_windowedWidth = 800, m_windowedHeight = 600;
};