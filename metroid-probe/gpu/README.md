# Standalone Metroid GPU probe

Builds a temporary Android-v4 boot image in a few seconds, without compiling
XNU. It extracts the actual GPU C implementation from overlay0033, and the
shared RPMh implementation from overlay0029, so fixes to those patches are
included in the next probe build. No storage driver or iOS root is included.

```sh
export PATH="/opt/homebrew/opt/lld/bin:/opt/homebrew/opt/llvm/bin:$PATH"
python3 metroid-probe/gpu/build.py --template nothing-bringup/boot_b.img \
  --gpu-firmware-dir /path/to/stock/gpu/firmware
python3 metroid-probe/gpu/test.py
fastboot boot metroid-probe/out/gpu/metroid-gpu.img
```

The firmware directory must contain `gen80600_gmu.bin`, `gen80000_sqe.fw`,
`gen80000_aqe.fw`, and `gen80000_zap.mbn`. They are embedded in ignored build
outputs only. The manifest records image/firmware/source hashes.

The test attempts retained fabric/rail votes, power/clocks, GMU stream5 SMMU,
firmware load/SCM setup, CM3/HFI startup, GPU stream0, scheduler memory requests,
GX/SQE/AQE setup, authenticated ZAP, context registration and fenced GPU writes.
The existing four-pixel proof is followed by 16 GPU-generated 32x32 pixel
patterns. Every output pixel is compared after fence retirement and visibility
synchronization; CPU presentation enlarges the verified pattern on-screen.
This tests command processing and GPU memory writes, **not shader rasterization
or iOS compositing**. Early failures stop before subsequent phases.

The CPU stays at EL1 with MMU/data cache off and interrupts masked. Allocations
come from a 128 MiB arena at the existing Metroid XNU RAM base after the live FDT
RAM/reservation validator succeeds. No allocation or vote is reused/released
after GPU exposure; manually restart after reading the results. This is a
single attempt per boot, with existing driver timeouts and quarantine guards.

v4 draws diagnostics and verified GPU pixels directly into the inherited
framebuffer, following Metroid XNU's retained-scanout console (overlays0017
and0019). It discovers and validates active surfaces once, snapshots each
surface's address/origin/stride, writes framebuffer RAM, and issues a memory
barrier. Later output does not access display registers. There are no
`CTL_START` writes, flush-mask programming, display polling or timer waits.
The CPU remains MMU/data-cache off, so it uses the physical framebuffer rather
than XNU's Normal-NC virtual alias; this is not a port of XNU's video-console
subsystem.

Record the last `GPU:` checkpoint and all result/exception fields. v1/v2 were
reported to show only their opening title and description (v2 also the zero
frames footer). v3 used the older CPU probe's CTL_START and 100 ms delay path.
Those observations do not locate a GPU failure or establish a timing cause.
The v4 framebuffer path still needs verification on the device.

Host validation checks the boot headers, firmware-bearing image, relative PIE
relocations at three load bases, reuse of the validated surface decoder,
split-pipe/padding bounds, cached discovery and absence of display-register
writes or waits. It runs the actual probe
pixel commands through the sanitizer-enabled integrated GPU model and verifies
that corrupted output fails. These checks do not replace device verification.
