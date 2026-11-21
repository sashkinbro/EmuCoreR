# EmuCoreR ECR CPU & GPU Accuracy Oracles

Standalone native ARM64 accuracy oracles for verifying CPU and GPU fidelity when optimizing or refactoring **ECR (`libecr`)**. Both binaries dynamically load `libecr_android.so` via the `libecr` / `libretro` C ABI and execute directly on a connected `arm64-v8a` Android device over ADB.

## CPU Oracle (`cpu_oracle.cpp`)

Compares **CachedInterpreter** and **Recompiler** (ARM64 JIT with block linking) against the golden **Interpreter** reference:

1. `alu_shifts_overflow` — ALU, shifts (`SLL`/`SRL`/`SRA`/variable shifts), comparisons, and signed overflow traps (`ADD`/`ADDI`/`SUB`).
2. `mul_div_edges` — `MULT`/`MULTU`/`DIV`/`DIVU`/`MTHI`/`MTLO`, negative operands, divide-by-zero, and `INT_MIN / -1`.
3. `unaligned_mem_scratchpad` — `LWL`/`LWR`/`SWL`/`SWR` across all 4 byte alignments, `LB`/`LBU`/`LH`/`LHU`/`SB`/`SH`, load-delay behavior, and Scratchpad RAM (`0x1F800000`).
4. `branches_delay_slots` — Branches/jumps, delay slots, link registers, `JALR`, and exception-in-delay-slot (`CAUSE.BD` + `EPC`).
5. `cop0_exceptions_rfe` — `SYSCALL`, `BREAK`, reserved opcodes, `AdEL`/`AdES` address errors, and `RFE` status stack restoration.
6. `smc_jit_invalidation` — Self-modifying RAM code executed -> JIT compiled -> patched -> re-executed.
7. `cop2_gte_ops` — GTE perspective/matrix/lighting/depth ops (`RTPS`, `RTPT`, `NCLIP`, `AVSZ3`, `AVSZ4`, `MVMVA`, `SQR`, `DPCS`, `INTPL`, `OP`, `NCDS`, `NCCS`, `CDP`, `GPF`, `GPL`) and full 64-register + `FLAG` dump.
8. Real-BIOS lockstep (`--bios <path> --frames N`) — compares full 2 MiB PS1 RAM and final framebuffer FNV-1a hashes across all 3 CPU execution modes.

## GPU Oracle (`gpu_oracle.cpp`)

Compares **SoftwareThreaded** (`ecr_GPU_UseThread = true`) and **HardwareOpenGL** (`OpenGL`, 1x resolution scale, `Nearest`, headless EGL 1.4 PBuffer ES 3.0) against the golden **Software Single-Thread** reference across 7 GP0/GP1 scenes (`320x240`):

1. `shaded_polys_dither_off`
2. `shaded_polys_dither_on`
3. `textured_clut_4_8_15` (+ Texture Window `0xE2`)
4. `semi_transparency_modes` (ABR modes `0..3` + STP)
5. `lines_sprites_rects`
6. `vram_blits_mask_bit` (`0x02` FillRect, `0x80` overlapping VRAM copy, `0xE6` force/check mask bit)
7. `clip_and_offset` (`0xE3`/`0xE4` window clip + signed `0xE5` offset)

- `SoftwareThreaded` must match `Software` **100% bit-for-bit**.
- `HardwareOpenGL` reports **Exact %** (5-bit channel exact), **±1-LSB %**, **MAE**, **Max Diff**, and **PSNR (dB)**.

## Usage

```powershell
# Prerequisite: build the debug native libraries once
.\gradlew.bat :app:assembleDebug

# Run both CPU (including real PS1 BIOS lockstep) and GPU oracles on device
powershell -ExecutionPolicy Bypass -File core\ecr\oracles\run_all_oracles.ps1 `
    -Serial HA28QVYS `
    -BiosPath /sdcard/Download/PS1Bios/scph101.bin
```
