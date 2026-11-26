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

v6 draws diagnostics and verified GPU pixels directly into the inherited
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
The retained-framebuffer path still needs device verification with an identified
image version/hash.

For the reported `err=-10 live=5 start=4 start-reg=0x1f81c/0`, v5 adds:

- Full ITCM/DTCM payload readback while CM3 is held in reset. A mismatch fails
  before reset release and records the register, observed word and expected
  word. `TCM verified words` counts successful comparisons, not execution.
- CM3 reset-release readback. A dropped release fails at `0x1f800` rather than
  being misreported as a firmware-ready timeout.
- First poll value and poll-read count. The vendor firmware-ready condition
  remains `(FW_INIT_RESULT & 0x1ff) == 0x100`, with the same 100 ms budget.
- A read-only GMU snapshot before reasserting CM3 reset: reset/result/config,
  HFI/interrupt/fence state, GBIF halt/ack, log write pointer, AO counter and
  the first two ITCM words. The original failure register/value is preserved.
- Global and selected-context SMMU faults read after CM3 reset, without clearing
  faults, and the first eight raw words of the probe's owned firmware log.

The exact reported timeout is still a failure: no HFI startup, retry, resource
reuse or forced-ready path follows it. A host model reproduces that tuple and
checks the pre-reset snapshot and retained fault details. These changes fix
missing validation and diagnostics; they do not establish or prove a cure for
the hardware firmware-ready timeout by themselves.

The v5 device photo supplies a concrete next fix: TCM readback passed139words,
CM3 reset was released, but the selected SMMU context recorded `FSR=0x408`,
`FAR=0x4000`, `FSYNR0=0x1a0063`. This is a privileged instruction-read
permission fault at the first external firmware page. Our GMU page builder set
AP_UNPRIV, making firmware memory writable by unprivileged accesses. AArch64
stage-1 treats such pages as implicitly privileged-execute-never, even if the
descriptor's explicit PXN bit is clear. The vendor GMU allocator requests
`IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV` instead.

v6 removes AP_UNPRIV from the GMU-only page builder. GMU data remains
non-executable; the GPU VM builder and its user permissions are unchanged.
The integrated model now checks the actual GMU PTE permissions before
signalling firmware-ready, rather than automatically succeeding on reset
release. Old PTEs reproduce the exact photographed fault tuple under that
architectural permission model; corrected PTEs permit privileged firmware
fetches. This is a source/model result, not confirmation of hardware startup.
Temporary-boot v6 next and capture its final result; all v5 diagnostics and
firmware-ready/time-limit/quarantine checks remain enabled.

Host validation checks the boot headers, firmware-bearing image, relative PIE
relocations at three load bases, reuse of the validated surface decoder,
split-pipe/padding bounds, cached discovery and absence of display-register
writes or waits. It runs the actual probe
pixel commands through the sanitizer-enabled integrated GPU model and verifies
that corrupted output fails. These checks do not replace device verification.
