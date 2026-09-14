#ifndef PHYSICS_WORLD_H
#define PHYSICS_WORLD_H

#include <vector>
#include "rigidbody.h"
#include "collider.h"
#include "collision.h" // Your mesh collision header

class PhysicsWorld {
public:
    float gravity = 22.0f;

    void update(std::vector<RigidBody*>& bodies, const CollisionMesh& worldMesh, float deltaTime) {
        // Fixed physics timestep sub-stepping (prevents tunneling at high velocities)
        const float fixedTimeStep = 1.0f / 120.0f;
        static float accumulator = 0.0f;
        accumulator += glm::min(deltaTime, 0.1f);

        while (accumulator >= fixedTimeStep) {
            step(bodies, worldMesh, fixedTimeStep);
            accumulator -= fixedTimeStep;
        }
    }

private:
    void step(std::vector<RigidBody*>& bodies, const CollisionMesh& worldMesh, float dt) {
        for (auto* body : bodies) {
            if (body->inverseMass == 0.0f) continue;

            glm::vec3 startPos = body->position;

            // Step 1: Physics Integration
            body->integrate(dt, gravity);

            // Step 2: Mesh Collision Resolution
            worldMesh.resolveSphereCollision(body->position, 0.35f);

            // Step 3: Ground State Check
            float yDisplacement = body->position.y - startPos.y;
            if (body->velocity.y <= 0.0f && yDisplacement > (body->velocity.y * dt + 0.001f)) {
                body->isGrounded = true;
                body->velocity.y = 0.0f;
            }
            else {
                body->isGrounded = false;
            }
        }
    }
};

#endif