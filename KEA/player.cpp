#include "player.h"
#include "collision.h"
#include <cmath>
#include <algorithm>

void Player::update(GLFWwindow* window, float dt, Camera& camera, const CollisionMesh& collisionMesh) {
    dt = std::min(dt, 0.05f);

    const glm::vec3 up = {0, 1, 0};

    // --- Input & Wish Direction ---
    applyInput(window, camera, dt);

    // --- Jump ---
    handleJump(window, dt);

    // --- Horizontal Movement ---
    if (grounded) {
        applyFriction(dt);
        accelerateVec(smoothWishDir, walkSpeed * sprintLerp, accelerate, dt);
    } else {
        accelerateVec(smoothWishDir, walkSpeed * sprintLerp, airAccelerate, dt);
    }

    // --- Horizontal move & collide (slides, ramp mounting, corner checking) ---
    glm::vec3 preMove = position;
    glm::vec3 horizVel = velocity;
    horizVel.y = 0;
    float horizSpeed = glm::length(horizVel);
    if (horizSpeed > 1e-4f) {
        glm::vec3 dir = horizVel / horizSpeed;
        moveHorizontalCollide(collisionMesh, horizSpeed * dt, dir);
    }

    // --- Horizontal speed cap ---
    // Air has no friction, so turning the wish direction while jumping (bunny
    // hopping) would otherwise let air acceleration add speed past the run and
    // sprint caps every frame. Keep the player from ever exceeding the current
    // walk/sprint cap, airborne or grounded.
    float maxSpeed = walkSpeed * sprintLerp;
    glm::vec3 hv = velocity;
    hv.y = 0;
    float hs = glm::length(hv);
    if (hs > maxSpeed) {
        glm::vec3 capped = hv * (maxSpeed / hs);
        velocity.x = capped.x;
        velocity.z = capped.z;
    }

    // --- Vertical (Gravity + Floor/Ceiling) ---
    stepVertical(dt, collisionMesh);

    // --- Eye Height ---
    float targetEye = crouching ? 0.65f : eyeHeight;
    currentEyeHeight += (targetEye - currentEyeHeight) * (1.0f - std::exp(-15.0f * dt));

    camera.position = position + up * currentEyeHeight;
}

void Player::applyInput(GLFWwindow* window, const Camera& camera, float dt) {
    // Forward/right from camera yaw only
    glm::vec3 forward = glm::normalize(glm::vec3(camera.front.x, 0, camera.front.z));
    if (glm::length(forward) < 0.001f) forward = {0, 0, -1};
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));

    // Raw input direction
    glm::vec3 wishDir = {0, 0, 0};
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) wishDir += forward;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) wishDir -= forward;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) wishDir += right;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) wishDir -= right;

    float wishLen = glm::length(wishDir);
    if (wishLen > 0.001f) {
        wishDir /= wishLen;
        // Smooth direction
        float t = 1.0f - std::exp(-dt / turnSmoothTime);
        smoothWishDir = glm::normalize(glm::mix(smoothWishDir, wishDir, t));
        moveYaw = std::atan2(smoothWishDir.x, smoothWishDir.z);
    } else {
        smoothWishDir = {0, 0, 0};
    }

    // Sprint
    bool sprint = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
    float targetSprint = sprint ? (sprintSpeed / walkSpeed) : 1.0f;
    sprintLerp += (targetSprint - sprintLerp) * (1.0f - std::exp(-dt / sprintSmoothTime));

    // Crouch
    crouching = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || 
                glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS;
}

void Player::applyFriction(float dt) {
    glm::vec3 horiz = velocity;
    horiz.y = 0;
    float speed = glm::length(horiz);
    if (speed < 0.01f) return;

    float control = std::max(speed, stopSpeed);
    float drop = control * friction * dt;
    float newSpeed = std::max(0.0f, speed - drop);
    float factor = newSpeed / speed;
    velocity.x *= factor;
    velocity.z *= factor;
}

void Player::accelerateVec(const glm::vec3& wishDir, float wishSpeed, float accel, float dt) {
    if (glm::length(wishDir) < 0.001f) return;

    float currentSpeed = glm::dot(velocity, wishDir);
    float addSpeed = wishSpeed - currentSpeed;
    if (addSpeed <= 0) return;

    float accelSpeed = std::min(accel * dt * wishSpeed, addSpeed);
    velocity += wishDir * accelSpeed;
}

void Player::handleJump(GLFWwindow* window, float dt) {
    bool space = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;

    // Buffer
    if (space) jumpBufferTimer = jumpBufferTime;
    else jumpBufferTimer = std::max(0.0f, jumpBufferTimer - dt);

    // Coyote
    if (grounded) coyoteTimer = coyoteTime;
    else coyoteTimer = std::max(0.0f, coyoteTimer - dt);

    // Jump
    if (jumpBufferTimer > 0 && coyoteTimer > 0) {
        velocity.y = jumpSpeed;
        jumpBufferTimer = 0;
        coyoteTimer = 0;
        grounded = false;
    }
}

void Player::moveHorizontalCollide(const CollisionMesh& mesh, float dist, const glm::vec3& dir) {
    glm::vec3 start = position;

    // Slide along at this height. Edges/kerbs/stairs are not auto-climbed: if
    // the sweep is blocked, the capsule simply stops there. Ramps and slopes
    // are still ridden by slideMove's mountable branch (surface under foot).
    slideMove(position, dir, dist, mesh, position.y);

    // Reflect the resolved slide direction back into the velocity so the
    // movement keeps the player running along walls instead of stalling.
    glm::vec3 moved = position - start;
    moved.y = 0;
    float movedLen = glm::length(moved);
    if (movedLen > 1e-4f) {
        float speed = glm::length(glm::vec3(velocity.x, 0.0f, velocity.z));
        glm::vec3 newDir = moved / movedLen;
        velocity.x = newDir.x * speed;
        velocity.z = newDir.z * speed;
    } else {
        velocity.x = 0.0f;
        velocity.z = 0.0f;
    }
}

// Sweep the capsule along `dir` for `dist`, sliding along any wall it touches.
// Returns how far along `dir` the capsule actually travelled (0..dist).
// stepBaseY = the feet level step-over rules are judged from (the *original*
// feet height during step-up attempts, whose pos.y rises and must not widen
// the walkable gate).
float Player::slideMove(glm::vec3& pos, glm::vec3 dir, float dist, const CollisionMesh& mesh, float stepBaseY) {
    const glm::vec3 up = {0, 1, 0};
    const float r = capsuleRadius;
    const float halfH = capsuleHeight * 0.5f;
    const float hh = halfH - r;                 // medial segment half-length

    float achieved = 0.0f;
    float remaining = dist;
    for (int iter = 0; iter < 4 && remaining > 1e-4f; ++iter) {
        const glm::vec3 segCenter = pos + up * halfH;   // recompute as we move
        RaycastHit hit = mesh.sweepCapsule(segCenter, dir, remaining + 1e-3f, r, hh);

        if (!hit.hit) {
            pos += dir * remaining;
            achieved += remaining;
            break;
        }

        // A contact is only "walkable" (the capsule may roll over it) when the
        // surface is at step-reachable height above the reference feet. Without
        // this, wall tops and slanted walls produce upward-facing normals and
        // the player could walk straight through them. The gate must NOT depend
        // on isGrounded either: the flat floor under an airborne capsule would
        // otherwise register as an overlapping side-contact and freeze the
        // player's horizontal movement during every jump.
        bool walkable = hit.normal.y > slopeLimit &&
                        hit.surfacePoint.y <= stepBaseY + maxStepHeight + 0.05f;
        if (walkable) {
            // Only ever climb a walkable contact the capsule can genuinely rest
            // ON: a tread or ramp toe has its surface under the capsule bottom.
            // Wall-base chamfers/kerbs meet the capsule on the side (contact
            // well above the foot) - climbing them drags the capsule straight
            // through the wall behind them, so those must block instead.
            bool mountable = hit.surfacePoint.y <= pos.y + r + 0.05f;
            // Mounting is a WALKING behaviour (step-up / ramp riding). While
            // ASCENDING it must never auto-climb: an in-air contact with a ledge
            // edge would otherwise teleport the player up onto the ledge in one
            // frame (rise is capped at maxStepHeight+0.05 measured from the
            // raised feet, so even a tall wall pops you onto its top). Landing
            // is handled by the vertical sweep in stepVertical instead. After a
            // grounded step-up the centre-ray glue briefly reports "off ground"
            // (the capsule is still over the lower floor behind the riser), so
            // level/descending contacts may still mount - which is also the
            // natural ledge latch when drifting down alongside a step.
            bool canMount = grounded || velocity.y <= 0.0f;
            if (mountable && canMount) {
                // Rise to the surface level and re-sweep the remaining path
                // from there. Mount only when no surface stands in front of
                // the raised capsule - a wall sitting right behind a step or
                // lip must still stop us.
                float rise = hit.surfacePoint.y - pos.y;
                rise = glm::clamp(rise, 0.0f, maxStepHeight + 0.05f);
                glm::vec3 raised = pos + up * (rise + 0.02f);
                RaycastHit raisedHit = mesh.sweepCapsule(raised + up * halfH,
                                                         dir, remaining + 1e-3f, r, hh);
                bool frontBlocked = raisedHit.hit &&
                                    glm::dot(raisedHit.surfacePoint - pos, dir) > 1e-4f;
                if (!frontBlocked) {
                    pos = raised;
                    pos += dir * remaining;
                    achieved += remaining;
                }
            } else {
                // A walkable surface (normal.y > slopeLimit) whose surface lies
                // above the capsule's foot is a wall-base chamfer/kerb, not a
                // ramp or tread. Block right here rather than attempting to
                // climb it, which would drag the capsule through the wall.
            }
            break;
        }

        float t = std::min(hit.distance, remaining);
        pos += dir * t;
        achieved += t;
        remaining -= t;
        if (remaining < 1e-4f) break;

        // Slide along the wall plane.
        glm::vec3 n = hit.normal;
        n.y = 0;
        float nLen = glm::length(n);
        if (nLen < 1e-3f) break;                 // blocked dead-on, no slide dir
        n /= nLen;
        glm::vec3 out = dir - n * glm::dot(dir, n);
        float oLen = glm::length(out);
        if (oLen < 1e-3f) break;
        dir = out / oLen;
    }
    return achieved;
}

void Player::stepVertical(float dt, const CollisionMesh& mesh) {
    const glm::vec3 up = {0, 1, 0};
    const float r = capsuleRadius;
    const float halfH = capsuleHeight * 0.5f;
    const float hh = halfH - r;
    const glm::vec3 segCenter = position + up * halfH;

    // --- Gravity ---
    if (!grounded) {
        velocity.y -= gravity * dt;
        velocity.y = std::max(velocity.y, -maxFallSpeed);
    } else if (velocity.y > 0.0f) {
        velocity.y = 0.0f;
    }

    float vertDist = velocity.y * dt;

    // --- On the ground: keep glued to the floor, or start falling if we've left it ---
    if (grounded && std::fabs(vertDist) < 1e-4f) {
        RaycastHit g = mesh.raycast(segCenter, {0, -1, 0}, halfH + groundKeepDistance);
        if (g.hit && g.distance <= halfH + 0.05f) {
            grounded = true;
            velocity.y = 0.0f;
            position.y = g.point.y;        // feet rest exactly on the floor
        } else {
            grounded = false;              // walked off a ledge, gravity takes over
        }
        return;
    }

    if (std::fabs(vertDist) < 1e-5f) {
        // Barely moving vertically (e.g. at the apex of a jump): simple check.
        RaycastHit g = mesh.raycast(position + up * (r + 0.02f), {0, -1, 0}, 0.6f);
        if (g.hit && g.distance < 0.1f) {
            grounded = true;
            velocity.y = 0.0f;
            position.y = g.point.y;
        }
        return;
    }

    // --- Move vertically with a full capsule sweep ---
    glm::vec3 dir = vertDist < 0 ? glm::vec3(0, -1, 0) : up;
    float sweep = std::fabs(vertDist);

    RaycastHit hit = mesh.sweepCapsule(segCenter, dir, sweep + 1e-3f, r, hh);
    float t = hit.hit ? std::min(hit.distance, sweep) : sweep;
    position += dir * t;

    if (hit.hit) {
        velocity.y = 0.0f;
        if (dir.y < 0) {
            grounded = true;
            // Snap flush to the exact floor under the new position. The ray
            // fires straight down from the capsule CENTRE, so at a platform
            // edge the centre can be past the ledge while the capsule still
            // overhangs it - a long ray would then find the lower floor BELOW
            // the ledge and teleport the feet down a full step. Only apply the
            // snap when the found floor is essentially at the feet already
            // (within a small slop); a floor far below belongs to a lower
            // platform the capsule hasn't actually landed on yet.
            float feetY = position.y;
            RaycastHit g = mesh.raycast(segCenter + dir * t, {0, -1, 0}, halfH + 0.5f);
            if (g.hit && g.point.y >= feetY - 0.1f) position.y = g.point.y;
        }
    } else if (dir.y < 0) {
        grounded = false;
    }
}