// ============================================================================
// ssao.cpp - Screen-Space Ambient Occlusion
// ============================================================================
//
// WHAT THIS FILE IS
// ----------------------------------------------------------------------------
// Makes corners, cracks and crevices darker where they are hard for light to
// reach. It is a screen-space trick: instead of tracing rays through the 3D
// world, it reads the G-Buffer and samples nearby pixels to estimate how
// "blocked in" each point is. The result is a grayscale texture (0 = fully
// occluded/dark, 1 = fully open/bright).
//
// HOW TO UNDERSTAND IT
// ----------------------------------------------------------------------------
// - The GLSL ssaoFragSrc is the whole algorithm in ~25 lines. Read it line by
//   line: for each pixel, take the position/normal from the G-Buffer, then for
//   each of the 32 kernel samples compare depths. If the sampled point is closer
//   to the camera than our sample (and within range), it counts as occlusion.
// - The 32 samples (m_kernel) are generated on the CPU in init() with random
//   values, then biased to be denser near the surface (lerp(0.1,1.0,scale^2)).
// - The tiny 4x4 noise texture (m_noiseTex) rotates the kernel per pixel so the
//   banding of 32 samples doesn't look like repeating patterns.
// - renderSSAO(): draws a full-screen quad with the G-Buffer + noise bound,
//   outputting the occlusion value into m_ssaoTex. blur() then smooths it with
//   a simple 4x4 box blur to hide noise.
//
// KEY IDEAS
// ----------------------------------------------------------------------------
// - SSAO is the cheapest form of ambient occlusion - it only looks at the 2D
//   screen, so it can be wrong at edges, but it runs in real time.
// - radius (0.5) and the kernel size (32) in the shader are the quality knobs.
// - The final lighting pass (main.cpp) multiplies the sun and point-light terms
//   by this AO value.
// ============================================================================
#include "ssao.h"
#include "renderpipeline.h"
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

static const char* ssaoFragSrc = R"(
#version 330 core
out float FragColor;
in vec2 TexCoords;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D texNoise;

uniform vec3 samples[16];
uniform mat4 view;
uniform mat4 projection;
uniform vec2 noiseScale;
uniform float radius;
uniform float strength;

const int kernelSize = 16;
const float biasAmt = 0.025;

void main() {
    vec3 fragPosWorld = texture(gPosition, TexCoords).rgb;
    vec3 normalWorld = normalize(texture(gNormal, TexCoords).rgb);

    vec3 fragPos = vec3(view * vec4(fragPosWorld, 1.0));
    vec3 normal = normalize(mat3(view) * normalWorld);

    vec3 randomVec = normalize(texture(texNoise, TexCoords * noiseScale).xyz);
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < kernelSize; i++) {
        vec3 samplePos = TBN * samples[i];
        samplePos = fragPos + samplePos * radius;

        vec4 offset = vec4(samplePos, 1.0);
        offset = projection * offset;
        offset.xyz /= offset.w;
        offset.xyz = offset.xyz * 0.5 + 0.5;

        vec3 sampledPosWorld = texture(gPosition, offset.xy).rgb;
        vec3 sampledPosView = vec3(view * vec4(sampledPosWorld, 1.0));

        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(fragPos.z - sampledPosView.z));
        occlusion += (sampledPosView.z >= samplePos.z + biasAmt ? 1.0 : 0.0) * rangeCheck;
    }
    occlusion = 1.0 - strength * (occlusion / float(kernelSize));
    FragColor = clamp(occlusion, 0.0, 1.0);
}
)";

static const char* blurFragSrc = R"(
#version 330 core
out float FragColor;
in vec2 TexCoords;
uniform sampler2D ssaoInput;
uniform sampler2D gPosition;
uniform sampler2D gNormal;

// Cross-bilateral blur: a normal gaussian weighted by how much the surrounding
// pixels belong to the same surface. Position distance and the normal dot
// product act as edge stops, so corners/edges keep their own occlusion instead
// of smearing the neighbour's AO into them.
void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(ssaoInput, 0));

    vec3 centerPos = texture(gPosition, TexCoords).rgb;
    vec3 centerNrm = normalize(texture(gNormal, TexCoords).rgb);

    float total = texture(ssaoInput, TexCoords).r;
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

            total += texture(ssaoInput, uv).r * w;
            totalW += w;
        }
    FragColor = total / totalW;
}
)";

void SSAO::init(int width, int height) {
    m_width = width; m_height = height;
    m_ssaoShader = new Shader(quadVertSrc, ssaoFragSrc);
    m_blurShader = new Shader(quadVertSrc, blurFragSrc);

    uniform_real_distribution<float> randomFloats(0.0f, 1.0f);
    default_random_engine generator;

    for (int i = 0; i < 16; i++) {
        glm::vec3 sample(
            randomFloats(generator) * 2.0f - 1.0f,
            randomFloats(generator) * 2.0f - 1.0f,
            randomFloats(generator)
        );
        sample = glm::normalize(sample);
        sample *= randomFloats(generator);
        float scale = (float)i / 16.0f;
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

    glGenFramebuffers(1, &m_ssaoFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
    glGenTextures(1, &m_ssaoTex);
    glBindTexture(GL_TEXTURE_2D, m_ssaoTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssaoTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        cerr << "SSAO FBO incomplete\n";

    glGenFramebuffers(1, &m_ssaoBlurFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
    glGenTextures(1, &m_ssaoBlurTex);
    glBindTexture(GL_TEXTURE_2D, m_ssaoBlurTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssaoBlurTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        cerr << "SSAO blur FBO incomplete\n";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SSAO::renderSSAO(GLuint gPositionTex, GLuint gNormalTex,
    const glm::mat4& view, const glm::mat4& projection, GLuint quadVAO) {
    if (!RenderPipeline::ssaoEnabled()) {
        // Disabled: leave a flat white result so the lighting pass multiplies
        // by 1.0 (no artificial occlusion) instead of stale data.
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
        glViewport(0, 0, m_width, m_height);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
    glViewport(0, 0, m_width, m_height);
    glClear(GL_COLOR_BUFFER_BIT);

    m_ssaoShader->use();
    for (int i = 0; i < 16; i++)
        m_ssaoShader->setVec3("samples[" + to_string(i) + "]", m_kernel[i]);
    m_ssaoShader->setMat4("view", view);
    m_ssaoShader->setMat4("projection", projection);
    m_ssaoShader->setVec2("noiseScale", glm::vec2(m_width / 4.0f, m_height / 4.0f));
    m_ssaoShader->setFloat("radius", RenderPipeline::ssaoRadius());
    m_ssaoShader->setFloat("strength", RenderPipeline::ssaoStrength());
    m_ssaoShader->setInt("gPosition", 0);
    m_ssaoShader->setInt("gNormal", 1);
    m_ssaoShader->setInt("texNoise", 2);

    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gPositionTex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gNormalTex);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_noiseTex);

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void SSAO::blur(GLuint quadVAO, GLuint gPositionTex, GLuint gNormalTex) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
    glViewport(0, 0, m_width, m_height);
    glClear(GL_COLOR_BUFFER_BIT);

    m_blurShader->use();
    m_blurShader->setInt("ssaoInput", 0);
    m_blurShader->setInt("gPosition", 1);
    m_blurShader->setInt("gNormal", 2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssaoTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gPositionTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, gNormalTex);

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}