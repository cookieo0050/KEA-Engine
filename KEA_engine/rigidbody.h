#ifndef RIGIDBODY_H
#define RIGIDBODY_H

#include <glm/glm.hpp>

struct RigidBody {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec3 acceleration = glm::vec3(0.0f);
    glm::vec3 force = glm::vec3(0.0f);

    float mass = 1.0f; // Mass in kg (0.0 = static/immovable)
    float inverseMass = 1.0f; // Cache 1/mass (0.0 for immovable)
    float drag = 0.01f; // Linear damping/air resistance

    bool isGrounded = false;
    bool useGravity = true;

    void setMass(float m) {
        mass = m;
        inverseMass = (m > 0.0f) ? (1.0f / m) : 0.0f;
    }

    void applyForce(const glm::vec3& f) {
        force += f;
    }

    void applyImpulse(const glm::vec3& impulse) {
        if (inverseMass == 0.0f) return;
        velocity += impulse * inverseMass;
    }

    // Semi-Implicit Euler Integration (more stable than standard Euler)
    void integrate(float deltaTime, float gravityAccel = 22.0f) {
        if (inverseMass == 0.0f) return; // Static body

        // 1. Accumulate gravity force
        if (useGravity && !isGrounded) {
            force.y -= gravityAccel * mass;
        }

        // 2. Acceleration = Force / Mass
        acceleration = force * inverseMass;

        // 3. Update velocity
        velocity += acceleration * deltaTime;

        // 4. Apply drag (air resistance)
        velocity *= glm::clamp(1.0f - drag * deltaTime, 0.0f, 1.0f);

        // 5. Update position using NEW velocity (Semi-Implicit)
        position += velocity * deltaTime;

        // 6. Reset force accumulator for next frame
        force = glm::vec3(0.0f);
    }
};

#endif