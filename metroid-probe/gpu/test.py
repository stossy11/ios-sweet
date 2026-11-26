#!/usr/bin/env python3
"""Validate the built probe and exercise its actual pixel/screen code on host."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT/'metroid-probe/out/gpu')
    args = parser.parse_args()
    out = args.output_dir.resolve()
    boot = (out/'metroid-gpu.img').read_bytes()
    raw = (out/'metroid-gpu.Image').read_bytes()
    manifest = json.loads((out/'manifest.json').read_text())
    assert hashlib.sha256(boot).hexdigest() == manifest['sha256']
    assert boot[:8] == b'ANDROID!' and struct.unpack_from('<I',boot,40)[0] == 4
    assert struct.unpack_from('<I',boot,1580)[0] == 0
    assert struct.unpack_from('<I',boot,12)[0] == 0
    assert struct.unpack_from('<I',boot,8)[0] == len(raw) and boot[4096:] == raw
    assert raw[56:60] == b'ARM\x64' and struct.unpack_from('<Q',raw,16)[0] == len(raw)
    assert b'METROID GPU PROBE v6' in raw
    # Validate every actual runtime relocation at multiple physical bases.
    elf = (out/'metroid-gpu.elf').read_bytes()
    shoff = struct.unpack_from('<Q',elf,40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<3H',elf,58)
    headers = [struct.unpack_from('<II4QII2Q',elf,shoff+i*shentsize) for i in range(shnum)]
    names = elf[headers[shstrndx][4]:headers[shstrndx][4]+headers[shstrndx][5]]
    sections = {names[h[0]:].split(b'\0')[0].decode(): h for h in headers}
    reloc = sections['.rela.dyn']; count = 0
    for base in (0x80080000, 0xa8000000, 0x8c1000000):
        relocated = bytearray(raw)
        for at in range(reloc[4],reloc[4]+reloc[5],24):
            target, info, addend = struct.unpack_from('<3Q',elf,at)
            assert info == 1027 and target % 8 == 0
            assert target+8 <= len(raw) and addend < len(raw)
            struct.pack_into('<Q',relocated,target,base+addend)
            assert struct.unpack_from('<Q',relocated,target)[0] == base+addend
            count += 1
    print('PASS: boot headers, firmware-bearing Image and relative callback/GOT relocation:',count//3,'entries')
    assert 'metroid_console' not in (out/'metroid-gpu.map').read_text()
    display = (out/'display.c').read_text()
    original = (ROOT/'metroid-probe/display.c').read_text()
    decoder = original[original.index('int surface_decode('):original.index('/* 16-pixel')]
    assert decoder in display
    assert (HERE/'framebuffer.inc').read_text() in display
    assert 'write32(' not in display and 'CTL_FLUSH' not in display
    with tempfile.TemporaryDirectory(prefix='metroid-gpu-probe-test-') as directory:
        temp = Path(directory)
        flags = ['clang','-std=c11','-Wall','-Wextra','-Werror','-O1','-g',
                 '-fsanitize=address,undefined','-fno-omit-frame-pointer']
        # Adapt existing connected MMIO model to execute CP_MEM_WRITE IBs of
        # either the four-pixel selftest or the probe's 1024-pixel pattern.
        model = (ROOT/'tools/tests/metroid_gpu_accel_model.c').read_text()
        model = model.replace('int main(int argc, char **argv)', 'static int pixels_test(void);\nstatic unsigned frames;\nint main(int argc, char **argv)')
        model = model.replace('packet[10]==32', '(packet[10]==32 || packet[10]==4112)')
        model = model.replace('gpu_pointer(packet[8],32)', 'gpu_pointer(packet[8],packet[10])')
        model = model.replace('assert(ib[0]==0x703d8006 && ib[2]==0 && ib[7]==0x70268000);',
            'unsigned words=packet[10]/4-4; assert((ib[0]&0x7fff)==words+2 && ib[2]==0 && ib[words+3]==0x70268000);')
        model = model.replace('gpu_pointer(ib[1],16)', 'gpu_pointer(ib[1],words*4)')
        model = model.replace('memcpy(target,ib+3,16)', 'memcpy(target,ib+3,words*4)')
        needle = 'powered_state=false; assert(!owner->render.ops.ready(&owner->render)); powered_state=true;'
        assert model.count(needle) == 1
        model = model.replace(needle, '''assert(!pixels_test() && frames==16 && issue_count==17);
            failure=4; assert(pixels_test()==MG_IO && frames==16); failure=0;
            '''+needle)
        main = (HERE/'main.c').read_text()
        functions = main[main.index('static unsigned packet7('):main.index('void probe_main(')]
        begin = functions.index('static void display_pixels(')
        end = functions.index('static int pixels_test(',begin)
        functions = functions[:begin]+'''static void display_pixels(const uint32_t *pixels)
{ for(unsigned i=0;i<1024;i++) assert(pixels[i]==color(i,frames-1)); }
'''+functions[end:]
        model += '\n#define accelerator (*owner)\nstatic struct mg_gen8_job pixel_job;\n'+functions
        source = temp/'pixels.c'; source.write_text(model)
        core = out/'core'
        cores = ('accel','hardware','zap','vm','render','scheduler','submit','queue',
                 'hfi','live','memory','power','smmu','start','load','fw')
        binary = temp/'pixels'
        subprocess.run(flags+['-DMG_HOST_TEST','-I'+str(core),'-I'+str(ROOT/'tools/tests'),str(source),
                             *[str(core/('metroid_gpu_'+name+'.c')) for name in cores],'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
        print('PASS: actual probe 16 fenced pixel patterns through real VM/scheduler/submit code; corrupted output rejected')
        # Exercise retained scanout: discovery once, then framebuffer RAM only.
        source = temp/'screen-test.c'
        source.write_text(r'''
#include "screen.h"
#include "display.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t regs[0x30000/4], *pixels;
static unsigned reads;
void delay(unsigned us) { (void)us; assert(!"display must not wait"); }
uint32_t test_read32(uintptr_t a) { reads++; assert(a>=0xae00000 && a<0xae30000 && !(a&3)); return regs[(a-0xae00000)/4]; }
void test_write32(uintptr_t a,uint32_t v) { (void)a; (void)v; assert(!"display must not write MMIO"); }
volatile uint32_t *test_framebuffer(uint64_t a) { assert(a==SPLASH_START); return pixels; }
static void reg(unsigned o,uint32_t v) { regs[o/4]=v; }
int main(int argc, char **argv) {
  unsigned stride=1264;
  pixels=malloc((size_t)stride*2800*4); assert(pixels);
  for(unsigned i=0;i<stride*2800;i++) pixels[i]=0x12345678;
  reg(0x160f4,2);reg(0x16014,1u<<17);reg(0x1612c,1u<<16);
  reg(0x5014,SPLASH_START);reg(0x5000,(2800u<<16)|630);reg(0x5024,stride*4);
  reg(0x5030,0x237ff);reg(0x5034,0x03020001);reg(0x5008,630);
  if(argc>1) {
    if(!strcmp(argv[1],"writeback")) reg(0x160ec,1);
    else reg(0x5030,0x237ff|(2u<<30));
    assert(!gpu_screen_present());
    for(unsigned i=0;i<stride*2800;i++) assert(pixels[i]==0x12345678);
    unsigned before=reads; assert(!gpu_screen_present() && reads==before);
    free(pixels); return 0;
  }
  for(unsigned i=0;i<100;i++) gpu_screen_string("CHECKPOINT long diagnostic line\n");
  assert(gpu_screen_count==GPU_SCREEN_ROWS-1);
  uint32_t pattern[1024];for(unsigned i=0;i<1024;i++) pattern[i]=0xff000000|i;
  gpu_screen_pixels=pattern;gpu_screen_frames=16;
  assert(gpu_screen_present());
  for(unsigned y=0;y<2800;y++) for(unsigned x=0;x<stride;x++) {
    uint32_t v=pixels[(size_t)y*stride+x];
    if(x<630 || x>=1260) assert(v==0x12345678);
    if(x>=646 && x<902 && y>=2480 && y<2736) assert(v==pattern[((y-2480)/8)*32+(x-646)/8]);
  }
  unsigned before=reads;
  memset(regs,0,sizeof(regs)); /* MMIO can disappear after discovery. */
  pattern[0]=0xff123456;
  assert(gpu_screen_present() && reads==before);
  assert(pixels[(size_t)2480*stride+646]==pattern[0]);
  free(pixels);puts("PASS: GPU snapshot log, split-pipe/padding guards, scaled GPU pixels, retained scanout, no display MMIO writes/waits");
}
''')
        binary = temp/'screen'
        subprocess.run(flags+['-DHOST_TEST','-I'+str(HERE),'-I'+str(HERE/'include'),
                              '-I'+str(ROOT/'metroid-probe'),str(source),str(HERE/'screen.c'),
                              str(out/'display.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
        for mode in ('writeback', 'invalid-format'):
            subprocess.run([str(binary),mode],check=True)


if __name__ == '__main__':
    run()
