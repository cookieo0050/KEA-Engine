// EditorRenderer.h - Rendering pipeline
#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <map>
#include "EditorScene.h"
#include "EditorCamera.h"
#include "renderpipeline.h"
#include "texture.h"

class EditorRenderer {
public:
    EditorRenderer();
    ~EditorRenderer();

    bool init(int width, int height);
    void shutdown();

    void resize(int width, int height);
    void render(const EditorScene& scene, const EditorCamera& camera);
    void renderShadowMap(const EditorScene& scene, const EditorCamera& camera);

    GLuint editorOutputTexture() const { return m_editorOutTex; }
    void setEditorOutputSize(int w, int h);

    // Pipeline settings
    bool shadowsEnabled() const { return m_shadowsEnabled; }
    void setShadowsEnabled(bool v) { m_shadowsEnabled = v; }
    bool wireframe() const { return m_wireframe; }
    void setWireframe(bool v) { m_wireframe = v; }

private:
    // Shaders
    GLuint m_chunkShader = 0;
    GLuint m_gridShader = 0;
    GLuint m_markerShader = 0;
    GLuint m_sunDepthShader = 0;
    GLuint m_sunDepthShaderSingle = 0; // For single texture

    // Shadow map
    GLuint m_sunShadowFBO = 0;
    GLuint m_sunShadowDepthTex = 0;
    static constexpr int SHADOW_RES = 2048;
    GLuint m_sunDepthShaderInstanced = 0;

    // PostFX
    struct PostFX {
        GLuint hdrFBO = 0, hdrColorTex = 0, hdrIndirectTex = 0, hdrDepthTex = 0;
        GLuint ssaoFBO = 0, ssaoTex = 0;
        GLuint ssaoBlurFBO = 0, ssaoBlurTex = 0;
        GLuint noiseTex = 0;
        GLuint quadVAO = 0;
        GLuint ssaoShader = 0, blurShader = 0, tonemapShader = 0;
        std::vector<glm::vec3> kernel;
        int width = 0, height = 0;
    } m_postFX;

    // Editor output (for ImGui viewport)
    GLuint m_editorOutFBO = 0;
    GLuint m_editorOutTex = 0;
    int m_editorOutW = -1, m_editorOutH = -1;

    // Grid
    GLuint m_gridVAO = 0, m_gridVBO = 0;
    int m_gridIndexCount = 0;

    // Marker cube
    GLuint m_markerVAO = 0, m_markerVBO = 0;

    bool m_shadowsEnabled = true;
    bool m_wireframe = false;

    // Shader sources
    static const char* chunkVertSrc;
    static const char* chunkFragSrc;
    static const char* gridVertSrc;
    static const char* gridFragSrc;
    static const char* markerVertSrc;
    static const char* markerFragSrc;
    static const char* sunDepthVertSrc;
    static const char* sunDepthFragSrc;
    static const char* postQuadVertSrc;
    static const char* ssaoFragSrc;
    static const char* ssaoBlurFragSrc;
    static const char* tonemapFragSrc;

    void initShaders();
    void initPostFX(int w, int h);
    void initGrid();
    void initMarkers();
    void initShadowMap();
    void rebuildGrid(const EditorScene& scene);

    void renderScene(const EditorScene& scene, const EditorCamera& camera, const glm::mat4& viewProj, const glm::mat4& lightSpaceMatrix);
    void renderPostFX(const EditorScene& scene, const EditorCamera& camera);
    void renderEditorOutput(int w, int h);
};