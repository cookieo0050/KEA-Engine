// ============================================================================
// Map_editor.cpp - Standalone Level Editor Viewport
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// A minimal level editor that reuses KEA's shared parsing code (mapfile.cpp,
// fgd.cpp, texture.cpp) via direct source inclusion - no engine runtime, no
// Jolt physics, no deferred renderer. It opens the .map file, renders the brush
// geometry with a simple forward pass and lets you fly around with a free camera.
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// 1. main(): GLFW window + OpenGL 3.3 context -> load map via MapLoader::load
//    -> build one VAO per texture chunk (the 8-float vertex layout the engine
//    uses) -> free-fly camera -> main loop.
// 2. Every frame: handle camera input, draw a ground grid, draw each chunk with
//    a basic ambient + directional shader, then draw the ImGui panels (entity
//    list + map info) on top.
//
// CONTROLS
// ----------------------------------------------------------------------------
// - WASD move, Q/E up/down, Shift = boost, hold Right Mouse = look around.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cctype>
#include <random>
#include <algorithm>
#include <functional>
#include <glm/gtc/quaternion.hpp>
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "ImGuizmo.h"
#include "mapfile.h"
#include "texture.h"
#include "fgd.h"
#include "objmesh.h"
#include "renderpipeline.h"
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

const string MAP_PATH = workspaceRoot() + "/Maps/Testroom.map";
const string TEXTURES_FOLDER = workspaceRoot() + "/GameRoot/textures/";
const string MODELS_FOLDER = workspaceRoot() + "/GameRoot/models/";
const string FONT_PATH = workspaceRoot() + "/oldschool_pc_font_pack_v2.2_FULL/ttf - Px (pixel outline)/Px437_IBM_VGA_8x16.ttf";
const string DBG_LOG = workspaceRoot() + "/x64/Debug/editor_dbg.txt";

// Last-write time of a file (FILETIME -> uint64). Used by hot-reload to detect
// when the .map on disk changed without polling file size/contents.
static ULONGLONG fileWriteTimeOf(const string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32)
        | fad.ftLastWriteTime.dwLowDateTime;
}

// Recursively collect relative .obj paths under MODELS_FOLDER ("props/crate.obj")
// for the model-key combo in the Properties panel.
static vector<string> collectObjModelPaths() {
    vector<string> out;
    std::function<void(const string&, const string&)> walk =
        [&](const string& dir, const string& prefix) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                walk(dir + name + "\\", prefix + name + "/");
            else {
                string lower = name;
                for (char& c : lower) c = (char)tolower((unsigned char)c);
                if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".obj")
                    out.push_back(prefix + name);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    };
    walk(MODELS_FOLDER, "");
    return out;
}

// Cache of relative .obj paths for the model-key combo in Properties (scanned once).
static const vector<string>& cachedObjModelPaths() {
    static vector<string> paths = collectObjModelPaths();
    return paths;
}

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

static bool s_rightMouse = false;
static bool s_firstMouse = true;
static double s_lastX = 0, s_lastY = 0;
static bool s_viewportHovered = false;  // set each frame when the mouse is over the Viewport panel

// Editor-side gizmo state, persisted into imgui.ini via a custom settings
// handler. ImGui only persists window layout by default, so we register a
// "[Editor][Settings]" section that is read/written alongside the window
// entries - snapEnabled, the active gizmo operation and world/local mode
// survive restarts without a separate config file.
struct EditorSettings {
    bool snapEnabled = true;
    int  gizmoOp = 0;      // 0 translate, 1 rotate, 2 scale
    bool gizmoLocal = false;
    float snapTranslate = 1.0f;  // grid step in map units (gizmo TRANSLATE)
    float snapRotate = 15.0f;    // degrees (gizmo ROTATE)
    float snapScale = 0.1f;      // scale step (gizmo SCALE)
    bool showGrid = true;
    bool wireframe = false;
    bool ortho = false;
    bool autoReload = false;     // hot-reload the .map when it changes on disk
};
static EditorSettings s_editorSettings;

// Last observed .map write time (hot-reload detection) + a toggle for the
// snap-settings dockable panel (opened by View->Snap Settings).
static ULONGLONG g_mapWriteTime = fileWriteTimeOf(MAP_PATH);
static bool s_snapPanelOpen = false;
static double g_lastReloadCheck = 0.0;

static void* EditorSettings_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    return (void*)&s_editorSettings;
}

static void EditorSettings_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    EditorSettings* s = (EditorSettings*)entry;
    int v = 0;
    if (sscanf_s(line, "Snap=%d", &v) == 1) s->snapEnabled = (v != 0);
    else if (sscanf_s(line, "Op=%d", &v) == 1) s->gizmoOp = v < 0 ? 0 : (v > 2 ? 2 : v);
    else if (sscanf_s(line, "Local=%d", &v) == 1) s->gizmoLocal = (v != 0);
    else if (sscanf_s(line, "SnapT=%f", &s->snapTranslate) == 1) {}
    else if (sscanf_s(line, "SnapR=%f", &s->snapRotate) == 1) {}
    else if (sscanf_s(line, "SnapS=%f", &s->snapScale) == 1) {}
    else if (sscanf_s(line, "Grid=%d", &v) == 1) s->showGrid = (v != 0);
    else if (sscanf_s(line, "Wire=%d", &v) == 1) s->wireframe = (v != 0);
    else if (sscanf_s(line, "Ortho=%d", &v) == 1) s->ortho = (v != 0);
    else if (sscanf_s(line, "AutoReload=%d", &v) == 1) s->autoReload = (v != 0);
}

static void EditorSettings_WriteAll(ImGuiContext*, ImGuiSettingsHandler*, ImGuiTextBuffer* out_buf) {
    out_buf->appendf("[Editor][Settings]\nSnap=%d\nOp=%d\nLocal=%d\nSnapT=%.3f\nSnapR=%.3f\nSnapS=%.3f\nGrid=%d\nWire=%d\nOrtho=%d\nAutoReload=%d\n\n",
        s_editorSettings.snapEnabled ? 1 : 0, s_editorSettings.gizmoOp, s_editorSettings.gizmoLocal ? 1 : 0,
        s_editorSettings.snapTranslate, s_editorSettings.snapRotate, s_editorSettings.snapScale,
        s_editorSettings.showGrid ? 1 : 0, s_editorSettings.wireframe ? 1 : 0,
        s_editorSettings.ortho ? 1 : 0, s_editorSettings.autoReload ? 1 : 0);
}

static void EditorSettings_Register() {
    ImGuiSettingsHandler handler;
    handler.TypeName = "Editor";
    handler.TypeHash = ImHashStr("Editor");
    handler.ReadOpenFn = EditorSettings_ReadOpen;
    handler.ReadLineFn = EditorSettings_ReadLine;
    handler.WriteAllFn = EditorSettings_WriteAll;
    ImGui::AddSettingsHandler(&handler);
}

static void cursor_pos_callback(GLFWwindow* window, double xpos, double ypos) {
    // CRITICAL: this callback is registered AFTER ImGui_ImplGlfw_InitForOpenGL,
    // so it overwrote ImGui's installed callback and ImGui never received mouse
    // position events (which is why windows couldn't be dragged). Forward the
    // event to ImGui first so its internal state stays in sync, then handle the
    // editor's free-look camera. See backends/imgui_impl_glfw.h "Since 1.87".
    ImGui_ImplGlfw_CursorPosCallback(window, xpos, ypos);

    EditorCamera* cam = static_cast<EditorCamera*>(glfwGetWindowUserPointer(window));
    if (!cam) return;
    // Only fly the camera when the cursor is inside the viewport panel AND the
    // right mouse button is held (picking/gizmos use the left button).
    if (!s_rightMouse || !s_viewportHovered) { s_firstMouse = true; return; }
    if (s_firstMouse) {
        s_lastX = xpos; s_lastY = ypos;
        s_firstMouse = false;
    }
    float xoffset = (float)(xpos - s_lastX);
    float yoffset = (float)(s_lastY - ypos);
    s_lastX = xpos; s_lastY = ypos;

    cam->yaw += xoffset * cam->sensitivity;
    cam->pitch += yoffset * cam->sensitivity;
    if (cam->pitch > 89.0f) cam->pitch = 89.0f;
    if (cam->pitch < -89.0f) cam->pitch = -89.0f;
    cam->updateVectors();
}

static void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
}

static GLuint compileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        cerr << "Shader compile error: " << log << "\n";
    }
    return shader;
}

static GLuint buildProgram(const char* vertSrc, const char* fragSrc) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        cerr << "Program link error: " << log << "\n";
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static const char* chunkVertSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
uniform mat4 uMVP;
out vec2 TexCoords;
out vec3 WorldPos;
out vec3 Normal;
void main() {
    TexCoords = aUV;
    WorldPos = aPos;
    Normal = aNormal;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

static const char* chunkFragSrc = R"(
#version 330 core
out vec4 FragColor;       // location 0: full lit colour (HDR)
out vec4 IndirectColor;   // location 1: ambient + point-light terms (SSAO applies here)
in vec2 TexCoords;
in vec3 WorldPos;
in vec3 Normal;
uniform sampler2D uTexture;
uniform sampler2D uShadowMap;
uniform mat4 uLightSpaceMatrix;
uniform float uShadowEnabled;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uSunIntensity;
uniform vec3 uAmbientColor;
uniform float uAmbientIntensity;
uniform vec3 uCamPos;
uniform int uNumLights;
uniform vec3 uLightPos[16];
uniform vec3 uLightColor[16];
uniform float uLightIntensity[16];
uniform float uLightRadius[16];
float computeSunShadow(vec3 worldPos, vec3 norm) {
    vec4 fragPosLightSpace = uLightSpaceMatrix * vec4(worldPos + norm * 0.005, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    if (projCoords.z > 1.0 || projCoords.z < 0.0) return 0.0;
    float currentDepth = projCoords.z;
    // Slope-scaled bias (matches the engine) to kill shadow acne near grazing
    // angles without a constant offset that would cause peter-panning.
    vec3 lightDir = normalize(-uSunDir);
    float bias = max(0.001 * (1.0 - dot(norm, lightDir)), 0.0001);
    float softEdge = 0.003;  // soft depth transition per PCF tap (engine value)
    vec2 texelSize = 1.0 / textureSize(uShadowMap, 0);
    float shadow = 0.0;
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++) {
            float pcfDepth = texture(uShadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            float diff = (currentDepth - bias) - pcfDepth;
            shadow += smoothstep(-softEdge, softEdge, diff);
        }
    return shadow / 9.0;
}
void main() {
    vec4 texColor = texture(uTexture, TexCoords);
    if (texColor.a < 0.5) discard;
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(-uSunDir);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 viewDir = normalize(uCamPos - WorldPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    float shadow = mix(0.0, computeSunShadow(WorldPos, norm), uShadowEnabled);
    vec3 sunTerm = texColor.rgb * (1.0 - shadow) * uSunColor * uSunIntensity * diff;
    vec3 specTerm = (1.0 - shadow) * uSunColor * uSunIntensity * 0.25 * spec;
    vec3 ambientTerm = texColor.rgb * uAmbientColor * uAmbientIntensity;
    vec3 lightContrib = vec3(0.0);
    for (int i = 0; i < uNumLights; i++) {
        vec3 toLight = uLightPos[i] - WorldPos;
        float dist = length(toLight);
        vec3 lDir = toLight / max(dist, 0.0001);
        float lDiff = max(dot(norm, lDir), 0.0);
        float atten = clamp(1.0 - dist / max(uLightRadius[i], 0.0001), 0.0, 1.0);
        atten *= atten;
        lightContrib += lDiff * uLightColor[i] * uLightIntensity[i] * atten;
    }
    vec3 pointTerm = texColor.rgb * lightContrib;
    // Sun (direct) light is not occluded; ambient + point light are. The composite
    // pass rebuilds: colour = (FragColor - IndirectColor) + IndirectColor * AO.
    FragColor = vec4(sunTerm + specTerm + ambientTerm + pointTerm, 1.0);
    IndirectColor = vec4(ambientTerm + pointTerm, 1.0);
}
)";

static const char* gridVertSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

static const char* gridFragSrc = R"(
#version 330 core
out vec4 FragColor;
out vec4 IndirectColor;
void main() {
    FragColor = vec4(0.35, 0.35, 0.4, 1.0);
    IndirectColor = vec4(0.35, 0.35, 0.4, 1.0);
}
)";

// Point-entity markers (lights, player start). Drawn as 3D line boxes so they are
// visible in the viewport and pickable by clicking. Writes both HDR targets so the
// composite's AO subtraction leaves the marker colour untouched.
static const char* markerVertSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform vec3 uColor;
out vec3 vColor;
void main() {
    vColor = uColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

static const char* markerFragSrc = R"(
#version 330 core
out vec4 FragColor;
out vec4 IndirectColor;
in vec3 vColor;
void main() {
    FragColor = vec4(vColor, 1.0);
    IndirectColor = vec4(vColor, 1.0);
}
)";

// Sun shadow depth pass: renders the map from the sun's viewpoint into a
// depth-only texture (the same ShadowMap pattern the engine uses). The matrix
// is recomputed and the pass re-rendered every frame, so any sun direction
// change immediately rebuilds the shadow map - no caching, no stale shadows.
static const char* sunDepthVertSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 uLightSpaceMatrix;
void main() {
    gl_Position = uLightSpaceMatrix * vec4(aPos, 1.0);
}
)";

static const char* sunDepthFragSrc = R"(
#version 330 core
void main() { }
)";

// ============================================================================
// Post-Processing & Pipeline (Tier 2)
// ----------------------------------------------------------------------------
// The viewport used to draw straight to the screen. Now the scene pass renders
// into an HDR framebuffer (two targets: the full lit colour and the "indirect"
// portion - ambient + point light - that SSAO should darken), a screen-space AO
// pass runs over the depth buffer, and a final composite applies SSAO, exposure
// (colour *= 2^EV) and Reinhard tonemapping + gamma to the screen.
//
// All parameter changes come from RenderPipeline (the centralized config that
// the UI setters write to). Every pass re-uploads its uniforms each frame, so
// dragging a slider takes effect immediately - no level rebuild, no shader
// recompile. The SSAO kernel/noise setup mirrors the engine's SSAO so the
// editor preview matches the game's look.
// ============================================================================

static const char* postQuadVertSrc = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUV;
out vec2 TexCoords;
void main() {
    TexCoords = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Depth-only SSAO (no G-buffer needed): reconstructs the view-space position
// from the non-linear depth texture, derives the normal from screen-space
// derivatives, then compares 32 hemisphere samples against the depth buffer.
// Strength multiplies the occlusion fraction, radius scales the sample offsets.
static const char* ssaoFragSrc = R"(
#version 330 core
out float FragColor;
in vec2 TexCoords;

uniform sampler2D uDepth;
uniform sampler2D uNoise;
uniform mat4 uProj;
uniform mat4 uInvProj;
uniform vec3 uKernel[32];
uniform vec2 uNoiseScale;
uniform float uStrength;
uniform float uRadius;

const float biasAmt = 0.025;
const int kernelSize = 32;

vec3 viewPosAt(vec2 uv) {
    float d = texture(uDepth, uv).r;
    if (d > 0.999) return vec3(0.0, 0.0, -1.0e6);  // sky / no geometry
    vec4 ndc = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 p = uInvProj * ndc;
    return p.xyz / p.w;
}

void main() {
    vec3 viewPos = viewPosAt(TexCoords);
    if (viewPos.z < -9.9e5) { FragColor = 1.0; return; }

    vec3 normal = normalize(cross(dFdx(viewPos), dFdy(viewPos)));
    if (normal.z < 0.0) normal = -normal;  // face the camera

    vec3 randomVec = normalize(texture(uNoise, TexCoords * uNoiseScale).xyz);
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < kernelSize; i++) {
        vec3 samplePos = TBN * uKernel[i];
        samplePos = viewPos + samplePos * uRadius;
        vec4 offset = uProj * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        offset.xy = offset.xy * 0.5 + 0.5;
        if (offset.x < 0.0 || offset.x > 1.0 || offset.y < 0.0 || offset.y > 1.0) continue;

        vec3 sampledView = viewPosAt(offset.xy);
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / abs(viewPos.z - sampledView.z));
        occlusion += (sampledView.z >= samplePos.z + biasAmt ? 1.0 : 0.0) * rangeCheck;
    }
    occlusion /= float(kernelSize);
    FragColor = clamp(1.0 - occlusion * uStrength, 0.0, 1.0);
}
)";

static const char* ssaoBlurFragSrc = R"(
#version 330 core
out float FragColor;
in vec2 TexCoords;
uniform sampler2D ssaoInput;
void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(ssaoInput, 0));
    float result = 0.0;
    float totalWeight = 0.0;
    for (int x = -2; x < 2; x++)
        for (int y = -2; y < 2; y++) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            float weight = 1.0 / (1.0 + float(x*x + y*y));
            result += texture(ssaoInput, TexCoords + offset).r * weight;
            totalWeight += weight;
        }
    FragColor = result / totalWeight;
}
)";

// Final composite. AO darkens only the indirect portion (sun light is not
// occluded, matching the engine). Exposure scales the HDR colour by 2^EV before
// Reinhard tonemapping + gamma correction.
static const char* tonemapFragSrc = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;
uniform sampler2D uHDR;
uniform sampler2D uIndirect;
uniform sampler2D uAO;
uniform float uExposureEV;
uniform float uAoEnabled;
void main() {
    vec3 lit = texture(uHDR, TexCoords).rgb;
    vec3 indirect = texture(uIndirect, TexCoords).rgb;
    float ao = mix(1.0, texture(uAO, TexCoords).r, uAoEnabled);
    vec3 color = lit - indirect + indirect * ao;
    color *= pow(2.0, uExposureEV);
    color = color / (color + vec3(1.0));      // Reinhard tonemap
    color = pow(color, vec3(1.0 / 2.2));      // gamma
    FragColor = vec4(color, 1.0);
}
)";

static float postLerp(float a, float b, float f) { return a + f * (b - a); }

struct PostFX {
    GLuint hdrFBO = 0, hdrColorTex = 0, hdrIndirectTex = 0, hdrDepthTex = 0;
    GLuint ssaoFBO = 0, ssaoTex = 0;
    GLuint ssaoBlurFBO = 0, ssaoBlurTex = 0;
    GLuint noiseTex = 0;
    GLuint quadVAO = 0, quadVBO = 0;
    GLuint ssaoShader = 0, blurShader = 0, tonemapShader = 0;
    std::vector<glm::vec3> kernel;
    int width = 0, height = 0;

    static GLuint makeDepthTex(int w, int h) {
        GLuint t;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return t;
    }

    static GLuint makeColorTex(int w, int h, GLenum fmt) {
        GLuint t;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt == GL_RGBA16F ? GL_RGBA : GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return t;
    }

    void init(int w, int h) {
        width = w; height = h;

        ssaoShader = buildProgram(postQuadVertSrc, ssaoFragSrc);
        blurShader = buildProgram(postQuadVertSrc, ssaoBlurFragSrc);
        tonemapShader = buildProgram(postQuadVertSrc, tonemapFragSrc);

        // SSAO kernel (32 hemisphere samples, denser near the surface) + 4x4
        // rotation noise - the same scheme the engine's SSAO uses. Fixed seed so
        // the preview is stable between runs.
        std::mt19937 gen(1234);
        std::uniform_real_distribution<float> rnd(0.0f, 1.0f);
        for (int i = 0; i < 32; i++) {
            glm::vec3 sample(rnd(gen) * 2.0f - 1.0f, rnd(gen) * 2.0f - 1.0f, rnd(gen));
            sample = glm::normalize(sample);
            sample *= rnd(gen);
            float scale = (float)i / 32.0f;
            scale = postLerp(0.1f, 1.0f, scale * scale);
            sample *= scale;
            kernel.push_back(sample);
        }
        std::vector<glm::vec3> noise;
        for (int i = 0; i < 16; i++)
            noise.push_back(glm::vec3(rnd(gen) * 2.0f - 1.0f, rnd(gen) * 2.0f - 1.0f, 0.0f));
        glGenTextures(1, &noiseTex);
        glBindTexture(GL_TEXTURE_2D, noiseTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 4, 4, 0, GL_RGB, GL_FLOAT, noise.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        // Full-screen quad for the passes.
        float verts[] = {
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 1.0f,
        };
        glGenVertexArrays(1, &quadVAO);
        glGenBuffers(1, &quadVBO);
        glBindVertexArray(quadVAO);
        glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);

        // HDR scene FBO: colour (target 0) + indirect (target 1) + depth.
        glGenFramebuffers(1, &hdrFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);
        hdrColorTex = makeColorTex(w, h, GL_RGBA16F);
        hdrIndirectTex = makeColorTex(w, h, GL_RGBA16F);
        hdrDepthTex = makeDepthTex(w, h);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdrColorTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, hdrIndirectTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, hdrDepthTex, 0);
        GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, drawBufs);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            cerr << "HDR FBO incomplete - post-processing disabled\n";

        // SSAO FBO (raw occlusion).
        glGenFramebuffers(1, &ssaoFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO);
        ssaoTex = makeColorTex(w, h, GL_RED);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoTex, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            cerr << "SSAO FBO incomplete\n";

        // SSAO blur FBO (smoothed occlusion).
        glGenFramebuffers(1, &ssaoBlurFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoBlurFBO);
        ssaoBlurTex = makeColorTex(w, h, GL_RED);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoBlurTex, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            cerr << "SSAO blur FBO incomplete\n";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void resize(int w, int h) {
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        width = w; height = h;

        glDeleteTextures(1, &hdrColorTex); glDeleteTextures(1, &hdrIndirectTex); glDeleteTextures(1, &hdrDepthTex);
        glDeleteTextures(1, &ssaoTex); glDeleteTextures(1, &ssaoBlurTex);

        glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);
        hdrColorTex = makeColorTex(w, h, GL_RGBA16F);
        hdrIndirectTex = makeColorTex(w, h, GL_RGBA16F);
        hdrDepthTex = makeDepthTex(w, h);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdrColorTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, hdrIndirectTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, hdrDepthTex, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO);
        ssaoTex = makeColorTex(w, h, GL_RED);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoTex, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, ssaoBlurFBO);
        ssaoBlurTex = makeColorTex(w, h, GL_RED);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoBlurTex, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void drawQuad() const {
        glBindVertexArray(quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // Screen-space AO: reads the HDR depth texture, writes the smoothed AO map.
    // Strength/radius come from RenderPipeline (updated live by the UI setters).
    void renderSSAO(const glm::mat4& proj, const glm::mat4& invProj) {
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO);
        glViewport(0, 0, width, height);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(ssaoShader);
        glUniformMatrix4fv(glGetUniformLocation(ssaoShader, "uProj"), 1, GL_FALSE, &proj[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(ssaoShader, "uInvProj"), 1, GL_FALSE, &invProj[0][0]);
        glUniform3fv(glGetUniformLocation(ssaoShader, "uKernel"), 32, &kernel[0][0]);
        glUniform2f(glGetUniformLocation(ssaoShader, "uNoiseScale"), width / 4.0f, height / 4.0f);
        glUniform1f(glGetUniformLocation(ssaoShader, "uStrength"), RenderPipeline::ssaoStrength());
        glUniform1f(glGetUniformLocation(ssaoShader, "uRadius"), RenderPipeline::ssaoRadius());
        glUniform1i(glGetUniformLocation(ssaoShader, "uDepth"), 0);
        glUniform1i(glGetUniformLocation(ssaoShader, "uNoise"), 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdrDepthTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, noiseTex);
        drawQuad();

        // Blur pass.
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoBlurFBO);
        glViewport(0, 0, width, height);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(blurShader);
        glUniform1i(glGetUniformLocation(blurShader, "ssaoInput"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ssaoTex);
        drawQuad();
    }

    // Composite: SSAO + exposure (2^EV) + Reinhard + gamma. Defaults to the
    // screen; pass a target FBO to composite into a texture shown in the viewport.
    void renderComposite(GLuint targetFBO = 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);

        glUseProgram(tonemapShader);
        glUniform1f(glGetUniformLocation(tonemapShader, "uExposureEV"), RenderPipeline::exposureEV());
        glUniform1f(glGetUniformLocation(tonemapShader, "uAoEnabled"), RenderPipeline::ssaoEnabled() ? 1.0f : 0.0f);
        glUniform1i(glGetUniformLocation(tonemapShader, "uHDR"), 0);
        glUniform1i(glGetUniformLocation(tonemapShader, "uIndirect"), 1);
        glUniform1i(glGetUniformLocation(tonemapShader, "uAO"), 2);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdrColorTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, hdrIndirectTex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ssaoBlurTex);
        drawQuad();

        glEnable(GL_DEPTH_TEST);
    }

    void destroy() {
        glDeleteTextures(1, &hdrColorTex); glDeleteTextures(1, &hdrIndirectTex); glDeleteTextures(1, &hdrDepthTex);
        glDeleteTextures(1, &ssaoTex); glDeleteTextures(1, &ssaoBlurTex); glDeleteTextures(1, &noiseTex);
        glDeleteFramebuffers(1, &hdrFBO); glDeleteFramebuffers(1, &ssaoFBO); glDeleteFramebuffers(1, &ssaoBlurFBO);
        glDeleteVertexArrays(1, &quadVAO); glDeleteBuffers(1, &quadVBO);
        glDeleteProgram(ssaoShader); glDeleteProgram(blurShader); glDeleteProgram(tonemapShader);
    }
};

// ============================================================================
// Lighting state + persistence
// ============================================================================
// Tier-1 lighting persistence split:
//   - Point lights live inside the .map as "light" entity keyvalues (radius is
//     stored in map units, which the engine divides by MAP_SCALE = 1/32).
//   - Sun + ambient are editor-only values stored in a sidecar .lighting.cfg
//     key=value file next to the .map (no JSON library in the repo).
// The Save button writes BOTH: the sidecar and the edited light-entity
// keyvalues written back into the .map file in place.
//   - LightingState / load / save / sidecarPathFor are SHARED with the runtime
//     (KEA/mapfile.h + mapfile.cpp) so the game can apply the same config.
// ============================================================================

static const float MAP_SCALE = 1.0f / 32.0f;
static const int MAX_LIGHTS = 16;
static const int SHADOW_RES = 2048;

// Inverse of mapfile.cpp convertPos: engine pos (y-up) -> .map origin (z-up).
// The .map stores origins in map units, the engine works in metres (x MAP_SCALE).
static glm::vec3 engineToMapOrigin(const glm::vec3& enginePos) {
    return glm::vec3(enginePos.x, -enginePos.z, enginePos.y) / MAP_SCALE;
}

// Forward of mapfile.cpp convertPos: .map origin (map units, z-up) -> engine
// pos (metres, y-up). Used to place point-entity markers in the viewport.
static glm::vec3 mapOriginToEngine(const glm::vec3& mapOrigin) {
    return glm::vec3(mapOrigin.x, mapOrigin.z, -mapOrigin.y) * MAP_SCALE;
}

static string formatVec3(const glm::vec3& v) {
    return std::to_string(v.x) + " " + std::to_string(v.y) + " " + std::to_string(v.z);
}

static vector<float> parseVec3(const string& s) {
    vector<float> v;
    stringstream ss(s);
    float f;
    while (ss >> f) v.push_back(f);
    return v;
}

// Write the edited "light" entity keyvalues back into the .map file, preserving
// everything else byte-for-byte. Mirrors MapLoader's parseEntities depth logic:
// '{' at depth 1 opens an entity, '}' at depth 1 closes it. Only light entities
// flagged dirty in `lightDirty` are touched, and only their color/intensity/
// radius value strings are rewritten in place (a missing key is inserted right
// before the closing brace); origin/classname and every other line stay as-is.
static bool saveLightEntitiesToMap(const string& mapPath,
                                   const vector<MapEntity>& entities,
                                   const vector<int>& lightEntityIndices,
                                   const vector<bool>& lightDirty) {
    ifstream in(mapPath, ios::binary);
    if (!in) return false;
    vector<string> lines;
    string line;
    while (getline(in, line)) lines.push_back(line);
    in.close();

    // Preserve the file's line ending style for rewritten lines (CRLF maps,
    // which binary getline leaves the trailing '\r' on, are the norm here).
    bool crlf = false;
    for (const string& l : lines)
        if (!l.empty()) { crlf = (l.back() == '\r'); break; }

    vector<bool> isEdited(entities.size(), false);
    for (size_t k = 0; k < lightEntityIndices.size() && k < lightDirty.size(); k++)
        if (lightDirty[k]) {
            int idx = lightEntityIndices[k];
            if (idx >= 0 && idx < (int)entities.size()) isEdited[idx] = true;
        }

    auto isClosingBrace = [](const string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == string::npos) return false;
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1) == "}";
    };

    vector<string> out;
    out.reserve(lines.size() + 8);
    int depth = 0;
    int entityIndex = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        const string& raw = lines[i];
        string t = raw;
        size_t bs = t.find_first_not_of(" \t\r\n");
        if (bs == string::npos) { out.push_back(raw); continue; }
        size_t be = t.find_last_not_of(" \t\r\n");
        t = t.substr(bs, be - bs + 1);

        if (t == "{") {
            depth++;
            if (depth == 1) {
                entityIndex++;
                if (entityIndex < (int)entities.size() && isEdited[entityIndex]) {
                    const MapEntity& ent = entities[entityIndex];
                    vector<string> keysToWrite = { "color", "intensity", "radius" };
                    vector<bool> found(3, false);
                    out.push_back(raw);
                    i++;
                    while (i < lines.size() && !isClosingBrace(lines[i])) {
                        const string& l = lines[i];
                        size_t q1 = l.find('"');
                        size_t q2 = q1 == string::npos ? string::npos : l.find('"', q1 + 1);
                        size_t q3 = q2 == string::npos ? string::npos : l.find('"', q2 + 1);
                        size_t q4 = q3 == string::npos ? string::npos : l.find('"', q3 + 1);
                        if (q1 != string::npos && q4 != string::npos) {
                            string key = l.substr(q1 + 1, q2 - q1 - 1);
                            size_t kIdx = (key == "color") ? 0 : (key == "intensity") ? 1 : (key == "radius") ? 2 : 3;
                            if (kIdx < 3) {
                                auto propIt = ent.properties.find(key);
                                if (propIt != ent.properties.end()) {
                                    out.push_back("\"" + key + "\" \"" + propIt->second + "\"" + (crlf ? "\r" : ""));
                                    found[kIdx] = true;
                                }
                                else {
                                    out.push_back(l);  // key removed in memory: keep file line
                                }
                            }
                            else {
                                out.push_back(l);
                            }
                        }
                        else {
                            out.push_back(l);
                        }
                        i++;
                    }
                    for (size_t kIdx = 0; kIdx < 3; kIdx++) {
                        if (!found[kIdx]) {
                            const string& key = keysToWrite[kIdx];
                            auto propIt = ent.properties.find(key);
                            if (propIt != ent.properties.end())
                                out.push_back("\"" + key + "\" \"" + propIt->second + "\"" + (crlf ? "\r" : ""));
                        }
                    }
                    if (i < lines.size()) { depth--; out.push_back(lines[i]); }
                    continue;
                }
            }
        }
        else if (t == "}") {
            depth--;
        }
        out.push_back(raw);
    }

    string tmpPath = mapPath + ".tmp";
    {
        ofstream fout(tmpPath, ios::binary | ios::trunc);
        if (!fout) return false;
        for (size_t i = 0; i < out.size(); i++)
            fout << out[i] << "\n";
    }
    if (remove(mapPath.c_str()) != 0) return false;
    if (rename(tmpPath.c_str(), mapPath.c_str()) != 0) return false;
    return true;
}

// Rewrite the transform keyvalues ("origin", "angles", "angle", "scale") of any
// entity flagged dirty in `entityDirty` back into the .map file, preserving
// everything else byte-for-byte. Mirrors saveLightEntitiesToMap's depth-walking
// (entities open/close at depth 1): a managed key present in the block is
// rewritten in place, a managed key absent from properties is dropped from the
// file (e.g. "angle" when the gizmo wrote full "angles"), and missing keys are
// inserted right before the closing brace. Used to persist gizmo moves,
// rotations and scales.
static bool saveEntityPropsToMap(const string& mapPath,
                                 const vector<MapEntity>& entities,
                                 const vector<bool>& entityDirty) {
    static const char* managedKeys[] = { "origin", "angles", "angle", "scale" };
    const int numManaged = (int)(sizeof(managedKeys) / sizeof(managedKeys[0]));

    ifstream in(mapPath, ios::binary);
    if (!in) return false;
    vector<string> lines;
    string line;
    while (getline(in, line)) lines.push_back(line);
    in.close();

    bool crlf = false;
    for (const string& l : lines)
        if (!l.empty()) { crlf = (l.back() == '\r'); break; }

    auto isClosingBrace = [](const string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == string::npos) return false;
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1) == "}";
    };

    auto managedIndex = [&](const string& key) -> int {
        for (int k = 0; k < numManaged; k++)
            if (key == managedKeys[k]) return k;
        return -1;
    };

    vector<string> out;
    out.reserve(lines.size() + 8);
    int depth = 0;
    int entityIndex = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        const string& raw = lines[i];
        string t = raw;
        size_t bs = t.find_first_not_of(" \t\r\n");
        if (bs == string::npos) { out.push_back(raw); continue; }
        size_t be = t.find_last_not_of(" \t\r\n");
        t = t.substr(bs, be - bs + 1);

        if (t == "{") {
            depth++;
            if (depth == 1) {
                entityIndex++;
                if (entityIndex < (int)entities.size() && entityIndex < (int)entityDirty.size()
                    && entityDirty[entityIndex]) {
                    const MapEntity& ent = entities[entityIndex];
                    vector<bool> found(numManaged, false);
                    out.push_back(raw);
                    i++;
                    while (i < lines.size() && !isClosingBrace(lines[i])) {
                        const string& l = lines[i];
                        size_t q1 = l.find('"');
                        size_t q2 = q1 == string::npos ? string::npos : l.find('"', q1 + 1);
                        size_t q3 = q2 == string::npos ? string::npos : l.find('"', q2 + 1);
                        size_t q4 = q3 == string::npos ? string::npos : l.find('"', q3 + 1);
                        if (q1 != string::npos && q4 != string::npos) {
                            string key = l.substr(q1 + 1, q2 - q1 - 1);
                            int kIdx = managedIndex(key);
                            if (kIdx >= 0) {
                                auto propIt = ent.properties.find(key);
                                if (propIt != ent.properties.end()) {
                                    out.push_back("\"" + key + "\" \"" + propIt->second + "\"" + (crlf ? "\r" : ""));
                                    found[kIdx] = true;
                                }
                                // key removed in memory (e.g. "angle" -> "angles"):
                                // drop the file line entirely
                            }
                            else {
                                out.push_back(l);
                            }
                        }
                        else {
                            out.push_back(l);
                        }
                        i++;
                    }
                    for (int kIdx = 0; kIdx < numManaged; kIdx++) {
                        if (!found[kIdx]) {
                            const string& key = managedKeys[kIdx];
                            auto propIt = ent.properties.find(key);
                            if (propIt != ent.properties.end())
                                out.push_back("\"" + key + "\" \"" + propIt->second + "\"" + (crlf ? "\r" : ""));
                        }
                    }
                    if (i < lines.size()) { depth--; out.push_back(lines[i]); }
                    continue;
                }
            }
        }
        else if (t == "}") {
            depth--;
        }
        out.push_back(raw);
    }

    string tmpPath = mapPath + ".tmp";
    {
        ofstream fout(tmpPath, ios::binary | ios::trunc);
        if (!fout) return false;
        for (size_t i = 0; i < out.size(); i++)
            fout << out[i] << "\n";
    }
    if (remove(mapPath.c_str()) != 0) return false;
    if (rename(tmpPath.c_str(), mapPath.c_str()) != 0) return false;
    return true;
}

// ----------------------------------------------------------------------------
// Persist arbitrary keyvalue edits (add / delete / retype) made from the
// generic entity Properties panel. Only entities flagged dirty in `propsDirty`
// are rewritten; structural keys (classname plus everything another save path
// already manages: origin/angles/angle/scale + light color/intensity/radius)
// pass through untouched so those rewriters stay the single source of truth.
// Entities added in the editor that have no block in the file yet get a full
// block serialized at the end of the .map. Mirrors the depth walking of the
// two helpers above ('{' at depth 1 opens an entity, '}' at depth 1 closes).
// ----------------------------------------------------------------------------
static bool saveGenericKeysToMap(const string& mapPath,
                                 const vector<MapEntity>& entities,
                                 const vector<bool>& propsDirty) {
    static const char* structuralKeys[] = {
        "classname", "origin", "angles", "angle", "scale",
        "color", "intensity", "radius"
    };
    auto isStructural = [](const string& key) -> bool {
        for (const char* k : structuralKeys)
            if (key == k) return true;
        return false;
    };

    ifstream in(mapPath, ios::binary);
    if (!in) return false;
    vector<string> lines;
    string line;
    while (getline(in, line)) lines.push_back(line);
    in.close();

    bool crlf = false;
    for (const string& l : lines)
        if (!l.empty()) { crlf = (l.back() == '\r'); break; }

    auto isClosingBrace = [](const string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == string::npos) return false;
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1) == "}";
    };

    // Pull the key out of a `"key" "value"` line ("" when not a keyvalue).
    auto lineKey = [](const string& l) -> string {
        size_t q1 = l.find('"');
        size_t q2 = q1 == string::npos ? string::npos : l.find('"', q1 + 1);
        if (q1 == string::npos || q2 == string::npos) return "";
        return l.substr(q1 + 1, q2 - q1 - 1);
    };

    vector<string> out;
    out.reserve(lines.size() + 64);
    vector<bool> sawBlock(entities.size(), false);
    int depth = 0;
    int entityIndex = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        const string& raw = lines[i];
        string t = raw;
        size_t bs = t.find_first_not_of(" \t\r\n");
        if (bs == string::npos) { out.push_back(raw); continue; }
        size_t be = t.find_last_not_of(" \t\r\n");
        t = t.substr(bs, be - bs + 1);

        if (t == "{") {
            depth++;
            if (depth == 1) {
                entityIndex++;
                if (entityIndex < (int)entities.size() && entityIndex < (int)propsDirty.size()
                    && propsDirty[entityIndex]) {
                    const MapEntity& ent = entities[entityIndex];
                    out.push_back(raw);
                    sawBlock[entityIndex] = true;
                    i++;

                    // Collect the block interior once so we can rewrite existing
                    // generic key lines, drop deleted ones, and splice new keys
                    // in after the last existing keyvalue (before any brushes).
                    vector<string> block;
                    while (i < lines.size() && !isClosingBrace(lines[i]))
                        block.push_back(lines[i++]);

                    vector<string> newBlock;
                    size_t lastKv = 0;      // index in newBlock after the last keyvalue
                    for (const string& l : block) {
                        string key = lineKey(l);
                        if (key.empty() || isStructural(key)) { newBlock.push_back(l); continue; }
                        auto propIt = ent.properties.find(key);
                        if (propIt == ent.properties.end()) continue;   // deleted in panel
                        newBlock.push_back("\"" + key + "\" \"" + propIt->second + "\"" + (crlf ? "\r" : ""));
                        lastKv = newBlock.size();
                    }

                    // New keys added in the panel: sorted insert after last keyvalue.
                    vector<pair<string, string>> fresh;
                    for (auto& kv : ent.properties) {
                        if (isStructural(kv.first)) continue;
                        bool present = false;
                        for (const string& l : block)
                            if (lineKey(l) == kv.first) { present = true; break; }
                        if (!present) fresh.emplace_back(kv.first, kv.second);
                    }
                    std::sort(fresh.begin(), fresh.end());
                    vector<string> inserts;
                    for (auto& f : fresh)
                        inserts.push_back("\"" + f.first + "\" \"" + f.second + "\"" + (crlf ? "\r" : ""));
                    newBlock.insert(newBlock.begin() + lastKv, inserts.begin(), inserts.end());
                    for (auto& nl : newBlock) out.push_back(nl);

                    if (i < lines.size()) { depth--; out.push_back(lines[i]); }
                    continue;
                }
            }
        }
        else if (t == "}") depth--;
        out.push_back(raw);
    }

    // Append entities the editor created that have no block in the file yet.
    for (size_t i = 0; i < entities.size(); i++) {
        if (i >= propsDirty.size() || !propsDirty[i] || sawBlock[i]) continue;
        out.push_back("{" + string(crlf ? "\r" : ""));
        out.push_back("\"classname\" \"" + entities[i].classname + "\"" + (crlf ? "\r" : ""));
        for (auto& kv : entities[i].properties)
            out.push_back("\"" + kv.first + "\" \"" + kv.second + "\"" + (crlf ? "\r" : ""));
        out.push_back("}" + string(crlf ? "\r" : ""));
    }

    string tmpPath = mapPath + ".tmp";
    {
        ofstream fout(tmpPath, ios::binary | ios::trunc);
        if (!fout) return false;
        for (size_t i = 0; i < out.size(); i++)
            fout << out[i] << "\n";
    }
    if (remove(mapPath.c_str()) != 0) return false;
    if (rename(tmpPath.c_str(), mapPath.c_str()) != 0) return false;
    return true;
}

// ----------------------------------------------------------------------------
// Unity-style dark editor theme. Mirrors Unity's "Pro" skin: flat charcoal
// surfaces, light-gray text, and a single blue accent used for selection and
// active controls. Everything is kept plain and uncluttered.
// ----------------------------------------------------------------------------
static void ApplyUnityTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImVec4* c = st.Colors;

    // Surface / text palette - darker Unity-style theme
    const ImVec4 kWindowBg     (0.090f, 0.090f, 0.090f, 1.00f); // #171717 panels (darker)
    const ImVec4 kChildBg      (0.110f, 0.110f, 0.110f, 1.00f); // #1C1C1C child windows (darker)
    const ImVec4 kToolBg       (0.070f, 0.070f, 0.070f, 1.00f); // #121212 toolbars/menus (darker)
    const ImVec4 kTitleBg      (0.050f, 0.050f, 0.050f, 1.00f); // #0D0D0D title bars (darker)
    const ImVec4 kFrameBg      (0.060f, 0.060f, 0.060f, 1.00f); // #0F0F0F input frames (darker)
    const ImVec4 kFrameHover   (0.100f, 0.100f, 0.100f, 1.00f);
    const ImVec4 kFrameActive  (0.130f, 0.130f, 0.130f, 1.00f);
    const ImVec4 kHoverBg      (0.150f, 0.150f, 0.150f, 1.00f); // #262626 hover (darker)
    const ImVec4 kActiveBg     (0.180f, 0.180f, 0.180f, 1.00f); // #2E2E2E pressed (darker)
    const ImVec4 kHeaderBg     (0.130f, 0.130f, 0.130f, 1.00f); // #212121 selection/list headers (darker)
    const ImVec4 kBorder       (0.040f, 0.040f, 0.040f, 1.00f); // #0A0A0A (darker)
    const ImVec4 kText         (0.831f, 0.831f, 0.831f, 1.00f); // #D4D4D4
    const ImVec4 kTextDisabled (0.478f, 0.478f, 0.478f, 1.00f); // #7A7A7A
    const ImVec4 kAccent       (0.231f, 0.510f, 0.969f, 1.00f); // #3B82F7 Unity blue

    // Flat, squared-off geometry (Unity is flat, not rounded)
    st.WindowRounding    = 0.0f;
    st.ChildRounding     = 0.0f;
    st.FrameRounding     = 2.0f;
    st.PopupRounding     = 0.0f;
    st.ScrollbarRounding = 12.0f;
    st.GrabRounding      = 2.0f;
    st.TabRounding       = 2.0f;
    st.WindowBorderSize  = 1.0f;
    st.ChildBorderSize   = 1.0f;
    st.PopupBorderSize   = 1.0f;
    st.FrameBorderSize   = 0.0f;
    st.TabBorderSize     = 0.0f;
    st.TabBarBorderSize  = 1.0f;
    st.TabBarOverlineSize= 0.0f;

    // Spacing / padding
    st.WindowPadding     = ImVec2(8.0f, 8.0f);
    st.FramePadding      = ImVec2(8.0f, 4.0f);
    st.ItemSpacing       = ImVec2(6.0f, 4.0f);
    st.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    st.CellPadding       = ImVec2(6.0f, 3.0f);
    st.ScrollbarSize     = 12.0f;
    st.ScrollbarPadding  = 0.0f;
    st.GrabMinSize       = 8.0f;
    st.DockingSeparatorSize = 1.0f;

    // Colors
    c[ImGuiCol_Text]                  = kText;
    c[ImGuiCol_TextDisabled]          = kTextDisabled;
    c[ImGuiCol_TextLink]              = kAccent;
    c[ImGuiCol_TextSelectedBg]        = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f);
    c[ImGuiCol_WindowBg]              = kWindowBg;
    c[ImGuiCol_ChildBg]               = kChildBg;
    c[ImGuiCol_PopupBg]               = kToolBg;
    c[ImGuiCol_Border]                = kBorder;
    c[ImGuiCol_BorderShadow]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]               = kFrameBg;
    c[ImGuiCol_FrameBgHovered]        = kFrameHover;
    c[ImGuiCol_FrameBgActive]         = kFrameActive;
    c[ImGuiCol_TitleBg]               = kTitleBg;
    c[ImGuiCol_TitleBgActive]         = kTitleBg;
    c[ImGuiCol_TitleBgCollapsed]      = kTitleBg;
    c[ImGuiCol_MenuBarBg]             = kToolBg;
    c[ImGuiCol_ScrollbarBg]           = kToolBg;
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.140f, 0.140f, 0.140f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.200f, 0.200f, 0.200f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.260f, 0.260f, 0.260f, 1.0f);
    c[ImGuiCol_CheckMark]             = kAccent;
    c[ImGuiCol_CheckboxSelectedBg]    = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    c[ImGuiCol_SliderGrab]            = kAccent;
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.302f, 0.561f, 1.000f, 1.0f);
    c[ImGuiCol_Button]                = kFrameBg;
    c[ImGuiCol_ButtonHovered]         = kHoverBg;
    c[ImGuiCol_ButtonActive]          = kActiveBg;
    c[ImGuiCol_Header]                = kHeaderBg;
    c[ImGuiCol_HeaderHovered]         = kHoverBg;
    c[ImGuiCol_HeaderActive]          = kActiveBg;
    c[ImGuiCol_Separator]             = kBorder;
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.250f, 0.250f, 0.250f, 1.0f);
    c[ImGuiCol_SeparatorActive]       = kAccent;
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.180f, 0.180f, 0.180f, 0.35f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.250f, 0.250f, 0.250f, 0.70f);
    c[ImGuiCol_ResizeGripActive]      = kAccent;
    c[ImGuiCol_InputTextCursor]       = kText;
    c[ImGuiCol_Tab]                   = kToolBg;
    c[ImGuiCol_TabHovered]            = kHoverBg;
    c[ImGuiCol_TabSelected]           = kActiveBg;
    c[ImGuiCol_TabSelectedOverline]   = kAccent;
    c[ImGuiCol_TabDimmed]             = kToolBg;
    c[ImGuiCol_TabDimmedSelected]     = kHeaderBg;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_DockingPreview]        = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f);
    c[ImGuiCol_DockingEmptyBg]        = ImVec4(0.050f, 0.050f, 0.050f, 1.0f);
    c[ImGuiCol_PlotLines]             = kAccent;
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c[ImGuiCol_PlotHistogram]         = kAccent;
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c[ImGuiCol_TableHeaderBg]         = kHeaderBg;
    c[ImGuiCol_TableBorderStrong]     = kBorder;
    c[ImGuiCol_TableBorderLight]      = kBorder;
    c[ImGuiCol_TableRowBg]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(0.050f, 0.050f, 0.050f, 0.15f);
    c[ImGuiCol_TreeLines]             = ImVec4(0.450f, 0.450f, 0.450f, 0.50f);
    c[ImGuiCol_DragDropTarget]        = kAccent;
    c[ImGuiCol_DragDropTargetBg]      = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.25f);
    c[ImGuiCol_NavCursor]             = kAccent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

// ----------------------------------------------------------------------------
// Undo / Redo
// ----------------------------------------------------------------------------
// Snapshot-based history for Ctrl+Z (undo) / Ctrl+Y (redo). Every user action
// that edits scene state (gizmo transforms, light / sun / ambient properties,
// post-processing settings) pushes a full snapshot of the editable state onto
// the undo stack before the change lands. Undo moves the current state to the
// redo stack and restores the top of the undo stack; redo does the reverse.
// Only in-memory state is touched - nothing is written to disk until Save.
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
    std::vector<bool> propsDirty;
};

int main() {
    std::ofstream("DBG_LOG", std::ios::trunc)
        << "main entered\n";
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1400, 900, "KEA - Map Editor", nullptr, nullptr);
    if (!window) {
        cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        cerr << "Failed to initialize GLAD\n";
        return 1;
    }
    std::ofstream("DBG_LOG", std::ios::app)
        << "window+glad done\n";

    glEnable(GL_DEPTH_TEST);
    // Brushes are rendered double-sided (winding order isn't guaranteed), matching
    // the engine's G-buffer pass which disables culling for the map geometry.
    glDisable(GL_CULL_FACE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // dockable panels, Nuake-style layout
    EditorSettings_Register();  // persist snap/op/mode in imgui.ini (before first NewFrame loads it)
    // Old-school pixel font (crisp, DOS-style). Falls back to Segoe UI / the
    // default font if the bundled TTF can't be loaded.
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 1;
    fontCfg.OversampleV = 1;
    fontCfg.PixelSnapH = true;
    if (io.Fonts->AddFontFromFileTTF(FONT_PATH.c_str(), 16.0f, &fontCfg) == nullptr) {
        fontCfg.OversampleH = 2;
        fontCfg.OversampleV = 2;
        if (io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 15.0f, &fontCfg) == nullptr)
            io.Fonts->AddFontDefault();
    }
    ApplyUnityTheme();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Load the shared entity definitions so the panel can show class descriptions.
    FgdFile fgd = FgdParser::load(
        (workspaceRoot() + "/GameRoot/kea.fgd"));
    std::ofstream("DBG_LOG", std::ios::app)
        << "fgd loaded\n";

    auto textureSizeLookup = [](const string& texName) -> glm::ivec2 {
        return Texture::getImageSize(TEXTURES_FOLDER + texName + ".png");
    };
    std::ofstream("DBG_LOG", std::ios::app)
        << "pre-mapload done\n";

    LevelData level;
    try {
        level = MapLoader::load(MAP_PATH, textureSizeLookup);
        std::ofstream("DBG_LOG", std::ios::app)
            << "map loaded\n";
    }
    catch (const std::exception& e) {
        std::ofstream("DBG_LOG", std::ios::app)
            << "map load EXCEPTION: " << e.what() << "\n";
    }
    catch (...) {
        std::ofstream("DBG_LOG", std::ios::app)
            << "map load UNKNOWN EXCEPTION\n";
    }
    cout << "Loaded map: " << level.renderChunks.size() << " texture chunks, "
        << (level.collisionVertices.size() / 9) << " collision triangles, "
        << level.pointLights.size() << " point lights, "
        << level.entities.size() << " entities\n";

    // Point-light persistence model: level.pointLights[k] is the k-th "light"
    // entity in level.entities (same parse order), so we can keep the live
    // renderer list and the .map keyvalues in sync by editing both.
    vector<int> lightEntityIndices;
    for (size_t i = 0; i < level.entities.size(); i++)
        if (level.entities[i].classname == "light") lightEntityIndices.push_back((int)i);

    vector<bool> lightDirty(lightEntityIndices.size(), false);

    // Any entity whose origin is moved by the viewport gizmo is flagged here so
    // the Save button can write the new "origin" keyvalue back into the .map.
    vector<bool> entityDirty(level.entities.size(), false);

    // Any entity whose keyvalues were edited/added/removed via the generic
    // Properties panel is flagged here so Save writes those arbitrary keys.
    vector<bool> propsDirty(level.entities.size(), false);

    // Live engine-space position for a point entity (drives its viewport marker
    // and the gizmo). Lights and the player start keep a live engine copy that
    // the panel/gizmo both edit; other entities read their .map "origin" key.
    auto entityEnginePos = [&](int idx) -> glm::vec3 {
        if (idx < 0 || idx >= (int)level.entities.size()) return glm::vec3(0.0f);
        const MapEntity& ent = level.entities[idx];
        if (ent.classname == "light") {
            for (size_t k = 0; k < lightEntityIndices.size() && k < level.pointLights.size(); k++)
                if (lightEntityIndices[k] == idx) return level.pointLights[k].position;
        }
        if (ent.classname == "info_player_start") return level.playerStart;
        vector<float> v = parseVec3(ent.properties.count("origin") ? ent.properties.at("origin") : "");
        if (v.size() < 3) return glm::vec3(0.0f);
        return mapOriginToEngine(glm::vec3(v[0], v[1], v[2]));
    };

    auto markerSizeFor = [](const string& classname) -> float {
        if (classname == "light") return 0.30f;
        if (classname == "info_player_start") return 0.45f;
        return 0.20f;
    };

    auto markerColorFor = [](const string& classname) -> glm::vec3 {
        if (classname == "light") return glm::vec3(1.0f, 0.85f, 0.2f);
        if (classname == "info_player_start") return glm::vec3(0.3f, 1.0f, 0.4f);
        return glm::vec3(0.5f, 0.7f, 1.0f);
    };

    // Write a live engine position back to the entity's .map "origin" keyvalue
    // (in map units) and any cached engine copy (lights / player start).
    auto setEntityEnginePos = [&](int idx, const glm::vec3& enginePos) {
        glm::vec3 mapOrigin = engineToMapOrigin(enginePos);
        level.entities[idx].properties["origin"] = formatVec3(mapOrigin);
        entityDirty[idx] = true;
        if (level.entities[idx].classname == "light") {
            for (size_t k = 0; k < lightEntityIndices.size(); k++)
                if (lightEntityIndices[k] == idx) {
                    level.pointLights[k].position = enginePos;
                    lightDirty[k] = true;
                }
        }
        else if (level.entities[idx].classname == "info_player_start") {
            level.playerStart = enginePos;
            level.hasPlayerStart = true;
        }
    };

    // --- Rotation / scale state ---------------------------------------------
    // The editor keeps one quaternion + uniform scale per entity (engine space,
    // y-up). The .map stores orientation as "Pitch Yaw Roll" degrees in z-up
    // map space (FGD "P Y R", TrenchBroom order), plus a legacy single "angle"
    // (yaw) key that "angles" overrides.
    //
    // Map->engine axis mapping (convertPos (x,y,z)->(x,z,-y)):
    //   map pitch (around map X) == engine pitch (around engine X)
    //   map yaw   (around map Z) == engine yaw   (around engine Y)
    //   map roll  (around map Y) == -engine roll (around engine Z)  [sign flip]
    // Engine rotation matrix R = Ry(roll) * Rz(yaw) * Rx(pitch), which matches
    // the axis order ImGuizmo's decompose/recompose helpers use.
    auto fmtDeg = [](float v) -> string {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.3f", v);
        return string(buf);
    };
    auto eulerToQuat = [](float pitchDeg, float yawDeg, float rollDeg) -> glm::quat {
        glm::mat4 R = glm::mat4(1.0f);
        R = glm::rotate(R, glm::radians(rollDeg), glm::vec3(0.0f, 1.0f, 0.0f));   // Ry
        R = glm::rotate(R, glm::radians(yawDeg), glm::vec3(0.0f, 0.0f, 1.0f));    // Rz
        R = glm::rotate(R, glm::radians(pitchDeg), glm::vec3(1.0f, 0.0f, 0.0f));  // Rx
        return glm::quat_cast(R);
    };
    auto quatToEuler = [](const glm::quat& q, float& pitch, float& yaw, float& roll) {
        glm::mat3 R = glm::mat3_cast(q);
        yaw = glm::degrees(atan2f(R[0][1], sqrtf(R[1][1] * R[1][1] + R[2][1] * R[2][1])));
        pitch = glm::degrees(atan2f(-R[2][1], R[1][1]));
        roll = -glm::degrees(atan2f(R[0][2], R[0][0]));
    };
    auto parseAngleValue = [](const string& s, float def) -> float {
        vector<float> v = parseVec3(s);
        return v.empty() ? def : v[0];
    };

    vector<glm::quat> entityRot(level.entities.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    vector<float> entityScale(level.entities.size(), 1.0f);
    for (size_t i = 0; i < level.entities.size(); i++) {
        const MapEntity& ent = level.entities[i];
        auto ait = ent.properties.find("angles");
        if (ait != ent.properties.end()) {
            vector<float> a = parseVec3(ait->second);
            if (a.size() >= 3)
                entityRot[i] = eulerToQuat(a[0], a[1], -a[2]);  // map roll = -engine roll
            else if (a.size() >= 1)
                entityRot[i] = eulerToQuat(0.0f, a[0], 0.0f);
        }
        else {
            auto git = ent.properties.find("angle");
            if (git != ent.properties.end())
                entityRot[i] = eulerToQuat(0.0f, parseAngleValue(git->second, 0.0f), 0.0f);
        }
        auto sit = ent.properties.find("scale");
        if (sit != ent.properties.end()) {
            float sc = parseAngleValue(sit->second, 1.0f);
            entityScale[i] = sc > 0.0001f ? sc : 1.0f;
        }
    }

    // OBJ previews for mesh entities (FGD "model" key). Loaded once on the GL
    // context: the mesh stays in model space and is transformed by the same
    // translate * rotate * scale matrix the markers/gizmo use. reloadEntityModel()
    // re-reads the entity's "model" keyvalue and rebuilds the preview so the
    // Properties panel / duplicate / hot-reload all reuse one code path.
    vector<ObjMesh> entityModels(level.entities.size());
    vector<bool> entityHasModel(level.entities.size(), false);
    auto reloadEntityModel = [&](int idx) {
        if (idx < 0 || idx >= (int)entityModels.size()) return;
        if (entityHasModel[idx]) {
            entityModels[idx].freeGPU();
            entityHasModel[idx] = false;
        }
        const MapEntity& ent = level.entities[idx];
        const FgdClass* cls = fgd.find(ent.classname);
        if (!cls || cls->modelPathKey.empty()) return;
        auto mit = ent.properties.find(cls->modelPathKey);
        if (mit == ent.properties.end() || mit->second.empty()) return;
        string meshPath = MODELS_FOLDER + mit->second;
        if (entityModels[idx].loadFromObj(meshPath, TEXTURES_FOLDER) && entityModels[idx].uploadToGPU()) {
            entityHasModel[idx] = true;
            cout << "Editor: model preview loaded -> " << meshPath << "\n";
        }
        else {
            cerr << "Editor: failed to load model '" << meshPath << "': "
                << entityModels[idx].error << "\n";
        }
    };
    for (size_t i = 0; i < level.entities.size(); i++) reloadEntityModel((int)i);

    // Write a quaternion back to the .map "angles" keyvalue ("Pitch Yaw Roll"
    // in map space) and drop any stale single "angle" key so the two can't
    // disagree. Also caches the quaternion for the gizmo/marker.
    auto setEntityAngles = [&](int idx, const glm::quat& q) {
        entityRot[idx] = q;
        float p, y, r;
        quatToEuler(q, p, y, r);
        level.entities[idx].properties["angles"] = fmtDeg(p) + " " + fmtDeg(y) + " " + fmtDeg(-r);
        level.entities[idx].properties.erase("angle");
        entityDirty[idx] = true;
    };
    auto setEntityScale = [&](int idx, float s) {
        if (s < 0.0001f) s = 0.0001f;
        entityScale[idx] = s;
        level.entities[idx].properties["scale"] = std::to_string(s);
        entityDirty[idx] = true;
    };

    // Some maps write colors as 0-255 comma strings (e.g. "191, 38, 186").
    // The engine's parser stops at the comma and reads those as garbage-bright
    // raw floats, so parse the raw keyvalue string ourselves (comma-aware),
    // normalize 0-255 colors to 0-1 space, sync the entity keyvalue and mark
    // the light dirty so the first Save persists the normalized value.
    auto parseColorFromString = [](const string& s) -> glm::vec3 {
        string fixed;
        for (char c : s) fixed += (c == ',') ? ' ' : c;
        vector<float> v = parseVec3(fixed);
        glm::vec3 c(1.0f);
        if (v.size() >= 3) c = glm::vec3(v[0], v[1], v[2]);
        else if (v.size() == 1) c = glm::vec3(v[0], 1.0f, 1.0f);
        return c;
    };
    auto fmt3 = [](const glm::vec3& v) {
        return std::to_string(v.r) + " " + std::to_string(v.g) + " " + std::to_string(v.b);
    };
    for (size_t k = 0; k < lightEntityIndices.size(); k++) {
        int idx = lightEntityIndices[k];
        auto cit = level.entities[idx].properties.find("color");
        if (cit == level.entities[idx].properties.end()) continue;
        glm::vec3 rawColor = parseColorFromString(cit->second);
        if (rawColor.x > 1.01f || rawColor.y > 1.01f || rawColor.z > 1.01f) {
            glm::vec3 norm = rawColor / 255.0f;
            level.pointLights[k].color = norm;
            cit->second = fmt3(norm);
            lightDirty[k] = true;
            cout << "Light #" << k << ": normalized 0-255 color to " << fmt3(norm) << "\n";
        }
    }

    // Recompute the cached rotation/scale (and light state) from the .map key
    // strings after a raw Properties-panel edit, so markers, gizmo and model
    // previews follow immediately. Mirrors the entityRot init logic above.
    auto refreshEntityFromKeys = [&](int idx) {
        if (idx < 0 || idx >= (int)level.entities.size()) return;
        const MapEntity& ent = level.entities[idx];
        auto ait = ent.properties.find("angles");
        if (ait != ent.properties.end()) {
            vector<float> a = parseVec3(ait->second);
            if (a.size() >= 3) entityRot[idx] = eulerToQuat(a[0], a[1], -a[2]);
            else if (a.size() >= 1) entityRot[idx] = eulerToQuat(0.0f, a[0], 0.0f);
        }
        else {
            auto git = ent.properties.find("angle");
            if (git != ent.properties.end())
                entityRot[idx] = eulerToQuat(0.0f, parseAngleValue(git->second, 0.0f), 0.0f);
        }
        auto sit = ent.properties.find("scale");
        if (sit != ent.properties.end()) {
            float sc = parseAngleValue(sit->second, 1.0f);
            entityScale[idx] = sc > 0.0001f ? sc : 1.0f;
        }
        if (ent.classname == "light") {
            for (size_t k = 0; k < lightEntityIndices.size() && k < level.pointLights.size(); k++) {
                if (lightEntityIndices[k] == idx) {
                    PointLight& pl = level.pointLights[k];
                    auto cit = ent.properties.find("color");
                    if (cit != ent.properties.end()) {
                        glm::vec3 c = parseColorFromString(cit->second);
                        if (c.x > 1.01f || c.y > 1.01f || c.z > 1.01f) c /= 255.0f;
                        pl.color = glm::clamp(c, 0.0f, 1.0f);
                    }
                    auto iit = ent.properties.find("intensity");
                    if (iit != ent.properties.end()) {
                        vector<float> v = parseVec3(iit->second);
                        if (!v.empty()) pl.intensity = v[0];
                    }
                    auto rit = ent.properties.find("radius");
                    if (rit != ent.properties.end()) {
                        vector<float> v = parseVec3(rit->second);
                        if (!v.empty()) pl.radius = v[0] * MAP_SCALE;
                    }
                    lightDirty[k] = true;
                    break;
                }
            }
        }
    };

    LightingState lighting;
    loadLightingSidecar(MAP_PATH, lighting);
    cout << "Lighting: sidecar '" << sidecarPathFor(MAP_PATH) << "' "
        << (ifstream(sidecarPathFor(MAP_PATH)) ? "loaded" : "missing (defaults)") << "\n";

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

    // Sun shadow map: depth-only FBO (2048^2), no colour attachment. Clamped to
    // border colour 1.0 ("no geometry here" = max depth = not shadowed), same as
    // the engine's ShadowMap.
    GLuint sunShadowFBO = 0, sunShadowDepthTex = 0;
    glGenFramebuffers(1, &sunShadowFBO);
    glGenTextures(1, &sunShadowDepthTex);
    glBindTexture(GL_TEXTURE_2D, sunShadowDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, SHADOW_RES, SHADOW_RES, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glBindFramebuffer(GL_FRAMEBUFFER, sunShadowFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sunShadowDepthTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        cerr << "Sun shadow FBO is incomplete - shadows will not render\n";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Ground grid on the XZ plane to give the free camera a reference frame.
    GLuint gridVAO = 0, gridVBO = 0;
    vector<float> gridVerts;
    float half = sceneRadius * 1.5f;
    float step = sceneRadius / 5.0f;
    for (float g = -half; g <= half + 0.01f; g += step) {
        gridVerts.insert(gridVerts.end(), { -half, 0.0f, g, half, 0.0f, g });
        gridVerts.insert(gridVerts.end(), { g, 0.0f, -half, g, 0.0f, half });
    }
    glGenVertexArrays(1, &gridVAO);
    glGenBuffers(1, &gridVBO);
    glBindVertexArray(gridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO);
    glBufferData(GL_ARRAY_BUFFER, gridVerts.size() * sizeof(float), gridVerts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Unit-cube edge list used to draw point-entity markers (lights, player start)
    // as 3D line boxes in the viewport. 12 edges, 24 vertices.
    static const float cubeEdges[] = {
        -1,-1,-1,  1,-1,-1,
         1,-1,-1,  1, 1,-1,
         1, 1,-1, -1, 1,-1,
        -1, 1,-1, -1,-1,-1,
        -1,-1, 1,  1,-1, 1,
         1,-1, 1,  1, 1, 1,
         1, 1, 1, -1, 1, 1,
        -1, 1, 1, -1,-1, 1,
        -1,-1,-1, -1,-1, 1,
         1,-1,-1,  1,-1, 1,
         1, 1,-1,  1, 1, 1,
        -1, 1,-1, -1, 1, 1,
    };
    GLuint markerVAO = 0, markerVBO = 0;
    glGenVertexArrays(1, &markerVAO);
    glGenBuffers(1, &markerVBO);
    glBindVertexArray(markerVAO);
    glBindBuffer(GL_ARRAY_BUFFER, markerVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeEdges), cubeEdges, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    EditorCamera camera;
    if (level.hasPlayerStart)
        camera.position = level.playerStart + glm::vec3(0.0f, 1.0f, 0.0f);
    else
        camera.position = sceneCenter + glm::vec3(0.0f, 0.5f, sceneRadius);
    glfwSetWindowUserPointer(window, &camera);
    glfwSetCursorPosCallback(window, cursor_pos_callback);

    GLuint chunkShader = buildProgram(chunkVertSrc, chunkFragSrc);
    GLuint gridShader = buildProgram(gridVertSrc, gridFragSrc);
    GLuint sunDepthShader = buildProgram(sunDepthVertSrc, sunDepthFragSrc);
    GLuint markerShader = buildProgram(markerVertSrc, markerFragSrc);
    std::ofstream("DBG_LOG", std::ios::app)
        << "shaders built\n";

    // Post-processing pipeline (Tier 2): HDR scene target, screen-space AO,
    // exposure + tonemap. Sized to the window now and resized on the fly.
    PostFX postFX;
    int initFbW, initFbH;
    glfwGetFramebufferSize(window, &initFbW, &initFbH);
    std::ofstream("DBG_LOG", std::ios::app)
        << "postFX.init starting\n";
    postFX.init(initFbW < 1 ? 1 : initFbW, initFbH < 1 ? 1 : initFbH);
    std::ofstream("DBG_LOG", std::ios::app)
        << "postFX.init done\n";

    // Active scene state for the post-processing settings. The UI edits this
    // record and pushes it to RenderPipeline; Tier 3 will serialize it into the
    // worldspawn metadata.
    PostProcessState postState;

    // Editor viewport output: the post chain (SSAO -> exposure -> tonemap)
    // composites into this RGBA texture, which the docked "Viewport" window
    // displays with ImGui::Image. Resized to match the panel every frame.
    GLuint editorOutFBO = 0, editorOutTex = 0;
    int editorOutW = -1, editorOutH = -1;
    auto ensureEditorOutput = [&](int w, int h) {
        if (w == editorOutW && h == editorOutH) return;
        editorOutW = w; editorOutH = h;
        if (editorOutTex) glDeleteTextures(1, &editorOutTex);
        glGenTextures(1, &editorOutTex);
        glBindTexture(GL_TEXTURE_2D, editorOutTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (!editorOutFBO) glGenFramebuffers(1, &editorOutFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, editorOutFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, editorOutTex, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            cerr << "Editor output FBO incomplete\n";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };

    std::ofstream("DBG_LOG", std::ios::app)
        << "postFX init done, entering loop\n";

    // Selection state. `selection` holds every currently selected entity index
    // (multi-select via Ctrl+click and marquee); `selectedEntity` is the anchor
    // (last-clicked, the one the gizmo attaches to); `selectedLight` mirrors a
    // single selected light into the Lighting panel.
    vector<int> selection;
    int selectedEntity = -1;
    int selectedLight = -1;
    bool shadowsEnabled = true;
    bool kWasDown = false;
    double lastTime = glfwGetTime();

    auto syncLight = [&]() {
        selectedLight = -1;
        if (selectedEntity >= 0 && selectedEntity < (int)lightEntityIndices.size() * 1) {
            for (size_t k = 0; k < lightEntityIndices.size(); k++)
                if (lightEntityIndices[k] == selectedEntity) { selectedLight = (int)k; break; }
        }
    };
    auto selectOnly = [&](int idx) {
        selection.assign(1, idx);
        selectedEntity = idx;
        syncLight();
    };
    auto toggleSelect = [&](int idx) {
        auto it = std::find(selection.begin(), selection.end(), idx);
        if (it != selection.end()) selection.erase(it);
        else selection.push_back(idx);
        selectedEntity = selection.empty() ? -1 : selection.back();
        syncLight();
    };
    auto clearSelection = [&]() {
        selection.clear();
        selectedEntity = -1;
        selectedLight = -1;
    };
    auto setSelection = [&](const vector<int>& sel) {
        selection = sel;
        selectedEntity = selection.empty() ? -1 : selection.back();
        syncLight();
    };
    auto isSelected = [&](int idx) {
        return std::find(selection.begin(), selection.end(), idx) != selection.end();
    };
    auto selectionCenter = [&]() -> glm::vec3 {
        glm::vec3 c(0.0f);
        if (selection.empty()) return c;
        for (int idx : selection) c += entityEnginePos(idx);
        return c / (float)selection.size();
    };

    // --- Undo / Redo history -------------------------------------------------
    // Every action that edits scene state pushes a snapshot onto undoStack BEFORE
    // the change; doUndo/doRedo restore snapshots and swap stacks.
    std::vector<EditorStateSnapshot> undoStack;
    std::vector<EditorStateSnapshot> redoStack;
    const size_t kMaxUndo = 64;

    auto captureState = [&]() -> EditorStateSnapshot {
        EditorStateSnapshot s;
        s.pointLights = level.pointLights;
        s.entities = level.entities;
        s.hasPlayerStart = level.hasPlayerStart;
        s.playerStart = level.playerStart;
        s.entityRot = entityRot;
        s.entityScale = entityScale;
        s.lighting = lighting;
        s.postState = postState;
        s.lightDirty = lightDirty;
        s.entityDirty = entityDirty;
        s.propsDirty = propsDirty;
        return s;
    };
    auto restoreState = [&](const EditorStateSnapshot& s) {
        level.pointLights = s.pointLights;
        level.entities = s.entities;
        level.hasPlayerStart = s.hasPlayerStart;
        level.playerStart = s.playerStart;
        entityRot = s.entityRot;
        entityScale = s.entityScale;
        lighting = s.lighting;
        postState = s.postState;
        lightDirty = s.lightDirty;
        entityDirty = s.entityDirty;
        propsDirty = s.propsDirty;
    };
    auto pushUndoState = [&](const EditorStateSnapshot& s) {
        undoStack.push_back(s);
        if (undoStack.size() > kMaxUndo)
            undoStack.erase(undoStack.begin());
        redoStack.clear();
    };
    auto pushUndo = [&]() { pushUndoState(captureState()); };
    auto doUndo = [&]() {
        if (undoStack.empty()) return;
        redoStack.push_back(captureState());
        restoreState(undoStack.back());
        undoStack.pop_back();
    };
    auto doRedo = [&]() {
        if (redoStack.empty()) return;
        undoStack.push_back(captureState());
        restoreState(redoStack.back());
        redoStack.pop_back();
    };

    // Create a brand-new entity from an FGD class and drop it next to the
    // camera. FGD defaults (including keys inherited from base(...) classes)
    // are applied, the parallel per-entity vectors are kept in sync, and the
    // entity is flagged so the next Save appends its block to the .map.
    auto addEntityFromClass = [&](const FgdClass& cls) {
        MapEntity ent;
        ent.classname = cls.name;

        // Flatten the class key list (the parser stores only base() names).
        vector<const FgdKeyValue*> classKeys;
        for (const FgdKeyValue& kv : cls.keyValues) classKeys.push_back(&kv);
        for (const string& b : cls.baseClasses) {
            const FgdClass* base = fgd.find(b);
            if (base)
                for (const FgdKeyValue& kv : base->keyValues) classKeys.push_back(&kv);
        }
        for (const FgdKeyValue* kv : classKeys)
            if (ent.properties.count(kv->key) == 0 && !kv->defaultValue.empty())
                ent.properties[kv->key] = kv->defaultValue;

        // Place a couple of metres in front of the camera (engine -> map units).
        glm::vec3 enginePos = camera.position + camera.front * 3.0f;
        ent.properties["origin"] = formatVec3(engineToMapOrigin(enginePos));
        if (cls.name == "info_player_start")
            ent.properties["angle"] = fmtDeg(camera.yaw);

        pushUndo();
        level.entities.push_back(ent);
        entityRot.push_back(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        entityScale.push_back(1.0f);
        entityDirty.push_back(false);
        propsDirty.push_back(true);
        entityModels.push_back(ObjMesh());
        entityHasModel.push_back(false);

        int idx = (int)level.entities.size() - 1;
        if (cls.name == "light") {
            ent.properties["color"] = "0.9 0.85 0.7";
            ent.properties["intensity"] = "3";
            ent.properties["radius"] = "300";
            PointLight pl;
            pl.position = enginePos;
            pl.color = glm::vec3(0.9f, 0.85f, 0.7f);
            pl.intensity = 3.0f;
            pl.radius = 300.0f * MAP_SCALE;
            level.pointLights.push_back(pl);
            lightEntityIndices.push_back(idx);
            lightDirty.push_back(true);
        }
        else if (cls.name == "info_player_start") {
            level.playerStart = enginePos;
            level.hasPlayerStart = true;
        }
        selectOnly(idx);
        cout << "Editor: added entity '" << cls.name << "' (index " << idx << ")\n";
    };

    // ------------------------------------------------------------------
    // High-level edit actions shared by the menu bar, toolbar and hotkeys.
    // Sorting must happen BEFORE entities are removed so the delete loop can
    // erase in descending index order (erasing a low index shifts everything
    // above it; erasing high-to-low keeps all remaining indices valid).
    // ------------------------------------------------------------------
    auto sortDescending = [](vector<int>& v) {
        sort(v.begin(), v.end(), std::greater<int>());
    };

    // Persist every pending edit: the lighting sidecar (sun/ambient), point
    // light keyvalues, entity transforms ("origin"/"angles"/"scale") and
    // generic keyvalues. Mirrors the Lighting panel's Save button so Ctrl+S
    // and menu File->Save behave identically.
    auto saveAll = [&]() {
        saveLightingSidecar(MAP_PATH, lighting);
        bool okL = saveLightEntitiesToMap(MAP_PATH, level.entities, lightEntityIndices, lightDirty);
        bool okE = saveEntityPropsToMap(MAP_PATH, level.entities, entityDirty);
        bool okG = saveGenericKeysToMap(MAP_PATH, level.entities, propsDirty);
        cout << "Save: sidecar written, .map light entities "
            << (okL ? "updated" : "FAILED") << ", entity transforms "
            << (okE ? "updated" : "FAILED") << ", generic properties "
            << (okG ? "updated" : "FAILED") << "\n";
    };

    // Select every non-worldspawn entity (Ctrl+A / menu Edit->Select All).
    auto selectAllEntities = [&]() {
        vector<int> all;
        for (size_t i = 1; i < level.entities.size(); i++)  // skip worldspawn (idx 0)
            all.push_back((int)i);
        setSelection(all);
    };

    // Delete selected entities (worldspawn is protected). The .map files
    // themselves are never rewritten to drop blocks - only keyvalues inside
    // existing blocks are - so deleted entities' blocks stay in the file.
    auto deleteSelectedEntities = [&]() {
        if (selection.empty()) return;
        pushUndo();
        vector<int> doomed = selection;
        sortDescending(doomed);

        for (int idx : doomed) {
            if (idx <= 0 || idx >= (int)level.entities.size()) continue;
            const string& cls = level.entities[idx].classname;
            if (cls == "worldspawn") continue;

            // Free the OBJ preview on the GL context so the VBO/VAO aren't leaked.
            if (idx < (int)entityHasModel.size() && entityHasModel[idx])
                entityModels[idx].freeGPU();

            // If the deleted entity is a light, also remove its slot from
            // level.pointLights[] and the parallel lightEntityIndices[].
            if (cls == "light") {
                for (size_t k = 0; k < lightEntityIndices.size(); k++) {
                    if (lightEntityIndices[k] == idx) {
                        level.pointLights.erase(level.pointLights.begin() + k);
                        lightEntityIndices.erase(lightEntityIndices.begin() + k);
                        lightDirty.erase(lightDirty.begin() + k);
                        break;
                    }
                }
            }

            if (level.entities[idx].classname == "info_player_start") {
                level.hasPlayerStart = false;
                level.playerStart = glm::vec3(0.0f);
            }

            level.entities.erase(level.entities.begin() + idx);
            entityRot.erase(entityRot.begin() + idx);
            entityScale.erase(entityScale.begin() + idx);
            entityDirty.erase(entityDirty.begin() + idx);
            propsDirty.erase(propsDirty.begin() + idx);
            entityModels.erase(entityModels.begin() + idx);
            entityHasModel.erase(entityHasModel.begin() + idx);

            // Every remaining light whose entity index sat above the erased one
            // shifted down by one - remap now so future lookups stay valid.
            for (size_t k = 0; k < lightEntityIndices.size(); k++)
                if (lightEntityIndices[k] > idx) lightEntityIndices[k]--;
        }
        clearSelection();
        cout << "Editor: deleted " << doomed.size() << " entit(y/ies). "
            "NOTE: in-memory only - the .map file keeps the block until hand-edited.\n";
    };

    // Duplicate each selected entity with a 2-unit engine-space offset (nudge).
    // Duplicates push undo (capture happened before add) and are marked dirty so
    // the next Save appends them. A duplicated light copies its point light.
    auto duplicateSelectedEntities = [&]() {
        if (selection.empty()) return;
        pushUndo();
        vector<int> added;
        vector<int> src = selection;
        for (int idx : src) {
            if (idx < 0 || idx >= (int)level.entities.size()) continue;
            const MapEntity& srcEnt = level.entities[idx];
            if (srcEnt.classname == "worldspawn") continue;

            MapEntity copy = srcEnt;
            glm::vec3 pos = entityEnginePos(idx) + glm::vec3(2.0f, 0.0f, 0.0f);
            copy.properties["origin"] = formatVec3(engineToMapOrigin(pos));

            level.entities.push_back(copy);
            entityRot.push_back(entityRot[idx]);
            entityScale.push_back(entityScale[idx]);
            entityDirty.push_back(true);
            propsDirty.push_back(true);
            entityModels.push_back(ObjMesh());
            entityHasModel.push_back(false);

            int nidx = (int)level.entities.size() - 1;

            if (copy.classname == "light") {
                for (size_t k = 0; k < lightEntityIndices.size(); k++) {
                    if (lightEntityIndices[k] == idx) {
                        PointLight pl = level.pointLights[k];
                        pl.position = pos;
                        level.pointLights.push_back(pl);
                        lightEntityIndices.push_back(nidx);
                        lightDirty.push_back(true);
                        break;
                    }
                }
            }
            else if (copy.classname == "info_player_start") {
                level.playerStart = pos;
                level.hasPlayerStart = true;
            }

            reloadEntityModel(nidx);
            added.push_back(nidx);
        }
        setSelection(added);
        cout << "Editor: duplicated " << added.size() << " entit(y/ies)\n";
    };

    // reloadEntityModel() above frees + reloads. For hot-reload we restore the
    // full pipeline state to match boot: entities, rotations/scales, models,
    // lighting sidecar, scene bounds, chunks, grid, camera start and editor state
    // (selection + undo history are discarded - indices all changed).
    // Runs on EVERY reload: frees every GL resource then rebuilds from the file.
    auto reloadFromDisk = [&]() {
        // Free chunk VAOs/VBOs + their textures (Texture has no free(): the
        // GL name is destroyed via glDeleteTextures using the public id()).
        for (auto& c : chunks) {
            if (c.VAO) glDeleteVertexArrays(1, &c.VAO);
            if (c.VBO) glDeleteBuffers(1, &c.VBO);
            GLuint t = c.texture.id();
            if (t) glDeleteTextures(1, &t);
        }
        chunks.clear();
        for (auto& m : entityModels)
            m.freeGPU();

        // Free grid buffers.
        if (gridVAO) glDeleteVertexArrays(1, &gridVAO);
        if (gridVBO) glDeleteBuffers(1, &gridVBO);
        gridVAO = gridVBO = 0;
        gridVerts.clear();

        // Re-parse the map (same call as boot; throws are caught there).
        LevelData fresh;
        try {
            fresh = MapLoader::load(MAP_PATH, textureSizeLookup);
        }
        catch (const std::exception& e) {
            cerr << "Editor: hot-reload FAILED - " << e.what() << "\n";
            return;
        }
        catch (...) {
            cerr << "Editor: hot-reload FAILED (unknown)\n";
            return;
        }
        level = fresh;

        // Rebuild per-entity parallel arrays exactly like boot.
        lightEntityIndices.clear();
        for (size_t i = 0; i < level.entities.size(); i++)
            if (level.entities[i].classname == "light") lightEntityIndices.push_back((int)i);
        lightDirty.assign(lightEntityIndices.size(), false);
        entityDirty.assign(level.entities.size(), false);
        propsDirty.assign(level.entities.size(), false);

        entityRot.assign(level.entities.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        entityScale.assign(level.entities.size(), 1.0f);
        for (size_t i = 0; i < level.entities.size(); i++) {
            const MapEntity& ent = level.entities[i];
            auto ait = ent.properties.find("angles");
            if (ait != ent.properties.end()) {
                vector<float> a = parseVec3(ait->second);
                if (a.size() >= 3) entityRot[i] = eulerToQuat(a[0], a[1], -a[2]);
                else if (a.size() >= 1) entityRot[i] = eulerToQuat(0.0f, a[0], 0.0f);
            }
            else {
                auto git = ent.properties.find("angle");
                if (git != ent.properties.end())
                    entityRot[i] = eulerToQuat(0.0f, parseAngleValue(git->second, 0.0f), 0.0f);
            }
            auto sit = ent.properties.find("scale");
            if (sit != ent.properties.end()) {
                float sc = parseAngleValue(sit->second, 1.0f);
                entityScale[i] = sc > 0.0001f ? sc : 1.0f;
            }
        }

        entityModels.assign(level.entities.size(), ObjMesh());
        entityHasModel.assign(level.entities.size(), false);
        for (size_t i = 0; i < level.entities.size(); i++) reloadEntityModel((int)i);

        // Color normalization pass (mirror of boot) so 0-255 colors re-normalize.
        for (size_t k = 0; k < lightEntityIndices.size(); k++) {
            int idx = lightEntityIndices[k];
            auto cit = level.entities[idx].properties.find("color");
            if (cit == level.entities[idx].properties.end()) continue;
            glm::vec3 rawColor = parseColorFromString(cit->second);
            if (rawColor.x > 1.01f || rawColor.y > 1.01f || rawColor.z > 1.01f) {
                glm::vec3 norm = rawColor / 255.0f;
                level.pointLights[k].color = norm;
                cit->second = fmt3(norm);
                lightDirty[k] = true;
            }
        }

        lighting = LightingState();
        loadLightingSidecar(MAP_PATH, lighting);

        sceneMin = glm::vec3(1e9f);
        sceneMax = glm::vec3(-1e9f);
        for (size_t i = 0; i + 2 < level.collisionVertices.size(); i += 3) {
            glm::vec3 p(level.collisionVertices[i], level.collisionVertices[i + 1], level.collisionVertices[i + 2]);
            sceneMin = glm::min(sceneMin, p);
            sceneMax = glm::max(sceneMax, p);
        }
        sceneCenter = (sceneMin + sceneMax) * 0.5f;
        sceneRadius = glm::length(sceneMax - sceneMin) * 0.5f;
        if (sceneRadius < 1.0f) sceneRadius = 20.0f;

        // Rebuild chunk VAOs/VBOs.
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

        // Rebuild ground grid.
        float half = sceneRadius * 1.5f;
        float step = sceneRadius / 5.0f;
        for (float g = -half; g <= half + 0.01f; g += step) {
            gridVerts.insert(gridVerts.end(), { -half, 0.0f, g, half, 0.0f, g });
            gridVerts.insert(gridVerts.end(), { g, 0.0f, -half, g, 0.0f, half });
        }
        glGenVertexArrays(1, &gridVAO);
        glGenBuffers(1, &gridVBO);
        glBindVertexArray(gridVAO);
        glBindBuffer(GL_ARRAY_BUFFER, gridVBO);
        glBufferData(GL_ARRAY_BUFFER, gridVerts.size() * sizeof(float), gridVerts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        // Reset camera to player start (or centre) + wipe selection/undo -
        // every entity index changed, old snapshots would corrupt the state.
        if (level.hasPlayerStart)
            camera.position = level.playerStart + glm::vec3(0.0f, 1.0f, 0.0f);
        else
            camera.position = sceneCenter + glm::vec3(0.0f, 0.5f, sceneRadius);
        clearSelection();
        undoStack.clear();
        redoStack.clear();
        cout << "Editor: map reloaded from disk (" << level.entities.size()
            << " entities, " << chunks.size() << " chunks)\n";
    };

    // Undo capture for continuous-edit widgets (sliders, color edits): the frame
    // a widget is activated it hasn't modified its value yet, so snapshot then;
    // commit the undo entry only when the widget is deactivated after an actual
    // value change. One drag == one undo entry.
    EditorStateSnapshot s_undoPre;
    bool s_undoPreValid = false;
    auto trackWidgetUndo = [&](bool activated, bool deactivated, bool deactivatedAfterEdit) {
        if (activated && !s_undoPreValid) {
            s_undoPre = captureState();
            s_undoPreValid = true;
        }
        if (deactivated) {
            if (deactivatedAfterEdit && s_undoPreValid)
                pushUndoState(s_undoPre);
            s_undoPreValid = false;
        }
    };

    // Gizmo drag-start snapshot (per-selected-entity) so a multi-select drag
    // applies the same world delta to every selected entity.
    vector<glm::vec3> s_dragStartPos;
    vector<glm::quat> s_dragStartRot;
    vector<float> s_dragStartScale;
    bool s_gizmoWasUsing = false;

    // Marquee (rubber-band) selection state, coordinates are viewport-local.
    bool s_marqueeActive = false;
    ImVec2 s_marqueeStart(0.0f, 0.0f);
    bool s_marqueeCtrl = false;
    vector<int> s_marqueeBase;

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = (float)(now - lastTime);
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f;

        s_rightMouse = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
        camera.processKeyboard(window, dt);
        camera.updateVectors();

        // Hot-reload: when autoReload is on, poll the .map's write time once a
        // second. If it changed AND nothing unsaved is pending in memory, rebuild
        // everything from the file (see reloadFromDisk()). The check runs before
        // the ImGui frame so the freshly-loaded geometry is drawn this frame.
        if (s_editorSettings.autoReload && now - g_lastReloadCheck > 1.0) {
            g_lastReloadCheck = now;
            ULONGLONG t = fileWriteTimeOf(MAP_PATH);
            if (t != 0 && t != g_mapWriteTime) {
                bool dirty = false;
                for (bool b : lightDirty) { if (b) { dirty = true; break; } }
                if (!dirty) for (bool b : entityDirty) { if (b) { dirty = true; break; } }
                if (!dirty) for (bool b : propsDirty) { if (b) { dirty = true; break; } }
                if (!dirty && !postState.dirty) {
                    g_mapWriteTime = t;
                    reloadFromDisk();
                }
                else {
                    cout << "Editor: .map changed on disk but unsaved edits in memory "
                        "exist - auto-reload skipped (Ctrl+R to force).\n";
                    g_mapWriteTime = t;
                }
            }
        }

        // K toggles sun shadows (visual confirmation that they track sun movement).
        bool kDown = glfwGetKey(window, GLFW_KEY_K) == GLFW_PRESS;
        if (kDown && !kWasDown) shadowsEnabled = !shadowsEnabled;
        kWasDown = kDown;

        // Current sun direction for this frame (shared by the shadow pass and the
        // lit-surface pass so they can never disagree).
        float sunPitchRad = glm::radians(lighting.sunPitch);
        float sunYawRad = glm::radians(lighting.sunYaw);
        glm::vec3 sunDir(cos(sunPitchRad) * cos(sunYawRad), sin(sunPitchRad), cos(sunPitchRad) * sin(sunYawRad));

        // ImGui frames before the viewport render so the panel size is known.
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();  // must be called right after NewFrame

        // ------------------------------------------------------------------
        // Main menu bar (File / Edit / View). Fits the dock space's work area
        // automatically via ImGui's viewport work-rect handling (the dock space
        // is built over WorkPos/WorkSize, which this shrinks by one bar height).
        // ------------------------------------------------------------------
        {
            bool ctrl = ImGui::GetIO().KeyCtrl;  // used by accelerators listed below
            if (ImGui::BeginMainMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("Save", "Ctrl+S")) saveAll();
                    if (ImGui::MenuItem("Reload Map", "Ctrl+R")) {
                        reloadFromDisk();
                        g_mapWriteTime = fileWriteTimeOf(MAP_PATH);
                    }
                    ImGui::MenuItem("Auto-Reload", nullptr, &s_editorSettings.autoReload);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Exit", "Escape")) glfwSetWindowShouldClose(window, GLFW_TRUE);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Edit")) {
                    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undoStack.empty())) doUndo();
                    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redoStack.empty())) doRedo();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Select All", "Ctrl+A")) selectAllEntities();
                    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !selection.empty())) duplicateSelectedEntities();
                    if (ImGui::MenuItem("Delete", "Delete", false, !selection.empty())) deleteSelectedEntities();
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("View")) {
                    if (ImGui::MenuItem("Grid", nullptr, &s_editorSettings.showGrid)) {}
                    if (ImGui::MenuItem("Wireframe", "Z", &s_editorSettings.wireframe)) {}
                    if (ImGui::MenuItem("Orthographic", "P", &s_editorSettings.ortho)) {}
                    if (ImGui::MenuItem("Snap", "G", &s_editorSettings.snapEnabled)) {}
                    ImGui::Separator();
                    if (ImGui::MenuItem("Snap Settings", nullptr, &s_snapPanelOpen)) {}
                    ImGui::EndMenu();
                }
                ImGui::EndMainMenuBar();
            }
        }

        // Full-window DockSpace hosts all panels (Nuake-style multi-panel layout).
        ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

        // First frame only: build the default layout (viewport centre, hierarchy
        // left, inspector + lighting right, logger/FGD bottom).
        static bool s_dockLayoutDone = false;
        if (!s_dockLayoutDone) {
            s_dockLayoutDone = true;
            ImGui::DockBuilderRemoveNode(dockspace);
            ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
            ImGuiID left, right;
            ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.20f, &left, &right);
            ImGuiID center, rightCol;
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Right, 0.28f, &rightCol, &center);
            ImGuiID vpNode, bottomNode;
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.20f, &bottomNode, &vpNode);
            ImGuiID propsNode, rightBottomNode;
            ImGui::DockBuilderSplitNode(rightCol, ImGuiDir_Down, 0.42f, &rightBottomNode, &propsNode);
            ImGui::DockBuilderDockWindow("Viewport", vpNode);
            ImGui::DockBuilderDockWindow("Entities", left);
            ImGui::DockBuilderDockWindow("Map Info", bottomNode);
            ImGui::DockBuilderDockWindow("FGD Classes", bottomNode);
            ImGui::DockBuilderDockWindow("Snap Settings", bottomNode);
            ImGui::DockBuilderDockWindow("Properties", propsNode);
            ImGui::DockBuilderDockWindow("Lighting", rightBottomNode);
            ImGui::DockBuilderDockWindow("Post-Processing & Pipeline Settings", rightBottomNode);
            ImGui::DockBuilderFinish(dockspace);
        }

        // ------------------------------------------------------------------
        // Viewport panel: the scene renders into the HDR framebuffer, SSAO +
        // tonemap composite into editorOutTex, then ImGui::Image shows it.
        // A slim toolbar on top holds the gizmo operation / mode / snap toggles
        // (also reachable by 1/2/3, T and G keys).
        // ------------------------------------------------------------------
        ImGui::Begin("Viewport", nullptr);
        s_viewportHovered = ImGui::IsWindowHovered();

        // Flat tool-bar: the active operation is shown with a highlighted
        // background + blue label (Unity's selected-tool look).
        auto toolButton = [&](const char* label, bool active) {
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            }
            bool pressed = ImGui::Button(label);
            if (active) ImGui::PopStyleColor(2);
            return pressed;
        };

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
        ImGui::BeginChild("##vpToolbar", ImVec2(0.0f, 30.0f), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDecoration);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 4.0f));
        if (toolButton("Move", s_editorSettings.gizmoOp == 0)) s_editorSettings.gizmoOp = 0;
        ImGui::SameLine();
        if (toolButton("Rotate", s_editorSettings.gizmoOp == 1)) s_editorSettings.gizmoOp = 1;
        ImGui::SameLine();
        if (toolButton("Scale", s_editorSettings.gizmoOp == 2)) s_editorSettings.gizmoOp = 2;
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &s_editorSettings.snapEnabled);
        ImGui::SameLine();
        if (ImGui::Button(s_editorSettings.gizmoLocal ? "Local" : "World"))
            s_editorSettings.gizmoLocal = !s_editorSettings.gizmoLocal;
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        if (ImGui::Button("Undo")) doUndo();
        ImGui::SameLine();
        if (ImGui::Button("Redo")) doRedo();
        ImGui::SameLine();
        if (ImGui::Button("Save")) { saveAll(); g_mapWriteTime = fileWriteTimeOf(MAP_PATH); }
        ImGui::SameLine();
        ImGui::Checkbox("Grid", &s_editorSettings.showGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Wire", &s_editorSettings.wireframe);
        ImGui::SameLine();
        ImGui::Checkbox("Ortho", &s_editorSettings.ortho);
        ImGui::SameLine();
        ImGui::TextDisabled("1/2/3 op  T local/world  G snap  Del delete  Ctrl+S/D/Z/Y/R  Z/P view  F frame");
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImVec2 vpAvail = ImGui::GetContentRegionAvail();
        int vpW = (int)vpAvail.x, vpH = (int)vpAvail.y;
        if (vpW < 1) vpW = 1;
        if (vpH < 1) vpH = 1;
        if (vpW != postFX.width || vpH != postFX.height)
            postFX.resize(vpW, vpH);
        ensureEditorOutput(vpW, vpH);

        // Image top-left in screen coords. The scene raycast/gizmo/marquee all
        // use this (NOT the window content region) so the toolbar above the
        // image never skews picking.
        ImVec2 imgPos = ImGui::GetCursorScreenPos();
        float gx = imgPos.x;
        float gy = imgPos.y;
        ImGuizmo::SetRect(gx, gy, (float)vpW, (float)vpH);
        ImGuizmo::SetOrthographic(s_editorSettings.ortho);
        auto inImage = [&](const ImVec2& p) {
            return p.x >= gx && p.x < gx + vpW && p.y >= gy && p.y < gy + vpH;
        };

        // Sun shadow pass. Rebuilt every frame from the current sun direction, so
        // moving a slider immediately regenerates the shadow map (per-frame rebuild,
        // matching the engine - no cache to go stale).
        glm::mat4 lightSpaceMatrix;
        {
            glm::vec3 lightPos = sceneCenter - glm::normalize(sunDir) * sceneRadius * 2.0f;
            glm::mat4 lightView = glm::lookAt(lightPos, sceneCenter, glm::vec3(0.0f, 1.0f, 0.0f));
            glm::mat4 lightProj = glm::ortho(-sceneRadius, sceneRadius, -sceneRadius, sceneRadius,
                0.1f, sceneRadius * 4.0f);
            lightSpaceMatrix = lightProj * lightView;

            glViewport(0, 0, SHADOW_RES, SHADOW_RES);
            glBindFramebuffer(GL_FRAMEBUFFER, sunShadowFBO);
            glClear(GL_DEPTH_BUFFER_BIT);
            glUseProgram(sunDepthShader);
            glUniformMatrix4fv(glGetUniformLocation(sunDepthShader, "uLightSpaceMatrix"), 1, GL_FALSE, &lightSpaceMatrix[0][0]);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);  // avoid shadow acne, like the engine's pass
            for (auto& chunk : chunks) {
                glBindVertexArray(chunk.VAO);
                glDrawArrays(GL_TRIANGLES, 0, chunk.vertexCount);
            }
            glCullFace(GL_BACK);
            glDisable(GL_CULL_FACE);  // editor's main pass renders brushes double-sided
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, vpW, vpH);
        }

        // Orthographic keeps the gizmo/marker picking accurate: the projected half-size
        // scales with the scene radius so a top-down ortho covers the whole map
        // (fovy mirrors the default 60 deg perspective at that distance).
        glm::mat4 proj;
        if (s_editorSettings.ortho) {
            float halfH = sceneRadius;
            if (halfH < 1.0f) halfH = 1.0f;
            float aspect = (float)vpW / (float)vpH;
            proj = glm::ortho(-halfH * aspect, halfH * aspect, -halfH, halfH, 0.1f, 4000.0f);
        }
        else {
            proj = glm::perspective(glm::radians(60.0f), (float)vpW / (float)vpH, 0.1f, 2000.0f);
        }
        glm::mat4 view = camera.view();
        glm::mat4 vp = proj * view;
        glm::mat4 invProj = glm::inverse(proj);

        // Keep the centralized pipeline config in lock-step with the editor's
        // scene state (the UI setters push changes too; this covers every path).
        RenderPipeline::SyncFrom(postState);

        // Viewport rect in screen coords + the cursor ray. ImGuizmo uses the
        // rect to place/hit-test the gizmo; the ray picks point-entity markers.
        glm::vec3 rayOrigin, rayDir;
        int hoverEntity = -1;
        {
            ImVec2 mp = ImGui::GetMousePos();
            glm::vec2 ndc((mp.x - gx) / (float)vpW * 2.0f - 1.0f, 1.0f - (mp.y - gy) / (float)vpH * 2.0f);
            glm::mat4 invVP = glm::inverse(vp);
            glm::vec4 nearP = invVP * glm::vec4(ndc.x, ndc.y, -1.0f, 1.0f);
            glm::vec4 farP = invVP * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
            rayOrigin = glm::vec3(nearP) / nearP.w;
            rayDir = glm::normalize(glm::vec3(farP / farP.w) - rayOrigin);

            // Hover highlight: raycast against the marker boxes (no click needed).
            // Skipped while the mouse is over the gizmo so the gizmo wins.
            bool gizmoBlocking = (!selection.empty() && ImGuizmo::IsOver());
            if (s_viewportHovered && !gizmoBlocking) {
                float bestT = 1e30f;
                for (size_t i = 0; i < level.entities.size(); i++) {
                    const MapEntity& ent = level.entities[i];
                    if (ent.classname == "worldspawn") continue;
                    bool hasLivePos = (ent.classname == "light" || ent.classname == "info_player_start");
                    if (!hasLivePos && ent.properties.count("origin") == 0) continue;
                    float hs = markerSizeFor(ent.classname) * entityScale[i] * 1.2f;
                    glm::vec3 c = entityEnginePos((int)i);
                    glm::vec3 invD = 1.0f / rayDir;
                    glm::vec3 t0 = (c - hs - rayOrigin) * invD;
                    glm::vec3 t1 = (c + hs - rayOrigin) * invD;
                    glm::vec3 tmn = glm::min(t0, t1);
                    glm::vec3 tmx = glm::max(t0, t1);
                    float tn = glm::max(glm::max(tmn.x, tmn.y), tmn.z);
                    float tf = glm::min(glm::min(tmx.x, tmx.y), tmx.z);
                    if (tf >= 0.0f && tf >= tn && tn < bestT) { bestT = tn; hoverEntity = (int)i; }
                }
            }
        }

        // Scene pass -> HDR framebuffer (colour + indirect + depth): grid, brush
        // chunks and point-entity marker line boxes in one double-sided pass.
        {
            glBindFramebuffer(GL_FRAMEBUFFER, postFX.hdrFBO);
            glViewport(0, 0, postFX.width, postFX.height);
            glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Grid.
            if (s_editorSettings.showGrid) {
                glBindVertexArray(gridVAO);
                glUseProgram(gridShader);
                glUniformMatrix4fv(glGetUniformLocation(gridShader, "uMVP"), 1, GL_FALSE, &vp[0][0]);
                glDrawArrays(GL_LINES, 0, (GLsizei)(gridVerts.size() / 3));
            }

            // Brush chunks (wireframe mode draws them as a line overlay).
            if (s_editorSettings.wireframe)
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glBindVertexArray(0);
            glUseProgram(chunkShader);
            glUniformMatrix4fv(glGetUniformLocation(chunkShader, "uMVP"), 1, GL_FALSE, &vp[0][0]);
            {
                glUniform3f(glGetUniformLocation(chunkShader, "uSunDir"), sunDir.x, sunDir.y, sunDir.z);
                glUniform3f(glGetUniformLocation(chunkShader, "uSunColor"), lighting.sunColor.r, lighting.sunColor.g, lighting.sunColor.b);
                glUniform1f(glGetUniformLocation(chunkShader, "uSunIntensity"), lighting.sunIntensity);
                glUniform3f(glGetUniformLocation(chunkShader, "uAmbientColor"), lighting.ambientColor.r, lighting.ambientColor.g, lighting.ambientColor.b);
                glUniform1f(glGetUniformLocation(chunkShader, "uAmbientIntensity"), lighting.ambientIntensity);
                glUniform3f(glGetUniformLocation(chunkShader, "uCamPos"), camera.position.x, camera.position.y, camera.position.z);

                glUniformMatrix4fv(glGetUniformLocation(chunkShader, "uLightSpaceMatrix"), 1, GL_FALSE, &lightSpaceMatrix[0][0]);
                glUniform1f(glGetUniformLocation(chunkShader, "uShadowEnabled"), shadowsEnabled ? 1.0f : 0.0f);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, sunShadowDepthTex);
                glUniform1i(glGetUniformLocation(chunkShader, "uShadowMap"), 1);

                int numLights = (int)glm::min<size_t>(level.pointLights.size(), MAX_LIGHTS);
                glUniform1i(glGetUniformLocation(chunkShader, "uNumLights"), numLights);
                for (int i = 0; i < numLights; i++) {
                    const auto& pl = level.pointLights[i];
                    string posName = "uLightPos[" + std::to_string(i) + "]";
                    string colName = "uLightColor[" + std::to_string(i) + "]";
                    string intName = "uLightIntensity[" + std::to_string(i) + "]";
                    string radName = "uLightRadius[" + std::to_string(i) + "]";
                    glUniform3f(glGetUniformLocation(chunkShader, posName.c_str()), pl.position.x, pl.position.y, pl.position.z);
                    glUniform3f(glGetUniformLocation(chunkShader, colName.c_str()), pl.color.r, pl.color.g, pl.color.b);
                    glUniform1f(glGetUniformLocation(chunkShader, intName.c_str()), pl.intensity);
                    glUniform1f(glGetUniformLocation(chunkShader, radName.c_str()), pl.radius);
                }
            }
            for (auto& chunk : chunks) {
                chunk.texture.bind(0);
                glUniform1i(glGetUniformLocation(chunkShader, "uTexture"), 0);
                glBindVertexArray(chunk.VAO);
                glDrawArrays(GL_TRIANGLES, 0, chunk.vertexCount);
            }

            // OBJ model previews for mesh entities (FGD "model" key). Rendered
            // with the same lit chunk shader so textures match the game; only
            // models safely within the camera's clip range are drawn.
            for (size_t i = 0; i < level.entities.size(); i++) {
                if (i >= entityHasModel.size() || !entityHasModel[i]) continue;
                if (entityModels[i].VAO == 0) continue;
                const MapEntity& ent = level.entities[i];
                if (ent.classname == "worldspawn") continue;
                bool hasLivePos = (ent.classname == "light" || ent.classname == "info_player_start");
                if (!hasLivePos && ent.properties.count("origin") == 0) continue;
                glm::mat4 model = glm::translate(glm::mat4(1.0f), entityEnginePos((int)i))
                    * glm::mat4(entityRot[i])
                    * glm::scale(glm::mat4(1.0f), glm::vec3(entityScale[i]));
                glUniformMatrix4fv(glGetUniformLocation(chunkShader, "uMVP"), 1, GL_FALSE, &(vp * model)[0][0]);
                glUniform1i(glGetUniformLocation(chunkShader, "uTexture"), 0);
                entityModels[i].draw();
            }
            if (s_editorSettings.wireframe)
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

            // Point-entity markers (3D line boxes), oriented/scaled to the
            // entity's rotation/scale so rotate/scale gizmo edits are visible.
            // Selected = larger + red, hovered = bright yellow.
            glBindVertexArray(markerVAO);
            glUseProgram(markerShader);
            for (size_t i = 0; i < level.entities.size(); i++) {
                const MapEntity& ent = level.entities[i];
                if (ent.classname == "worldspawn") continue;
                bool hasLivePos = (ent.classname == "light" || ent.classname == "info_player_start");
                if (!hasLivePos && ent.properties.count("origin") == 0) continue;
                float s = markerSizeFor(ent.classname) * entityScale[i];
                bool selected = isSelected((int)i);
                glm::mat4 model = glm::translate(glm::mat4(1.0f), entityEnginePos((int)i))
                    * glm::mat4(entityRot[i])
                    * glm::scale(glm::mat4(1.0f), glm::vec3(selected ? s * 1.5f : s));
                glm::vec3 col = markerColorFor(ent.classname);
                if (selected) col = glm::vec3(1.0f, 0.15f, 0.15f);
                else if ((int)i == hoverEntity) col = glm::vec3(1.0f, 1.0f, 0.5f);
                glUniformMatrix4fv(glGetUniformLocation(markerShader, "uMVP"), 1, GL_FALSE, &(vp * model)[0][0]);
                glUniform3f(glGetUniformLocation(markerShader, "uColor"), col.x, col.y, col.z);
                glDrawArrays(GL_LINES, 0, 24);
            }
            glBindVertexArray(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        // Post chain composites into the editor output texture, not the screen.
        postFX.renderSSAO(proj, invProj);
        postFX.renderComposite(editorOutFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Show the composited scene in the panel, then the gizmo on top.
        // OpenGL textures store row 0 at the bottom but ImGui images start UV(0,0)
        // at the top, so flip V here. ImGuizmo's NDC->screen mapping applies the
        // same flip (worldToPos: trans.y = 1.f - trans.y), keeping gizmo and markers
        // aligned with the displayed image.
        ImGui::Image((ImTextureID)(intptr_t)editorOutTex, vpAvail,
            ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

        ImVec2 mp = ImGui::GetMousePos();
        bool leftClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        bool leftReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        bool doubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

        // --- marquee (rubber-band) live update + overlay --------------------
        // While the left button is held after starting a marquee, entities whose
        // projected marker centre falls inside the drag rect are selected live.
        if (s_marqueeActive && leftDown && s_viewportHovered) {
            ImVec2 cur(mp.x - gx, mp.y - gy);
            ImVec2 mn(glm::min(s_marqueeStart.x, cur.x), glm::min(s_marqueeStart.y, cur.y));
            ImVec2 mx(glm::max(s_marqueeStart.x, cur.x), glm::max(s_marqueeStart.y, cur.y));
            vector<int> inRect;
            for (size_t i = 0; i < level.entities.size(); i++) {
                const MapEntity& ent = level.entities[i];
                if (ent.classname == "worldspawn") continue;
                bool hasLivePos = (ent.classname == "light" || ent.classname == "info_player_start");
                if (!hasLivePos && ent.properties.count("origin") == 0) continue;
                glm::vec4 clip = vp * glm::vec4(entityEnginePos((int)i), 1.0f);
                if (clip.w <= 0.0f) continue;
                glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
                ImVec2 sp((ndc.x * 0.5f + 0.5f) * (float)vpW, (1.0f - ndc.y * 0.5f - 0.5f) * (float)vpH);
                if (sp.x >= mn.x && sp.x <= mx.x && sp.y >= mn.y && sp.y <= mx.y)
                    inRect.push_back((int)i);
            }
            if (s_marqueeCtrl) {
                vector<int> sel = s_marqueeBase;
                for (int e : inRect)
                    if (std::find(sel.begin(), sel.end(), e) == sel.end()) sel.push_back(e);
                setSelection(sel);
            }
            else {
                setSelection(inRect);
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(gx + mn.x, gy + mn.y), ImVec2(gx + mx.x, gy + mx.y),
                IM_COL32(60, 110, 255, 40));
            dl->AddRect(ImVec2(gx + mn.x, gy + mn.y), ImVec2(gx + mx.x, gy + mx.y),
                IM_COL32(120, 170, 255, 255), 0.0f, 0, 1.0f);
        }
        if (s_marqueeActive && leftReleased) {
            float dx = (mp.x - gx) - s_marqueeStart.x;
            float dy = (mp.y - gy) - s_marqueeStart.y;
            if (fabsf(dx) < 4.0f && fabsf(dy) < 4.0f && !s_marqueeCtrl)
                clearSelection();  // plain click on empty space = deselect
            s_marqueeActive = false;
        }

        // --- gizmo shortcuts + camera framing ------------------------------
        // Operation / mode keys are ignored mid-drag so a gizmo edit can't be
        // derailed by an accidental keypress.
        if (!ImGuizmo::IsUsing()) {
            if (ImGui::IsKeyPressed(ImGuiKey_1, false)) s_editorSettings.gizmoOp = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_2, false)) s_editorSettings.gizmoOp = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_3, false)) s_editorSettings.gizmoOp = 2;
            if (ImGui::IsKeyPressed(ImGuiKey_T, false)) s_editorSettings.gizmoLocal = !s_editorSettings.gizmoLocal;
            if (ImGui::IsKeyPressed(ImGuiKey_G, false)) s_editorSettings.snapEnabled = !s_editorSettings.snapEnabled;
            // Z toggles wireframe, P toggles orthographic (non-ctrl only).
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && !ImGui::GetIO().KeyCtrl
                && !ImGui::IsAnyItemActive()) s_editorSettings.wireframe = !s_editorSettings.wireframe;
            if (ImGui::IsKeyPressed(ImGuiKey_P, false) && !ImGui::IsAnyItemActive())
                s_editorSettings.ortho = !s_editorSettings.ortho;
            // Delete = remove selected entities.
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !ImGui::IsAnyItemActive())
                deleteSelectedEntities();
            // Ctrl+Z undo / Ctrl+Y redo, Ctrl+S save, Ctrl+A select-all,
            // Ctrl+D duplicate, Ctrl+R force reload. Ignored mid-widget-drag
            // (an active ImGui item such as a slider) so history can't be
            // corrupted mid-edit.
            bool ctrl = ImGui::GetIO().KeyCtrl;
            if (ctrl && !ImGui::IsAnyItemActive()) {
                if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) doUndo();
                if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) doRedo();
                if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                    saveAll();
                    g_mapWriteTime = fileWriteTimeOf(MAP_PATH);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_A, false)) selectAllEntities();
                if (ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelectedEntities();
                if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                    reloadFromDisk();
                    g_mapWriteTime = fileWriteTimeOf(MAP_PATH);
                }
            }
        }

        // Double-click frames the clicked entity (or the ground point under the
        // cursor); F frames the selection (or the whole scene).
        if (doubleClicked && s_viewportHovered && inImage(mp) && !ImGuizmo::IsUsing()) {
            s_marqueeActive = false;
            glm::vec3 target;
            if (hoverEntity >= 0) {
                target = entityEnginePos(hoverEntity);
            }
            else {
                target = sceneCenter;
                if (fabsf(rayDir.y) > 1e-4f) {
                    float t = (0.0f - rayOrigin.y) / rayDir.y;
                    if (t > 0.0f) target = rayOrigin + rayDir * t;
                }
            }
            float dist = glm::clamp(sceneRadius * 0.6f, 5.0f, 1000.0f);
            camera.position = target - camera.front * dist;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && s_viewportHovered && !ImGuizmo::IsUsing()) {
            glm::vec3 target = selection.empty() ? sceneCenter : selectionCenter();
            float dist = glm::clamp(sceneRadius * 0.6f, 5.0f, 1000.0f);
            camera.position = target - camera.front * dist;
        }

        // --- picking / marquee start ----------------------------------------
        // Left click selects the hovered entity (Ctrl toggles); clicking empty
        // space begins a marquee drag. Alt+click (window move) and gizmo
        // clicks/drags are excluded. Double-click frames instead of picking.
        if (leftClicked && s_viewportHovered && inImage(mp)
            && !ImGui::GetIO().KeyAlt && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing() && !doubleClicked) {
            if (hoverEntity >= 0) {
                if (ImGui::GetIO().KeyCtrl) toggleSelect(hoverEntity);
                else selectOnly(hoverEntity);
            }
            else {
                s_marqueeActive = true;
                s_marqueeStart = ImVec2(mp.x - gx, mp.y - gy);
                s_marqueeCtrl = ImGui::GetIO().KeyCtrl;
                s_marqueeBase = selection;
            }
        }

        // --- ImGuizmo gizmo -------------------------------------------------
        // Attached to the anchor (last-selected) entity. Moves/rotates/scales
        // EVERY selected entity by the same world delta, so multi-select edits
        // behave like a rigid group (rotation pivots around the selection's
        // centre at drag start).
        //
        // Draw the gizmo into THIS window's draw list: the dedicated "gizmo"
        // window that BeginFrame() creates is pushed to the back of the ImGui
        // z-order (it uses ImGuiWindowFlags_NoBringToFrontOnFocus), so without
        // SetDrawlist() the gizmo would be hidden behind the Viewport panel.
        // This also makes IsHoveringWindow()/CanActivate() match correctly,
        // because the draw list's _OwnerName is the Viewport window.
        ImGuizmo::SetDrawlist();
        if (selectedEntity >= 0 && selectedEntity < (int)level.entities.size()
            && level.entities[selectedEntity].classname != "worldspawn") {
            ImGuizmo::OPERATION gizmoOp = s_editorSettings.gizmoOp == 1 ? ImGuizmo::ROTATE
                : (s_editorSettings.gizmoOp == 2 ? ImGuizmo::SCALE : ImGuizmo::TRANSLATE);
            ImGuizmo::MODE gizmoMode = s_editorSettings.gizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

            // Gizmo sits at the selection centroid so multi-select rotate/scale
            // pivots exactly where the gizmo is drawn. During a rotation drag it
            // stays pinned to the drag-start centroid (the pivot); otherwise it
            // follows the current centroid so a translate drag carries it along
            // with the group.
            glm::vec3 pivot(0.0f);
            if (!s_dragStartPos.empty()) {
                for (const glm::vec3& p : s_dragStartPos) pivot += p;
                pivot /= (float)s_dragStartPos.size();
            }
            glm::vec3 gizmoPos = selectionCenter();
            if (gizmoOp == ImGuizmo::ROTATE && ImGuizmo::IsUsing()
                && (int)s_dragStartPos.size() == (int)selection.size())
                gizmoPos = pivot;

            glm::mat4 gizmoModel = glm::translate(glm::mat4(1.0f), gizmoPos)
                * glm::mat4(entityRot[selectedEntity])
                * glm::scale(glm::mat4(1.0f), glm::vec3(entityScale[selectedEntity]));

            if ((int)s_dragStartPos.size() != (int)selection.size()) {
                s_dragStartPos.clear();
                s_dragStartRot.clear();
                s_dragStartScale.clear();
                for (int idx : selection) {
                    s_dragStartPos.push_back(entityEnginePos(idx));
                    s_dragStartRot.push_back(entityRot[idx]);
                    s_dragStartScale.push_back(entityScale[idx]);
                }
            }

            float snap[3];
            float* snapPtr = nullptr;
            if (s_editorSettings.snapEnabled) {
                if (gizmoOp == ImGuizmo::TRANSLATE) {
                    snap[0] = snap[1] = snap[2] = s_editorSettings.snapTranslate * MAP_SCALE;
                }
                else if (gizmoOp == ImGuizmo::ROTATE) {
                    snap[0] = s_editorSettings.snapRotate;
                    snap[1] = snap[2] = 0.0f;
                }
                else {
                    snap[0] = snap[1] = snap[2] = s_editorSettings.snapScale;
                }
                snapPtr = snap;
            }

            glm::mat4 deltaMatrix(1.0f);
            bool manipulated = ImGuizmo::Manipulate(&view[0][0], &proj[0][0], gizmoOp, gizmoMode,
                &gizmoModel[0][0], &deltaMatrix[0][0], snapPtr);

            // Snapshot per-selected-entity start state on the frame a drag
            // begins so the whole selection moves as one rigid group.
            // ImGuizmo only flips its internal "using" flag inside
            // Manipulate(), so the start of a drag has to be detected after
            // that call - checking IsUsing() beforehand never fired and left
            // stale snapshots from the previous drag, which made a subsequent
            // rotate/scale drag snap entities back to their old positions.
            bool usingNow = ImGuizmo::IsUsing();
            if (usingNow && !s_gizmoWasUsing) {
                // A new gizmo drag begins on this frame - the state is still the
                // pre-drag state (deltas apply from the next frame), so record the
                // undo entry before the positions/rotations/scales start moving.
                pushUndo();
                s_dragStartPos.clear();
                s_dragStartRot.clear();
                s_dragStartScale.clear();
                for (int idx : selection) {
                    s_dragStartPos.push_back(entityEnginePos(idx));
                    s_dragStartRot.push_back(entityRot[idx]);
                    s_dragStartScale.push_back(entityScale[idx]);
                }
            }

            if (manipulated && usingNow && (int)s_dragStartPos.size() == (int)selection.size()) {
                if (gizmoOp == ImGuizmo::TRANSLATE) {
                    // deltaMatrix's translation is the incremental world-space
                    // delta from ImGuizmo's internal drag origin, so add it to each
                    // entity's CURRENT position (a rigid group move that tracks the
                    // cursor); adding to the drag-start position would lag behind.
                    glm::vec3 d(deltaMatrix[3][0], deltaMatrix[3][1], deltaMatrix[3][2]);
                    if (glm::length(d) > 0.00001f) {
                        for (size_t k = 0; k < selection.size(); k++) {
                            setEntityEnginePos(selection[k], entityEnginePos(selection[k]) + d);
                        }
                    }
                }
                else if (gizmoOp == ImGuizmo::ROTATE) {
                    // ImGuizmo rewrites gizmoModel with the anchor's cumulative
                    // rotation (start rotation * local delta). The world-space
                    // rotation delta is therefore final * start^-1, which is
                    // mode-independent and free of the anchor's own orientation.
                    glm::quat qAnchor = s_dragStartRot.back();
                    glm::quat qWorld = glm::quat_cast(glm::mat3(gizmoModel)) * glm::conjugate(qAnchor);
                    for (size_t k = 0; k < selection.size(); k++) {
                        glm::vec3 p = pivot + (qWorld * (s_dragStartPos[k] - pivot));
                        if (glm::length(p - entityEnginePos(selection[k])) > 0.00001f)
                            setEntityEnginePos(selection[k], p);
                        setEntityAngles(selection[k], qWorld * s_dragStartRot[k]);
                    }
                }
                else {  // SCALE (ImGuizmo applies scale in local space only)
                    float s = deltaMatrix[0][0];
                    if (s > 0.0f) {
                        for (size_t k = 0; k < selection.size(); k++) {
                            float ns = s_dragStartScale[k] * s;
                            if (fabsf(ns - entityScale[selection[k]]) > 1e-4f)
                                setEntityScale(selection[k], ns);
                        }
                    }
                }
            }
            s_gizmoWasUsing = usingNow;
        }
        else {
            s_gizmoWasUsing = false;
        }

        ImGui::End();

        ImGui::Begin("Map Info");
        ImGui::Text("Chunks: %zu", chunks.size());
        ImGui::Text("Entities: %zu", level.entities.size());
        ImGui::Text("Point lights: %zu", level.pointLights.size());
        ImGui::SeparatorText("Controls");
        ImGui::BulletText("WASD move, Q/E up/down");
        ImGui::BulletText("Hold Right Mouse to look");
        ImGui::BulletText("Shift = faster");
        ImGui::BulletText("K = toggle sun shadows (currently %s)", shadowsEnabled ? "ON" : "OFF");
        ImGui::BulletText("Ctrl+Z = undo, Ctrl+Y = redo");
        ImGui::BulletText("Del = delete, Ctrl+D = duplicate, Ctrl+A = select all");
        ImGui::BulletText("Z = wireframe, P = orthographic, G = snap");
        ImGui::BulletText("Ctrl+S = save, Ctrl+R = reload, Escape = exit");
        ImGui::End();

        if (s_snapPanelOpen) {
            ImGui::Begin("Snap Settings", &s_snapPanelOpen);
            ImGui::Checkbox("Snap enabled (G)", &s_editorSettings.snapEnabled);
            ImGui::Separator();
            ImGui::SliderFloat("Translate (map u)", &s_editorSettings.snapTranslate, 0.25f, 32.0f, "%.2f");
            ImGui::SliderFloat("Rotate (deg)", &s_editorSettings.snapRotate, 1.0f, 90.0f, "%.0f");
            ImGui::SliderFloat("Scale (mult)", &s_editorSettings.snapScale, 0.05f, 2.0f, "%.2f");
            ImGui::Separator();
            ImGui::Checkbox("Show grid", &s_editorSettings.showGrid);
            ImGui::Checkbox("Wireframe (Z)", &s_editorSettings.wireframe);
            ImGui::Checkbox("Orthographic (P)", &s_editorSettings.ortho);
            ImGui::Separator();
            ImGui::Checkbox("Auto-reload .map on disk change", &s_editorSettings.autoReload);
            ImGui::End();
        }

        ImGui::Begin("Entities");
        if (level.entities.empty()) {
            ImGui::Text("No entities found.");
        }
        else {
            // Entity search/filter box: filters the list by classname substring.
            static char entFilter[64] = "";
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##entfilter", "Filter by classname...", entFilter, sizeof(entFilter));
            string filterStr = entFilter;

            if (ImGui::BeginChild("entityList", ImVec2(0.0f, 200.0f), true)) {
                size_t shown = 0;
                for (size_t i = 0; i < level.entities.size(); i++) {
                    const MapEntity& ent = level.entities[i];
                    if (!filterStr.empty() && ent.classname.find(filterStr) == string::npos
                        && std::to_string(i).find(filterStr) == string::npos)
                        continue;
                    shown++;
                    bool selected = isSelected((int)i);
                    string label = ent.classname.empty()
                        ? "(no classname) " + std::to_string(i)
                        : ent.classname + " [" + std::to_string(i) + "]";
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        if (ImGui::GetIO().KeyCtrl) toggleSelect((int)i);
                        else selectOnly((int)i);
                    }
                }
                if (shown == 0) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "(no matches)");
            }
            ImGui::EndChild();

            if (selectedEntity >= 0 && selectedEntity < (int)level.entities.size()) {
                const MapEntity& ent = level.entities[selectedEntity];
                ImGui::Separator();
                ImGui::Text("Entity #%d - %s", selectedEntity, ent.classname.c_str());
                const FgdClass* cls = ent.classname.empty() ? nullptr : fgd.find(ent.classname.c_str());
                if (cls && !cls->description.empty())
                    ImGui::TextWrapped("%s", cls->description.c_str());

                if (ImGui::BeginTable("props", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Key");
                    ImGui::TableSetupColumn("Value");
                    ImGui::TableHeadersRow();
                    for (auto& kv : ent.properties) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%s", kv.first.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", kv.second.c_str());
                    }
                    ImGui::EndTable();
                }
            }
        }
        ImGui::End();

        if (fgd.loaded) {
            ImGui::Begin("FGD Classes");
            ImGui::Text("Loaded %zu entity classes", fgd.classes.size());

            static char fgdFilter[64] = "";
            ImGui::InputText("Filter", fgdFilter, sizeof(fgdFilter));

            if (ImGui::BeginChild("fgdList", ImVec2(0.0f, 240.0f), true)) {
                string filterStr = fgdFilter;
                for (const FgdClass& cls : fgd.classes) {
                    if (!filterStr.empty() && cls.name.find(filterStr) == string::npos) continue;
                    const char* typeName = "point";
                    if (cls.type == FgdClassType::Solid) typeName = "solid";
                    else if (cls.type == FgdClassType::Base) typeName = "base";
                    string header = cls.name + "  [" + typeName + "]" +
                        "  (" + std::to_string(cls.keyValues.size()) + " keys)";
                    if (ImGui::TreeNode(header.c_str())) {
                        if (cls.type != FgdClassType::Base && ImGui::Button("Add to map")) {
                            addEntityFromClass(cls);
                            ImGui::TreePop();
                            continue;
                        }
                        if (!cls.description.empty()) ImGui::TextWrapped("%s", cls.description.c_str());
                        if (cls.type != FgdClassType::Base)
                            ImGui::TextWrapped("Drops a new entity 3 m in front of the camera.");
                        ImGui::TreePop();
                    }
                }
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::TextWrapped("Tip: add an entity, select it, then edit its key/values in the Properties panel.");
            ImGui::End();
        }

        // ------------------------------------------------------------------
        // Generic entity property panel. Renders the selected entity's key/
        // values with FGD-aware widgets (choices list, flags, integers, text),
        // lets the user add/remove arbitrary keys, and flags the entity so Save
        // writes them back into the .map as generic keyvalues.
        // ------------------------------------------------------------------
        ImGui::Begin("Properties");
        if (selectedEntity < 0 || selectedEntity >= (int)level.entities.size()) {
            ImGui::TextWrapped("Select an entity in the Entities panel to edit its properties.");
        }
        else {
            const int idx = selectedEntity;
            MapEntity& ent = level.entities[idx];

            ImGui::Text("%s  (entity #%d)", ent.classname.c_str(), idx);
            const FgdClass* cls = fgd.find(ent.classname);
            if (cls && !cls->description.empty())
                ImGui::TextWrapped("%s", cls->description.c_str());

            auto markProps = [&]() { propsDirty[idx] = true; };

            // Flatten the FGD key set (incl. base(...) inherited keys) so we
            // can render generic widget for the class's declared keys.
            vector<const FgdKeyValue*> classKeys;
            if (cls) {
                for (const FgdKeyValue& kv : cls->keyValues) classKeys.push_back(&kv);
                for (const string& b : cls->baseClasses) {
                    const FgdClass* base = fgd.find(b);
                    if (base)
                        for (const FgdKeyValue& kv : base->keyValues) classKeys.push_back(&kv);
                }
            }
            auto keyIsStructural = [&](const string& k) -> bool {
                return k == "classname" || k == "origin" || k == "angles" ||
                    k == "angle" || k == "scale";
            };

            ImGui::SeparatorText("Transform");
            if (ent.properties.count("origin")) {
                vector<float> o = parseVec3(ent.properties["origin"]);
                if (o.size() < 3) o = { 0.0f, 0.0f, 0.0f };
                bool changed = false;
                changed |= ImGui::DragFloat("Pos X (map)", &o[0], 8.0f, -1e6f, 1e6f, "%.1f");
                changed |= ImGui::DragFloat("Pos Y (map)", &o[1], 8.0f, -1e6f, 1e6f, "%.1f");
                changed |= ImGui::DragFloat("Pos Z (map)", &o[2], 8.0f, -1e6f, 1e6f, "%.1f");
                if (changed) {
                    setEntityEnginePos(idx, mapOriginToEngine(glm::vec3(o[0], o[1], o[2])));
                    markProps();
                }
            }
            vector<float> ang = parseVec3(ent.properties.count("angles") ?
                ent.properties.at("angles") : ent.properties.count("angle") ? ent.properties.at("angle") : "");
            if (ent.properties.count("angles") || ent.properties.count("angle")) {
                if (ang.size() < 3) ang.resize(3, 0.0f);
                bool changed = false;
                changed |= ImGui::DragFloat("Pitch (map)", &ang[0], 1.0f, -360.0f, 360.0f, "%.1f");
                changed |= ImGui::DragFloat("Yaw (map)", &ang[1], 1.0f, -360.0f, 360.0f, "%.1f");
                changed |= ImGui::DragFloat("Roll (map)", &ang[2], 1.0f, -360.0f, 360.0f, "%.1f");
                if (changed) {
                    ent.properties["angles"] = fmtDeg(ang[0]) + " " + fmtDeg(ang[1]) + " " + fmtDeg(ang[2]);
                    ent.properties.erase("angle");
                    entityRot[idx] = eulerToQuat(ang[0], ang[1], -ang[2]);
                    entityDirty[idx] = true;
                    markProps();
                }
            }
            if (ent.properties.count("scale")) {
                float sc = parseAngleValue(ent.properties["scale"], 1.0f);
                if (ImGui::DragFloat("Scale", &sc, 0.01f, 0.0001f, 100.0f, "%.3f")) {
                    entityScale[idx] = sc > 0.0001f ? sc : 1.0f;
                    ent.properties["scale"] = std::to_string(sc);
                    entityDirty[idx] = true;
                    markProps();
                }
            }

            ImGui::SeparatorText("Key / Values");
            char buf[512];
            bool keysChanged = false;

            // FGD-declared keys with type-appropriate widgets.
            vector<string> drawnKeys;
            for (const FgdKeyValue* kv : classKeys) {
                if (keyIsStructural(kv->key)) continue;
                drawnKeys.push_back(kv->key);
                string current = ent.properties.count(kv->key) ? ent.properties[kv->key] : "";
                string label = kv->displayName.empty() ? kv->key : kv->displayName;

                if (kv->isFlags) {
                    int flagsVal = 0;
                    {
                        vector<float> v = parseVec3(current);
                        if (!v.empty()) flagsVal = (int)v[0];
                    }
                    bool anyChanged = false;
                    for (const FgdFlag& f : kv->flags) {
                        string id = kv->key + "::flag_" + std::to_string(f.bit);
                        bool on = (flagsVal & f.bit) != 0;
                        if (ImGui::Checkbox((f.name + "##" + id).c_str(), &on)) {
                            if (on) flagsVal |= f.bit;
                            else flagsVal &= ~f.bit;
                            anyChanged = true;
                        }
                    }
                    if (anyChanged) {
                        ent.properties[kv->key] = std::to_string(flagsVal);
                        keysChanged = true;
                    }
                }
                else if (kv->isChoices) {
                    int currentIdx = -1;
                    {
                        vector<float> v = parseVec3(current);
                        if (!v.empty()) {
                            int iv = (int)v[0];
                            for (size_t c = 0; c < kv->choices.size(); c++)
                                if (kv->choices[c].first == iv) { currentIdx = (int)c; break; }
                        }
                    }
                    if (ImGui::BeginCombo((label + "##" + kv->key).c_str(),
                        currentIdx >= 0 ? kv->choices[currentIdx].second.c_str() : "" )) {
                        for (size_t c = 0; c < kv->choices.size(); c++)
                            if (ImGui::Selectable(kv->choices[c].second.c_str(), (int)c == currentIdx)) {
                                ent.properties[kv->key] = std::to_string(kv->choices[c].first);
                                keysChanged = true;
                            }
                        ImGui::EndCombo();
                    }
                }
                else if (cls && kv->key == cls->modelPathKey) {
                    // Model file browser: pick a .obj under GameRoot/models. The
                    // value is the path relative to the models folder (e.g.
                    // "props/crate.obj"), exactly what MODELS_FOLDER + key yields.
                    const vector<string>& modelPaths = cachedObjModelPaths();
                    int cur = -1;
                    for (size_t m = 0; m < modelPaths.size(); m++)
                        if (modelPaths[m] == current) { cur = (int)m; break; }
                    string preview = cur >= 0 ? modelPaths[cur] : ("(none)");
                    if (ImGui::BeginCombo((label + "##" + kv->key).c_str(), preview.c_str())) {
                        if (ImGui::Selectable("(none)", cur < 0)) {
                            ent.properties[kv->key] = "";
                            keysChanged = true;
                            entityHasModel[idx] = false;
                        }
                        for (size_t m = 0; m < modelPaths.size(); m++) {
                            if (ImGui::Selectable(modelPaths[m].c_str(), (int)m == cur)) {
                                ent.properties[kv->key] = modelPaths[m];
                                keysChanged = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered() && !kv->description.empty())
                        ImGui::SetTooltip("%s", kv->description.c_str());
                }
                else if (kv->type == "integer") {
                    int iv = 0;
                    {
                        vector<float> v = parseVec3(current);
                        if (!v.empty()) iv = (int)v[0];
                    }
                    if (ImGui::DragInt((label + "##" + kv->key).c_str(), &iv, 0.1f)) {
                        ent.properties[kv->key] = std::to_string(iv);
                        keysChanged = true;
                    }
                }
                else {
                    // Generic text / number / whatever - raw string editor.
                    snprintf(buf, sizeof(buf), "%s", current.c_str());
                    if (ImGui::InputText((label + "##" + kv->key).c_str(), buf, sizeof(buf))) {
                        ent.properties[kv->key] = buf;
                        keysChanged = true;
                    }
                }
                if (ImGui::IsItemHovered() && !kv->description.empty())
                    ImGui::SetTooltip("%s", kv->description.c_str());
            }

            // Any leftover raw keys not declared by the FGD (custom data) - also
            // list keys that came from the file but aren't in classKeys so users
            // can prune legacy/typo'd keyvalues.
            ImGui::Separator();
            for (auto kit = ent.properties.begin(); kit != ent.properties.end(); /**/) {
                const string& k = kit->first;
                bool handled = keyIsStructural(k) || std::find(drawnKeys.begin(), drawnKeys.end(), k) != drawnKeys.end();
                if (!handled) {
                    snprintf(buf, sizeof(buf), "%s", kit->second.c_str());
                    bool edited = ImGui::InputText((k + "##raw").c_str(), buf, sizeof(buf));
                    ImGui::SameLine();
                    bool doDelete = ImGui::Button(("X##del_" + k).c_str());
                    if (doDelete) { kit = ent.properties.erase(kit); keysChanged = true; continue; }
                    if (edited) {
                        kit->second = buf;
                        keysChanged = true;
                    }
                }
                ++kit;
            }

            if (keysChanged) {
                markProps();
                refreshEntityFromKeys(idx);
                reloadEntityModel(idx);
            }

            ImGui::SeparatorText("Add Key");
            static char newKeyBuf[64] = "";
            static char newValBuf[256] = "";
            ImGui::InputText("Key##newk", newKeyBuf, sizeof(newKeyBuf));
            ImGui::InputText("Value##newv", newValBuf, sizeof(newValBuf));
            if (ImGui::Button("Add key") && newKeyBuf[0]) {
                string k = newKeyBuf;
                if (ent.properties.count(k) == 0) {
                    ent.properties[k] = newValBuf;
                    markProps();
                    newKeyBuf[0] = '\0';
                }
            }

            if (propsDirty[idx])
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Modified - not saved");
        }
        ImGui::End();

        // ------------------------------------------------------------------
        // Lighting panel (Tier 1): sun, ambient, editable point lights.
        // Live edits write BOTH level.pointLights[k] (drives the renderer this
        // frame) AND the corresponding entity's properties strings (so Save can
        // persist them). Point-light radius is shown in engine units and stored
        // in map units (x 32) for the .map file.
        // ------------------------------------------------------------------
        ImGui::Begin("Lighting");

        ImGui::SeparatorText("Sun");
        ImGui::SliderFloat("Pitch", &lighting.sunPitch, -89.0f, 89.0f, "%.1f");
        trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());
        ImGui::SliderFloat("Yaw", &lighting.sunYaw, -180.0f, 180.0f, "%.1f");
        trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());
        ImGui::ColorEdit3("Sun color", &lighting.sunColor.x);
        trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());
        ImGui::SliderFloat("Sun intensity", &lighting.sunIntensity, 0.0f, 10.0f, "%.2f");
        trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());

        ImGui::SeparatorText("Ambient");
        ImGui::ColorEdit3("Ambient color", &lighting.ambientColor.x);
        trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());
        ImGui::SliderFloat("Ambient intensity", &lighting.ambientIntensity, 0.0f, 5.0f, "%.2f");
        trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());

        char lightHeader[64];
        snprintf(lightHeader, sizeof(lightHeader), "Point lights (%zu)", level.pointLights.size());
        ImGui::SeparatorText(lightHeader);
        if (lightEntityIndices.empty()) {
            ImGui::Text("No 'light' entities in this map.");
        }
        else {
            if (ImGui::BeginChild("lightList", ImVec2(0.0f, 140.0f), true)) {
                for (size_t k = 0; k < lightEntityIndices.size(); k++) {
                    int idx = lightEntityIndices[k];
                    const auto& pl = level.pointLights[k];
                    string label = "light #" + std::to_string(k) + " @ ("
                        + std::to_string((int)pl.position.x) + ", "
                        + std::to_string((int)pl.position.y) + ", "
                        + std::to_string((int)pl.position.z) + ")";
                    if (ImGui::Selectable(label.c_str(), ((int)k == selectedLight)))
                        selectedLight = (int)k;
                }
            }
            ImGui::EndChild();

            if (selectedLight >= 0 && selectedLight < (int)level.pointLights.size()) {
                int k = selectedLight;
                int idx = lightEntityIndices[k];
                PointLight& pl = level.pointLights[k];
                MapEntity& ent = level.entities[idx];

                ImGui::Separator();
                ImGui::Text("Light #%d  (entity #%d)", k, idx);

                auto fmt3 = [](const glm::vec3& v) {
                    return std::to_string(v.r) + " " + std::to_string(v.g) + " " + std::to_string(v.b);
                };
                auto propStr = [](float v) { return std::to_string(v); };

                if (ImGui::ColorEdit3("Color", &pl.color.x)) {
                    ent.properties["color"] = fmt3(pl.color);
                    lightDirty[k] = true;
                }
                trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());
                if (ImGui::SliderFloat("Intensity", &pl.intensity, 0.0f, 20.0f, "%.2f")) {
                    ent.properties["intensity"] = propStr(pl.intensity);
                    lightDirty[k] = true;
                }
                trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());
                if (ImGui::SliderFloat("Radius (engine u)", &pl.radius, 0.5f, 50.0f, "%.2f")) {
                    ent.properties["radius"] = propStr(pl.radius / MAP_SCALE);
                    lightDirty[k] = true;
                }
                trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());
                if (ImGui::Button("Reset to map origin")) {
                    pushUndo();
                    vector<float> v = parseVec3(ent.properties.count("origin") ? ent.properties["origin"] : "");
                    if (v.size() >= 3) {
                        // Mirror mapfile.cpp convertPos: (x, y, z) -> (x, z, -y) * MAP_SCALE.
                        pl.position = glm::vec3(v[0], v[2], -v[1]) * MAP_SCALE;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Select entity")) selectOnly(idx);
            }
        }
        ImGui::Separator();

        if (ImGui::Button("Save lighting, lights & moved entities")) {
            saveAll();
            g_mapWriteTime = fileWriteTimeOf(MAP_PATH);
        }
        ImGui::TextWrapped("Save writes sun/ambient to %s and point-light + gizmo-moved keyvalues back into the .map file.",
            sidecarPathFor(MAP_PATH).c_str());
        ImGui::End();

        // ------------------------------------------------------------------
        // Post-Processing & Pipeline Settings panel (Tier 2).
        // The controls edit the editor's active scene state (postState) and push
        // the values to the centralized RenderPipeline config via its setters.
        // The render loop re-uploads them every frame, so the viewport updates
        // in real time - no level rebuild, no shader recompile. Any change sets
        // 'dirty' so Tier 3 knows the worldspawn metadata needs writing.
        // ------------------------------------------------------------------
        ImGui::Begin("Post-Processing & Pipeline Settings");

        bool postChanged = false;
        ImGui::SeparatorText("Screen-Space Ambient Occlusion");
        if (ImGui::Checkbox("Enabled", &postState.ssaoEnabled)) {
            postChanged = true;
            // Checkbox flips its value on the click frame, so the pre-toggle state
            // is reconstructed by reverting the toggle in the snapshot.
            EditorStateSnapshot pre = captureState();
            pre.postState.ssaoEnabled = !pre.postState.ssaoEnabled;
            pushUndoState(pre);
        }
        if (ImGui::SliderFloat("Strength", &postState.ssaoStrength, 0.0f, 5.0f, "%.2f")) postChanged = true;
        trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());
        if (ImGui::SliderFloat("Radius", &postState.ssaoRadius, 0.01f, 2.0f, "%.3f")) postChanged = true;
        trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());

        ImGui::SeparatorText("Exposure / Tonemapping");
        if (ImGui::SliderFloat("Exposure (EV)", &postState.exposureEV, -5.0f, 5.0f, "%.2f")) postChanged = true;
        trackWidgetUndo(ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemDeactivatedAfterEdit());

        if (postChanged) {
            postState.dirty = true;
            RenderPipeline::SetSSAOParams(postState.ssaoStrength, postState.ssaoRadius);
            RenderPipeline::SetExposure(postState.exposureEV);
            RenderPipeline::SetSSAOEnabled(postState.ssaoEnabled);
        }

        ImGui::Separator();
        if (postState.dirty)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                "Modified - not saved (Tier 3 will write to worldspawn metadata)");
        else
            ImGui::Text("Post-processing settings match the saved scene state.");
        ImGui::End();

        // ------------------------------------------------------------------
        // Status bar: derives the current world/editor state on the left and
        // frame timing on the right. Lives in the main viewport bottom strip,
        // below the dock space, so it never covers panel content.
        // ------------------------------------------------------------------
        {
            ImGuiViewport* sbVp = ImGui::GetMainViewport();
            bool barOpen = ImGui::BeginViewportSideBar("##StatusBar", sbVp, ImGuiDir_Down,
                ImGui::GetFrameHeight(), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDecoration);
            if (barOpen) {
                bool dirtyAny = !lightDirty.empty() || !entityDirty.empty() || !propsDirty.empty();
                ImGui::Text("%s", MAP_PATH.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", s_editorSettings.autoReload ? "(auto-reload)" : "");
                ImGui::SameLine();
                ImGui::Text("| %zu sel", selection.size());
                ImGui::SameLine();
                const char* opName = s_editorSettings.gizmoOp == 1 ? "rotate" : (s_editorSettings.gizmoOp == 2 ? "scale" : "move");
                ImGui::Text("| %s %s snap=%s", opName, s_editorSettings.gizmoLocal ? "local" : "world",
                    s_editorSettings.snapEnabled ? "on" : "off");
                ImGui::SameLine();
                ImGui::Text("| view=%s",
                    s_editorSettings.ortho ? "ortho" : "persp");
                ImGui::SameLine();
                if (dirtyAny || postState.dirty)
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "| * unsaved");
                ImGui::SameLine();
                ImGui::Text("| %5.1f FPS", ImGui::GetIO().Framerate);
                ImGui::End();
            }
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    postFX.destroy();
    glDeleteTextures(1, &sunShadowDepthTex);
    glDeleteFramebuffers(1, &sunShadowFBO);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
