// ============================================================================
// gbuffer.cpp - The Deferred Rendering G-Buffer
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// Creates an off-screen framebuffer with THREE colour textures (plus a depth
// renderbuffer). During the "geometry pass" the map is rendered into these
// textures instead of the screen:
//     m_gPosition    (RGBA16F) - world position of each surface point
//     m_gNormal      (RGBA16F) - surface normal at each point
//     m_gAlbedoSpec  (RGBA8)   - the flat texture colour (albedo)
// Together these three images are called the "G-Buffer" (Geometry Buffer).
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - init(): the whole file, really. For each texture it does:
//     glGenTextures -> glTexImage2D (allocate GPU memory) -> attach to FBO.
//   Then it tells OpenGL which attachments to write with glDrawBuffers(3, ...)
//   and attaches a depth renderbuffer so geometry also records depth.
// - bindForWriting(): binds the FBO and sets the viewport - call this before
//   the geometry pass so draws go into the G-Buffer instead of the screen.
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - 16F textures hold floats (positions/normals need precision); 8-bit is fine
//   for plain colours.
// - NEAREST filtering: G-Buffer values are samples, not pictures - no filtering.
// - "Deferred" means: we defer (postpone) lighting until later. main.cpp pass #6
//   reads these three textures + shadows + AO + GI to compute final lighting.
// - Viewing these textures directly = the F1 debug panel's Position/Normal/Albedo
//   modes. They are the engine's raw "memory" of the scene.
// ============================================================================
#include "gbuffer.h"
#include <iostream>
using namespace std;

void GBuffer::init(int width, int height) {
    // Free any previous allocations so init() can be called again on resize.
    if (m_fbo != 0) {
        glDeleteFramebuffers(1, &m_fbo);
        glDeleteTextures(1, &m_gPosition);
        glDeleteTextures(1, &m_gNormal);
        glDeleteTextures(1, &m_gAlbedoSpec);
        glDeleteRenderbuffers(1, &m_rbo);
        m_fbo = m_gPosition = m_gNormal = m_gAlbedoSpec = m_rbo = 0;
    }

    m_width = width; m_height = height;

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    glGenTextures(1, &m_gPosition);
    glBindTexture(GL_TEXTURE_2D, m_gPosition);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gPosition, 0);

    glGenTextures(1, &m_gNormal);
    glBindTexture(GL_TEXTURE_2D, m_gNormal);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gNormal, 0);

    glGenTextures(1, &m_gAlbedoSpec);
    glBindTexture(GL_TEXTURE_2D, m_gAlbedoSpec);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_gAlbedoSpec, 0);

    GLuint attachments[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
    glDrawBuffers(3, attachments);

    glGenRenderbuffers(1, &m_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        cerr << "GBuffer framebuffer incomplete!\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GBuffer::bindForWriting() {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_width, m_height);
}