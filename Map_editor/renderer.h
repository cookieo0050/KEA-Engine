// ============================================================================
// renderer.h - GL renderer for the map editor
// ============================================================================
// Owns every GL object the editor draws with: the shaders, the per-texture
// chunk VAOs, the ground grid, the point-entity marker cube, the brush
// wireframe (used by the 2D ortho views), the sun shadow FBO and the whole
// PostFX chain (HDR scene target + SSAO + exposure/tonemap composite). The 3D
// viewport and the Top/Front/Side ortho views render through this module; the
// ImGui panels only consume the produced textures.
// ============================================================================
#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include "editor.h"

// ----------------------------------------------------------------------------
// GL program helpers
// ----------------------------------------------------------------------------
GLuint editorCompileShader(GLenum type, const char* src);
GLuint editorBuildProgram(const char* vertSrc, const char* fragSrc);

// ----------------------------------------------------------------------------
// Post-Processing & Pipeline (Tier 2)
// ----------------------------------------------------------------------------
struct PostFX {
    GLuint hdrFBO = 0, hdrColorTex = 0, hdrIndirectTex = 0, hdrDepthTex = 0;
    GLuint ssaoFBO = 0, ssaoTex = 0;
    GLuint ssaoBlurFBO = 0, ssaoBlurTex = 0;
    GLuint noiseTex = 0;
    GLuint quadVAO = 0, quadVBO = 0;
    GLuint ssaoShader = 0, blurShader = 0, tonemapShader = 0;
    std::vector<glm::vec3> kernel;
    int width = 0, height = 0;

    void init(int w, int h);
    void resize(int w, int h);
    void drawQuad() const;
    void renderSSAO(const glm::mat4& proj, const glm::mat4& invProj);
    void renderComposite(GLuint targetFBO = 0);
    void destroy();
};

// ----------------------------------------------------------------------------
// Ortho view basis. Right = the world axis that points right on screen,
// screenUp = the world axis that points up on screen.
// ----------------------------------------------------------------------------
struct OrthoBasis {
    glm::vec3 right;
    glm::vec3 up;
};
OrthoBasis orthoBasisFor(OrthoPlane plane);

// ----------------------------------------------------------------------------
// The renderer
// ----------------------------------------------------------------------------
struct Renderer {
    // ---- shaders ----------------------------------------------------------
    GLuint chunkShader = 0, gridShader = 0, sunDepthShader = 0, markerShader = 0;

    // ---- geometry ---------------------------------------------------------
    std::vector<LevelChunk> chunks;           // one VAO per texture chunk
    GLuint gridVAO = 0, gridVBO = 0;          // ground grid (XZ plane)
    std::vector<float> gridVerts;
    GLuint markerVAO = 0, markerVBO = 0;      // unit-cube edges (point entities)
    GLuint wireVAO = 0, wireVBO = 0;          // brush triangle edges (ortho views)
    int wireCount = 0;
    GLuint orthoGridVAO = 0, orthoGridVBO = 0;// dynamic per-axis grid for 2D views

    // ---- sun shadow -------------------------------------------------------
    GLuint sunShadowFBO = 0, sunShadowDepthTex = 0;

    // ---- output targets ---------------------------------------------------
    GLuint editorOutFBO = 0, editorOutTex = 0;   // 3D viewport composite target
    int editorOutW = -1, editorOutH = -1;
    GLuint orthoFBO[kOrthoViewCount] = { 0, 0, 0 };
    GLuint orthoTex[kOrthoViewCount] = { 0, 0, 0 };
    int orthoW[kOrthoViewCount] = { -1, -1, -1 };
    int orthoH[kOrthoViewCount] = { -1, -1, -1 };

    PostFX postFX;

    // ---- lifecycle --------------------------------------------------------
    void init(EditorContext& ctx);
    void buildChunks(EditorContext& ctx);
    void ensureEditorOutput(int w, int h);
    void ensureOrthoOutput(int which, int w, int h);
    void destroy();

    // ---- sun / matrices ---------------------------------------------------
    glm::vec3 sunDirFor(const EditorContext& ctx) const;
    void renderShadow(EditorContext& ctx, const glm::vec3& sunDir, glm::mat4& outLightSpace,
        int restoreW, int restoreH);

    // ---- 3D scene pass ----------------------------------------------------
    void renderScene3D(EditorContext& ctx, int vpW, int vpH,
        const glm::mat4& proj, const glm::mat4& view, const glm::mat4& vp,
        const glm::mat4& lightSpace, int hoverEntity);

    // ---- 2D ortho view ----------------------------------------------------
    void renderOrtho(EditorContext& ctx, int which, int w, int h);
};