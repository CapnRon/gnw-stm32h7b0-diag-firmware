/* Auto-generated: sound asset offset table into external flash blob. Mono 16-bit PCM @ 48kHz, <=2.5s each. */
#pragma once
#include <stdint.h>

#define SOUND_COUNT 7

static const uint32_t sound_offsets[SOUND_COUNT] = {
  0u,
  181676u,
  367808u,
  551712u,
  692148u,
  860448u,
  1013960u
};

static const uint32_t sound_lengths[SOUND_COUNT] = {
  181674u,
  186132u,
  183904u,
  140436u,
  168300u,
  153510u,
  85444u
};

/* True-color photo overlay, 140x140 RGB565, magenta (0xF81F) = transparent */
#define IMAGE_OFFSET 1099404u
#define IMAGE_WIDTH  140
#define IMAGE_HEIGHT 140
#define IMAGE_TRANSPARENT_COLOR 0xF81Fu
