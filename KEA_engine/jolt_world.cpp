// ============================================================================
// jolt_world.cpp - Minimal Physics: Gravity + Vertical Collision Only
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// Minimal Jolt wrapper that ONLY handles:
//   - Gravity integration (vertical velocity)
//   - Vertical collision resolution (don't fall through floor, hit ceiling)
//   - Ground detection (raycast down)
// 
// HORIZONTAL MOVEMENT & COLLISION ARE HANDLED BY PLAYER DIRECTLY
// using the CollisionMesh for raycasts/sweeps. This gives full control
// over movement feel without physics engine interference.
//
// ============================================================================
#include "jolt_world.h"
#include "console.h"
#include <iostream>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Body/BodyInterface.h>

namespace {

namespace Layers {
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING = 1;
    static constexpr JPH::uint NUM_LAYERS = 2;
}

namespace BroadPhaseLayers {
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr JPH::uint NUM_LAYERS = 2;
}

} // namespace

struct JoltWorld::BPLayerInterface : public JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
        return inLayer == Layers::NON_MOVING ? BroadPhaseLayers::NON_MOVING : BroadPhaseLayers::MOVING;
    }
};

struct JoltWorld::ObjVsBpFilter : public JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        switch (inLayer1) {
        case Layers::NON_MOVING: return inLayer2 == BroadPhaseLayers::MOVING;
        case Layers::MOVING:     return true;
        default: JPH_ASSERT(false); return false;
        }
    }
};

struct JoltWorld::ObjPairFilter : public JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        switch (inObject1) {
        case Layers::NON_MOVING: return inObject2 == Layers::MOVING;
        case Layers::MOVING:     return true;
        default: JPH_ASSERT(false); return false;
        }
    }
};

JoltWorld::JoltWorld() = default;

JoltWorld::~JoltWorld() {
    shutdown();
}

bool JoltWorld::init(const CollisionMesh& collisionMesh) {
    shutdown();

    m_CollisionMesh = &collisionMesh;

    JPH::RegisterDefaultAllocator();
    m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
    if (JPH::Factory::sInstance == nullptr)
        JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    m_BpLayerInterface = std::make_unique<BPLayerInterface>();
    m_ObjVsBpFilter = std::make_unique<ObjVsBpFilter>();
    m_ObjPairFilter = std::make_unique<ObjPairFilter>();

    const JPH::uint maxBodies = 4096;
    const JPH::uint numBodyMutexes = 0;
    const JPH::uint maxBodyPairs = 65536;
    const JPH::uint maxContactConstraints = 10240;
    m_PhysicsSystem.Init(maxBodies, numBodyMutexes, maxBodyPairs, maxContactConstraints,
        *m_BpLayerInterface, *m_ObjVsBpFilter, *m_ObjPairFilter);

    // Static world mesh built from the level collision triangles.
    JPH::VertexList vertices;
    JPH::IndexedTriangleList indices;
    const std::vector<Triangle>& tris = collisionMesh.triangles();
    vertices.reserve(tris.size() * 3);
    indices.reserve(tris.size());
    JPH::uint32 base = 0;
    for (const Triangle& t : tris) {
        vertices.push_back(JPH::Float3(t.v0.x, t.v0.y, t.v0.z));
        vertices.push_back(JPH::Float3(t.v1.x, t.v1.y, t.v1.z));
        vertices.push_back(JPH::Float3(t.v2.x, t.v2.y, t.v2.z));
        indices.push_back(JPH::IndexedTriangle(base, base + 1, base + 2));
        base += 3;
    }
    if (vertices.empty()) {
        std::cerr << "[JoltWorld] No collision geometry to build physics world\n";
        g_Console.logError("No collision geometry to build physics world");
        return false;
    }

    JPH::MeshShapeSettings meshSettings(vertices, indices);
    meshSettings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult meshResult = meshSettings.Create();
    if (meshResult.HasError()) {
        std::cerr << "[JoltWorld] MeshShape error: " << meshResult.GetError() << "\n";
        g_Console.logError("MeshShape error: " + std::string(meshResult.GetError().c_str()));
        return false;
    }
    JPH::ShapeRefC meshShape = meshResult.Get();

    JPH::BodyCreationSettings worldSettings(meshShape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, Layers::NON_MOVING);
    JPH::BodyInterface& bodyInterface = m_PhysicsSystem.GetBodyInterface();
    JPH::Body* worldBody = bodyInterface.CreateBody(worldSettings);
    if (worldBody == nullptr) {
        std::cerr << "[JoltWorld] Failed to create world body\n";
        g_Console.logError("Failed to create world body");
        return false;
    }
    m_WorldBody = worldBody->GetID();
    bodyInterface.AddBody(m_WorldBody, JPH::EActivation::DontActivate);

    m_Initialized = true;
    return true;
}

void JoltWorld::shutdown() {
    m_Initialized = false;
    m_CollisionMesh = nullptr;

    if (!m_WorldBody.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = m_PhysicsSystem.GetBodyInterface();
        bodyInterface.RemoveBody(m_WorldBody);
        bodyInterface.DestroyBody(m_WorldBody);
        m_WorldBody = JPH::BodyID();
    }

    m_BpLayerInterface.reset();
    m_ObjVsBpFilter.reset();
    m_ObjPairFilter.reset();

    if (JPH::Factory::sInstance != nullptr) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

void JoltWorld::step(float deltaTime, glm::vec3& position, glm::vec3& velocity, bool& grounded, glm::vec3& outGroundNormal) {
    if (!m_Initialized)
        return;

    // 1. Apply gravity to vertical velocity
    velocity.y -= m_Gravity * deltaTime;
    if (velocity.y < -m_MaxFallSpeed)
        velocity.y = -m_MaxFallSpeed;

    // Capsule shape for vertical sweeps
    float capsuleHalfHeight = 0.5f * (m_CharacterHeight - 2.0f * m_CharacterRadius);
    float capsuleCenterY = m_CharacterHeight * 0.5f; // center offset from feet
    JPH::Ref<JPH::CapsuleShape> capsuleShape = new JPH::CapsuleShape(capsuleHalfHeight, m_CharacterRadius);
    JPH::Vec3 capsuleScale(1.0f, 1.0f, 1.0f);

    // 2. Vertical collision resolution using ShapeCast (capsule sweep)
    // Down sweep (falling/landing)
    if (velocity.y < 0.0f) {
        float sweepDist = -velocity.y * deltaTime + 0.05f; // small buffer
        JPH::RVec3 startPos(position.x, position.y + capsuleCenterY, position.z);
        JPH::RMat44 startTransform = JPH::RMat44::sIdentity();
        startTransform.SetTranslation(startPos);
        JPH::Vec3 direction(0.0f, -1.0f, 0.0f);
        JPH::RShapeCast shapeCast(capsuleShape, capsuleScale, startTransform, direction * sweepDist);
        
        JPH::ShapeCastSettings castSettings;
        castSettings.mReturnDeepestPoint = true;
        castSettings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
        castSettings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
        
        // Use closest hit collector
        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        
        m_PhysicsSystem.GetNarrowPhaseQuery().CastShape(
            shapeCast, castSettings, startPos, collector,
            m_PhysicsSystem.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
            m_PhysicsSystem.GetDefaultLayerFilter(Layers::MOVING),
            JPH::BodyFilter(), JPH::ShapeFilter());
        
        if (collector.HadHit()) {
            // Hit ground - stop at contact point
            float hitFraction = collector.mHit.mFraction;
            float newCenterY = (float)(startPos.GetY() + direction.GetY() * hitFraction * sweepDist);
            position.y = newCenterY - capsuleCenterY;
            velocity.y = 0.0f;
            grounded = true;
            outGroundNormal = glm::vec3(collector.mHit.mPenetrationAxis.GetX(), collector.mHit.mPenetrationAxis.GetY(), collector.mHit.mPenetrationAxis.GetZ());
        } else {
            // No ground hit - apply vertical movement
            position.y += velocity.y * deltaTime;
            grounded = false;
            outGroundNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }
    // Up sweep (hitting ceiling)
    else if (velocity.y > 0.0f) {
        float sweepDist = velocity.y * deltaTime + 0.05f;
        JPH::RVec3 startPos(position.x, position.y + capsuleCenterY, position.z);
        JPH::RMat44 startTransform = JPH::RMat44::sIdentity();
        startTransform.SetTranslation(startPos);
        JPH::Vec3 direction(0.0f, 1.0f, 0.0f);
        JPH::RShapeCast shapeCast(capsuleShape, capsuleScale, startTransform, direction * sweepDist);
        
        JPH::ShapeCastSettings castSettings;
        castSettings.mReturnDeepestPoint = true;
        castSettings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
        castSettings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
        
        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        
        m_PhysicsSystem.GetNarrowPhaseQuery().CastShape(
            shapeCast, castSettings, startPos, collector,
            m_PhysicsSystem.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
            m_PhysicsSystem.GetDefaultLayerFilter(Layers::MOVING),
            JPH::BodyFilter(), JPH::ShapeFilter());
        
        if (collector.HadHit()) {
            float hitFraction = collector.mHit.mFraction;
            float newCenterY = (float)(startPos.GetY() + direction.GetY() * hitFraction * sweepDist);
            position.y = newCenterY - capsuleCenterY;
            velocity.y = 0.0f;
        } else {
            position.y += velocity.y * deltaTime;
        }
    }

    // 3. Ground check (raycast down from feet) if not already grounded
    if (!grounded) {
        JPH::RVec3 rayStart(position.x, position.y + 0.05f, position.z);
        JPH::RRayCast ray(rayStart, JPH::Vec3(0.0f, -1.0f, 0.0f) * 0.5f);
        JPH::RayCastResult rayResult;
        
        if (m_PhysicsSystem.GetNarrowPhaseQuery().CastRay(
            ray, rayResult,
            m_PhysicsSystem.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
            m_PhysicsSystem.GetDefaultLayerFilter(Layers::MOVING),
            JPH::BodyFilter())) {
            grounded = true;
            outGroundNormal = glm::vec3(0.0f, 1.0f, 0.0f);
            // Snap to ground
            position.y = (float)(rayStart.GetY() + ray.mDirection.GetY() * rayResult.mFraction);
            velocity.y = 0.0f;
        }
    }

    // 4. Horizontal position - NO JOLT COLLISION
    // Player handles horizontal collision via CollisionMesh directly
    position.x += velocity.x * deltaTime;
    position.z += velocity.z * deltaTime;
}