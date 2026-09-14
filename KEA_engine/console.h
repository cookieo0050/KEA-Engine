#pragma once
#include <string>
#include <deque>
#include <vector>
#include <functional>

// Developer console overlay: a semi-transparent bar across the top of the screen that
// shows real-time engine log/error output and accepts typed commands. Toggled with the
// tilde (~) key.
class Console {
public:
    using CommandFn = std::function<void(const std::vector<std::string>& args)>;

    Console();

    void toggle();
    void open();
    void close();
    bool isOpen() const { return m_open; }

    // Append a line to the console log (errors, warnings, script output, ...).
    void log(const std::string& line);
    void logError(const std::string& line);
    void clear();

    // Commands registered by the engine (name, handler). `help` lists them.
    void registerCommand(const std::string& name, CommandFn fn);
    void setQuitCallback(std::function<void()> quit);

    // Draws the overlay (ImGui). Called once per frame while open.
    void draw(float screenWidth, float screenHeight);

private:
    void execute(const std::string& line);

    bool m_open = false;
    bool m_justOpened = false;
    bool m_autoScroll = false;
    std::deque<std::string> m_lines;
    size_t m_maxLines = 200;
    char m_input[256] = { 0 };
    std::vector<std::pair<std::string, CommandFn>> m_commands;
    std::function<void()> m_quit;
};

// Global console instance, available engine-wide so any system can log to it.
extern Console g_Console;
