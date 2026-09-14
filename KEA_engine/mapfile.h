#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <map>
#include <functional>

struct PointLight {
    glm::vec3 position;
    glm::vec3 color;
    float intensity;
    float radius;
};

struct MapEntity {
    std::string classname;
    std::map<std::string, std::string> properties;
};

struct LevelData {
    std::map<std::string, std::vector<float>> renderChunks;
    std::vector<float> collisionVertices;
    bool hasPlayerStart = false;
    glm::vec3 playerStart{ 0.0f, 1.0f, 0.0f };
    std::vector<PointLight> pointLights;
    std::vector<MapEntity> entities;
};

struct LightingState {
    float sunPitch = -45.0f;      // elevation in degrees (sun looks down)
    float sunYaw = 0.0f;          // azimuth in degrees
    glm::vec3 sunColor = glm::vec3(1.0f, 0.95f, 0.85f);  // warm white
    float sunIntensity = 1.0f;
    glm::vec3 ambientColor = glm::vec3(0.35f);
    float ambientIntensity = 1.0f;
};

// Sidecar lighting config (.lighting.cfg) shared by the editor and the runtime.
// Key=value text file living next to the .map (e.g. Testroom.lighting.cfg).
std::string sidecarPathFor(const std::string& mapPath);
void saveLightingSidecar(const std::string& mapPath, const LightingState& ls);
void loadLightingSidecar(const std::string& mapPath, LightingState& ls);

class MapLoader {
public:
    static LevelData load(const std::string& path,
        const std::function<glm::ivec2(const std::string&)>& textureSizeLookup);
};