#ifndef GPU_PROBE_SCREEN_H
#define GPU_PROBE_SCREEN_H
#include <stdint.h>
#define GPU_SCREEN_ROWS 52u
#define GPU_SCREEN_COLS 36u
extern char gpu_screen_lines[GPU_SCREEN_ROWS][GPU_SCREEN_COLS+1];
extern unsigned gpu_screen_count, gpu_screen_frames;
extern const uint32_t *gpu_screen_pixels;
void gpu_screen_putc(char);
void gpu_screen_string(const char *);
int gpu_screen_present(void);
#endif
