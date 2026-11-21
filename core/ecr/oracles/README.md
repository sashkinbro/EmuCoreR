# EmuCoreR ECR CPU, GPU, PGXP Oracles & Headless Performance Suite

Standalone native ARM64 accuracy oracles and performance benchmarks for verifying **ECR (`libecr`)** correctness and speed across real Android hardware. All binaries dynamically load `libecr_android.so` via `libecr` / `libretro` and execute directly on `arm64-v8a` devices over ADB.

## CPU Oracle (`cpu_oracle.cpp`)

Compares **CachedInterpreter** and **Recompiler** variants (`Safe` / `LUT` / `MMap` / `ICache` / `PGXP_M` / `PGXP_C`) against golden **Interpreter** (and `CachedInterpreter` for non-ICache `CodeCache` SMC):

1. `alu_shifts_overflow` — ALU, negative immediates, constant/variable shifts (`0/1/15/31/32/37`), overflow traps.
2. `mul_div_edges` — `MULT`/`MULTU`/`DIV`/`DIVU`/`MTHI`/`MTLO`, divide-by-zero, `INT_MIN / -1`, cross-block operands.
3. `unaligned_mem_scratchpad` — KUSEG/KSEG0/KSEG1/2 MiB RAM mirrors, Scratchpad aliasing, `LB`/`LBU`/`LH`/`LHU`, `LWL`/`LWR`/`SWL`/`SWR`.
4. `load_delay_hazards` — Load-delay slot reads, cancellation, back-to-back loads, branch-delay load boundaries, `MFC2`.
5. `branches_delay_slots` — All 12 branch/jump opcodes, `BLTZAL`/`BGEZAL` link writes, `JALR rX, rX`, delay-slot exceptions, 1000-iter loop linking.
6. `cop0_exceptions_traps` — `SYSCALL`, `BREAK`, `RI`, `CpU` (COP2 disabled), software interrupts, `RFE`.
7. `mem_align_exceptions` — `AdEL`/`AdES` unaligned load/store traps (`MemoryExceptions=true`).
8. `smc_jit_invalidation` — Flushed I-cache SMC loop across all modes vs Interpreter.
9. `smc_codecache_invalid` — Un-flushed 20x SMC loop stressing CodeCache MMap write-faults vs CachedInterpreter.
10. `cop2_gte_full` — All 22 GTE opcodes (`RTPS`, `RTPT`, `NCLIP`, `AVSZ3`, `AVSZ4`, `MVMVA`, `OP`, `SQR`, `DPCS`, `INTPL`, `NCDS`, `CDP`, `NCDT`, `NCCS`, `CC`, `NCS`, `NCT`, `DCPL`, `DPCT`, `GPF`, `GPL`, `NCCT`), `LWC2`/`SWC2`, `SXYP`, `IRGB`/`ORGB`, `LZCS`/`LZCR`, `FLAG`, and full 64-reg dump.
11. `timers_irq_controller` — Timer 2 + `I_STAT`/`I_MASK` acknowledge.
12. `dma_otc_and_gpu` — DMA6 OTC reverse table + DMA2 linked-list GPU packet.
13. `spu_ram_and_voices` — SPU manual FIFO RAM transfer + voice key-on + status readback.
14. `mdec_decode` — MDEC quant/scale tables + 24-bit macroblock decode FIFO hash.
15. `fastmem_mixed_io_stress` — Hot dynamic loop alternating RAM / Scratchpad / BIOS / MMIO pointers.
16. Real BIOS (`--bios`) & Real Game (`--game`) lockstep — Full 2 MiB RAM + final framebuffer FNV-1a verification.

## GPU & PGXP Oracle (`gpu_oracle.cpp`)

Compares **SoftwareThreaded**, **HardwareOpenGL** (EGL ES 3.0), and **HardwareVulkan** (headless Vulkan 1.0 + staging readback) against **Software Single-Thread** across 14 scenes, plus PGXP 3D perspective/depth tests and `--dump-dir` PNG export.

## Headless Performance Benchmark (`perf_bench.cpp`)

Benchmarks synthetic CPU ALU/Mem, GTE 3D PGXP, real PS1 BIOS boot, and real PS1 games across Interpreter/Cached/JIT and Software/OpenGL/Vulkan (`1x` and `2x`).

## Usage

```powershell
# Oracles (CPU + BIOS + Game lockstep + GPU GL/Vulkan/PGXP + PNG dumps)
powershell -ExecutionPolicy Bypass -File core\ecr\oracles\run_all_oracles.ps1 `
    -Serial HA28QVYS -BiosPath /sdcard/Download/PS1Bios/scph101.bin

# Headless performance benchmark
powershell -ExecutionPolicy Bypass -File core\ecr\oracles\run_perf_bench.ps1 -Serial HA28QVYS
```
