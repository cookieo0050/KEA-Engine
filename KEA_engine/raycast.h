#ifndef RAYCAST_H
#define RAYCAST_H

#include <glm/glm.hpp>
#include <limits>

struct Ray {
    glm::vec3 origin;
    glm::vec3 direction; // Must be normalized!

    Ray(glm::vec3 orig, glm::vec3 dir)
        : origin(orig), direction(glm::normalize(dir)) {
    }

    glm::vec3 getPoint(float distance) const {
        return origin + direction * distance;
    }
};

struct RaycastHit {
    bool hit = false;
    float distance = std::numeric_limits<float>::max();
    glm::vec3 point = glm::vec3(0.0f);
    glm::vec3 normal = glm::vec3(0.0f);
};

namespace Physics {

    // 1. Ray vs Sphere Intersection
    inline bool raycastSphere(const Ray& ray, const glm::vec3& sphereCenter, float sphereRadius, RaycastHit& outHit) {
        glm::vec3 oc = ray.origin - sphereCenter;
        float b = glm::dot(oc, ray.direction);
        float c = glm::dot(oc, oc) - sphereRadius * sphereRadius;
        float discriminant = b * b - c;

        if (discriminant < 0.0f) return false; // Ray missed the sphere

        float sqrtD = glm::sqrt(discriminant);
        float t = -b - sqrtD; // First entry point

        if (t < 0.0f) {
            t = -b + sqrtD; // If origin was inside, check exit point
        }

        if (t < 0.0f) return false; // Sphere is behind the ray

        outHit.hit = true;
        outHit.distance = t;
        outHit.point = ray.getPoint(t);
        outHit.normal = glm::normalize(outHit.point - sphereCenter);
        return true;
    }

    // 2. Ray vs AABB (Slab Method - Fast & Standard)
    inline bool raycastAABB(const Ray& ray, const glm::vec3& boxMin, const glm::vec3& boxMax, RaycastHit& outHit) {
        float tMin = 0.0f;
        float tMax = std::numeric_limits<float>::max();

        for (int i = 0; i < 3; ++i) {
            if (std::abs(ray.direction[i]) < 0.00001f) {
                // Ray is parallel to slab. No hit if origin is outside the slab.
                if (ray.origin[i] < boxMin[i] || ray.origin[i] > boxMax[i]) return false;
            }
            else {
                float invD = 1.0f / ray.direction[i];
                float t1 = (boxMin[i] - ray.origin[i]) * invD;
                float t2 = (boxMax[i] - ray.origin[i]) * invD;

                if (t1 > t2) std::swap(t1, t2);

                tMin = glm::max(tMin, t1);
                tMax = glm::min(tMax, t2);

                if (tMin > tMax) return false;
            }
        }

        outHit.hit = true;
        outHit.distance = tMin;
        outHit.point = ray.getPoint(tMin);

        // Calculate surface normal based on which face was hit
        glm::vec3 center = (boxMin + boxMax) * 0.5f;
        glm::vec3 size = (boxMax - boxMin) * 0.5f;
        glm::vec3 localPoint = outHit.point - center;
        glm::vec3 normal(0.0f);

        float bias = 1.0001f;
        for (int i = 0; i < 3; ++i) {
            if (std::abs(localPoint[i] / size[i]) >= 0.999f) {
                normal[i] = (localPoint[i] > 0.0f) ? 1.0f : -1.0f;
                break;
            }
        }
        outHit.normal = normal;
        return true;
    }
}

#endif // RAYCAST_H