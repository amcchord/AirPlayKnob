#pragma once
#include <stdint.h>

namespace AudioBuffer {
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr uint32_t FRAME_BYTES = 4;
constexpr uint32_t PCM_BYTES = 512 * 1024;
constexpr uint32_t PCM_KIB = PCM_BYTES / 1024;
constexpr uint32_t PCM_FRAMES = PCM_BYTES / FRAME_BYTES;
constexpr uint32_t PCM_USABLE_MS = ((uint64_t)(PCM_FRAMES - 1) * 1000) / SAMPLE_RATE;
constexpr uint32_t MP3_BYTES = 256 * 1024;

constexpr uint32_t CONNECT_PCM_MS = 1000;
constexpr uint32_t CONNECT_MIN_PCM_MS = 500;
constexpr uint32_t CONNECT_MP3_BYTES = 48 * 1024;
constexpr uint32_t START_PCM_MS = 1000;
constexpr uint32_t START_UNBUFFERED_PCM_MS = 1500;
constexpr uint32_t START_MP3_BYTES = 16 * 1024;
constexpr uint32_t ARTWORK_START_PCM_MS = 2000;
constexpr uint32_t ARTWORK_ABORT_PCM_MS = 1000;
constexpr uint32_t ARTWORK_START_MP3_BYTES = 32 * 1024;

constexpr uint32_t frames_for_ms(uint32_t ms) {
    return ((uint64_t)SAMPLE_RATE * ms + 999) / 1000;
}
constexpr uint32_t playback_start_frames(uint32_t mp3_bytes) {
    return frames_for_ms(mp3_bytes >= START_MP3_BYTES ? START_PCM_MS : START_UNBUFFERED_PCM_MS);
}
constexpr bool ready_to_connect(uint32_t frames, uint32_t mp3_bytes, uint32_t wifi_age_ms) {
    return frames >= frames_for_ms(CONNECT_PCM_MS) ||
           (frames >= frames_for_ms(CONNECT_MIN_PCM_MS) &&
            (mp3_bytes >= CONNECT_MP3_BYTES || wifi_age_ms >= 7500));
}
static_assert(PCM_FRAMES && !(PCM_FRAMES & (PCM_FRAMES - 1)), "PCM ring must be a power of two");
static_assert(frames_for_ms(ARTWORK_START_PCM_MS) < PCM_FRAMES, "Artwork threshold must fit PCM ring");
static_assert(frames_for_ms(START_UNBUFFERED_PCM_MS) < PCM_FRAMES, "Playback threshold must fit PCM ring");
}
