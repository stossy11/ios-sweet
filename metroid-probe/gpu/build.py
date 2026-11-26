#!/usr/bin/env python3
"""Build a standalone Metroid GPU test from the same sources as overlay0033.

No XNU build, device command, storage driver, or downloaded firmware in git.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
CORES = ('fw', 'hfi', 'power', 'smmu', 'start', 'load', 'memory', 'live',
         'vm', 'queue', 'scheduler', 'submit', 'render', 'hardware', 'zap', 'accel', 'scm')


def additions(path):
    """Extract whole-file additions, checking hunk lengths; never shell-apply."""
    lines = path.read_text().splitlines(keepends=True)
    files = {}
    for i in range(len(lines)-2):
        if lines[i] != '--- /dev/null\n' or not lines[i+1].startswith('+++ b/'):
            continue
        name = lines[i+1][6:].strip()
        hunk = lines[i+2].strip()
        if not hunk.startswith('@@ -0,0 +1,'):
            raise ValueError('unexpected added-file hunk: ' + name)
        count = int(hunk.split('+1,')[1].split(' ')[0])
        body = lines[i+3:i+3+count]
        if len(body) != count or any(not line.startswith('+') for line in body):
            raise ValueError('incomplete added file: ' + name)
        files[name] = ''.join(line[1:] for line in body)
    return files


def package(template, image):
    if (len(template) < 8192 or template[:8] != b'ANDROID!' or
            struct.unpack_from('<I', template, 40)[0] != 4 or
            struct.unpack_from('<I', template, 20)[0] != 1584 or
            struct.unpack_from('<I', template, 12)[0]):
        raise ValueError('expected Android boot v4 with no ramdisk')
    size = struct.unpack_from('<I', template, 8)[0]
    original = template[4096:4096+size]
    if len(original) != size or original[56:60] != b'ARM\x64':
        raise ValueError('template must contain an uncompressed ARM64 Image')
    offset, _, flags = struct.unpack_from('<3Q', original, 8)
    if flags & 1 or flags & ~0xf:
        raise ValueError('unsupported ARM64 Image flags')
    image = bytearray(image)
    if image[56:60] != b'ARM\x64':
        raise ValueError('compiled image header missing')
    image.extend(bytes(-len(image) % 4096))
    struct.pack_into('<3Q', image, 8, offset, len(image), flags)
    header = bytearray(template[:1584])
    struct.pack_into('<I', header, 8, len(image))
    struct.pack_into('<I', header, 1580, 0)  # Never retain stale boot signature.
    boot = header + bytes(4096-len(header)) + image
    return bytes(image), bytes(boot)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--template', type=Path, required=True)
    parser.add_argument('--gpu-firmware-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, default=ROOT/'metroid-probe/out/gpu')
    args = parser.parse_args()
    sys.path.insert(0, str(ROOT/'tools'))
    spec = importlib.util.spec_from_file_location('gpu_payload', ROOT/'tools/metroid-gpu-payload.py')
    firmware = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(firmware)
    images = firmware.load_firmware(args.gpu_firmware_dir, tuple(firmware.FIRMWARE))
    template = args.template.read_bytes()
    # Validate all inputs before changing outputs.
    head = bytearray(64); head[56:60] = b'ARM\x64'
    package(template, head)
    tool = {name: shutil.which(name) for name in ('clang', 'ld.lld', 'llvm-objcopy', 'llvm-nm', 'llvm-readelf')}
    if not all(tool.values()):
        parser.error('requires clang, ld.lld, llvm-objcopy, llvm-nm and llvm-readelf')
    out = args.output_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    core = out/'core'; core.mkdir(exist_ok=True)
    added = additions(ROOT/'xnu/metroid/0033-metroid-gpu.patch')
    for name, body in added.items():
        if name.startswith('bsd/dev/metroid_gpu'):
            (core/Path(name).name).write_text(body)
    touch = additions(ROOT/'xnu/metroid/0029-metroid-touch.patch')
    (out/'touch_power.h').write_text(touch['bsd/dev/metroid_touch_power.h'])
    # Reuse the existing EL1 handoff and synchronous exception vectors.
    # ADRP permits growth beyond the original loader's one-MiB ADR range.
    start = (ROOT/'metroid-loader/start.S').read_text()
    start = start.replace('adr x1, _stack_top', 'adrp x1, _stack_top\n    add x1, x1, :lo12:_stack_top')
    # Zero BSS explicitly, even if the bootloader did not preserve file padding.
    start = start.replace('    mov x0, x19\n', '''    adrp x1, _bss_start
    add x1, x1, :lo12:_bss_start
    adrp x2, _bss_end
    add x2, x2, :lo12:_bss_end
bss_zero:
    cmp x1, x2
    b.hs bss_done
    strb wzr, [x1], #1
    b bss_zero
bss_done:
    // Relocate PIE GOT and callback tables at the real, unknown load base.
    adr x9, _head
    adrp x10, _rela_start
    add x10, x10, :lo12:_rela_start
    adrp x14, _rela_end
    add x14, x14, :lo12:_rela_end
relocate:
    cmp x10, x14
    b.hs relocated
    ldp x11, x12, [x10]
    ldr x13, [x10, #16]
    mov x15, #1027
    cmp x12, x15
    b.ne fault
    add x11, x11, x9
    add x13, x13, x9
    str x13, [x11]
    add x10, x10, #24
    b relocate
relocated:
    mov x0, x19
''')
    (out/'start.S').write_text(start)
    # Reuse the validated surface decoder; retain scanout like Metroid XNU.
    # Replace only the CPU information renderer with GPU log/pixel content.
    display_source = (ROOT/'metroid-probe/display.c').read_text()
    begin = display_source.index('void render_info(')
    end = display_source.index('#ifdef HOST_TEST', begin)
    renderer = '''void render_info(volatile uint32_t *base, const struct surface *s,
                 const struct cpu_info *cpu)
{
    (void)cpu;
    for(unsigned y=140;y<2400;y++) for(unsigned x=0;x<s->width;x++)
        base[(uint64_t)(s->y+y)*(s->stride/4)+s->x+x]=s->black;
    for(unsigned row=0;row<=gpu_screen_count;row++)
        line(base,s,row,gpu_screen_lines[row]);
    hexline(base,s,53,"GPU FRAMES ",gpu_screen_frames);
    if(gpu_screen_pixels) {
        for(unsigned y=0;y<256;y++) for(unsigned x=0;x<256;x++)
            base[(uint64_t)(s->y+s->height-320+y)*(s->stride/4)+s->x+16+x]=
                gpu_screen_pixels[(y/8)*32+x/8];
    }
}

'''
    display_source = ('#include "screen.h"\n' + display_source[:begin] + renderer + display_source[end:])
    # GPU bring-up must not depend on a display kick or timer wait. Match
    # XNU's console: discover once, draw into retained framebuffer RAM, dsb.
    display_source = display_source[:display_source.index('static int display_render(')]
    display_source = display_source.replace('void test_write32(uintptr_t address, uint32_t value);\n', '')
    display_source = display_source.replace('#define write32 test_write32\n', '')
    display_source = display_source.replace('''static void write32(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
}
''', '')
    display_source += (HERE/'framebuffer.inc').read_text()
    display_source = display_source.replace('#include "display.h"', '#include '+json.dumps(str(ROOT/'metroid-probe/display.h')))
    display_source = display_source.replace('#include "platform.h"', '#include '+json.dumps(str(ROOT/'metroid-probe/platform.h')))
    display_source = display_source.replace('#include "../shim/font.h"', '#include '+json.dumps(str(ROOT/'shim/font.h')))
    (out/'display.c').write_text(display_source)
    assembly = ['.section .rodata.firmware,"a"', '.balign 16384']
    for prop, name in zip(firmware.FIRMWARE, ('gmu', 'sqe', 'aqe', 'zap')):
        path = out/firmware.FIRMWARE[prop]
        path.write_bytes(images[prop])
        assembly += [f'.global fw_{name}, fw_{name}_end', f'fw_{name}:',
                     '.incbin ' + json.dumps(str(path)), f'fw_{name}_end:', '.balign 16']
    (out/'firmware.S').write_text('\n'.join(assembly)+'\n')
    sources = [out/'start.S', HERE/'main.c', HERE/'runtime.c', HERE/'rpmh.c',
               HERE/'screen.c', out/'display.c', ROOT/'metroid-loader/memory.c',
               ROOT/'metroid-loader/ram.c', out/'firmware.S']
    sources += [core/('metroid_gpu_'+name+'.c') for name in CORES]
    objects = []
    source_hashes = {}
    for index, source in enumerate(sources):
        obj = out/(f'{index:02}-'+source.name+'.o')
        command = [tool['clang'], '--target=aarch64-none-elf', '-march=armv8-a',
                   '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                   '-fpie', '-fvisibility=hidden', '-mgeneral-regs-only', '-mstrict-align',
                   '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-Werror',
                   '-DARM64_BOARD_CONFIG_SWEET', '-DPROBE_KIND=6',
                   '-I'+str(HERE/'include'), '-I'+str(HERE), '-I'+str(out), '-I'+str(core)]
        if source.name == 'rpmh.c':
            command += ['-Wno-unused-function']  # Unused touch entry points discarded.
        subprocess.run(command+['-c', str(source), '-o', str(obj)], check=True)
        objects.append(obj)
        source_hashes[str(source.relative_to(ROOT)) if source.is_relative_to(ROOT) else source.name] = hashlib.sha256(source.read_bytes()).hexdigest()
    elf = out/'metroid-gpu.elf'
    subprocess.run([tool['ld.lld'], '-pie', '--no-dynamic-linker', '-Bsymbolic',
                    '-T', str(HERE/'link.ld'), '--gc-sections', '--build-id=none',
                    '-Map='+str(out/'metroid-gpu.map'), *map(str, objects), '-o', str(elf)], check=True)
    undefined = subprocess.check_output([tool['llvm-nm'], '--undefined-only', str(elf)], text=True)
    if undefined.strip():
        raise ValueError('unresolved symbols: '+undefined)
    relocations = subprocess.check_output([tool['llvm-readelf'], '-r', str(elf)], text=True)
    (out/'relocations.txt').write_text(relocations)
    if any('R_AARCH64_' in line and 'R_AARCH64_RELATIVE' not in line for line in relocations.splitlines()):
        raise ValueError('unsupported non-relative runtime relocation')
    raw = out/'metroid-gpu.Image'
    subprocess.run([tool['llvm-objcopy'], '-O', 'binary', str(elf), str(raw)], check=True)
    image, boot = package(template, raw.read_bytes())
    raw.write_bytes(image)
    path = out/'metroid-gpu.img'; path.write_bytes(boot)
    manifest = dict(image=path.name, bytes=len(boot), sha256=hashlib.sha256(boot).hexdigest(),
                    template_sha256=hashlib.sha256(template).hexdigest(),
                    firmware={name:dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest()) for name,data in images.items()},
                    sources=source_hashes, ram_base='0x8c0000000', arena_bytes=128*1024*1024,
                    test='real GPU init, fenced writes, 16 verified 32x32 pixel patterns; no shaders',
                    device_tested=False)
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(f'{path}: {len(boot)} bytes, SHA256 {manifest["sha256"]}')


if __name__ == '__main__':
    main()
