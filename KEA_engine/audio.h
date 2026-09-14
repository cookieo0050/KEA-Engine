#pragma once
#include <glm/glm.hpp>
#include <string>
#include <cstdint>

// ============================================================================
// audio.h - Thin wrapper over miniaudio for the KEA engine
// ============================================================================
//
// WHAT THIS IS
// ----------------------------------------------------------------------------
// Provides device init/shutdown, fire-and-forget and positional 3D sounds, and
// per-frame listener updates so the game loop just calls audioUpdate() once.
// Internally it wraps miniaudio's high-level engine + sound API.
//
// USAGE
// ----------------------------------------------------------------------------
//   audioInit();                             // once at startup
//   audioUpdate(pos, front, up);             // once per frame
//   AudioHandle h = play("sfx/boom.wav");    // 2D, non-positional
//   AudioHandle s = play3D("sfx/boom.wav", worldPos); // 3D positional
//   stopSound(h);
//   audioShutdown();                         // at exit
//
// SOUND FILES
// ----------------------------------------------------------------------------
// Paths passed to play()/play3D() should be absolute or resolved by the caller.
// The caller (main.cpp) prepends GameRoot/audio/ for FGD-driven entities.
// miniaudio decodes WAV, FLAC, MP3, OGG (via dr_flac/stb_vorbis embedded).
// ============================================================================

using AudioHandle = uint32_t;
static const AudioHandle AUDIO_INVALID = 0;

// Initialize the audio device and engine. Call once at startup.
bool audioInit();

// Shut down the audio device and free all loaded sounds.
void audioShutdown();

// Update the 3D listener position/orientation once per frame.
void audioUpdate(const glm::vec3& listenerPos,
                 const glm::vec3& listenerFront,
                 const glm::vec3& listenerUp);

// Play a 2D (non-positional) sound. Returns a handle to stop/volume-change.
AudioHandle play(const std::string& path, float volume = 1.0f, bool loop = false);

// Play a positional 3D sound at the given world position.
AudioHandle play3D(const std::string& path, const glm::vec3& position,
                   float volume = 1.0f, bool loop = false);

// Stop a playing sound (also frees its slot).
void stopSound(AudioHandle h);

// Set volume (0..1) on a sound handle.
void setVolume(AudioHandle h, float vol);

// Move a positional sound.
void setSoundPosition(AudioHandle h, const glm::vec3& pos);

// Enable/disable looping on a sound.
void setLooping(AudioHandle h, bool loop);

// True if the handle refers to an active sound slot.
bool isPlaying(AudioHandle h);