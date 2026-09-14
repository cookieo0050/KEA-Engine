#pragma once
#include <glm/glm.hpp>
#include <memory>

#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Body/BodyInterface.h>

#include "collision.h"

// Minimal physics: only gravity + vertical collision + ground detection.
// Horizontal movement & collision are handled by the player directly.
class JoltWorld {
public:
    JoltWorld();
    ~JoltWorld();
    JoltWorld(const JoltWorld&) = delete;
    JoltWorld& operator=(const JoltWorld&) = delete;

    // Builds static world mesh from level triangles.
    bool init(const CollisionMesh& collisionMesh);
    void shutdown();

    // Applies gravity to velocity.y, resolves vertical collision only.
    // Returns ground normal if grounded, else (0,1,0).
    void step(float deltaTime, glm::vec3& position, glm::vec3& velocity, bool& grounded, glm::vec3& outGroundNormal);

    float gravity() const { return m_Gravity; }
    bool initialized() const { return m_Initialized; }

private:
    struct BPLayerInterface;
    struct ObjVsBpFilter;
    struct ObjPairFilter;

    float m_Gravity = 21.5f;
    float m_MaxFallSpeed = 16.0f;
    float m_CharacterHeight = 1.8f;
    float m_CharacterRadius = 0.35f;
    float m_StepHeight = 0.5f;

    std::unique_ptr<BPLayerInterface> m_BpLayerInterface;
    std::unique_ptr<ObjVsBpFilter> m_ObjVsBpFilter;
    std::unique_ptr<ObjPairFilter> m_ObjPairFilter;

    JPH::PhysicsSystem m_PhysicsSystem;
    std::unique_ptr<JPH::TempAllocatorImpl> m_TempAllocator;
    JPH::BodyID m_WorldBody;

    const CollisionMesh* m_CollisionMesh = nullptr;
    bool m_Initialized = false;
};
