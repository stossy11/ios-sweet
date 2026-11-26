/* Match Metroid XNU's retained-scanout console (overlay0017/0019):
 * discover once, then touch framebuffer RAM only. No display kicks or waits.
 * Included after the shared decoder and framebuffer access helpers. */
static struct surface scanout[28];
static unsigned scanout_count, scanout_initialized;

static void discover_scanout(void)
{
    scanout_initialized = 1;
    for (unsigned c = 0; c < 4; c++) {
        uintptr_t ctl = MDP + 0x16000 + c * 0x1000;
        uint32_t intf = read32(ctl + 0xf4);
        if (!intf || (intf & ~6u) || !(read32(ctl + 0x14) & (1u << 17)) ||
            read32(ctl + 0xec) || read32(ctl + 0xf0))
            continue;
        uint32_t active = read32(ctl + 0x12c);
        for (unsigned p = 0; p < 7; p++) {
            if (!(active & (1u << pipe_bits[p]))) continue;
            uintptr_t pipe = MDP + pipe_offsets[p];
            struct surface s;
            if (!surface_decode(&s, read32(pipe + 0x14), read32(pipe),
                    read32(pipe + 8), read32(pipe + 0x24), read32(pipe + 0x30),
                    read32(pipe + 0x34), read32(pipe + 0x38),
                    read32(pipe + 0x70), read32(pipe + 0x170)))
                continue;
            unsigned duplicate = 0;
            for (unsigned i = 0; i < scanout_count; i++) {
                const struct surface old = scanout[i];
                if (old.address == s.address && old.stride == s.stride &&
                    old.width == s.width && old.height == s.height &&
                    old.x == s.x && old.y == s.y && old.black == s.black)
                    duplicate = 1;
            }
            if (!duplicate) scanout[scanout_count++] = s;
        }
    }
}

int display_info(const struct cpu_info *cpu)
{
    if (!scanout_initialized) discover_scanout();
    for (unsigned i = 0; i < scanout_count; i++) {
        /* Snapshot metadata before uncached stores, as in the XNU console. */
        const struct surface s = scanout[i];
        render_info(framebuffer(s.address), &s, cpu);
    }
    barrier();
    return scanout_count != 0;
}
