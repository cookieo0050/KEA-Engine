#ifndef COLLIDER_H
#define COLLIDER_H

#include <glm/glm.hpp>

struct SphereCollider {
    glm::vec3 centerOffset = glm::vec3(0.0f);
    float radius = 0.35f;
};

struct AABB {
    glm::vec3 minBound;
    glm::vec3 maxBound;

    // Fast Sphere vs AABB check
    static bool intersectsSphere(const AABB& box, const glm::vec3& spherePos, float radius, glm::vec3& closestPoint) {
        // Clamp sphere center to box bounds to find closest point on box
        closestPoint.x = glm::clamp(spherePos.x, box.minBound.x, box.maxBound.x);
        closestPoint.y = glm::clamp(spherePos.y, box.minBound.y, box.maxBound.y);
        closestPoint.z = glm::clamp(spherePos.z, box.minBound.z, box.maxBound.z);

        // Distance from sphere center to closest point on box
        float distanceSq = glm::distance2(spherePos, closestPoint);
        return distanceSq < (radius * radius);
    }
};

#endif