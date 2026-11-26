#include "runtime.h"
#include "screen.h"
#include "../display.h"
char gpu_screen_lines[GPU_SCREEN_ROWS][GPU_SCREEN_COLS+1];
unsigned gpu_screen_count, gpu_screen_frames;
const uint32_t *gpu_screen_pixels;
static unsigned column;

static void new_line(void)
{
    if(gpu_screen_count<GPU_SCREEN_ROWS-1) gpu_screen_count++;
    else {
        for(unsigned i=1;i<GPU_SCREEN_ROWS;i++)
            memcpy(gpu_screen_lines[i-1],gpu_screen_lines[i],GPU_SCREEN_COLS+1);
        memset(gpu_screen_lines[GPU_SCREEN_ROWS-1],0,GPU_SCREEN_COLS+1);
    }
    column=0;
}
void gpu_screen_putc(char c)
{
    if(c=='\r') { column=0; return; }
    if(c=='\n') { new_line(); return; }
    if(column==GPU_SCREEN_COLS) new_line();
    gpu_screen_lines[gpu_screen_count][column++]=c>=32 && c<=126 ? c : '?';
    gpu_screen_lines[gpu_screen_count][column]=0;
}
void gpu_screen_string(const char *s) { while(*s) gpu_screen_putc(*s++); }
int gpu_screen_present(void)
{
    const struct cpu_info cpu={0};
    /* Like Metroid XNU: draw directly into the discovered framebuffer.
     * Firmware owns scanout; no display-register writes or timer waits. */
    return display_info(&cpu);
}
