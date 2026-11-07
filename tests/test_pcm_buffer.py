"""Exercise the actual PCM ring at 512 KiB across eight simulated minutes."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / 'src/audio/radio_streamer.cpp').read_text()
ring = source[source.index('static uint32_t ring_available_frames()'):source.index('static void update_vu_meters(')]
fixture = r'''
#include "audio_buffer_config.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <algorithm>
constexpr uint32_t PCM_RING_FRAMES = AudioBuffer::PCM_FRAMES;
constexpr uint32_t RAOP_CHUNK_FRAMES = 352;
static int16_t *s_pcm_ring = new int16_t[PCM_RING_FRAMES*2];
static volatile uint32_t s_ring_write=0,s_ring_read=0,s_ring_seq=0;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
'''
cases = r'''
static int16_t sample(uint32_t frame, uint32_t ch) { return ch ? (int16_t)((frame >> 16) ^ (frame * 31337U)) : (int16_t)(frame * 17U + 101U); }
int main() {
    int16_t input[1152*2], output[352*2];
    uint32_t written=0,read=0;
    // Independent sequence oracle, MP3-sized pushes and RTP-sized pops.
    // Repeatedly fill/drain so both read and write cross the ring boundary.
    while (read < 44100U*8*60) {
        uint32_t n=std::min<uint32_t>(1152,ring_free_frames());
        for(uint32_t i=0;i<n;i++)for(uint32_t ch=0;ch<2;ch++)input[2*i+ch]=sample(written+i,ch);
        ring_push_frames(input,n);written+=n;
        if(ring_available_frames()>PCM_RING_FRAMES/2 || n==0) {
            for(int j=0;j<11 && ring_available_frames()>=352;j++) {
                assert(ring_pop_352(output));
                for(uint32_t i=0;i<352;i++)for(uint32_t ch=0;ch<2;ch++)assert(output[2*i+ch]==sample(read+i,ch));
                read+=352;
            }
        }
        assert(ring_available_frames()==written-read);
        assert(ring_available_frames()<PCM_RING_FRAMES);
    }
    ring_clear();assert(ring_available_frames()==0 && !ring_pop_352(output));
    // Saturation truncates at capacity-1 and never overwrites unread data.
    written=read=0;
    while(ring_free_frames()) {
        uint32_t n=std::min<uint32_t>(1152,ring_free_frames());
        for(uint32_t i=0;i<n;i++)for(uint32_t ch=0;ch<2;ch++)input[2*i+ch]=sample(written+i,ch);
        ring_push_frames(input,n);written+=n;
    }
    assert(written==PCM_RING_FRAMES-1);ring_push_frames(input,1152);
    assert(ring_available_frames()==PCM_RING_FRAMES-1);
    while(ring_pop_352(output)) {
        for(uint32_t i=0;i<352;i++)for(uint32_t ch=0;ch<2;ch++)assert(output[2*i+ch]==sample(read+i,ch));
        read+=352;
    }
    assert(ring_available_frames()<352);
    ring_clear();delete[] s_pcm_ring;
    puts("PASS 512-KiB PCM ring: eight simulated minutes, wraparound, stereo integrity, saturation and clear.");
}
'''
(ROOT / 'scratch').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='pcm-ring-test-', dir=ROOT / 'scratch') as directory:
    temp = pathlib.Path(directory)
    (temp / 'test.cpp').write_text(fixture + ring + cases)
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-I', str(ROOT / 'include'),
                    str(temp / 'test.cpp'), '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)
