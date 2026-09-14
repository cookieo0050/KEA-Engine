// ============================================================================
// renderpipeline.h - Centralized post-processing / pipeline render config
// ============================================================================
//
// The single place render settings live so UI code never talks to the GPU
// directly. The editor's "Post-Processing & Pipeline Settings" panel edits a
// PostProcessState (its active scene state) and pushes the values through the
// RenderPipeline setters; the render loop then uploads them to the post-pass
// uniforms every frame. Nothing here triggers a level rebuild or a shader
// recompile - the passes re-read the config each frame.
//
// Compiled into BOTH the editor and the runtime (shared source), so the game
// can later consume the same values that were authored in the editor.
// ============================================================================
#pragma once

// Editor scene-state copy of the post-processing settings. The editor holds one
// of these as part of its active scene state so Tier 3 file saves can write the
// values into the worldspawn metadata. 'dirty' is raised whenever a control
// changes so the editor knows a save / worldspawn update is pending.
struct PostProcessState {
    float ssaoStrength = 1.0f;   // 0..5     - multiplies the SSAO occlusion term
    float ssaoRadius = 0.5f;     // 0.01..2  - view-space AO sample radius (metres)
    float exposureEV = 0.0f;     // -5..5    - HDR exposure: colour *= 2^EV
    bool ssaoEnabled = true;
    bool dirty = false;          // modified since last save
};

// Centralized render configuration. The UI calls these setters; the render
// loop reads the getters to update the GPU pass constants every frame.
class RenderPipeline {
public:
    static void SetSSAOParams(float strength, float radius);
    static void SetExposure(float ev);
    static void SetSSAOEnabled(bool enabled);

    // One-shot copy of an entire scene-state record into the config (keeps the
    // renderer in lock-step with the editor's serializable state).
    static void SyncFrom(const PostProcessState& state);

    static float ssaoStrength();
    static float ssaoRadius();
    static float exposureEV();
    static bool ssaoEnabled();

private:
    static float s_ssaoStrength;
    static float s_ssaoRadius;
    static float s_exposureEV;
    static bool s_ssaoEnabled;
};
