// ============================================================================
// console.cpp - In-Game Developer Console
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// The Windows 98-style bar across the top of the screen you open with the
// tilde (~) key. It shows live engine log lines (errors, map info, ...) and
// lets you type commands (help, clear, echo, pos, quit). Any engine system can
// write to it by calling the global `g_Console.log(...)` / `.logError(...)`.
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - Console() constructor: registers the built-in commands. registerCommand()
//   stores (name, function) pairs; execute() looks one up by name and calls it.
// - log()/logError()/clear(): manage the ring buffer m_lines (a deque capped at
//   m_maxLines = 200 so it can't grow forever).
// - draw(): the entire UI in one ImGui function. Read it in order:
//     1. If closed, return immediately.
//     2. Push style vars + colors (window size, transparency, grey palette).
//     3. A scrollable child window that prints every line, colouring errors red
//        and unknown commands amber (otherwise plain grey).
//     4. The "> " prompt and the ImGui InputText box. Submitting the box calls
//        execute(). consoleCharFilter blocks the `/~ characters being typed.
//   IMPORTANT: every PushStyleColor/PushStyleVar MUST be matched by a
//   PopStyleColor/PopStyleVar - if the counts differ, ImGui logs
//   "Calling PopStyleColor() too many times!" (we hit exactly that bug before).
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - m_autoScroll keeps the log pinned to the newest line while you're at the
//   bottom, but lets you scroll up to read history.
// - Esc closes the console before it quits the game (logic lives in main.cpp).
// - The console is the easiest place to add a debug command: registerCommand()
//   in the constructor and it shows up in `help` automatically.
// ============================================================================
#include "console.h"
#include "imgui.h"
#include <algorithm>
#include <sstream>

Console g_Console;

namespace {
    int consoleCharFilter(ImGuiInputTextCallbackData* data) {
        if (data->EventChar == '`' || data->EventChar == '~')
            return 1;
        return 0;
    }
}

Console::Console() {
    registerCommand("help", [this](const std::vector<std::string>&) {
        log("COMMANDS:");
        for (const auto& c : m_commands)
            log("  " + c.first);
    });
    registerCommand("clear", [this](const std::vector<std::string>&) {
        clear();
    });
    registerCommand("echo", [](const std::vector<std::string>& args) {
        std::string line;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) line += " ";
            line += args[i];
        }
        g_Console.log(line);
    });
    registerCommand("quit", [this](const std::vector<std::string>&) {
        if (m_quit) m_quit();
    });
}

void Console::toggle() {
    if (m_open) close(); else open();
}

void Console::open() {
    if (m_open) return;
    m_open = true;
    m_justOpened = true;
    m_autoScroll = true;
    m_input[0] = '\0';
}

void Console::close() {
    m_open = false;
}

void Console::log(const std::string& line) {
    m_lines.push_back(line);
    while (m_lines.size() > m_maxLines)
        m_lines.pop_front();
    m_autoScroll = true;
}

void Console::logError(const std::string& line) {
    log("[ERROR] " + line);
}

void Console::clear() {
    m_lines.clear();
    m_autoScroll = true;
}

void Console::registerCommand(const std::string& name, CommandFn fn) {
    m_commands.push_back({ name, std::move(fn) });
}

void Console::setQuitCallback(std::function<void()> quit) {
    m_quit = std::move(quit);
}

void Console::execute(const std::string& line) {
    if (line.empty()) return;

    log("> " + line);

    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd.empty()) return;

    std::vector<std::string> args;
    std::string arg;
    while (ss >> arg) args.push_back(arg);

    for (const auto& c : m_commands)
        if (c.first == cmd) {
            c.second(args);
            return;
        }
    log("[UNKNOWN] " + cmd + " (type 'help')");
}

void Console::draw(float screenWidth, float screenHeight) {
    if (!m_open) return;

    const float height = std::min(280.0f, screenHeight * 0.45f);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(screenWidth, height));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.76f, 0.76f, 0.76f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.96f, 0.96f, 0.96f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.90f, 0.90f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.78f, 0.78f, 0.78f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.65f, 0.65f, 0.65f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.70f, 0.70f, 0.70f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));

    if (ImGui::Begin("##EngineConsole", nullptr, flags)) {
        const float inputHeight = ImGui::GetFrameHeightWithSpacing();

        ImGui::BeginChild("##ConsoleLog", ImVec2(0.0f, -inputHeight - 4.0f), true,
            ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            ImGui::PushFont(nullptr);
            const bool atBottom = ImGui::GetScrollMaxY() < 1.0f
                || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

            for (const auto& line : m_lines) {
                ImVec4 color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
                if (line.rfind("[ERROR]", 0) == 0)
                    color = ImVec4(0.85f, 0.0f, 0.0f, 1.0f);
                else if (line.rfind("[UNKNOWN]", 0) == 0)
                    color = ImVec4(0.65f, 0.35f, 0.0f, 1.0f);
                else if (line.rfind(">", 0) == 0)
                    color = ImVec4(0.35f, 0.35f, 0.35f, 1.0f);

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopStyleColor();
            }

            if (m_autoScroll && atBottom)
                ImGui::SetScrollHereY(1.0f);
            ImGui::PopFont();
        }
        ImGui::EndChild();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        ImGui::TextUnformatted("> ");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 4.0f);

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (m_justOpened) {
            ImGui::SetKeyboardFocusHere();
            m_justOpened = false;
        }

        const bool submit = ImGui::InputText("##ConsoleInput", m_input, sizeof(m_input),
            ImGuiInputTextFlags_EnterReturnsTrue
            | ImGuiInputTextFlags_AutoSelectAll
            | ImGuiInputTextFlags_CallbackCharFilter,
            consoleCharFilter);

        if (submit) {
            std::string line(m_input);
            m_input[0] = '\0';
            execute(line);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(11);
    ImGui::PopStyleVar(5);

    m_autoScroll = false;
}
