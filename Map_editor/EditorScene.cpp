// EditorScene.cpp
#include "EditorScene.h"
#include "mapfile.h"
#include "fgd.h"
#include "renderpipeline.h"
#include <glad/glad.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

EditorScene::EditorScene() {
    // Load FGD
    std::string fgdPath = workspaceRoot() + "/GameRoot/kea.fgd";
    m_fgd.load(fgdPath.c_str());
}

bool EditorScene::loadMap(const std::string& path) {
    m_mapPath = path;
    m_entities.clear();
    m_lights.clear();
    m_selection.clear();
    m_anchorEntity = -1;

    bool ok = MapLoader::load(path.c_str(), m_level);
    if (!ok) {
        std::cerr << "Failed to load map: " << path << "\n";
        return false;
    }

    // Convert level entities to editor entities
    m_entities.reserve(m_level.entities.size());
    for (const auto& ent : m_level.entities) {
        EditorEntity e;
        e.base = ent;
        e.position = ent.origin;
        e.rotation = eulerToQuat(ent.angles);
        m_entities.push_back(e);
    }

    // Convert lights
    m_lights.reserve(m_level.pointLights.size());
    for (size_t i = 0; i < m_level.pointLights.size(); ++i) {
        EditorLight l;
        l.base = m_level.pointLights[i];
        // Find matching entity
        for (size_t j = 0; j < m_entities.size(); ++j) {
            if (m_entities[j].base.classname == "light" &&
                glm::distance(m_entities[j].position, l.base.position) < 0.1f) {
                l.entityIndex = (int)j;
                break;
            }
        }
        m_lights.push_back(l);
    }

    // Load sidecar
    std::string sidecar = sidecarPathFor(path);
    std::ifstream sc(sidecar);
    if (sc.good()) {
        m_lighting = LightingSettings::loadFromFile(sidecar.c_str());
        m_postState = PostProcessState::loadFromFile(sidecar.c_str());
    } else {
        m_lighting = LightingSettings::defaults();
        m_postState = PostProcessState::defaults();
    }

    // Apply post state to render pipeline
    RenderPipeline::setSunDirection(m_lighting.sunDir);
    RenderPipeline::setSunColor(m_lighting.sunColor);
    RenderPipeline::setSunIntensity(m_lighting.sunIntensity);
    RenderPipeline::setAmbient(m_lighting.ambientColor, m_lighting.ambientIntensity);
    RenderPipeline::setExposureEV(m_postState.exposureEV);
    RenderPipeline::setSSAOEnabled(m_postState.ssaoEnabled);
    RenderPipeline::setSSREnabled(m_postState.ssrEnabled);
    RenderPipeline::setBloomEnabled(m_postState.bloomEnabled);
    RenderPipeline::setSSAOParams(m_postState.ssaoStrength, m_postState.ssaoRadius);
    RenderPipeline::setSSRParams(m_postState.ssrStrength, m_postState.ssrMaxDist);
    RenderPipeline::setBloomParams(m_postState.bloomThreshold, m_postState.bloomStrength);

    rebuildRenderChunks();
    return true;
}

void EditorScene::saveMap(const std::string& path) {
    // Sync entities back to level
    m_level.entities.clear();
    m_level.entities.reserve(m_entities.size());
    for (const auto& e : m_entities) {
        MapEntity ent = e.base;
        ent.origin = e.position;
        ent.angles = quatToEuler(e.rotation);
        m_level.entities.push_back(ent);
    }

    // Sync lights
    m_level.pointLights.clear();
    m_level.pointLights.reserve(m_lights.size());
    for (const auto& l : m_lights) {
        m_level.pointLights.push_back(l.base);
    }

    bool ok = MapLoader::save(path.c_str(), m_level);
    if (!ok) {
        std::cerr << "Failed to save map: " << path << "\n";
    }

    // Save sidecar
    std::string sidecar = sidecarPathFor(path);
    m_lighting.saveToFile(sidecar.c_str());
    m_postState.saveToFile(sidecar.c_str());
}

int EditorScene::addEntity(const MapEntity& ent) {
    EditorEntity e;
    e.base = ent;
    e.position = ent.origin;
    e.rotation = eulerToQuat(ent.angles);
    int idx = (int)m_entities.size();
    m_entities.push_back(e);
    return idx;
}

void EditorScene::removeEntity(int index) {
    if (index < 0 || index >= (int)m_entities.size()) return;
    m_entities.erase(m_entities.begin() + index);

    // Fix light indices
    for (auto& l : m_lights) {
        if (l.entityIndex == index) l.entityIndex = -1;
        else if (l.entityIndex > index) l.entityIndex--;
    }

    // Fix selection
    auto it = std::find(m_selection.begin(), m_selection.end(), index);
    if (it != m_selection.end()) m_selection.erase(it);
    for (auto& sel : m_selection) if (sel > index) sel--;
    if (m_anchorEntity == index) m_anchorEntity = -1;
    else if (m_anchorEntity > index) m_anchorEntity--;
}

void EditorScene::removeEntities(const std::vector<int>& indices) {
    std::vector<int> sorted = indices;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int idx : sorted) removeEntity(idx);
}

void EditorScene::duplicateEntities(const std::vector<int>& indices) {
    std::vector<int> newIndices;
    for (int idx : indices) {
        if (idx < 0 || idx >= (int)m_entities.size()) continue;
        EditorEntity e = m_entities[idx];
        e.position += glm::vec3(16, 0, 16); // Offset
        e.base.origin = e.position;
        int newIdx = (int)m_entities.size();
        m_entities.push_back(e);
        newIndices.push_back(newIdx);
    }
    clearSelection();
    for (int idx : newIndices) selectEntity(idx, true);
}

void EditorScene::selectEntity(int index, bool additive) {
    if (index < 0 || index >= (int)m_entities.size()) return;
    if (!additive) clearSelection();
    if (std::find(m_selection.begin(), m_selection.end(), index) == m_selection.end()) {
        m_selection.push_back(index);
    }
    m_anchorEntity = index;
}

void EditorScene::deselectEntity(int index) {
    auto it = std::find(m_selection.begin(), m_selection.end(), index);
    if (it != m_selection.end()) {
        m_selection.erase(it);
        if (m_anchorEntity == index) {
            m_anchorEntity = m_selection.empty() ? -1 : m_selection.back();
        }
    }
}

void EditorScene::clearSelection() {
    m_selection.clear();
    m_anchorEntity = -1;
}

void EditorScene::selectAll() {
    m_selection.clear();
    m_selection.reserve(m_entities.size());
    for (int i = 0; i < (int)m_entities.size(); ++i) m_selection.push_back(i);
    m_anchorEntity = m_selection.empty() ? -1 : m_selection.back();
}

void EditorScene::setEntityPosition(int index, const glm::vec3& pos) {
    if (index < 0 || index >= (int)m_entities.size()) return;
    m_entities[index].position = pos;
    m_entities[index].base.origin = pos;
}

void EditorScene::setEntityRotation(int index, const glm::quat& rot) {
    if (index < 0 || index >= (int)m_entities.size()) return;
    m_entities[index].rotation = rot;
    m_entities[index].base.angles = quatToEuler(rot);
}

void EditorScene::setEntityScale(int index, float scale) {
    if (index < 0 || index >= (int)m_entities.size()) return;
    m_entities[index].scale = scale;
}

void EditorScene::translateEntities(const std::vector<int>& indices, const glm::vec3& delta) {
    for (int idx : indices) {
        if (idx >= 0 && idx < (int)m_entities.size()) {
            m_entities[idx].position += delta;
            m_entities[idx].base.origin = m_entities[idx].position;
        }
    }
}

void EditorScene::rotateEntities(const std::vector<int>& indices, const glm::quat& delta, const glm::vec3& pivot) {
    for (int idx : indices) {
        if (idx >= 0 && idx < (int)m_entities.size()) {
            auto& e = m_entities[idx];
            glm::vec3 localPos = e.position - pivot;
            localPos = delta * localPos;
            e.position = pivot + localPos;
            e.rotation = delta * e.rotation;
            e.base.origin = e.position;
            e.base.angles = quatToEuler(e.rotation);
        }
    }
}

void EditorScene::scaleEntities(const std::vector<int>& indices, float factor, const glm::vec3& pivot) {
    for (int idx : indices) {
        if (idx >= 0 && idx < (int)m_entities.size()) {
            auto& e = m_entities[idx];
            glm::vec3 localPos = e.position - pivot;
            localPos *= factor;
            e.position = pivot + localPos;
            e.scale *= factor;
            e.base.origin = e.position;
        }
    }
}

bool EditorScene::computeBounds(glm::vec3& outMin, glm::vec3& outMax) const {
    if (m_level.collisionVertices.empty()) return false;
    outMin = {1e9f, 1e9f, 1e9f};
    outMax = {-1e9f, -1e9f, -1e9f};
    for (size_t i = 0; i + 2 < m_level.collisionVertices.size(); i += 3) {
        glm::vec3 p(m_level.collisionVertices[i], m_level.collisionVertices[i+1], m_level.collisionVertices[i+2]);
        outMin = glm::min(outMin, p);
        outMax = glm::max(outMax, p);
    }
    return true;
}

int EditorScene::pickEntity(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDist, float& outDist) const {
    int bestIdx = -1;
    float bestDist = maxDist;
    for (int i = 0; i < (int)m_entities.size(); ++i) {
        const auto& e = m_entities[i];
        // Simple sphere test
        glm::vec3 toCenter = e.position - rayOrigin;
        float proj = glm::dot(toCenter, rayDir);
        if (proj < 0 || proj > maxDist) continue;
        glm::vec3 closest = rayOrigin + rayDir * proj;
        float dist = glm::length(closest - e.position);
        if (dist < 1.0f && proj < bestDist) {
            bestDist = proj;
            bestIdx = i;
        }
    }
    outDist = bestDist;
    return bestIdx;
}

void EditorScene::rebuildRenderChunks() {
    m_renderChunks.clear();
    for (const auto& pair : m_level.renderChunks) {
        const std::string& texName = pair.first;
        const std::vector<float>& data = pair.second;
        if (data.empty()) continue;

        RenderChunk chunk;
        chunk.textureName = texName;
        chunk.vertexData = data;
        chunk.vertexCount = (int)(data.size() / 8);
        chunk.dirty = true;
        m_renderChunks[texName] = std::move(chunk);
    }
}

// Helpers
glm::quat eulerToQuat(const glm::vec3& euler) {
    glm::quat q = glm::quat(glm::radians(euler));
    return q;
}

glm::vec3 quatToEuler(const glm::quat& q) {
    return glm::degrees(glm::eulerAngles(q));
}

std::string workspaceRoot() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string dir = std::string(buf, n);
    size_t sep = dir.find_last_of("/\\");
    if (sep != std::string::npos) dir = dir.substr(0, sep);
    for (;;) {
        std::ifstream probe((dir + "/GameRoot/kea.fgd").c_str());
        if (probe.good()) return dir;
        size_t up = dir.find_last_of("/\\");
        if (up == std::string::npos) return dir;
        dir = dir.substr(0, up);
    }
}

std::string sidecarPathFor(const std::string& mapPath) {
    size_t dot = mapPath.find_last_of('.');
    return (dot == std::string::npos ? mapPath : mapPath.substr(0, dot)) + ".lighting.json";
}