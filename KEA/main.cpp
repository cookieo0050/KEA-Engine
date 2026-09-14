// ============================================================================
// main.cpp - Engine Entry Point and Main Render Loop
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// This is the "brain" of the engine. When the program starts, execution begins
// in main() at the bottom of this file. Everything the engine does happens
// either (a) during startup inside main() before the loop, or (b) once per
// frame inside the big `while (!window.shouldClose())` loop.
//
// HOW TO READ THIS FILE (read it top to bottom in this order)
// ----------------------------------------------------------------------------
// 1. The #include list        - tells you which other engine files this file uses.
// 2. The path constants       - hard-coded folder/file locations on disk
//    (TEXTURES_FOLDER, MAP_PATH, FONT_PATH, ...).
// 3. The SHADER SOURCE CODES  - the GLSL shader programs are written here as raw
//    C strings. GLSL is a separate GPU language, stored inline so the engine has
//    no external .glsl files to load.
// 4. Helper callbacks         - mouse_callback, glfw_error_callback.
// 5. main()                   - setup, then the frame loop, then cleanup.
//
// THE RENDER LOOP (the heart of the engine)
// ----------------------------------------------------------------------------
// Every frame the engine runs these passes in order. Each pass is numbered in a
// comment in the code. Think of them as stages in a factory line:
//
//   1. SUN SHADOW PASS      Render the map from the sun's point of view into a
//                           depth texture (shadowMap). Only stores distance-to-sun.
//   2. POINT LIGHT SHADOW   Same idea but for each coloured point light, stored in
//                           a cube map (6 directions). Runs ONLY ONCE (cached).
//   3. G-BUFFER PASS        Render the map into 3 textures: position, normal and
//                           colour (albedo). This is "deferred rendering" - we keep
//                           the raw surface data instead of lighting it immediately.
//   4. SSAO PASS             A single "post" pass that reads the G-Buffer and darkens
//                           corners/cracks (source of the soft contact shading), denoised
//                           with a depth/normal-aware bilateral blur. Runs at half res.
//   5. BLIT DEPTH BUFFER    Copies the G-Buffer depth into the HDR scene target so
//                           the skybox can depth-test later.
//   6. DEFERRED LIGHTING    Reads the G-Buffer + shadows + AO + GI and computes the
//                           final lit colour (in linear HDR) for every pixel into the
//                           RGB16F scene buffer. (Or shows a debug view.)
//   7. SKYBOX               Draws the sky behind everything (into the scene buffer).
//   8. POST-PROCESSING      Reads the HDR scene, applies exposure (2^EV), a filmic
//                           ACES tonemap, gamma curve and vignette, writes LDR.
//   9. FXAA                 2-pass anti-aliasing of the final image to the screen.
//  10. CROSSHAIR            A small texture drawn in the centre of the screen.
//  11. IMGUI OVERLAY        The "Debug Control Center" (F1) + developer console (~).
//
// KEY IDEAS TO UNDERSTAND
// ----------------------------------------------------------------------------
// - "Deferred rendering" vs "forward": forward lights each object immediately;
//   deferred saves geometry data to textures first, then lights the whole screen.
// - GLSL shader strings: search for gbufferFragSrc etc. These run on the GPU.
// - A "pass" is one full draw of something into a texture before compositing.
// - The debug panel (F1) lets you flip between Final / Position / Normal / Albedo
//   views of the G-Buffer - great way to see what each pass actually produces.
// - The console (~) is defined in console.cpp; typing `help` lists commands.
//
// COMMON MISTAKES WHEN EDITING
// ----------------------------------------------------------------------------
// - If you change a shader string, the error shows in the console/terminal.
// - If you move a file, update the hard-coded paths at the top of main().
// - Push/Pop of ImGui styles must be balanced (see console.cpp for a past bug).
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include "window.h"
#include "camera.h"
#include "shader.h"
#include "collision.h"
#include "debugdraw.h"
#include "skybox.h"
#include "texture.h"
#include "mapfile.h"
#include "shadow.h"
#include "gbuffer.h"
#include "ssao.h"
#include "renderpipeline.h"
#include "player.h"
#include "jolt_world.h"
#include "console.h"
#include "fgd.h"
#include "objmesh.h"
#include "audio.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

using namespace std;

static string workspaceRoot() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    string dir = string(buf, n);
    size_t sep = dir.find_last_of("/\\");
    if (sep != string::npos) dir = dir.substr(0, sep);
    for (;;) {
        ifstream probe((dir + "/GameRoot/kea.fgd").c_str());
        if (probe.good()) return dir;
        size_t up = dir.find_last_of("/\\");
        if (up == string::npos) return dir;
        dir = dir.substr(0, up);
    }
}

const int MAX_LIGHTS = 16;
const unsigned int SHADOW_CUBE_RES = 1024;

Camera camera;
Player player;
float lastX = 400, lastY = 300;
bool firstMouse = true;
bool mouseLookEnabled = true;

const string TEXTURES_FOLDER = workspaceRoot() + "/GameRoot/textures/";
const string MAP_PATH = workspaceRoot() + "/Maps/Testroom.map";
const string FGD_PATH = workspaceRoot() + "/GameRoot/kea.fgd";
const string CROSSHAIR_PATH = workspaceRoot() + "/Images/Crosshair.png";
const string FONT_PATH = workspaceRoot() + "/oldschool_pc_font_pack_v2.2_FULL/ttf - Px (pixel outline)/Px437_IBM_VGA_8x16.ttf";
const string MODELS_FOLDER = workspaceRoot() + "/GameRoot/models/";

// ==========================================
// Generic mesh-entity helpers
// ==========================================
// Any map entity whose FGD class declares model({ "path": <key> }) is loaded
// from MODELS_FOLDER and placed in the world using the SAME conversions the
// editor uses, so what you see in the editor matches the game:
//   - origin:   map (z-up, in map units) -> engine (y-up, in metres)
//   - rotation: map "Pitch Yaw Roll" -> engine quaternion (map roll is negated)
//   - scale:    uniform, from the FGD-declared scale keyvalue (default 1)
// The model matrix lives alongside the mesh so the render passes can draw it.
// ==========================================

static const float MAP_SCALE = 1.0f / 32.0f;

static glm::vec3 mapOriginToEngine(const glm::vec3& mapOrigin) {
    return glm::vec3(mapOrigin.x, mapOrigin.z, -mapOrigin.y) * MAP_SCALE;
}

// Engine rotation R = Ry(rollDeg) * Rz(yawDeg) * Rx(pitchDeg) - mirrors the
// editor's eulerToQuat helper. Call with (pitch, yaw, -roll) for map angles.
static glm::quat mapEulerToQuat(float pitchDeg, float yawDeg, float rollDeg) {
    glm::mat4 R = glm::mat4(1.0f);
    R = glm::rotate(R, glm::radians(rollDeg), glm::vec3(0.0f, 1.0f, 0.0f));   // Ry
    R = glm::rotate(R, glm::radians(yawDeg), glm::vec3(0.0f, 0.0f, 1.0f));    // Rz
    R = glm::rotate(R, glm::radians(pitchDeg), glm::vec3(1.0f, 0.0f, 0.0f));  // Rx
    return glm::quat_cast(R);
}

static vector<float> parseFloatList(const string& s) {
    vector<float> out;
    stringstream ss(s);
    float f;
    while (ss >> f) out.push_back(f);
    return out;
}

struct SpawnedProp {
    ObjMesh mesh;
    glm::mat4 modelMatrix = glm::mat4(1.0f);
};

// Loads every entity in `level` whose FGD class has a model path keyvalue.
// Returns the props placed in ENGINE space (positions already scaled/rotated).
static vector<SpawnedProp> spawnMeshEntities(const LevelData& level, const FgdFile& fgd) {
    vector<SpawnedProp> props;

    for (size_t i = 0; i < level.entities.size(); ++i) {
        const MapEntity& ent = level.entities[i];
        const FgdClass* cls = fgd.find(ent.classname);
        if (!cls || cls->modelPathKey.empty()) continue;

        auto mit = ent.properties.find(cls->modelPathKey);
        if (mit == ent.properties.end() || mit->second.empty()) continue;

        SpawnedProp sp;
        string meshPath = MODELS_FOLDER + mit->second;
        if (!sp.mesh.loadFromObj(meshPath, TEXTURES_FOLDER)) {
            cout << "Failed to load model '" << meshPath << "' for entity "
                << ent.classname << ": " << sp.mesh.error << "\n";
            g_Console.logError("Failed to load model '" + meshPath + "' for entity "
                + ent.classname + ": " + sp.mesh.error);
            continue;
        }

        // Origin (map units -> engine metres).
        glm::vec3 mapOrigin(0.0f);
        auto oit = ent.properties.find("origin");
        if (oit != ent.properties.end()) {
            vector<float> o = parseFloatList(oit->second);
            if (o.size() >= 3) mapOrigin = glm::vec3(o[0], o[1], o[2]);
        }

        // Rotation: "angles P Y R" overrides single-axis "angle" (Quake style).
        glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
        auto ait = ent.properties.find("angles");
        if (ait != ent.properties.end()) {
            vector<float> a = parseFloatList(ait->second);
            if (a.size() >= 3) rot = mapEulerToQuat(a[0], a[1], -a[2]);   // map roll = -engine roll
            else if (a.size() >= 1) rot = mapEulerToQuat(0.0f, a[0], 0.0f);
        }
        else {
            auto git = ent.properties.find("angle");
            if (git != ent.properties.end()) {
                vector<float> g = parseFloatList(git->second);
                if (!g.empty()) rot = mapEulerToQuat(0.0f, g[0], 0.0f);
            }
        }

        // Uniform scale from the FGD-declared keyvalue (fallback "scale").
        float sc = 1.0f;
        auto sit = ent.properties.find(cls->modelScaleKey.empty() ? "scale" : cls->modelScaleKey);
        if (sit != ent.properties.end()) {
            vector<float> s = parseFloatList(sit->second);
            if (!s.empty() && s[0] > 0.0001f) sc = s[0];
        }

        glm::mat4 m = glm::mat4(1.0f);
        m = glm::translate(m, mapOriginToEngine(mapOrigin));
        m = m * glm::mat4_cast(rot);
        m = glm::scale(m, glm::vec3(sc));
        sp.modelMatrix = m;

        cout << "Spawned mesh entity '" << ent.classname << "' -> " << meshPath
            << " (" << sp.mesh.vertexCount << " verts)\n";
        g_Console.log("Spawned mesh entity '" + ent.classname + "' -> " + meshPath);

        props.push_back(sp);
    }

    return props;
}

// ==========================================
// SHADER SOURCE CODES
// ==========================================

const char* gbufferVertSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
void main() {
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(model))) * aNormal;
    TexCoord = aTexCoord;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
)";

const char* gbufferFragSrc = R"(
#version 330 core
layout (location = 0) out vec3 gPosition;
layout (location = 1) out vec3 gNormal;
layout (location = 2) out vec4 gAlbedoSpec;
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;
uniform sampler2D texture1;
void main() {
    gPosition = FragPos;
    gNormal = normalize(Normal);
    gAlbedoSpec = vec4(texture(texture1, TexCoord).rgb, 1.0);
}
)";

const char* quadVertSrc = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUV;
out vec2 TexCoords;
void main() {
    TexCoords = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Point Light Shadow Depth Pass Shaders (No Geometry Shader Needed)
const char* pointShadowVertSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 model;
uniform mat4 shadowMatrix;
out vec4 FragPos;
void main() {
    FragPos = model * vec4(aPos, 1.0);
    gl_Position = shadowMatrix * FragPos;
}
)";

const char* pointShadowFragSrc = R"(
#version 330 core
in vec4 FragPos;
uniform vec3 lightPos;
uniform float farPlane;

void main() {
    float lightDistance = length(FragPos.xyz - lightPos);
    lightDistance = lightDistance / farPlane;
    gl_FragDepth = lightDistance;
}
)";

const char* lightingFragSrc = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform sampler2D ssao;
uniform sampler2D shadowMapTex;

#define MAX_LIGHTS 16
uniform samplerCube pointShadowMaps[MAX_LIGHTS];

uniform mat4 lightSpaceMatrix;
uniform vec3 sunDir;
uniform vec3 sunColor;
uniform float sunIntensity;
uniform vec3 viewPos;

uniform vec3 lightPositions[MAX_LIGHTS];
uniform vec3 lightColors[MAX_LIGHTS];
uniform float lightIntensities[MAX_LIGHTS];
uniform float lightRadii[MAX_LIGHTS];
uniform int numLights;

const vec2 poissonDisk[8] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
    vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379)
);

float computeSunShadow(vec3 fragPos, vec3 normal, vec3 lightDir) {
    vec4 fragPosLightSpace = lightSpaceMatrix * vec4(fragPos + normal * 0.005, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    if (projCoords.z > 1.0 || projCoords.z < 0.0) return 0.0;

    float bias = max(0.001 * (1.0 - dot(normal, lightDir)), 0.0001);
    float shadow = 0.0;
    float softRadius = 1.0;
    vec2 texelSize = 1.0 / textureSize(shadowMapTex, 0);
    float softEdge = 0.003;

    for (int i = 0; i < 8; i++) {
        vec2 offset = poissonDisk[i] * texelSize * softRadius;
        float pcfDepth = texture(shadowMapTex, projCoords.xy + offset).r;
        float diff = (projCoords.z - bias) - pcfDepth;
        shadow += smoothstep(-softEdge, softEdge, diff);
    }
    return shadow / 8.0;
}

float computePointShadow(int index, vec3 fragPos, vec3 lightPos, float farPlane, vec3 normal) {
    vec3 fragToLight = fragPos - lightPos;
    float currentDepth = length(fragToLight);

    if (currentDepth >= farPlane) return 0.0;

    vec3 lightDir = normalize(-fragToLight);
    float bias = max(0.05 * (1.0 - dot(normal, lightDir)), 0.005);

    float closestDepth = 0.0;
    if (index == 0) closestDepth = texture(pointShadowMaps[0], fragToLight).r;
    else if (index == 1) closestDepth = texture(pointShadowMaps[1], fragToLight).r;
    else if (index == 2) closestDepth = texture(pointShadowMaps[2], fragToLight).r;
    else if (index == 3) closestDepth = texture(pointShadowMaps[3], fragToLight).r;
    else if (index == 4) closestDepth = texture(pointShadowMaps[4], fragToLight).r;
    else if (index == 5) closestDepth = texture(pointShadowMaps[5], fragToLight).r;
    else if (index == 6) closestDepth = texture(pointShadowMaps[6], fragToLight).r;
    else if (index == 7) closestDepth = texture(pointShadowMaps[7], fragToLight).r;
    else if (index == 8) closestDepth = texture(pointShadowMaps[8], fragToLight).r;
    else if (index == 9) closestDepth = texture(pointShadowMaps[9], fragToLight).r;
    else if (index == 10) closestDepth = texture(pointShadowMaps[10], fragToLight).r;
    else if (index == 11) closestDepth = texture(pointShadowMaps[11], fragToLight).r;
    else if (index == 12) closestDepth = texture(pointShadowMaps[12], fragToLight).r;
    else if (index == 13) closestDepth = texture(pointShadowMaps[13], fragToLight).r;
    else if (index == 14) closestDepth = texture(pointShadowMaps[14], fragToLight).r;
    else if (index == 15) closestDepth = texture(pointShadowMaps[15], fragToLight).r;

    closestDepth *= farPlane;
    float diff = (currentDepth - bias) - closestDepth;
    float soft = max(0.02, closestDepth * 0.02);
    return smoothstep(-soft, soft, diff);
}

void main() {
    vec4 albedoSample = texture(gAlbedoSpec, TexCoords);
    if (albedoSample.a < 0.5) discard;

    vec3 FragPos = texture(gPosition, TexCoords).rgb;
    vec3 Normal = normalize(texture(gNormal, TexCoords).rgb);
    vec3 texColor = albedoSample.rgb;
    float ao = texture(ssao, TexCoords).r;

    // Source-style lighting without bounce: a slightly higher ambient floor
    // (raised now that SSGI is gone) keeps interiors readable.
    float ambientStrength = 0.25;
    vec3 ambient = ambientStrength * sunColor * ao;

    vec3 sunLightDir = normalize(-sunDir);
    float sunDiff = max(dot(Normal, sunLightDir), 0.0);
    vec3 sunDiffuse = sunDiff * sunColor * sunIntensity;

    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 sunReflect = reflect(-sunLightDir, Normal);
    float sunSpec = pow(max(dot(viewDir, sunReflect), 0.0), 32.0);
    vec3 sunSpecular = 0.5 * sunSpec * sunColor * sunIntensity;

    float sunShadow = computeSunShadow(FragPos, Normal, sunLightDir);
    vec3 sunContribution = (1.0 - sunShadow) * (sunDiffuse + sunSpecular);

    vec3 pointContribution = vec3(0.0);
    for (int i = 0; i < numLights; i++) {
        vec3 toLight = lightPositions[i] - FragPos;
        float dist = length(toLight);
        vec3 lightDir = toLight / max(dist, 0.0001);

        float diff = max(dot(Normal, lightDir), 0.0);
        float atten = clamp(1.0 - (dist / lightRadii[i]), 0.0, 1.0);
        atten = atten * atten;

        float pShadow = computePointShadow(i, FragPos, lightPositions[i], lightRadii[i], Normal);
        pointContribution += (1.0 - pShadow) * diff * lightColors[i] * lightIntensities[i] * atten;
    }
    pointContribution *= ao;

    vec3 result = (ambient + sunContribution + pointContribution) * texColor;
    FragColor = vec4(result, 1.0);
}
)";

const char* debugViewFragSrc = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;
uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform sampler2D ssao;
uniform int mode;

void main() {
    if (mode == 1) FragColor = vec4(texture(gPosition, TexCoords).rgb * 0.05 + 0.5, 1.0);
    else if (mode == 2) FragColor = vec4(texture(gNormal, TexCoords).rgb * 0.5 + 0.5, 1.0);
    else if (mode == 3) FragColor = vec4(texture(gAlbedoSpec, TexCoords).rgb, 1.0);
    else if (mode == 4) { float a = texture(ssao, TexCoords).r; FragColor = vec4(vec3(a), 1.0); }
    else FragColor = vec4(0.0);
}
)";

const char* crosshairVertSrc = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUV;
out vec2 TexCoords;
void main() {
    TexCoords = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* crosshairFragSrc = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;
uniform sampler2D crosshairTex;
void main() {
    FragColor = texture(crosshairTex, TexCoords);
}
)";

// ----------------------------------------------------------------------------
// POST-PROCESSING
// ----------------------------------------------------------------------------
// Now that the scene is rendered into an HDR (float) buffer, this pass turns it
// into the final LDR image: exposure scaling, filmic (ACES) tonemapping, gamma
// correction and an optional vignette. The whole pipeline up to here has been
// working in linear light; gamma must be applied here at the very end.
// ----------------------------------------------------------------------------
const char* postFragSrc = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D scene;
uniform float exposure;      // scale factor (2^EV), applied to colour
uniform float gamma;
uniform float vignette;      // 0..1 - darkening amount at the screen edges
uniform int tonemapEnabled;  // 1 = ACES filmic curve, 0 = hard clamp

vec3 acesFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 color = texture(scene, TexCoords).rgb * exposure;

    if (tonemapEnabled == 1) color = acesFilm(color);
    else color = clamp(color, 0.0, 1.0);

    color = pow(color, vec3(1.0 / gamma));

    if (vignette > 0.0) {
        vec2 uv = TexCoords - 0.5;
        float dist = length(uv) * 1.414214;
        float mask = 1.0 - smoothstep(0.45, 0.95, dist);
        color = mix(color, color * 0.35, vignette * (1.0 - mask));
    }

    FragColor = vec4(color, 1.0);
}
)";

// ----------------------------------------------------------------------------
// FXAA (Fast Approximate Anti-Aliasing) - GLSL port of the FXAA 3.11 "quality"
// preset. Detects jaggies by comparing local luma, then blends along the edge's
// dominant direction. Cheap and resolution-independent.
// ----------------------------------------------------------------------------
const char* fxaaFragSrc = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform vec2 texelSize;

const float FXAA_SPAN_MAX  = 8.0;
const float FXAA_REDUCE_MUL = 1.0 / 8.0;
const float FXAA_REDUCE_MIN = 1.0 / 128.0;

void main() {
    vec3 rgbNW = texture(screenTexture, TexCoords + (vec2(-1.0, -1.0) * texelSize)).rgb;
    vec3 rgbNE = texture(screenTexture, TexCoords + (vec2( 1.0, -1.0) * texelSize)).rgb;
    vec3 rgbSW = texture(screenTexture, TexCoords + (vec2(-1.0,  1.0) * texelSize)).rgb;
    vec3 rgbSE = texture(screenTexture, TexCoords + (vec2( 1.0,  1.0) * texelSize)).rgb;
    vec3 rgbM  = texture(screenTexture, TexCoords).rgb;

    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM,  luma);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);

    dir = min(vec2(FXAA_SPAN_MAX, FXAA_SPAN_MAX),
              max(vec2(-FXAA_SPAN_MAX, -FXAA_SPAN_MAX), dir * rcpDirMin)) * texelSize;

    vec3 rgbA = 0.5 * (texture(screenTexture, TexCoords + dir * (1.0 / 3.0 - 0.5)).rgb +
                       texture(screenTexture, TexCoords + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(screenTexture, TexCoords + dir * (0.0 / 3.0 - 0.5)).rgb +
                                     texture(screenTexture, TexCoords + dir * (3.0 / 3.0 - 0.5)).rgb);
    float lumaB = dot(rgbB, luma);

    if ((lumaB < lumaMin) || (lumaB > lumaMax)) FragColor = vec4(rgbA, 1.0);
    else FragColor = vec4(rgbB, 1.0);
}
)";

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    if (!mouseLookEnabled) { firstMouse = true; return; }
    if (firstMouse) {
        lastX = (float)xpos; lastY = (float)ypos;
        firstMouse = false;
    }
    float xoffset = (float)xpos - lastX;
    float yoffset = lastY - (float)ypos;
    lastX = (float)xpos; lastY = (float)ypos;
    camera.processMouseMovement(xoffset, yoffset);
}

// Runtime errors are printed into the developer console instead of only going to stderr.
void glfw_error_callback(int error, const char* description) {
    g_Console.logError("GLFW (" + std::to_string(error) + "): " + std::string(description));
}

struct LevelChunk {
    Texture texture;
    GLuint VAO = 0, VBO = 0;
    int vertexCount = 0;
};

int main() {
    glfwSetErrorCallback(glfw_error_callback);

    // High-resolution default window: use the primary monitor's native
    // resolution (capped at 1080p so small GPUs aren't crushed), falling back
    // to the old 800x600 if the monitor can't be queried.
    int winW = 800, winH = 600;
    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
    if (primaryMonitor) {
        const GLFWvidmode* vid = glfwGetVideoMode(primaryMonitor);
        if (vid) {
            winW = std::min((int)vid->width, 1920);
            winH = std::min((int)vid->height, 1080);
        }
    }

    Window window(winW, winH, "KEA Engine");
    glfwSetCursorPosCallback(window.handle(), mouse_callback);
    window.setCursorDisabled(true);

    g_Console.log("KEA Engine - Developer Console");
    g_Console.log("Toggle: ~ (tilde). Type 'help' for commands.");
    g_Console.registerCommand("pos", [](const std::vector<std::string>&) {
        char buf[160];
        snprintf(buf, sizeof(buf), "position (%.2f, %.2f, %.2f)  velocity (%.2f, %.2f, %.2f)",
            player.position.x, player.position.y, player.position.z,
            player.velocity.x, player.velocity.y, player.velocity.z);
        g_Console.log(buf);
    });
    g_Console.setQuitCallback([&window]() {
        glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
    });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Old-school IBM VGA 8x16 bitmap font. These fonts are designed on an 8x16 pixel
    // grid, so render at native size with oversampling disabled to keep them crisp.
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 1;
    fontCfg.OversampleV = 1;
    fontCfg.PixelSnapH = true;
    if (io.Fonts->AddFontFromFileTTF(FONT_PATH.c_str(), 16.0f, &fontCfg) == nullptr)
        io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window.handle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");

    Shader gbufferShader(gbufferVertSrc, gbufferFragSrc);
    Shader lightingShader(quadVertSrc, lightingFragSrc);
    Shader debugViewShader(quadVertSrc, debugViewFragSrc);
    Shader crosshairShader(crosshairVertSrc, crosshairFragSrc);
    Shader postShader(quadVertSrc, postFragSrc);
    Shader fxaaShader(quadVertSrc, fxaaFragSrc);

    // Shader now only takes 2 arguments as expected
    Shader pointShadowShader(pointShadowVertSrc, pointShadowFragSrc);

    auto textureSizeLookup = [](const string& texName) -> glm::ivec2 {
        string path = TEXTURES_FOLDER + texName + ".png";
        return Texture::getImageSize(path);
        };

    // Step 1 verification: load the entity definition file and report what it
    // found. The classes are consumed by the entity spawner later on.
    FgdFile fgd = FgdParser::load(FGD_PATH);
    if (!fgd.loaded) {
        g_Console.logError("Failed to load FGD: " + fgd.error);
    }
    else {
        g_Console.log("Loaded FGD: " + std::to_string(fgd.classes.size()) + " entity classes");
        for (const FgdClass& cls : fgd.classes) {
            g_Console.log("  - " + cls.name + " [" + std::to_string(cls.keyValues.size()) + " keys]");
        }
    }

    LevelData level = MapLoader::load(MAP_PATH, textureSizeLookup);
    cout << "Loaded map: " << level.renderChunks.size() << " texture chunks, "
        << (level.collisionVertices.size() / 9) << " collision triangles, "
        << level.pointLights.size() << " point lights\n";
    g_Console.log("Loaded map: " + std::to_string(level.renderChunks.size()) + " texture chunks, "
        + std::to_string(level.collisionVertices.size() / 9) + " collision triangles, "
        + std::to_string(level.pointLights.size()) + " point lights");

    LightingState lighting;
    loadLightingSidecar(MAP_PATH, lighting);
    cout << "Lighting sidecar: sunPitch=" << lighting.sunPitch << " sunYaw=" << lighting.sunYaw
        << " sunColor=(" << lighting.sunColor.r << ", " << lighting.sunColor.g << ", " << lighting.sunColor.b
        << ") sunIntensity=" << lighting.sunIntensity << "\n";
    g_Console.log("Lighting: sunPitch=" + std::to_string(lighting.sunPitch)
        + " sunYaw=" + std::to_string(lighting.sunYaw)
        + " sunColor=(" + std::to_string(lighting.sunColor.r) + ", "
        + std::to_string(lighting.sunColor.g) + ", " + std::to_string(lighting.sunColor.b) + ")");

    // Generic mesh-entity spawn: any entity whose FGD class declares a model
    // path keyvalue becomes a runtime-placed, collidable, shadow-casting prop.
    vector<SpawnedProp> props = spawnMeshEntities(level, fgd);

    glm::vec3 sceneMin(1e9f), sceneMax(-1e9f);
    for (size_t i = 0; i + 2 < level.collisionVertices.size(); i += 3) {
        glm::vec3 p(level.collisionVertices[i], level.collisionVertices[i + 1], level.collisionVertices[i + 2]);
        sceneMin = glm::min(sceneMin, p);
        sceneMax = glm::max(sceneMax, p);
    }
    glm::vec3 sceneCenter = (sceneMin + sceneMax) * 0.5f;
    float sceneRadius = glm::length(sceneMax - sceneMin) * 0.5f;
    if (sceneRadius < 1.0f) sceneRadius = 20.0f;

    vector<LevelChunk> chunks;
    for (auto& pair : level.renderChunks) {
        const string& texName = pair.first;
        vector<float>& data = pair.second;
        if (data.empty()) continue;

        LevelChunk chunk;
        chunk.texture.load(TEXTURES_FOLDER + texName + ".png");
        chunk.vertexCount = (int)(data.size() / 8);

        glGenVertexArrays(1, &chunk.VAO);
        glGenBuffers(1, &chunk.VBO);
        glBindVertexArray(chunk.VAO);
        glBindBuffer(GL_ARRAY_BUFFER, chunk.VBO);
        glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);

        chunks.push_back(chunk);
    }

    for (auto& sp : props) {
        if (!sp.mesh.uploadToGPU())
            g_Console.logError("ObjMesh GPU upload failed: " + sp.mesh.error);
    }

    CollisionMesh collisionMesh;
    collisionMesh.buildFromVertices(level.collisionVertices.data(), level.collisionVertices.size());
    for (const auto& sp : props) {
        vector<float> triSoup = sp.mesh.collisionVertices();
        collisionMesh.addFromVertices(triSoup.data(), triSoup.size(), sp.modelMatrix);
    }

    if (level.hasPlayerStart) {
        player.position = level.playerStart;
    } else {
        // Fallback: find the floor by raycasting down from above.
        auto hit = collisionMesh.raycast(glm::vec3(0.0f, 100.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f);
        if (hit.hit) {
            player.position = hit.point; // feet on the floor
            player.position.y += 0.1f;   // tiny settle drop
        } else {
            player.position = glm::vec3(0.0f, 50.0f, 0.0f); // High up, will fall to ground
        }
    }

    // Push the spawn out of any geometry it overlaps (sphere at the capsule's
    // bottom-center so resting feet stay flush with the floor).
    {
        glm::vec3 bottomCenter = player.position + glm::vec3(0.0f, 0.35f, 0.0f);
        collisionMesh.resolveSphereCollision(bottomCenter, 0.35f);
    }
    if (player.position.y < 0.05f) player.position.y = 0.05f;

    DebugDraw debugDraw; debugDraw.init();
    Skybox skybox; skybox.init(workspaceRoot() + "/Images/cubemap_3.png");
    ShadowMap shadowMap; shadowMap.init(2048);
    GBuffer gBuffer; gBuffer.init(window.width(), window.height());
    SSAO ssao; ssao.init(window.width() / 2, window.height() / 2);

    // HDR scene buffer: the deferred lighting pass renders into this RGB16F
    // target instead of the screen, so the post-processing pass can apply
    // exposure + tonemapping + gamma to an uncompressed image. LDR buffer holds
    // the tonemapped result before FXAA erases the final aliasing.
    GLuint sceneFBO = 0, sceneColor = 0, sceneDepthRBO = 0;
    GLuint ldrFBO = 0, ldrColor = 0;
    auto createPostTargets = [&](int w, int h) {
        if (sceneFBO) {
            glDeleteFramebuffers(1, &sceneFBO);
            glDeleteTextures(1, &sceneColor);
            glDeleteRenderbuffers(1, &sceneDepthRBO);
        }
        glGenFramebuffers(1, &sceneFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO);
        glGenTextures(1, &sceneColor);
        glBindTexture(GL_TEXTURE_2D, sceneColor);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, w, h, 0, GL_RGB, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColor, 0);
        // Depth target so the skybox can depth-test against the blitted G-Buffer depth.
        glGenRenderbuffers(1, &sceneDepthRBO);
        glBindRenderbuffer(GL_RENDERBUFFER, sceneDepthRBO);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sceneDepthRBO);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            g_Console.logError("Scene HDR FBO incomplete");

        if (ldrFBO) {
            glDeleteFramebuffers(1, &ldrFBO);
            glDeleteTextures(1, &ldrColor);
        }
        glGenFramebuffers(1, &ldrFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, ldrFBO);
        glGenTextures(1, &ldrColor);
        glBindTexture(GL_TEXTURE_2D, ldrColor);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ldrColor, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            g_Console.logError("LDR FBO incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };
    createPostTargets(window.width(), window.height());

    // Rebuild every size-dependent target when the window/framebuffer resizes.
    window.onFramebufferResize = [&](int w, int h) {
        if (w <= 0 || h <= 0) return;
        gBuffer.init(w, h);
        ssao.init(w / 2, h / 2);
        createPostTargets(w, h);
        g_Console.log("Rebuilt render targets: " + std::to_string(w) + "x" + std::to_string(h));
    };

    GLuint pointShadowFBO;
    glGenFramebuffers(1, &pointShadowFBO);

    int activePointLights = min((int)level.pointLights.size(), MAX_LIGHTS);
    GLuint pointShadowCubemaps[MAX_LIGHTS];
    glGenTextures(activePointLights, pointShadowCubemaps);

    for (int i = 0; i < activePointLights; ++i) {
        glBindTexture(GL_TEXTURE_CUBE_MAP, pointShadowCubemaps[i]);
        for (unsigned int f = 0; f < 6; ++f) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_DEPTH_COMPONENT,
                SHADOW_CUBE_RES, SHADOW_CUBE_RES, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }

    float quadVertices[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };
    GLuint quadVAO, quadVBO;
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    Texture crosshairTexture;
    crosshairTexture.load(CROSSHAIR_PATH);
    float crosshairSizePixels = 3.0f;

    GLuint crosshairVAO, crosshairVBO;
    glGenVertexArrays(1, &crosshairVAO);
    glGenBuffers(1, &crosshairVBO);
    glBindVertexArray(crosshairVAO);
    glBindBuffer(GL_ARRAY_BUFFER, crosshairVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glm::vec3 sunDir = glm::normalize(glm::vec3(
        cos(glm::radians(lighting.sunPitch)) * cos(glm::radians(lighting.sunYaw)),
        sin(glm::radians(lighting.sunPitch)),
        cos(glm::radians(lighting.sunPitch)) * sin(glm::radians(lighting.sunYaw))));
    glm::vec3 sunColor = lighting.sunColor;
    float sunIntensity = lighting.sunIntensity;
    int debugViewMode = 0;

    bool wireframe = false;
    bool tKeyWasDown = false;
    bool tildeWasDown = false;
    bool escWasDown = false;
    bool f1WasDown = false;
    bool showDebugPanel = false;

    float deltaTime = 0.0f, lastFrame = 0.0f;
    float fpsTimer = 0.0f;
    int frameCount = 0;
    float displayedFps = 0.0f;

    const int FRAME_HISTORY = 120;
    vector<float> frameTimeHistory(FRAME_HISTORY, 0.0f);
    int frameHistoryIdx = 0;

    // ------------------------------------------------------------------
    // Post-processing state (tweaked live from the console or debug panel).
    // ------------------------------------------------------------------
    bool postEnabled = true;      // tonemap/gamma/vignette pass on/off
    bool tonemapEnabled = true;   // ACES curve vs plain clamp
    bool fxaaEnabled = true;      // final anti-aliasing pass on/off
    float gamma = 2.2f;
    float vignetteStrength = 0.25f;
    float exposureEV = RenderPipeline::exposureEV();
    float ssaoStrength = RenderPipeline::ssaoStrength();
    float ssaoRadius = RenderPipeline::ssaoRadius();

    g_Console.registerCommand("resolution", [&window](const vector<string>& a) {
        if (a.size() < 2) { g_Console.log("usage: resolution <width> <height>"); return; }
        int w = atoi(a[0].c_str()), h = atoi(a[1].c_str());
        if (w < 640 || h < 360) { g_Console.log("too small (min 640x360)"); return; }
        glfwSetWindowSize(window.handle(), w, h);
        g_Console.log("resizing to " + a[0] + "x" + a[1] + " (targets rebuild on window resize)");
    });
    g_Console.registerCommand("fullscreen", [&window](const vector<string>&) {
        window.setFullscreen(!window.isFullscreen());
        g_Console.log(window.isFullscreen() ? "fullscreen on" : "fullscreen off");
    });
    g_Console.registerCommand("post", [&postEnabled](const vector<string>& a) {
        if (!a.empty()) postEnabled = (a[0] == "on" || a[0] == "1");
        g_Console.log(string("post processing: ") + (postEnabled ? "on" : "off"));
    });
    g_Console.registerCommand("fxaa", [&fxaaEnabled](const vector<string>& a) {
        if (!a.empty()) fxaaEnabled = (a[0] == "on" || a[0] == "1");
        g_Console.log(string("fxaa: ") + (fxaaEnabled ? "on" : "off"));
    });
    g_Console.registerCommand("tonemap", [&tonemapEnabled](const vector<string>& a) {
        if (!a.empty()) tonemapEnabled = (a[0] == "on" || a[0] == "1");
        g_Console.log(string("aces tonemap: ") + (tonemapEnabled ? "on" : "off"));
    });
    g_Console.registerCommand("exposure", [&exposureEV](const vector<string>& a) {
        if (a.empty()) { g_Console.log("exposure EV: " + std::to_string(exposureEV)); return; }
        exposureEV = (float)atof(a[0].c_str());
        if (exposureEV < -5.0f) exposureEV = -5.0f;
        if (exposureEV > 5.0f) exposureEV = 5.0f;
        g_Console.log("exposure EV: " + std::to_string(exposureEV));
    });
    g_Console.registerCommand("gamma", [&gamma](const vector<string>& a) {
        if (a.empty()) { g_Console.log("gamma: " + std::to_string(gamma)); return; }
        gamma = (float)atof(a[0].c_str());
        if (gamma < 0.5f) gamma = 0.5f;
        if (gamma > 4.0f) gamma = 4.0f;
        g_Console.log("gamma: " + std::to_string(gamma));
    });
    g_Console.registerCommand("vignette", [&vignetteStrength](const vector<string>& a) {
        if (a.empty()) { g_Console.log("vignette: " + std::to_string(vignetteStrength)); return; }
        vignetteStrength = (float)atof(a[0].c_str());
        if (vignetteStrength < 0.0f) vignetteStrength = 0.0f;
        if (vignetteStrength > 1.0f) vignetteStrength = 1.0f;
        g_Console.log("vignette: " + std::to_string(vignetteStrength));
    });
    g_Console.registerCommand("ssao", [&ssaoStrength, &ssaoRadius](const vector<string>& a) {
        if (a.empty()) {
            g_Console.log("ssao: " + string(RenderPipeline::ssaoEnabled() ? "on" : "off")
                + "  strength=" + std::to_string(ssaoStrength)
                + "  radius=" + std::to_string(ssaoRadius));
            return;
        }
        if (a[0] == "on" || a[0] == "1") RenderPipeline::SetSSAOEnabled(true);
        else if (a[0] == "off" || a[0] == "0") RenderPipeline::SetSSAOEnabled(false);
        else {
            g_Console.log("usage: ssao on|off, or ssao strength <0..5>, ssao radius <0.01..2>");
            return;
        }
    });

    // Audio: playsound <file> [volume] [loop]
    // Plays a sound file relative to GameRoot/audio/. Use volume 0..1 and "loop" to repeat.
    g_Console.registerCommand("playsound", [](const vector<string>& a) {
        if (a.size() < 1) {
            g_Console.log("usage: playsound <filename> [volume 0..1] [loop]");
            return;
        }
        string path = workspaceRoot() + "/GameRoot/audio/" + a[0];
        float vol = (a.size() >= 2) ? stof(a[1]) : 1.0f;
        bool loop = (a.size() >= 3 && (a[2] == "loop" || a[2] == "1"));
        AudioHandle h = play(path, vol, loop);
        if (h != AUDIO_INVALID) {
            g_Console.log("Playing '" + path + "' (handle " + to_string(h) + ")");
        } else {
            g_Console.logError("Failed to play '" + path + "'");
        }
    });

    // Audio system init (miniaudio device).
    if (!audioInit()) g_Console.logError("Audio: init failed");
    else g_Console.log("Audio system ready");

    while (!window.shouldClose()) {
        float currentFrame = (float)glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        frameTimeHistory[frameHistoryIdx] = deltaTime * 1000.0f;
        frameHistoryIdx = (frameHistoryIdx + 1) % FRAME_HISTORY;

        frameCount++;
        fpsTimer += deltaTime;
        if (fpsTimer >= 1.0f) {
            displayedFps = frameCount / fpsTimer;
            frameCount = 0;
            fpsTimer = 0.0f;
        }

        // Cursor/mouse state follows whichever overlay is open (console, debug panel).
        auto syncMouse = [&]() {
            mouseLookEnabled = !g_Console.isOpen() && !showDebugPanel;
            window.setCursorDisabled(mouseLookEnabled);
        };

        // Esc closes the console first; when it is closed Esc quits the game.
        bool escDown = glfwGetKey(window.handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS;
        if (escDown && !escWasDown) {
            if (g_Console.isOpen()) {
                g_Console.close();
                syncMouse();
            }
            else {
                glfwSetWindowShouldClose(window.handle(), true);
            }
        }
        escWasDown = escDown;

        // Tilde toggles the developer console.
        bool tildeDown = glfwGetKey(window.handle(), GLFW_KEY_GRAVE_ACCENT) == GLFW_PRESS;
        if (tildeDown && !tildeWasDown) {
            g_Console.toggle();
            syncMouse();
        }
        tildeWasDown = tildeDown;

        // F1 toggles the debug panel (moved off tilde to make room for the console).
        bool f1Down = glfwGetKey(window.handle(), GLFW_KEY_F1) == GLFW_PRESS;
        if (f1Down && !f1WasDown) {
            showDebugPanel = !showDebugPanel;
            syncMouse();
        }
        f1WasDown = f1Down;

        bool tDown = glfwGetKey(window.handle(), GLFW_KEY_T) == GLFW_PRESS;
        if (tDown && !tKeyWasDown) wireframe = !wireframe;
        tKeyWasDown = tDown;

        if (mouseLookEnabled) {
            player.update(window.handle(), deltaTime, camera, collisionMesh);
        }
        else {
            camera.position = player.position + glm::vec3(0.0f, player.getEyeHeight(), 0.0f);
        }

        audioUpdate(camera.position, camera.front, camera.up);

        glm::mat4 view = camera.getViewMatrix();
        glm::mat4 projection = glm::perspective(glm::radians(75.0f),
            (float)window.width() / (float)window.height(), 0.1f, 100.0f);
        glm::mat4 lightSpaceMatrix = shadowMap.getLightSpaceMatrix(sunDir, sceneCenter, sceneRadius);
        glm::mat4 identityModel = glm::mat4(1.0f);

        // 1. Sun Shadow Pass
        shadowMap.beginRender();
        shadowMap.depthShader()->use();
        shadowMap.depthShader()->setMat4("lightSpaceMatrix", lightSpaceMatrix);
        shadowMap.depthShader()->setMat4("model", identityModel);
        glCullFace(GL_FRONT);
        for (auto& chunk : chunks) {
            glBindVertexArray(chunk.VAO);
            glDrawArrays(GL_TRIANGLES, 0, chunk.vertexCount);
        }
        for (const auto& sp : props) {
            shadowMap.depthShader()->setMat4("model", sp.modelMatrix);
            glBindVertexArray(sp.mesh.VAO);
            glDrawArrays(GL_TRIANGLES, 0, sp.mesh.vertexCount);
        }
        glCullFace(GL_BACK);
        shadowMap.endRender(window.width(), window.height());

        // 2. OPTIMIZED: Point Light Depth Pass (Cached so it runs ONLY ONCE)
        static bool pointShadowsInitialized = false;
        if (!pointShadowsInitialized) {
            glViewport(0, 0, SHADOW_CUBE_RES, SHADOW_CUBE_RES);
            glBindFramebuffer(GL_FRAMEBUFFER, pointShadowFBO);
            pointShadowShader.use();

            glEnable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);

            float aspect = 1.0f;
            float nearPlane = 0.1f;

            for (int i = 0; i < activePointLights; ++i) {
                float farPlane = level.pointLights[i].radius;
                glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), aspect, nearPlane, farPlane);
                glm::vec3 pPos = level.pointLights[i].position;

                vector<glm::mat4> shadowTransforms;
                shadowTransforms.push_back(shadowProj * glm::lookAt(pPos, pPos + glm::vec3(1, 0, 0), glm::vec3(0, -1, 0)));
                shadowTransforms.push_back(shadowProj * glm::lookAt(pPos, pPos + glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0)));
                shadowTransforms.push_back(shadowProj * glm::lookAt(pPos, pPos + glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)));
                shadowTransforms.push_back(shadowProj * glm::lookAt(pPos, pPos + glm::vec3(0, -1, 0), glm::vec3(0, 0, -1)));
                shadowTransforms.push_back(shadowProj * glm::lookAt(pPos, pPos + glm::vec3(0, 0, 1), glm::vec3(0, -1, 0)));
                shadowTransforms.push_back(shadowProj * glm::lookAt(pPos, pPos + glm::vec3(0, 0, -1), glm::vec3(0, -1, 0)));

                pointShadowShader.setFloat("farPlane", farPlane);
                pointShadowShader.setVec3("lightPos", pPos);
                pointShadowShader.setMat4("model", identityModel);

                for (int f = 0; f < 6; ++f) {
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                        GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, pointShadowCubemaps[i], 0);
                    glClear(GL_DEPTH_BUFFER_BIT);

                    pointShadowShader.setMat4("shadowMatrix", shadowTransforms[f]);
                    pointShadowShader.setMat4("model", identityModel);

                    for (auto& chunk : chunks) {
                        glBindVertexArray(chunk.VAO);
                        glDrawArrays(GL_TRIANGLES, 0, chunk.vertexCount);
                    }
                    for (const auto& sp : props) {
                        pointShadowShader.setMat4("model", sp.modelMatrix);
                        glBindVertexArray(sp.mesh.VAO);
                        glDrawArrays(GL_TRIANGLES, 0, sp.mesh.vertexCount);
                    }
                }
            }
            pointShadowsInitialized = true;
        }

        // Restore standard culling
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);

        // 3. G-Buffer Geometry Pass
        glDisable(GL_CULL_FACE);
        glViewport(0, 0, window.width(), window.height());
        glEnable(GL_DEPTH_TEST);
        glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
        gBuffer.bindForWriting();
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        gbufferShader.use();
        gbufferShader.setMat4("model", identityModel);
        gbufferShader.setMat4("view", view);
        gbufferShader.setMat4("projection", projection);
        gbufferShader.setInt("texture1", 0);
        for (auto& chunk : chunks) {
            chunk.texture.bind(0);
            glBindVertexArray(chunk.VAO);
            glDrawArrays(GL_TRIANGLES, 0, chunk.vertexCount);
        }
        for (const auto& sp : props) {
            gbufferShader.setMat4("model", sp.modelMatrix);
            sp.mesh.draw();
        }
        gbufferShader.setMat4("model", identityModel);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // 4. SSAO Pass (half-res, bilateral blur)
        glDisable(GL_DEPTH_TEST);
        ssao.renderSSAO(gBuffer.positionTex(), gBuffer.normalTex(), view, projection, quadVAO);
        if (RenderPipeline::ssaoEnabled())
            ssao.blur(quadVAO, gBuffer.positionTex(), gBuffer.normalTex());

        // 5. Blit Depth Buffer into the HDR scene target (NOT the screen) so the
        //    skybox can depth-test against the geometry when it draws later.
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO);
        glViewport(0, 0, window.width(), window.height());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gBuffer.getFBO());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneFBO);
        glBlitFramebuffer(0, 0, gBuffer.width(), gBuffer.height(),
            0, 0, window.width(), window.height(),
            GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneFBO);

        // 6. Deferred Lighting Pass (into the HDR scene buffer, not the screen)
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO);
        glViewport(0, 0, window.width(), window.height());
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (debugViewMode == 0) {
            lightingShader.use();
            lightingShader.setInt("gPosition", 0);
            lightingShader.setInt("gNormal", 1);
            lightingShader.setInt("gAlbedoSpec", 2);
            lightingShader.setInt("ssao", 3);
            lightingShader.setInt("shadowMapTex", 4);
            lightingShader.setInt("ssgiTex", 5);

            lightingShader.setMat4("lightSpaceMatrix", lightSpaceMatrix);
            lightingShader.setVec3("sunDir", sunDir);
            lightingShader.setVec3("sunColor", sunColor);
            lightingShader.setFloat("sunIntensity", sunIntensity);
            lightingShader.setVec3("viewPos", camera.position);

            lightingShader.setInt("numLights", activePointLights);
            for (int i = 0; i < activePointLights; i++) {
                string idx = to_string(i);
                lightingShader.setVec3("lightPositions[" + idx + "]", level.pointLights[i].position);
                lightingShader.setVec3("lightColors[" + idx + "]", level.pointLights[i].color);
                lightingShader.setFloat("lightIntensities[" + idx + "]", level.pointLights[i].intensity);
                lightingShader.setFloat("lightRadii[" + idx + "]", level.pointLights[i].radius);

                glActiveTexture(GL_TEXTURE6 + i);
                glBindTexture(GL_TEXTURE_CUBE_MAP, pointShadowCubemaps[i]);
                lightingShader.setInt("pointShadowMaps[" + idx + "]", 6 + i);
            }

            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gBuffer.positionTex());
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gBuffer.normalTex());
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, gBuffer.albedoSpecTex());
            glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, ssao.resultTexture());
            glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, shadowMap.depthTexture());

            glBindVertexArray(quadVAO);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        else {
            debugViewShader.use();
            debugViewShader.setInt("gPosition", 0);
            debugViewShader.setInt("gNormal", 1);
            debugViewShader.setInt("gAlbedoSpec", 2);
            debugViewShader.setInt("ssao", 3);
            debugViewShader.setInt("mode", debugViewMode);

            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gBuffer.positionTex());
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gBuffer.normalTex());
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, gBuffer.albedoSpecTex());
            glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, ssao.resultTexture());

            glBindVertexArray(quadVAO);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // 7. Skybox Pass (depth-tests against the blitted geometry depth in the
        //    scene buffer, so it only appears where no geometry was lit).
        glEnable(GL_DEPTH_TEST);
        skybox.draw(view, projection);

        // 8. Post-Processing: Exposure + Tonemap + Gamma + Vignette (scene -> LDR)
        RenderPipeline::SetSSAOParams(ssaoStrength, ssaoRadius);
        RenderPipeline::SetExposure(exposureEV);

        glBindFramebuffer(GL_FRAMEBUFFER, ldrFBO);
        glViewport(0, 0, window.width(), window.height());
        glDisable(GL_DEPTH_TEST);
        if (postEnabled) {
            postShader.use();
            postShader.setInt("scene", 0);
            postShader.setFloat("exposure", powf(2.0f, exposureEV));
            postShader.setFloat("gamma", gamma);
            postShader.setFloat("vignette", vignetteStrength);
            postShader.setInt("tonemapEnabled", tonemapEnabled ? 1 : 0);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, sceneColor);
            glBindVertexArray(quadVAO);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        } else {
            // Bypass: copy the raw HDR colour straight through (clamped by the
            // 8-bit target) so the image still displays with post turned off.
            glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFBO);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ldrFBO);
            glBlitFramebuffer(0, 0, window.width(), window.height(),
                0, 0, window.width(), window.height(),
                GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // 9. FXAA anti-aliasing (LDR -> screen)
        glViewport(0, 0, window.width(), window.height());
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (fxaaEnabled) {
            fxaaShader.use();
            fxaaShader.setInt("screenTexture", 0);
            fxaaShader.setVec2("texelSize", glm::vec2(1.0f / window.width(), 1.0f / window.height()));
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, ldrColor);
            glBindVertexArray(quadVAO);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, ldrFBO);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, window.width(), window.height(),
                0, 0, window.width(), window.height(),
                GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        // 10. Crosshair Overlay Pass
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        float chHalfX = crosshairSizePixels / (float)window.width();
        float chHalfY = crosshairSizePixels / (float)window.height();
        float crosshairVertices[] = {
            -chHalfX,  chHalfY,  0.0f, 1.0f,
            -chHalfX, -chHalfY,  0.0f, 0.0f,
             chHalfX, -chHalfY,  1.0f, 0.0f,
            -chHalfX,  chHalfY,  0.0f, 1.0f,
             chHalfX, -chHalfY,  1.0f, 0.0f,
             chHalfX,  chHalfY,  1.0f, 1.0f
        };
        glBindBuffer(GL_ARRAY_BUFFER, crosshairVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(crosshairVertices), crosshairVertices);

        crosshairShader.use();
        crosshairShader.setInt("crosshairTex", 0);
        crosshairTexture.bind(0);
        glBindVertexArray(crosshairVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);

        // 11. ImGui Debug Overlay Pass
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (showDebugPanel) {
            ImGui::Begin("Debug Control Center");

            ImGui::Text("FPS: %.1f (%.2f ms)", displayedFps, deltaTime * 1000.0f);
            ImGui::PlotLines("Frame time (ms)", frameTimeHistory.data(), FRAME_HISTORY, frameHistoryIdx,
                nullptr, 0.0f, 33.0f, ImVec2(0, 60));

            ImGui::Separator();
            ImGui::Text("Player Controls & Physics");
            ImGui::Text("Position: (%.2f, %.2f, %.2f)", player.position.x, player.position.y, player.position.z);
            ImGui::Text("Velocity: (%.2f, %.2f, %.2f)", player.velocity.x, player.velocity.y, player.velocity.z);
            ImGui::Text("Grounded: %s", player.grounded ? "Yes" : "No");
            ImGui::Text("Crouching: %s", player.isCrouching() ? "Yes" : "No");

            ImGui::Separator();
            ImGui::Text("Level Info");
            ImGui::Text("Texture chunks: %d", (int)chunks.size());
            ImGui::Text("Collision triangles: %d", (int)collisionMesh.triangles().size());
            ImGui::Text("Active Point Lights: %d", activePointLights);

            ImGui::Separator();
            ImGui::Text("Debug Display Mode");
            const char* modes[] = { "Final", "Position", "Normal", "Albedo", "SSAO only" };
            ImGui::Combo("View mode", &debugViewMode, modes, 6);

            ImGui::Separator();
            ImGui::Text("Sun Light Settings");
            ImGui::SliderFloat3("Sun Direction", &sunDir.x, -1.0f, 1.0f);
            ImGui::ColorEdit3("Sun Color", &sunColor.x);

            ImGui::Separator();
            ImGui::Text("Post-Processing");
            ImGui::Checkbox("Post processing", &postEnabled);
            ImGui::Checkbox("ACES tonemap", &tonemapEnabled);
            ImGui::Checkbox("FXAA anti-aliasing", &fxaaEnabled);
            ImGui::SliderFloat("Exposure (EV)", &exposureEV, -3.0f, 3.0f);
            ImGui::SliderFloat("Gamma", &gamma, 1.0f, 3.0f);
            ImGui::SliderFloat("Vignette", &vignetteStrength, 0.0f, 1.0f);
            bool ssaoOn = RenderPipeline::ssaoEnabled();
            if (ImGui::Checkbox("SSAO", &ssaoOn)) RenderPipeline::SetSSAOEnabled(ssaoOn);
            ImGui::SliderFloat("SSAO strength", &ssaoStrength, 0.0f, 5.0f);
            ImGui::SliderFloat("SSAO radius", &ssaoRadius, 0.01f, 2.0f);

            ImGui::Separator();
            ImGui::Checkbox("Wireframe (T)", &wireframe);
            ImGui::Text("Crouch: Left Ctrl / C");
            ImGui::Text("Console: ~ (tilde)");
            ImGui::Text("Toggle Panel / Mouse: F1");

            ImGui::End();
        }

        // Developer console overlay (drawn on top of everything).
        g_Console.draw((float)window.width(), (float)window.height());

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        window.swapBuffersAndPollEvents();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glDeleteFramebuffers(1, &pointShadowFBO);
    glDeleteTextures(activePointLights, pointShadowCubemaps);

    for (auto& chunk : chunks) {
        glDeleteVertexArrays(1, &chunk.VAO);
        glDeleteBuffers(1, &chunk.VBO);
    }

    audioShutdown();

    return 0;
}