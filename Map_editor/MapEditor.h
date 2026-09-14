// MapEditor.h - Main Editor Application
#pragma once
#include <string>
#include <memory>

class EditorCore;
class EditorCamera;
class EditorRenderer;
class EditorUI;
class EditorScene;
class EditorHistory;
class EditorMap;

class MapEditor {
public:
    MapEditor();
    ~MapEditor();

    bool init();
    void run();
    void shutdown();

    EditorCore* core() { return m_core.get(); }
    EditorCamera* camera() { return m_camera.get(); }
    EditorRenderer* renderer() { return m_renderer.get(); }
    EditorUI* ui() { return m_ui.get(); }
    EditorScene* scene() { return m_scene.get(); }
    EditorHistory* history() { return m_history.get(); }
    EditorMap* map() { return m_map.get(); }

    bool shouldClose() const;
    void requestClose();

private:
    std::unique_ptr<EditorCore> m_core;
    std::unique_ptr<EditorCamera> m_camera;
    std::unique_ptr<EditorRenderer> m_renderer;
    std::unique_ptr<EditorUI> m_ui;
    std::unique_ptr<EditorScene> m_scene;
    std::unique_ptr<EditorHistory> m_history;
    std::unique_ptr<EditorMap> m_map;

    bool m_running = true;
};