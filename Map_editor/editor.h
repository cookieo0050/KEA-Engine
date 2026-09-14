// ============================================================================
// editor.h - Shared editor state, camera, and helpers
// ============================================================================
// The Map Editor was split into modules. editor.h/editor.cpp own the central
// EditorContext (everything main()'s lambdas captured before the split: level
// data, transform state, selection, undo history, settings) plus small helpers
// (coordinate round-trips, quaternion/euler conversion, the Unity-style theme).
// Renderer, panels and main.cpp all talk through the EditorContext.
// ============================================================================
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <vector>
#include <string>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "imgui.h"
#include "mapfile.h"
#include "texture.h"
#include "fgd.h"
#include "renderpipeline.h"

// ----------------------------------------------------------------------------
// Coordinate + formatting helpers (implemented in editor.cpp)
// ----------------------------------------------------------------------------
constexpr float kMapScale = 1.0f / 32.0f;   // metres per map unit (matches engine)
constexpr int   kMaxLights = 16;
constexpr int   kShadowRes = 2048;

std::string workspaceRoot();
void        ApplyUnityTheme();

// Inverse of the engine's convertPos: engine pos (y-up, metres) -> .map origin
// (z-up, map units). Also handy for serializing glider edits.
glm::vec3 engineToMapOrigin(const glm::vec3& enginePos);
// Forward of the engine's convertPos: .map origin -> engine pos. Used to place
// point-entity markers in the viewport.
glm::vec3 mapOriginToEngine(const glm::vec3& mapOrigin);

std::string          formatVec3(const glm::vec3& v);
std::vector<float>   parseVec3(const std::string& s);
glm::vec3            parseColorFromString(const std::string& s);
std::string          fmt3(const glm::vec3& v);

// .map stores orientation as "Pitch Yaw Roll" (map space) which maps to
// engine space as: map pitch==engine pitch, map yaw==engine yaw, map roll==-engine
// roll. Rotation matrix R = Ry(roll)*Rz(yaw)*Rx(pitch) matches ImGuizmo's order.
glm::quat eulerToQuat(float pitchDeg, float yawDeg, float rollDeg);
void      quatToEuler(const glm::quat& q, float& pitch, float& yaw, float& roll);

// ----------------------------------------------------------------------------
// Camera / chunk structs
// ----------------------------------------------------------------------------
struct EditorCamera {
    glm::vec3 position = glm::vec3(0.0f, 8.0f, 0.0f);
    glm::vec3 front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    float yaw = -90.0f;
    float pitch = 0.0f;
    float speed = 10.0f;
    float sensitivity = 0.1f;

    void updateVectors() {
        front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
        front.y = sin(glm::radians(pitch));
        front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
        front = glm::normalize(front);
    }

    glm::mat4 view() const {
        return glm::lookAt(position, position + front, up);
    }

    void processKeyboard(GLFWwindow* window, float dt) {
        float boost = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ? 4.0f : 1.0f;
        float s = speed * boost * dt;
        glm::vec3 right = glm::normalize(glm::cross(front, up));
        glm::vec3 flatFront = glm::normalize(glm::vec3(front.x, 0.0f, front.z));

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) position += flatFront * s;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) position -= flatFront * s;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) position += right * s;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) position -= right * s;
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) position += up * s;
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) position -= up * s;
    }
};

struct LevelChunk {
    Texture texture;
    GLuint VAO = 0, VBO = 0;
    int vertexCount = 0;
};

// ----------------------------------------------------------------------------
// Gizmo state persisted into imgui.ini via a custom settings handler.
// ----------------------------------------------------------------------------
struct EditorSettings {
    bool snapEnabled = true;
    int  gizmoOp = 0;      // 0 translate, 1 rotate, 2 scale
    bool gizmoLocal = false;
};
extern EditorSettings s_editorSettings;
void EditorSettings_Register();

// GLFW mouse state shared with the callbacks and the 3D viewport.
extern bool s_rightMouse;
extern bool s_viewportHovered;

void editorCursorPosCallback(GLFWwindow* window, double xpos, double ypos);
void editorFramebufferSizeCallback(GLFWwindow* window, int width, int height);

// ----------------------------------------------------------------------------
// Undo / redo snapshot
// ----------------------------------------------------------------------------
struct EditorStateSnapshot {
    std::vector<PointLight> pointLights;
    std::vector<MapEntity> entities;
    bool hasPlayerStart = false;
    glm::vec3 playerStart{ 0.0f, 1.0f, 0.0f };
    std::vector<glm::quat> entityRot;
    std::vector<float> entityScale;
    LightingState lighting;
    PostProcessState postState;
    std::vector<bool> lightDirty;
    std::vector<bool> entityDirty;
};

// ----------------------------------------------------------------------------
// 2D orthographic view state (Trenchbroom-style Top/Front/Side windows)
// ----------------------------------------------------------------------------
enum class OrthoPlane { Top = 0, Front = 1, Side = 2 };
constexpr int kOrthoViewCount = 3;

struct OrthoViewState {
    glm::vec3 center{ 0.0f };   // point the camera looks at (plane coords used per axis)
    float zoom = 60.0f;         // pixels per world unit
};

// ----------------------------------------------------------------------------
// Central editor context. Everything that main()'s lambdas used to capture now
// lives here; the panel/renderer modules read and write through it.
// ----------------------------------------------------------------------------
struct EditorContext {
    // ---- data -------------------------------------------------------------
    std::string mapPath;
    std::string texturesFolder;
    LevelData level;
    FgdFile fgd;

    // ---- entity / transform state -----------------------------------------
    std::vector<int> lightEntityIndices;   // indices into level.entities for "light"
    std::vector<bool> lightDirty;          // per light: keyvalues need re-writing on Save
    std::vector<bool> entityDirty;         // per entity: gizmo moved origin/angles/scale
    std::vector<glm::quat> entityRot;      // per entity, engine space
    std::vector<float> entityScale;        // per entity, uniform
    LightingState lighting;

    // ---- scene ------------------------------------------------------------
    glm::vec3 sceneCenter{ 0.0f };
    float sceneRadius = 20.0f;

    // ---- selection --------------------------------------------------------
    std::vector<int> selection;            // every selected entity index
    int selectedEntity = -1;               // anchor (last-clicked, gizmo target)
    int selectedLight = -1;                // synced from selectedEntity when a light
    bool shadowsEnabled = true;

    // ---- undo / redo ------------------------------------------------------
    std::vector<EditorStateSnapshot> undoStack;
    std::vector<EditorStateSnapshot> redoStack;
    const size_t kMaxUndo = 64;
    EditorStateSnapshot s_undoPre;
    bool s_undoPreValid = false;

    // ---- gizmo drag-start snapshots ---------------------------------------
    std::vector<glm::vec3> s_dragStartPos;
    std::vector<glm::quat> s_dragStartRot;
    std::vector<float> s_dragStartScale;
    bool s_gizmoWasUsing = false;

    // ---- marquee ----------------------------------------------------------
    bool s_marqueeActive = false;
    ImVec2 s_marqueeStart{ 0.0f, 0.0f };
    bool s_marqueeCtrl = false;
    std::vector<int> s_marqueeBase;

    // ---- cameras ----------------------------------------------------------
    EditorCamera camera;
    OrthoViewState ortho[kOrthoViewCount];  // Top, Front, Side

    PostProcessState postState;             // active post settings (edited by UI)

    // ---- entity helpers (editor.cpp) --------------------------------------
    glm::vec3 entityEnginePos(int idx) const;
    void setEntityEnginePos(int idx, const glm::vec3& enginePos);
    float markerSizeFor(const std::string& classname) const;
    glm::vec3 markerColorFor(const std::string& classname) const;
    void setEntityAngles(int idx, const glm::quat& q);
    void setEntityScale(int idx, float s);
    void initEntityTransforms();            // parse angles/scale + normalize light colors

    // ---- selection helpers (editor.cpp) -----------------------------------
    glm::vec3 selectionCenter() const;
    void syncLight();
    void selectOnly(int idx);
    void toggleSelect(int idx);
    void clearSelection();
    void setSelection(const std::vector<int>& sel);
    bool isSelected(int idx) const;

    // ---- undo / redo (editor.cpp) -----------------------------------------
    EditorStateSnapshot captureState() const;
    void restoreState(const EditorStateSnapshot& s);
    void pushUndoState(const EditorStateSnapshot& s);
    void pushUndo();
    void doUndo();
    void doRedo();
    void trackWidgetUndo(bool activated, bool deactivated, bool deactivatedAfterEdit);
};