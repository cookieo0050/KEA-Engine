#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>

struct Triangle {
    glm::vec3 v0, v1, v2;
    glm::vec3 normal() const;
};

struct RaycastHit {
    bool hit = false;
    float distance = 0.0f;
    glm::vec3 point{};
    glm::vec3 normal{};
    glm::vec3 surfacePoint{};   // for sweepCapsule: the point ON the geometry that was hit
};

struct CollisionContact {
    bool collided = false;
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
    float penetration = 0.0f;
};

class CollisionMesh {
public:
    void buildFromVertices(const float* vertices, size_t floatCount, const glm::mat4& modelMatrix = glm::mat4(1.0f));
    RaycastHit raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance = 1000.0f) const;
    
    // Sweep a capsule along a direction and return the FIRST contact against the mesh.
    // origin = center of the capsule's medial segment at the start of the sweep
    // dir    = sweep direction (does not need to be normalized)
    // maxDistance = how far to sweep
    // radius, halfHeight = capsule dimensions (medial segment spans origin +/- up * halfHeight)
    // The capsule is kept at radius + skinWidth (0.012) from the geometry when contact
    // is reported, so the caller never has to de-penetrate a wall contact.
    // Only contacts that push CLEARLY AGAINST the motion are reported: overlapping
    // geometry at the start (or reached mid-sweep) that pushes along or perpendicular
    // to the sweep direction is ignored, since the capsule is moving away from it
    // (e.g. the floor under a grounded or rising capsule during a horizontal sweep
    // or a jump). The perpendicular case matters: without ignoring it, floating-point
    // noise on the floor contact would occasionally "block" the sweep and make the
    // walkable-rollover logic sail straight through the wall behind it.
    RaycastHit sweepCapsule(const glm::vec3& origin, const glm::vec3& dir, float maxDistance, float radius, float halfHeight) const;

    void resolveSphereCollision(glm::vec3& position, float radius) const;
    CollisionContact resolveSphereCollisionDetailed(glm::vec3& position, float radius) const;

    const std::vector<Triangle>& triangles() const { return m_triangles; }

    void setCellSize(float size) { if (size > 0.1f) m_cellSize = size; }
    float cellSize() const { return m_cellSize; }

private:
    std::vector<Triangle> m_triangles;
    std::unordered_map<int64_t, std::vector<int>> m_grid;
    float m_cellSize = 2.0f;

    void buildGrid();
    void queryCells(const glm::vec3& minB, const glm::vec3& maxB, std::vector<int>& out) const;
    static int64_t cellKey(int cx, int cy, int cz);
};
