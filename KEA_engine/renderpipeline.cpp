// ============================================================================
// renderpipeline.cpp - Shared post-processing / pipeline render config state
// ============================================================================
#include "renderpipeline.h"

float RenderPipeline::s_ssaoStrength = 1.0f;
float RenderPipeline::s_ssaoRadius = 0.5f;
float RenderPipeline::s_exposureEV = 0.0f;
bool RenderPipeline::s_ssaoEnabled = true;

void RenderPipeline::SetSSAOParams(float strength, float radius) {
    s_ssaoStrength = strength;
    s_ssaoRadius = radius;
}

void RenderPipeline::SetExposure(float ev) {
    s_exposureEV = ev;
}

void RenderPipeline::SetSSAOEnabled(bool enabled) {
    s_ssaoEnabled = enabled;
}

void RenderPipeline::SyncFrom(const PostProcessState& state) {
    s_ssaoStrength = state.ssaoStrength;
    s_ssaoRadius = state.ssaoRadius;
    s_exposureEV = state.exposureEV;
    s_ssaoEnabled = state.ssaoEnabled;
}

float RenderPipeline::ssaoStrength() { return s_ssaoStrength; }
float RenderPipeline::ssaoRadius() { return s_ssaoRadius; }
float RenderPipeline::exposureEV() { return s_exposureEV; }
bool RenderPipeline::ssaoEnabled() { return s_ssaoEnabled; }
