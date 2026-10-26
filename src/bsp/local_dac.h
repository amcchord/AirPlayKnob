#ifndef LOCAL_DAC_H
#define LOCAL_DAC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void local_dac_init(void);
void local_dac_write_frames(const int16_t *stereo_pcm, size_t frames, int volume_pct);

#ifdef __cplusplus
}
#endif

#endif
