#pragma once
#include <glm/glm.hpp>
#include <GLFW/glfw3.h>
#include "camera.h"

class CollisionMesh;

class Player {
public:
    glm::vec3 position = {0, 0, 0};    // feet position
    glm::vec3 velocity = {0, 0, 0};
    bool grounded = false;
    float eyeHeight = 1.5f;

    // Movement settings (ENA Dream BBQ-inspired: slow walk, fast sprint,
    // floaty jumps, generous air steering, a touch of slide)
    float walkSpeed = 4.0f;
    float sprintSpeed = 8.0f;
    float accelerate = 20.0f;
    float airAccelerate = 8.0f;
    float friction = 5.0f;
    float stopSpeed = 2.0f;
    float jumpSpeed = 7.6f;
    float gravity = 18.0f;
    float maxFallSpeed = 14.0f;

    // Collision / movement feel
    float maxStepHeight = 0.4f;    // max rise the walkable gate / ramp mount will accept
    float slopeLimit = 0.6f;       // contacts with normal.y above this are walked over (ramps)
    float groundKeepDistance = 0.2f; // max gap below feet that still counts as "on ground"
    float jumpBufferTime = 0.1f;
    float coyoteTime = 0.1f;
    float jumpBufferTimer = 0.0f;
    float coyoteTimer = 0.0f;

    // Smoothing
    float turnSmoothTime = 0.05f;
    float sprintSmoothTime = 0.1f;

    // Capsule
    float capsuleRadius = 0.35f;
    float capsuleHeight = 1.8f;

    void update(GLFWwindow* window, float dt, Camera& camera, const CollisionMesh& collisionMesh);

    bool isCrouching() const { return crouching; }
    float getEyeHeight() const { return currentEyeHeight; }

private:
    void applyInput(GLFWwindow* window, const Camera& camera, float dt);
    void applyFriction(float dt);
    void accelerateVec(const glm::vec3& wishDir, float wishSpeed, float accel, float dt);
    void handleJump(GLFWwindow* window, float dt);
    void moveHorizontalCollide(const CollisionMesh& mesh, float dist, const glm::vec3& dir);
    float slideMove(glm::vec3& pos, glm::vec3 dir, float dist, const CollisionMesh& mesh, float stepBaseY);
    void stepVertical(float dt, const CollisionMesh& mesh);

    bool crouching = false;
    float currentEyeHeight = 1.5f;
    float sprintLerp = 1.0f;
    glm::vec3 smoothWishDir = {0, 0, 0};
    float moveYaw = 0.0f;
};