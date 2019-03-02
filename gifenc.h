#ifndef GIFENC_H
#define GIFENC_H

#include <stdint.h>

typedef struct ge_GIF {
    uint16_t w, h;
    int depth;
    int fd;
    int offset;
    int nframes;
    uint8_t *palette, *frame, *back;
    uint32_t partial;
    uint8_t buffer[0xFF];
} ge_GIF;

ge_GIF *ge_new_gif(
    const char *fname, uint16_t width, uint16_t height,
    uint8_t *palette, int depth, int loop
);
void ge_rgba_frame_to_indexed(uint8_t *palette, uint8_t depth, uint8_t *indexed_pixels, uint8_t *rgba_pixels, int rgba_pixels_len, uint8_t pixel_width);
void ge_populate_frame(ge_GIF *gif, uint8_t *indexed_pixels, int indexed_pixels_len);
void ge_add_frame(ge_GIF *gif, uint16_t delay);
void ge_close_gif(ge_GIF* gif);

#endif /* GIFENC_H */
