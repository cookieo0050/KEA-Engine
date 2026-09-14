// ============================================================================
// audio.cpp - miniaudio-based audio engine wrapper
// ============================================================================
//
// MINIAUDIO IMPLEMENTATION
// This is the ONE translation unit that defines MINIAUDIO_IMPLEMENTATION so
// the library's implementation is compiled into the static lib. All other
// files just include "miniaudio.h" for the declarations (if they need them).
//
// SOUND SLOT TABLE
// We keep a fixed-size array of SoundSlot entries. Each slot holds an
// ma_sound object plus a boolean tracking whether the slot is occupied.
// AudioHandle values are 1-based indices into this table (0 = invalid).
// When a sound ends naturally (e.g. !loop and playback finishes), the slot
// is NOT automatically freed — it stays valid until stopSound() is called.
// This lets callers still query volume/position for recently-finished sounds.
//
// THREAD SAFETY
// miniaudio's high-level engine is designed to be called from one thread. The
// audioUpdate/play/stop functions are all invoked from the main thread inside
// the game loop, so no locking is needed.
//
// WHAT WE DON'T DO (yet)
// - No streaming (MA_SOUND_FLAG_STREAM) for long music tracks. Every sound is
//   fully decoded into memory on load. Fine for short SFX; revisit when music
//   is added.
// - No per-sound attenuation rolloff curves. We use miniaudio's defaults.
// ============================================================================
#include "audio.h"
#include <iostream>
#include <cstring>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

using namespace std;

namespace {

ma_engine g_engine{};
bool g_engineReady = false;

static const int MAX_SOUNDS = 32;

struct SoundSlot {
    ma_sound sound{};
    bool inUse = false;
    bool loaded = false;   // true after ma_sound_init_from_file succeeded
};

SoundSlot g_sounds[MAX_SOUNDS]{};

// Convert a handle to a 0-based index; return -1 on bad handle.
int slotIndex(AudioHandle h) {
    if (h == AUDIO_INVALID || h > (AudioHandle)MAX_SOUNDS) return -1;
    return (int)h - 1;
}

} // namespace

bool audioInit() {
    if (g_engineReady) return true;

    ma_engine_config cfg = ma_engine_config_init();
    // Use sensible defaults: system device, stereo output, no special flags.
    if (ma_engine_init(&cfg, &g_engine) != MA_SUCCESS) {
        cerr << "Audio: failed to initialize miniaudio engine\n";
        return false;
    }
    g_engineReady = true;

    // Zero-init all sound slots (ma_sound must be zero before init).
    memset(g_sounds, 0, sizeof(g_sounds));
    return true;
}

void audioShutdown() {
    if (!g_engineReady) return;

    // Stop and uninit every active sound before tearing down the engine.
    for (int i = 0; i < MAX_SOUNDS; ++i) {
        if (g_sounds[i].loaded) {
            ma_sound_uninit(&g_sounds[i].sound);
        }
    }
    memset(g_sounds, 0, sizeof(g_sounds));

    ma_engine_uninit(&g_engine);
    g_engineReady = false;
}

void audioUpdate(const glm::vec3& listenerPos,
                 const glm::vec3& listenerFront,
                 const glm::vec3& listenerUp) {
    if (!g_engineReady) return;

    ma_engine_listener_set_position(&g_engine, 0,
        listenerPos.x, listenerPos.y, listenerPos.z);
    ma_engine_listener_set_direction(&g_engine, 0,
        listenerFront.x, listenerFront.y, listenerFront.z);
    ma_engine_listener_set_world_up(&g_engine, 0,
        listenerUp.x, listenerUp.y, listenerUp.z);
}

AudioHandle play(const string& path, float volume, bool loop) {
    if (!g_engineReady) return AUDIO_INVALID;

    // Find a free slot.
    int idx = -1;
    for (int i = 0; i < MAX_SOUNDS; ++i) {
        if (!g_sounds[i].inUse) { idx = i; break; }
    }
    if (idx < 0) {
        cerr << "Audio: no free sound slots (max " << MAX_SOUNDS << ")\n";
        return AUDIO_INVALID;
    }

    SoundSlot& slot = g_sounds[idx];
    memset(&slot.sound, 0, sizeof(ma_sound));

    ma_uint32 flags = 0;
    if (loop) flags |= MA_SOUND_FLAG_LOOPING;

    if (ma_sound_init_from_file(&g_engine, path.c_str(), flags, NULL, NULL, &slot.sound) != MA_SUCCESS) {
        cerr << "Audio: failed to load '" << path << "'\n";
        return AUDIO_INVALID;
    }

    ma_sound_set_volume(&slot.sound, volume);
    slot.inUse = true;
    slot.loaded = true;
    ma_sound_start(&slot.sound);

    return (AudioHandle)(idx + 1);
}

AudioHandle play3D(const string& path, const glm::vec3& position, float volume, bool loop) {
    AudioHandle h = play(path, volume, loop);
    if (h != AUDIO_INVALID) {
        int idx = slotIndex(h);
        ma_sound_set_position(&g_sounds[idx].sound,
            position.x, position.y, position.z);
        ma_sound_set_spatialization_enabled(&g_sounds[idx].sound, MA_TRUE);
    }
    return h;
}

void stopSound(AudioHandle h) {
    int idx = slotIndex(h);
    if (idx < 0 || !g_sounds[idx].inUse) return;

    if (g_sounds[idx].loaded) {
        ma_sound_stop(&g_sounds[idx].sound);
        ma_sound_uninit(&g_sounds[idx].sound);
    }
    g_sounds[idx] = SoundSlot{};
}

void setVolume(AudioHandle h, float vol) {
    int idx = slotIndex(h);
    if (idx < 0 || !g_sounds[idx].inUse || !g_sounds[idx].loaded) return;
    ma_sound_set_volume(&g_sounds[idx].sound, vol);
}

void setSoundPosition(AudioHandle h, const glm::vec3& pos) {
    int idx = slotIndex(h);
    if (idx < 0 || !g_sounds[idx].inUse || !g_sounds[idx].loaded) return;
    ma_sound_set_position(&g_sounds[idx].sound, pos.x, pos.y, pos.z);
}

void setLooping(AudioHandle h, bool loop) {
    int idx = slotIndex(h);
    if (idx < 0 || !g_sounds[idx].inUse || !g_sounds[idx].loaded) return;
    ma_sound_set_looping(&g_sounds[idx].sound, loop ? MA_TRUE : MA_FALSE);
}

bool isPlaying(AudioHandle h) {
    int idx = slotIndex(h);
    if (idx < 0 || !g_sounds[idx].inUse || !g_sounds[idx].loaded) return false;
    return ma_sound_is_playing(&g_sounds[idx].sound) == MA_TRUE;
}