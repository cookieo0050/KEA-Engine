// ============================================================================
// ssgi.cpp - Screen-Space Global Illumination
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// Adds the "bounce light": colour that spills from one surface onto another
// (e.g. a red wall tints the floor beside it). Like SSAO it works in screen
// space by sampling the G-Buffer instead of tracing real rays. The output is a
// colour texture (indirect light per pixel) that the lighting pass adds on top
// of the direct sun/point lighting.
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - ssgiFragSrc mirrors SSAO's structure but outputs COLOUR:
//     for each of 24 samples around the pixel:
//       1. project the sample back to screen space (like SSAO)
//       2. look up the sampled surface's albedo (its colour)
//       3. attenuate by distance (1/(1+d^2))
//       4. weight by how "facing" the receiver and emitter are
//          (receiverTerm = dot(normal, toSample), emitterTerm = dot(sampledNormal,
//          -toSample)) - this is the actual GI math, cheapened for screen space.
// - init(): builds the 24-sample kernel + noise texture and two framebuffers,
//   exactly like SSAO, but textures are RGB16F (they store colour, not depth).
// - render()/blur(): same full-screen quad pattern as SSAO.
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - SSGI makes interiors feel warm and grounded - without it light looks flat.
// - It is view-dependent and only bounces light that is on screen, hence
//   "screen-space"; a full GI solution (radiance cascades, lightmaps) would be
//   far more expensive.
// - intensity (1.5) and radius (1.0) in the shader control how strong/far the
//   bounce reaches.
// ============================================================================
#include "ssgi.h"
#include <random>
#include <iostream>
using namespace std;

static float lerp(float a, float b, float f) { return a + f * (b - a); }

static const char* quadVertSrc = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUV;
out vec2 TexCoords;
void main() {
    TexCoords = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* ssgiFragSrc = R"(
#version 330 core
out vec3 FragColor;
in vec2 TexCoords;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedo;
uniform sampler2D texNoise;

uniform vec3 samples[24];
uniform mat4 view;
uniform mat4 projection;
uniform vec2 noiseScale;

const int kernelSize = 24;
const float radius = 1.0;
const float intensity = 1.5;

void main() {
    vec3 fragPosWorld = texture(gPosition, TexCoords).rgb;
    vec3 normalWorld = normalize(texture(gNormal, TexCoords).rgb);

    vec3 fragPos = vec3(view * vec4(fragPosWorld, 1.0));
    vec3 normal = normalize(mat3(view) * normalWorld);

    vec3 randomVec = normalize(texture(texNoise, TexCoords * noiseScale).xyz);
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    vec3 indirect = vec3(0.0);
    float sampleCount = 0.0;

    for (int i = 0; i < kernelSize; i++) {
        vec3 samplePos = TBN * samples[i];
        samplePos = fragPos + samplePos * radius;

        vec4 offset = vec4(samplePos, 1.0);
        offset = projection * offset;
        offset.xyz /= offset.w;
        offset.xyz = offset.xyz * 0.5 + 0.5;

        if (offset.x < 0.0 || offset.x > 1.0 || offset.y < 0.0 || offset.y > 1.0) continue;

        vec3 sampledPosWorld = texture(gPosition, offset.xy).rgb;
        vec3 sampledPosView = vec3(view * vec4(sampledPosWorld, 1.0));
        vec3 sampledNormalWorld = normalize(texture(gNormal, offset.xy).rgb);
        vec3 sampledAlbedo = texture(gAlbedo, offset.xy).rgb;

        float depthDiff = abs(fragPos.z - sampledPosView.z);
        if (depthDiff > radius) continue;

        vec3 toSample = normalize(sampledPosWorld - fragPosWorld);
        float receiverTerm = max(dot(normalWorld, toSample), 0.0);
        float emitterTerm = max(dot(sampledNormalWorld, -toSample), 0.0);

        float dist = length(sampledPosWorld - fragPosWorld);
        float atten = 1.0 / (1.0 + dist * dist);

        indirect += sampledAlbedo * receiverTerm * emitterTerm * atten;
        sampleCount += 1.0;
    }

    if (sampleCount > 0.0) indirect /= sampleCount;
    FragColor = indirect * intensity;
}
)";

static const char* blurFragSrc = R"(
#version 330 core
out vec3 FragColor;
in vec2 TexCoords;
uniform sampler2D ssgiInput;
uniform sampler2D gPosition;
uniform sampler2D gNormal;

// Cross-bilateral blur (same edge-stopping as SSAO) so indirect light doesn't
// bleed across corners or depth discontinuities - keeps the GI tint crisp.
void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(ssgiInput, 0));

    vec3 centerPos = texture(gPosition, TexCoords).rgb;
    vec3 centerNrm = normalize(texture(gNormal, TexCoords).rgb);

    vec3 total = texture(ssgiInput, TexCoords).rgb;
    float totalW = 1.0;

    for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y) {
            if (x == 0 && y == 0) continue;
            vec2 uv = TexCoords + vec2(float(x), float(y)) * texelSize;

            vec3 p = texture(gPosition, uv).rgb;
            vec3 n = normalize(texture(gNormal, uv).rgb);

            float spatial = exp(-float(x * x + y * y) * 0.6);
            float positionW = exp(-length(centerPos - p) * 25.0);
            float normalW = pow(max(dot(centerNrm, n), 0.0), 8.0);
            float w = spatial * positionW * normalW;

            total += texture(ssgiInput, uv).rgb * w;
            totalW += w;
        }
    FragColor = total / totalW;
}
)";

void SSGI::init(int width, int height) {
    m_width = width; m_height = height;
    m_shader = new Shader(quadVertSrc, ssgiFragSrc);
    m_blurShader = new Shader(quadVertSrc, blurFragSrc);

    uniform_real_distribution<float> randomFloats(0.0f, 1.0f);
    default_random_engine generator;

    for (int i = 0; i < 24; i++) {
        glm::vec3 sample(
            randomFloats(generator) * 2.0f - 1.0f,
            randomFloats(generator) * 2.0f - 1.0f,
            randomFloats(generator)
        );
        sample = glm::normalize(sample);
        sample *= randomFloats(generator);
        float scale = (float)i / 24.0f;
        scale = lerp(0.1f, 1.0f, scale * scale);
        sample *= scale;
        m_kernel.push_back(sample);
    }

    vector<glm::vec3> noise;
    for (int i = 0; i < 16; i++) {
        noise.push_back(glm::vec3(
            randomFloats(generator) * 2.0f - 1.0f,
            randomFloats(generator) * 2.0f - 1.0f,
            0.0f
        ));
    }
    glGenTextures(1, &m_noiseTex);
    glBindTexture(GL_TEXTURE_2D, m_noiseTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 4, 4, 0, GL_RGB, GL_FLOAT, noise.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        cerr << "SSGI FBO incomplete\n";

    glGenFramebuffers(1, &m_blurFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_blurFBO);
    glGenTextures(1, &m_blurTex);
    glBindTexture(GL_TEXTURE_2D, m_blurTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_blurTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        cerr << "SSGI blur FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SSGI::render(GLuint gPositionTex, GLuint gNormalTex, GLuint gAlbedoTex,
    const glm::mat4& view, const glm::mat4& projection, GLuint quadVAO) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_width, m_height);
    glClear(GL_COLOR_BUFFER_BIT);

    m_shader->use();
    for (int i = 0; i < 24; i++)
        m_shader->setVec3("samples[" + to_string(i) + "]", m_kernel[i]);
    m_shader->setMat4("view", view);
    m_shader->setMat4("projection", projection);
    m_shader->setVec2("noiseScale", glm::vec2(m_width / 4.0f, m_height / 4.0f));
    m_shader->setInt("gPosition", 0);
    m_shader->setInt("gNormal", 1);
    m_shader->setInt("gAlbedo", 2);
    m_shader->setInt("texNoise", 3);

    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gPositionTex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gNormalTex);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, gAlbedoTex);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_noiseTex);

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void SSGI::blur(GLuint quadVAO, GLuint gPositionTex, GLuint gNormalTex) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_blurFBO);
    glViewport(0, 0, m_width, m_height);
    glClear(GL_COLOR_BUFFER_BIT);

    m_blurShader->use();
    m_blurShader->setInt("ssgiInput", 0);
    m_blurShader->setInt("gPosition", 1);
    m_blurShader->setInt("gNormal", 2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gPositionTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, gNormalTex);

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}