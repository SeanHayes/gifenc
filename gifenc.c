#include "gifenc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/* helper to write a little-endian 16-bit number portably */
#define write_num(fd, n) write((fd), (uint8_t []) {(n) & 0xFF, (n) >> 8}, 2)

static uint8_t vga[0x30] = {
    0x00, 0x00, 0x00,
    0xAA, 0x00, 0x00,
    0x00, 0xAA, 0x00,
    0xAA, 0x55, 0x00,
    0x00, 0x00, 0xAA,
    0xAA, 0x00, 0xAA,
    0x00, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA,
    0x55, 0x55, 0x55,
    0xFF, 0x55, 0x55,
    0x55, 0xFF, 0x55,
    0xFF, 0xFF, 0x55,
    0x55, 0x55, 0xFF,
    0xFF, 0x55, 0xFF,
    0x55, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
};

struct Node {
    uint16_t key;
    struct Node *children[];
};
typedef struct Node Node;

static Node *
new_node(uint16_t key, int degree)
{
    Node *node = calloc(1, sizeof(*node) + degree * sizeof(Node *));
    if (node)
        node->key = key;
    return node;
}

static Node *
new_trie(int degree, int *nkeys)
{
    Node *root = new_node(0, degree);
    /* Create nodes for single pixels. */
    for (*nkeys = 0; *nkeys < degree; (*nkeys)++)
        root->children[*nkeys] = new_node(*nkeys, degree);
    *nkeys += 2; /* skip clear code and stop code */
    return root;
}

static void
del_trie(Node *root, int degree)
{
    if (!root)
        return;
    for (int i = 0; i < degree; i++)
        del_trie(root->children[i], degree);
    free(root);
}

static void put_loop(ge_GIF *gif, uint16_t loop);

uint8_t * ge_make_palette(int depth)
{
    uint8_t *palette = malloc(3 << depth);
    if (depth <= 4) {
        memcpy(palette, vga, 3 << depth);
    } else {
        size_t idx = sizeof(vga);
        memcpy(palette, vga, idx);
        int i;
        uint8_t r, g, b, v;
        i = 0x10;
        for (r = 0; r < 6; r++) {
            for (g = 0; g < 6; g++) {
                for (b = 0; b < 6; b++) {
                    palette[idx] = r*51;
                    idx++;
                    palette[idx] = g*51;
                    idx++;
                    palette[idx] = b*51;
                    idx++;
                    if (++i == 1 << depth)
                        goto done;
                }
            }
        }
        for (i = 1; i <= 24; i++) {
            v = i * 0xFF / 25;
            palette[idx] = v;
            idx++;
            palette[idx] = v;
            idx++;
            palette[idx] = v;
            idx++;
        }
    }
done:
    return palette;
}

ge_GIF *
ge_new_gif(
    const char *fname, uint16_t width, uint16_t height,
    uint8_t *palette, int depth, int loop
)
{
    ge_GIF *gif = calloc(1, sizeof(*gif) + 2*width*height);
    if (!gif)
        goto no_gif;
    gif->w = width; gif->h = height;
    gif->depth = depth > 1 ? depth : 2;
    gif->frame = (uint8_t *) &gif[1];
    gif->back = &gif->frame[width*height];
    gif->fd = creat(fname, 0666);
    if (gif->fd == -1)
        goto no_fd;
#ifdef _WIN32
    setmode(gif->fd, O_BINARY);
#endif
    write(gif->fd, "GIF89a", 6);
    write_num(gif->fd, width);
    write_num(gif->fd, height);
    write(gif->fd, (uint8_t []) {0xF0 | (depth-1), 0x00, 0x00}, 3);

    if (!palette) {
        palette = ge_make_palette(depth);
    }
    gif->palette = palette;
    write(gif->fd, palette, 3 << depth);

    if (loop >= 0 && loop <= 0xFFFF)
        put_loop(gif, (uint16_t) loop);
    return gif;
no_fd:
    free(gif);
no_gif:
    return NULL;
}

static void
put_loop(ge_GIF *gif, uint16_t loop)
{
    write(gif->fd, (uint8_t []) {'!', 0xFF, 0x0B}, 3);
    write(gif->fd, "NETSCAPE2.0", 11);
    write(gif->fd, (uint8_t []) {0x03, 0x01}, 2);
    write_num(gif->fd, loop);
    write(gif->fd, "\0", 1);
}

/* Add packed key to buffer, updating offset and partial.
 *   gif->offset holds position to put next *bit*
 *   gif->partial holds bits to include in next byte */
static void
put_key(ge_GIF *gif, uint16_t key, int key_size)
{
    int byte_offset, bit_offset, bits_to_write;
    byte_offset = gif->offset / 8;
    bit_offset = gif->offset % 8;
    gif->partial |= ((uint32_t) key) << bit_offset;
    bits_to_write = bit_offset + key_size;
    while (bits_to_write >= 8) {
        gif->buffer[byte_offset++] = gif->partial & 0xFF;
        if (byte_offset == 0xFF) {
            write(gif->fd, "\xFF", 1);
            write(gif->fd, gif->buffer, 0xFF);
            byte_offset = 0;
        }
        gif->partial >>= 8;
        bits_to_write -= 8;
    }
    gif->offset = (gif->offset + key_size) % (0xFF * 8);
}

static void
end_key(ge_GIF *gif)
{
    int byte_offset;
    byte_offset = gif->offset / 8;
    if (gif->offset % 8)
        gif->buffer[byte_offset++] = gif->partial & 0xFF;
    write(gif->fd, (uint8_t []) {byte_offset}, 1);
    write(gif->fd, gif->buffer, byte_offset);
    write(gif->fd, "\0", 1);
    gif->offset = gif->partial = 0;
}

static void
put_image(ge_GIF *gif, uint16_t w, uint16_t h, uint16_t x, uint16_t y)
{
    int nkeys, key_size, i, j;
    Node *node, *child, *root;
    int degree = 1 << gif->depth;

    write(gif->fd, ",", 1);
    write_num(gif->fd, x);
    write_num(gif->fd, y);
    write_num(gif->fd, w);
    write_num(gif->fd, h);
    write(gif->fd, (uint8_t []) {0x00, gif->depth}, 2);
    root = node = new_trie(degree, &nkeys);
    key_size = gif->depth + 1;
    put_key(gif, degree, key_size); /* clear code */
    for (i = y; i < y+h; i++) {
        for (j = x; j < x+w; j++) {
            uint8_t pixel = gif->frame[i*gif->w+j] & (degree - 1);
            child = node->children[pixel];
            if (child) {
                node = child;
            } else {
                put_key(gif, node->key, key_size);
                if (nkeys < 0x1000) {
                    if (nkeys == (1 << key_size))
                        key_size++;
                    node->children[pixel] = new_node(nkeys++, degree);
                } else {
                    put_key(gif, degree, key_size); /* clear code */
                    del_trie(root, degree);
                    root = node = new_trie(degree, &nkeys);
                    key_size = gif->depth + 1;
                }
                node = root->children[pixel];
            }
        }
    }
    put_key(gif, node->key, key_size);
    put_key(gif, degree + 1, key_size); /* stop code */
    end_key(gif);
    del_trie(root, degree);
}

static int
get_bbox(ge_GIF *gif, uint16_t *w, uint16_t *h, uint16_t *x, uint16_t *y)
{
    int i, j, k;
    int left, right, top, bottom;
    left = gif->w; right = 0;
    top = gif->h; bottom = 0;
    k = 0;
    for (i = 0; i < gif->h; i++) {
        for (j = 0; j < gif->w; j++, k++) {
            if (gif->frame[k] != gif->back[k]) {
                if (j < left)   left    = j;
                if (j > right)  right   = j;
                if (i < top)    top     = i;
                if (i > bottom) bottom  = i;
            }
        }
    }
    if (left != gif->w && top != gif->h) {
        *x = left; *y = top;
        *w = right - left + 1;
        *h = bottom - top + 1;
        return 1;
    } else {
        return 0;
    }
}

static void
set_delay(ge_GIF *gif, uint16_t d)
{
    write(gif->fd, (uint8_t []) {'!', 0xF9, 0x04, 0x04}, 4);
    write_num(gif->fd, d);
    write(gif->fd, "\0\0", 2);
}

uint8_t
ge_get_palette_index(uint8_t *palette, uint8_t depth, uint8_t *rgb)
{
    int dist;
    int best_dist = 256 * 2 * 3;
    int offset;
    int closest_idx = 0;

    for(int i = 0; i < (1 << depth); i++)
    {
        offset = i * 3;
        // we want to:
        // 1. get abs val
        // 2. multiply by 2 to exaggerate color channel distances
        dist = (abs(palette[offset] - rgb[0]) << 1) + (abs(palette[offset + 1] - rgb[1]) << 1) + (abs(palette[offset + 2] - rgb[2]) << 1);
        if (dist < best_dist)
        {
            best_dist = dist;
            closest_idx = i;
            // If dist is 0 we have an exact match
            if (!dist) {
                break;
            }
        }
    }

    return (uint8_t) closest_idx;
}

/*
 * pixel_width = 3 for rgb, 4 for rgba
 */
void
ge_rgba_frame_to_indexed(uint8_t *palette, uint8_t depth, uint8_t *indexed_pixels, uint8_t *rgba_pixels, int rgba_pixels_len, uint8_t pixel_width)
{
    int indexed_pixels_len = rgba_pixels_len / pixel_width;

    uint8_t rgb[3];
    int rgba_idx;
    for(int i = 0; i < indexed_pixels_len; i++)
    {
        // alpha gets ignored if present.
        rgba_idx = i * pixel_width;
        rgb[0] = rgba_pixels[rgba_idx];
        rgb[1] = rgba_pixels[rgba_idx + 1];
        rgb[2] = rgba_pixels[rgba_idx + 2];
        indexed_pixels[i] = ge_get_palette_index(palette, depth, rgb);
    }
}

/*
 * pixel_width = 3 for rgb, 4 for rgba
 */
void
ge_populate_frame(ge_GIF *gif, uint8_t *indexed_pixels, int indexed_pixels_len)
{
    memcpy(gif->frame, indexed_pixels, indexed_pixels_len);
}

void
ge_add_frame(ge_GIF *gif, uint16_t delay)
{
    uint16_t w, h, x, y;
    uint8_t *tmp;

    if (delay)
        set_delay(gif, delay);
    if (gif->nframes == 0) {
        w = gif->w;
        h = gif->h;
        x = y = 0;
    } else if (!get_bbox(gif, &w, &h, &x, &y)) {
        /* image's not changed; save one pixel just to add delay */
        w = h = 1;
        x = y = 0;
    }
    put_image(gif, w, h, x, y);
    gif->nframes++;
    tmp = gif->back;
    gif->back = gif->frame;
    gif->frame = tmp;
}

void
ge_close_gif(ge_GIF* gif)
{
    write(gif->fd, ";", 1);
    close(gif->fd);
    free(gif);
}
