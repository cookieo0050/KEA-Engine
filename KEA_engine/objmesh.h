#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include "texture.h"

// ============================================================================
// objmesh.h - Wavefront .obj mesh ({+, no-extension) loading
// ============================================================================
//
// WHAT THIS IS
// ----------------------------------------------------------------------------
// Loads a Wavefront ".obj" file (the format referenced by the FGD "model"
// keyvalue) into an interleaved GPU vertex stream and optional diffuse texture.
// It is the payload behind the engine's generic mesh-entity spawner: any FGD
// @PointClass that declares model({ "path": <key> }) gets its mesh loaded here
// and placed by main.cpp.
//
// VERTEX LAYOUT
// ----------------------------------------------------------------------------
// Same 8-float layout as LevelChunk so an ObjMesh binds to the EXACT same
// vertex attributes (pos3, normal3, uv2):
//     float[0..2]  position
//     float[3..5]  normal
//     float[6..7]  texcoord
// Because the stream is triangle-soup (3 -> 12 -> 24 ... floats per triangle),
// CollisionMesh::addFromVertices can consume it directly.
//
// WHAT IT PARSES
// ----------------------------------------------------------------------------
//   v / vt / vn, f (triangulated into fans, supports v, v/vt, v//vn, v/vt/vn
//   and negative "relative" indices), mtllib + usemtl + map_Kd (first used
//   material becomes the diffuse texture), and # comments. Missing normals get
//   flat-faced normals; missing UVs get (0,0).
// ============================================================================

struct ObjMesh {
    bool loaded = false;
    std::string error;
    std::vector<float> vertexData;   // pos3 + normal3 + uv2 per vertex, triangle soup

    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};

    std::string diffusePath;         // resolved texture file (empty -> white fallback)

    GLuint VAO = 0;
    GLuint VBO = 0;
    GLuint fallbackTexture = 0;      // 1x1 white used when no diffuse texture loads
    int vertexCount = 0;

    Texture diffuseTexture;          // valid() after uploadToGPU() when a texture loaded

    // Parses an .obj (+ optional .mtl in the same folder). `texturesFolder` is a
    // fallback lookup directory for the material's diffuse map (e.g. a texture
    // referenced by bare filename that ships in GameRoot/textures/). The mesh
    // stays in MODEL space; the caller applies the placement transform.
    bool loadFromObj(const std::string& path, const std::string& texturesFolder);

    // Builds VAO/VBO and loads the diffuse texture on the CURRENT GL context.
    bool uploadToGPU();

    // Binds the diffuse (or white fallback) texture on unit 0 and draws the mesh.
    void draw() const;

    // Raw xyz triangle soup (3 floats/vertex, model space) for collision meshes.
    std::vector<float> collisionVertices() const;

    void freeGPU();
};