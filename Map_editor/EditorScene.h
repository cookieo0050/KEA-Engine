// EditorScene.h - Scene data, entities, selection
#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include "mapfile.h"
#include "fgd.h"

struct EditorEntity {
    MapEntity base;
    glm::vec3 position = {0, 0, 0};
    glm::quat rotation = {0, 0, 0, 1};
    float scale = 1.0f;
    bool visible = true;
    bool selected = false;
};

struct EditorLight {
    PointLight base;
    int entityIndex = -1; // Index into entities
    bool selected = false;
};

struct EditorStateSnapshot {
    std::vector<EditorEntity> entities;
    std::vector<EditorLight> lights;
    bool hasPlayerStart = false;
    glm::vec3 playerStart = {0, 1, 0};
    LightingSettings lighting;
    PostProcessState postState;
    std::vector<int> selection;
};

class EditorScene {
public:
    EditorScene();
    ~EditorScene() = default;

    // Map loading
    bool loadMap(const std::string& path);
    void saveMap(const std::string& path);

    // Entity management
    int addEntity(const MapEntity& ent);
    void removeEntity(int index);
    void removeEntities(const std::vector<int>& indices);
    void duplicateEntities(const std::vector<int>& indices);

    // Selection
    void selectEntity(int index, bool additive = false);
    void deselectEntity(int index);
    void clearSelection();
    void selectAll();
    const std::vector<int>& selection() const { return m_selection; }
    int anchorEntity() const { return m_anchorEntity; }
    void setAnchorEntity(int idx) { m_anchorEntity = idx; }

    // Transform
    void setEntityPosition(int index, const glm::vec3& pos);
    void setEntityRotation(int index, const glm::quat& rot);
    void setEntityScale(int index, float scale);
    void translateEntities(const std::vector<int>& indices, const glm::vec3& delta);
    void rotateEntities(const std::vector<int>& indices, const glm::quat& delta, const glm::vec3& pivot);
    void scaleEntities(const std::vector<int>& indices, float factor, const glm::vec3& pivot);

    // Getters
    const std::vector<EditorEntity>& entities() const { return m_entities; }
    std::vector<EditorEntity>& entities() { return m_entities; }
    const std::vector<EditorLight>& lights() const { return m_lights; }
    std::vector<EditorLight>& lights() { return m_lights; }
    const LevelData& level() const { return m_level; }
    LevelData& level() { return m_level; }
    const FGDDatabase& fgd() const { return m_fgd; }
    FGDDatabase& fgd() { return m_fgd; }

    bool hasPlayerStart() const { return m_level.hasPlayerStart; }
    const glm::vec3& playerStart() const { return m_level.playerStart; }
    void setPlayerStart(const glm::vec3& pos) { m_level.playerStart = pos; m_level.hasPlayerStart = true; }

    const LightingSettings& lighting() const { return m_lighting; }
    LightingSettings& lighting() { return m_lighting; }
    const PostProcessState& postState() const { return m_postState; }
    PostProcessState& postState() { return m_postState; }

    // Scene bounds
    bool computeBounds(glm::vec3& outMin, glm::vec3& outMax) const;

    // Raycast picking
    int pickEntity(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDist, float& outDist) const;

    // Texture chunks for rendering
    struct RenderChunk {
        std::string textureName;
        std::vector<float> vertexData; // 8 floats: pos(3) normal(3) uv(2)
        GLuint vao = 0, vbo = 0;
        int vertexCount = 0;
        bool dirty = true;
    };
    const std::map<std::string, RenderChunk>& renderChunks() const { return m_renderChunks; }
    std::map<std::string, RenderChunk>& renderChunks() { return m_renderChunks; }
    void rebuildRenderChunks();

private:
    LevelData m_level;
    FGDDatabase m_fgd;
    std::vector<EditorEntity> m_entities;
    std::vector<EditorLight> m_lights;
    std::vector<int> m_selection;
    int m_anchorEntity = -1;
    LightingSettings m_lighting;
    PostProcessState m_postState;
    std::map<std::string, RenderChunk> m_renderChunks;
    std::string m_mapPath;
};