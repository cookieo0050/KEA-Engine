// ============================================================================
// collision.cpp - Triangle Mesh Collision & Raycasting
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// Builds a spatial structure over the level's collision triangles and answers
// three questions fast:
//   1. "Does this ray hit the world, and where?"  -> raycast()
//   2. "Push this sphere out of any triangles it overlaps." -> resolveSphereCollision()
//   3. "Where does this swept capsule first touch the world?" -> sweepCapsule()
// It is the engine's own (pre-Jolt) collision system. The Jolt physics world in
// jolt_world.cpp is built from the SAME triangles, but the custom queries here
// drive the player character (walking, sliding, step-up, landing).
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - buildFromVertices(): copies the level's triangle soup into m_triangles
//   (optionally transforming each point by a matrix), then calls buildGrid().
// - buildGrid() + queryCells(): a simple "uniform grid". The world is divided
//   into m_cellSize boxes; each triangle is registered in every cell it touches.
//   Instead of testing every triangle, we only test the cells near a point.
//   `cellKey()` packs x/y/z cell indices into one int64 to use as a map key.
// - raycast(): the classic Amanatides & Woo grid traversal (Google that name).
//   It steps from cell to cell along the ray, testing only the triangles in each
//   visited cell, and keeps the closest hit. This is what makes raycasts cheap.
// - rayTriangleIntersect(): the Möller-Trumbore ray/triangle intersection test.
// - closestPointOnTriangle() / resolveSphereCollisionDetailed(): finds the
//   nearest point on a triangle to the sphere centre; if closer than the radius,
//   the sphere is pushed out along the contact normal. Repeats up to 3 passes so
//   a corner/crack between two triangles can't trap the player.
// - sweepCapsule(): a REAL swept-capsule test. The capsule's medial segment is
//   moved along the sweep direction; the exact distance from segment to triangle
//   (via point/plane and segment/edge distances) is sampled and the first contact
//   time is refined with bisection. Contact is reported at radius + skinWidth so
//   the player never clips into walls and gets a correct slide normal.
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - Collision here is done in the engine's own coordinate space (converted from
//   map space in mapfile.cpp) - no scaling is applied inside this file.
// - If you tune m_cellSize (header), smaller = more memory but faster queries.
// ============================================================================
#include "collision.h"
#include <cmath>
#include <algorithm>

glm::vec3 Triangle::normal() const {
    return glm::normalize(glm::cross(v1 - v0, v2 - v0));
}

void CollisionMesh::buildFromVertices(const float* vertices, size_t floatCount, const glm::mat4& modelMatrix) {
    m_triangles.clear();
    size_t vertCount = floatCount / 3;

    for (size_t i = 0; i + 2 < vertCount; i += 3) {
        auto toVec3 = [&](size_t idx) {
            glm::vec4 p(vertices[idx * 3], vertices[idx * 3 + 1], vertices[idx * 3 + 2], 1.0f);
            p = modelMatrix * p;
            return glm::vec3(p);
            };
        Triangle tri;
        tri.v0 = toVec3(i);
        tri.v1 = toVec3(i + 1);
        tri.v2 = toVec3(i + 2);
        m_triangles.push_back(tri);
    }

    buildGrid();
}

int64_t CollisionMesh::cellKey(int cx, int cy, int cz) {
    return (int64_t)(cx & 0x1FFFF) | ((int64_t)(cy & 0x1FFFF) << 17) | ((int64_t)(cz & 0x1FFFF) << 34);
}

void CollisionMesh::buildGrid() {
    m_grid.clear();
    m_grid.reserve(m_triangles.size());

    for (size_t i = 0; i < m_triangles.size(); ++i) {
        const Triangle& tri = m_triangles[i];
        glm::vec3 mn = glm::min(glm::min(tri.v0, tri.v1), tri.v2);
        glm::vec3 mx = glm::max(glm::max(tri.v0, tri.v1), tri.v2);

        int x0 = (int)glm::floor(mn.x / m_cellSize), x1 = (int)glm::floor(mx.x / m_cellSize);
        int y0 = (int)glm::floor(mn.y / m_cellSize), y1 = (int)glm::floor(mx.y / m_cellSize);
        int z0 = (int)glm::floor(mn.z / m_cellSize), z1 = (int)glm::floor(mx.z / m_cellSize);

        for (int cx = x0; cx <= x1; ++cx)
            for (int cy = y0; cy <= y1; ++cy)
                for (int cz = z0; cz <= z1; ++cz)
                    m_grid[cellKey(cx, cy, cz)].push_back((int)i);
    }
}

void CollisionMesh::queryCells(const glm::vec3& minB, const glm::vec3& maxB, std::vector<int>& out) const {
    out.clear();

    int x0 = (int)glm::floor(minB.x / m_cellSize), x1 = (int)glm::floor(maxB.x / m_cellSize);
    int y0 = (int)glm::floor(minB.y / m_cellSize), y1 = (int)glm::floor(maxB.y / m_cellSize);
    int z0 = (int)glm::floor(minB.z / m_cellSize), z1 = (int)glm::floor(maxB.z / m_cellSize);

    for (int cx = x0; cx <= x1; ++cx) {
        int64_t xPart = (int64_t)(cx & 0x1FFFF);
        for (int cy = y0; cy <= y1; ++cy) {
            int64_t yPart = ((int64_t)(cy & 0x1FFFF) << 17);
            for (int cz = z0; cz <= z1; ++cz) {
                auto it = m_grid.find(xPart | yPart | ((int64_t)(cz & 0x1FFFF) << 34));
                if (it != m_grid.end())
                    out.insert(out.end(), it->second.begin(), it->second.end());
            }
        }
    }
}

static bool rayTriangleIntersect(const glm::vec3& orig, const glm::vec3& dir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& t) {
    const float EPSILON = 1e-6f;
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(dir, edge2);
    float a = glm::dot(edge1, h);
    if (fabs(a) < EPSILON) return false;

    float f = 1.0f / a;
    glm::vec3 s = orig - v0;
    float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;

    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return false;

    t = f * glm::dot(edge2, q);
    return t > EPSILON;
}

RaycastHit CollisionMesh::raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance) const {
    RaycastHit result;

    float dirLen = glm::length(dir);
    if (dirLen < 1e-8f) return result;
    glm::vec3 d = dir / dirLen;

    const float BIG = 1e30f;
    glm::vec3 invD(
        fabsf(d.x) < 1e-9f ? BIG : 1.0f / d.x,
        fabsf(d.y) < 1e-9f ? BIG : 1.0f / d.y,
        fabsf(d.z) < 1e-9f ? BIG : 1.0f / d.z);

    int cx = (int)glm::floor(origin.x / m_cellSize);
    int cy = (int)glm::floor(origin.y / m_cellSize);
    int cz = (int)glm::floor(origin.z / m_cellSize);

    int stepX = d.x > 0 ? 1 : (d.x < 0 ? -1 : 0);
    int stepY = d.y > 0 ? 1 : (d.y < 0 ? -1 : 0);
    int stepZ = d.z > 0 ? 1 : (d.z < 0 ? -1 : 0);

    float tMaxX = stepX == 0 ? BIG : ((stepX > 0 ? (cx + 1) * m_cellSize : cx * m_cellSize) - origin.x) * invD.x;
    float tMaxY = stepY == 0 ? BIG : ((stepY > 0 ? (cy + 1) * m_cellSize : cy * m_cellSize) - origin.y) * invD.y;
    float tMaxZ = stepZ == 0 ? BIG : ((stepZ > 0 ? (cz + 1) * m_cellSize : cz * m_cellSize) - origin.z) * invD.z;

    float tDeltaX = stepX != 0 ? m_cellSize * fabsf(invD.x) : BIG;
    float tDeltaY = stepY != 0 ? m_cellSize * fabsf(invD.y) : BIG;
    float tDeltaZ = stepZ != 0 ? m_cellSize * fabsf(invD.z) : BIG;

    float closest = maxDistance;
    float t = 0.0f;
    int guard = 0;

    while (t <= maxDistance && guard < 100000) {
        if (result.hit && t >= closest) break;

        auto it = m_grid.find(cellKey(cx, cy, cz));
        if (it != m_grid.end()) {
            for (int idx : it->second) {
                const Triangle& tri = m_triangles[idx];
                float hitT;
                if (rayTriangleIntersect(origin, d, tri.v0, tri.v1, tri.v2, hitT) && hitT < closest) {
                    closest = hitT;
                    result.hit = true;
                    result.distance = hitT;
                    result.point = origin + d * hitT;
                    result.normal = tri.normal();
                }
            }
        }

        float tMin;
        if (tMaxX <= tMaxY && tMaxX <= tMaxZ) { tMin = tMaxX; tMaxX += tDeltaX; cx += stepX; }
        else if (tMaxY <= tMaxZ) { tMin = tMaxY; tMaxY += tDeltaY; cy += stepY; }
        else { tMin = tMaxZ; tMaxZ += tDeltaZ; cz += stepZ; }
        t = tMin;
        ++guard;
    }

    return result;
}

static glm::vec3 closestPointOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 ab = b - a;
    glm::vec3 ac = c - a;
    glm::vec3 ap = p - a;

    float d1 = glm::dot(ab, ap);
    float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp);
    float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return a + v * ab;
    }

    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp);
    float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return a + w * ac;
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return a + ab * v + ac * w;
}

CollisionContact CollisionMesh::resolveSphereCollisionDetailed(glm::vec3& position, float radius) const {
    CollisionContact result;
    const int MAX_PASSES = 3;
    std::vector<int> nearby;

    for (int pass = 0; pass < MAX_PASSES; ++pass) {
        queryCells(position - glm::vec3(radius), position + glm::vec3(radius), nearby);
        if (nearby.empty()) break;

        std::sort(nearby.begin(), nearby.end());
        nearby.erase(std::unique(nearby.begin(), nearby.end()), nearby.end());

        bool pushed = false;
        float maxPen = 0.0f;
        glm::vec3 contactNormal(0.0f);

        for (int idx : nearby) {
            const Triangle& tri = m_triangles[idx];
            glm::vec3 closest = closestPointOnTriangle(position, tri.v0, tri.v1, tri.v2);
            glm::vec3 delta = position - closest;
            float dist = glm::length(delta);

            if (dist < radius) {
                pushed = true;
                glm::vec3 pushDir;
                if (dist > 1e-6f) {
                    pushDir = delta / dist;
                    position += pushDir * (radius - dist);
                    maxPen = glm::max(maxPen, radius - dist);
                }
                else {
                    pushDir = tri.normal();
                    position += pushDir * radius;
                    maxPen = glm::max(maxPen, radius);
                }
                contactNormal += pushDir;
            }
        }

        if (!pushed) break;

        float normalLen = glm::length(contactNormal);
        if (normalLen > 1e-8f) {
            result.collided = true;
            result.normal = contactNormal / normalLen;
            result.penetration = maxPen;
        }
    }

    return result;
}

void CollisionMesh::resolveSphereCollision(glm::vec3& position, float radius) const {
    resolveSphereCollisionDetailed(position, radius);
}

// ----------------------------------------------------------------------------
// Swept capsule collision helpers
// ----------------------------------------------------------------------------
// A capsule is a sphere of `radius` swept along a medial segment. Sweeping the
// capsule along a direction creates a volume; we find the earliest position
// along the sweep where that volume just touches a triangle. Instead of
// intersecting the volume directly we sample the EXACT distance between the
// moving medial segment and each candidate triangle along the sweep, then
// binary-search the exact first-contact time between two samples.
// ----------------------------------------------------------------------------

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Closest points between two segments (Ericson, Real-Time Collision Detection 5.1.8).
static void closestPtSegSeg(const glm::vec3& p1, const glm::vec3& q1,
                            const glm::vec3& p2, const glm::vec3& q2,
                            glm::vec3& c1, glm::vec3& c2) {
    glm::vec3 d1 = q1 - p1;
    glm::vec3 d2 = q2 - p2;
    glm::vec3 r = p1 - p2;

    float a = glm::dot(d1, d1);
    float e = glm::dot(d2, d2);
    float f = glm::dot(d2, r);

    float s, t;
    if (a <= 1e-12f && e <= 1e-12f) {
        s = 0.0f; t = 0.0f;
    } else if (a <= 1e-12f) {
        s = 0.0f; t = clampf(f / e, 0.0f, 1.0f);
    } else {
        float c = glm::dot(d1, r);
        if (e <= 1e-12f) {
            t = 0.0f; s = clampf(-c / a, 0.0f, 1.0f);
        } else {
            float b = glm::dot(d1, d2);
            float denom = a * e - b * b;
            s = denom != 0.0f ? clampf((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = clampf(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = clampf((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

static float distPointTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    return glm::length(p - closestPointOnTriangle(p, a, b, c));
}

// Exact distance between a segment and a triangle. Handles the segment
// piercing the triangle face (0 distance), both segment endpoints, and the
// three segment<->edge pairs (which also captures the cylinder-vs-edge case).
static float distSegmentTriangle(const glm::vec3& segA, const glm::vec3& segB,
                                 const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 segDir = segB - segA;
    float segLen = glm::length(segDir);
    if (segLen > 1e-9f) {
        float t;
        if (rayTriangleIntersect(segA, segDir, a, b, c, t) && t <= segLen)
            return 0.0f;
    }

    float best = std::min(distPointTriangle(segA, a, b, c), distPointTriangle(segB, a, b, c));

    const glm::vec3* edge[6] = { &a, &b, &b, &c, &c, &a };
    for (int i = 0; i < 3; ++i) {
        glm::vec3 c1, c2;
        closestPtSegSeg(segA, segB, *edge[i * 2], *edge[i * 2 + 1], c1, c2);
        float d = glm::length(c1 - c2);
        if (d < best) best = d;
    }
    return best;
}

RaycastHit CollisionMesh::sweepCapsule(const glm::vec3& origin, const glm::vec3& dir,
                                       float maxDistance, float radius, float halfHeight) const {
    RaycastHit result;

    float dirLen = glm::length(dir);
    if (dirLen < 1e-8f || maxDistance <= 0.0f) return result;
    glm::vec3 d = dir / dirLen;

    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 segLo = origin - up * halfHeight;
    const glm::vec3 segHi = origin + up * halfHeight;

    // Broad phase: gather every triangle in the spatial grid cells that the
    // swept capsule's bounding box overlaps (expanded by one cell + radius so
    // near-cell-boundary contacts are never missed).
    const glm::vec3 lo0 = glm::min(segLo, segHi);
    const glm::vec3 hi0 = glm::max(segLo, segHi);
    const glm::vec3 lo1 = lo0 + d * maxDistance;
    const glm::vec3 hi1 = hi0 + d * maxDistance;
    const glm::vec3 pad = glm::vec3(radius + m_cellSize);
    glm::vec3 bmin = glm::min(glm::min(lo0, lo1), glm::min(hi0, hi1)) - pad;
    glm::vec3 bmax = glm::max(glm::max(lo0, lo1), glm::max(hi0, hi1)) + pad;

    std::vector<int> nearby;
    queryCells(bmin, bmax, nearby);
    if (nearby.empty()) return result;

    std::sort(nearby.begin(), nearby.end());
    nearby.erase(std::unique(nearby.begin(), nearby.end()), nearby.end());

    // Sample spacing must be <= ~0.3*radius along the sweep so a thin triangle
    // sitting exactly between two samples is still detected.
    const float skin = 0.012f;                       // keep capsule this far off geometry
    const float threshold = radius + skin;
    const float step = std::max(0.08f, radius * 0.3f);
    int nSamples = (int)std::ceil(maxDistance / step);
    nSamples = std::max(2, std::min(nSamples, 48));
    const float stepT = maxDistance / (float)(nSamples - 1);

    float bestT = -1.0f;
    glm::vec3 bestPoint(0.0f);
    glm::vec3 bestSurface(0.0f);
    glm::vec3 bestNormal(0.0f);

    auto segDistAt = [&](const glm::vec3& center, const Triangle& tri) {
        return distSegmentTriangle(center - up * halfHeight, center + up * halfHeight,
                                   tri.v0, tri.v1, tri.v2);
    };

    for (int idx : nearby) {
        const Triangle& tri = m_triangles[idx];

        glm::vec3 center0 = origin;
        if (segDistAt(center0, tri) <= threshold) {
            // Already overlapping at the start of the sweep. Only treat it as
            // a contact if the push is OPPOSITE the motion; a contact behind
            // the capsule (e.g. the floor when jumping upward) must not cancel
            // the sweep. The push must be clearly against the motion: the floor
            // under a grounded capsule pushes perpendicular to a horizontal
            // sweep (dot ~ 0), and without the tolerance its tiny floating
            // point sign would occasionally "block" the sweep, making
            // slideMove sail through the whole remaining distance -- through
            // whatever wall is behind the floor contact.
            glm::vec3 closest = closestPointOnTriangle(origin, tri.v0, tri.v1, tri.v2);
            glm::vec3 n = origin - closest;
            if (glm::length(n) > 1e-5f) n /= glm::length(n); else n = tri.normal();
            if (glm::dot(n, d) > -1e-4f) continue;
            // Several triangles can overlap the capsule at the start of a sweep
            // (e.g. a wall next to a sloped step at its base). Keep the contact
            // that pushes MOST against the motion, so a real blocker is never
            // shadowed by a walkable face that shares the capsule's boundary.
            if (bestT < 0.0f ||
                (bestT == 0.0f && glm::dot(n, d) < glm::dot(bestNormal, d))) {
                bestT = 0.0f;
                bestPoint = origin;
                bestSurface = closest;
                bestNormal = n;
            }
            continue;
        }

        for (int i = 1; i < nSamples; ++i) {
            float t = (float)i * stepT;
            if (segDistAt(origin + d * t, tri) <= threshold) {
                // Contact bracketed in [(i-1)*stepT, i*stepT]; refine it.
                float lo = (float)(i - 1) * stepT, hi = t;
                for (int it = 0; it < 12; ++it) {
                    float mid = (lo + hi) * 0.5f;
                    if (segDistAt(origin + d * mid, tri) <= threshold) hi = mid;
                    else lo = mid;
                }
                float contactT = (lo + hi) * 0.5f;
                glm::vec3 cp = origin + d * contactT;
                glm::vec3 closest = closestPointOnTriangle(cp, tri.v0, tri.v1, tri.v2);
                glm::vec3 n = cp - closest;
                if (glm::length(n) > 1e-5f) n /= glm::length(n); else n = tri.normal();
                // Skip contacts that push along the motion (behind us); they
                // can't stop the capsule. Keep scanning for a real blocker.
                // (Tolerance: perpendicular push, like the floor under a
                // grounded capsule, must never register as a blocker.)
                if (glm::dot(n, d) > -1e-4f) continue;
                // Prefer an EARLIER contact, and among (essentially) equal
                // contact times the one that pushes MOST against the motion, so
                // a blocking wall is never shadowed by a walkable step/ramp
                // face that shares the same contact point.
                if (bestT < 0.0f ||
                    contactT < bestT - 1e-5f ||
                    (std::fabs(contactT - bestT) <= 1e-5f &&
                     glm::dot(n, d) < glm::dot(bestNormal, d))) {
                    bestT = contactT;
                    bestPoint = cp;
                    bestSurface = closest;
                    bestNormal = n;
                }
                break;
            }
        }
    }

    if (bestT < 0.0f) return result;

    result.hit = true;
    result.distance = bestT;
    result.point = bestPoint;
    result.surfacePoint = bestSurface;
    result.normal = bestNormal;
    return result;
}
