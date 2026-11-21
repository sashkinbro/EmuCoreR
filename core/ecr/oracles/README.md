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

## Hardware Benchmark Results

All 109 CPU oracle checks and 60 GPU/PGXP oracle checks pass with `0 failures` on both devices below.

### 1. Lenovo Xiaoxin Pad Pro GT (`TB710FU`) — Qualcomm Snapdragon 8 Gen 3 / Adreno 750

| Workload / Game | Mode & Renderer | Baseline (FPS) | Pre-LTO Optimized (FPS) | Final with ThinLTO (FPS) | Total Gain |
| :--- | :--- | :---: | :---: | :---: | :---: |
| **Synthetic GTE 3D + PGXP** | `JIT_MMap_SW` | 1832.4 | 2002.6 | **2070.5** | **+13.0%** |
| **Synthetic GTE 3D + PGXP** | `JIT_PGXP_Mem_SW` | 1830.6 | 2003.3 | **2107.1** | **+15.1%** |
| **Synthetic GTE 3D + PGXP** | `JIT_PGXP_CPU_SW` | 1848.7 | 2002.2 | **2108.7** | **+14.1%** |
| **PS1 BIOS Boot (`scph101`)** | `JIT_MMap_SW` | 1678.1 | 2062.0 | **2274.1** | **+35.5%** |
| **PS1 BIOS Boot (`scph101`)** | `JIT_MMap_GL_1x` | 1651.9 | 2007.8 | **2171.5** | **+31.5%** |
| **PS1 BIOS Boot (`scph101`)** | `JIT_MMap_VK_1x` | 1412.8 | 1693.5 | **1883.0** | **+33.3%** |
| **PS1 BIOS Boot (`scph101`)** | `JIT_PGXP_CPU_VK_2x` | 459.8 | 789.4 | **746.4** | **+62.3%** |
| **Crash Bandicoot 2** | `JIT_PGXP_M_GL_2x` | 541.0 | 934.5 | **956.4** | **+76.8%** |
| **Crash Bandicoot 2** | `JIT_PGXP_C_GL_2x` | 439.5 | 664.1 | **688.9** | **+56.7%** |
| **Crash Bandicoot 2** | `JIT_PGXP_C_VK_2x` | 385.6 | 574.2 | **602.7** | **+56.3%** |
| **Dragon Ball GT: Final Bout** | `JIT_MMap_SW_Thr_1x` | 917.2 | 957.6 | **1003.4** | **+9.4%** |
| **Dragon Ball GT: Final Bout** | `JIT_PGXP_M_GL_2x` | 471.8 | 687.3 | **742.0** | **+57.3%** |
| **Dragon Ball GT: Final Bout** | `JIT_PGXP_C_GL_2x` | 419.4 | 539.7 | **579.0** | **+38.1%** |
| **Dragon Ball GT: Final Bout** | `JIT_PGXP_C_VK_2x` | 389.9 | 471.3 | **515.6** | **+32.2%** |
| **Tekken 3** | `JIT_MMap_SW_Thr_1x` | 466.8 | 1197.3 | **1292.3** | **+176.8%** |
| **Tekken 3** | `JIT_MMap_GL_1x` | 1295.9 | 1371.0 | **1413.4** | **+9.1%** |
| **Tekken 3** | `JIT_PGXP_M_GL_2x` | 515.5 | 795.9 | **805.0** | **+56.2%** |
| **Tekken 3** | `JIT_PGXP_C_GL_2x` | 378.5 | 494.8 | **543.3** | **+43.5%** |
| **Tekken 3** | `JIT_PGXP_C_VK_2x` | 371.4 | 463.2 | **491.6** | **+32.4%** |

### 2. Anbernic RG556 Handheld — Unisoc T820 / ARM Mali-G57 MC4

| Workload / Game | Mode & Renderer | Baseline (FPS) | Pre-LTO Optimized (FPS) | Final with ThinLTO (FPS) | Total Gain |
| :--- | :--- | :---: | :---: | :---: | :---: |
| **Synthetic GTE 3D + PGXP** | `JIT_MMap_SW` | 880.4 | 1052.0 | **1083.8** | **+23.1%** |
| **Synthetic GTE 3D + PGXP** | `JIT_PGXP_Mem_SW` | 889.0 | 1045.1 | **1084.6** | **+22.0%** |
| **Synthetic GTE 3D + PGXP** | `JIT_PGXP_CPU_SW` | 887.0 | 1041.3 | **1083.6** | **+22.2%** |
| **PS1 BIOS Boot (`scph101`)** | `JIT_MMap_SW` | 970.4 | 1072.8 | **1189.5** | **+22.6%** |
| **PS1 BIOS Boot (`scph101`)** | `JIT_MMap_GL_1x` | 989.5 | 1042.2 | **1121.2** | **+13.3%** |
| **PS1 BIOS Boot (`scph101`)** | `JIT_MMap_VK_1x` | 667.9 | 738.9 | **801.5** | **+20.0%** |
| **PS1 BIOS Boot (`scph101`)** | `JIT_PGXP_CPU_VK_2x` | 198.3 | 321.1 | **320.1** | **+61.4%** |
| **Crash Bandicoot 2** | `JIT_MMap_SW_1x` | 841.3 | 850.5 | **911.2** | **+8.3%** |
| **Crash Bandicoot 2** | `JIT_MMap_GL_1x` | 750.3 | 746.1 | **817.5** | **+9.0%** |
| **Crash Bandicoot 2** | `JIT_MMap_GL_2x` | 764.4 | 775.4 | **823.6** | **+7.7%** |
| **Crash Bandicoot 2** | `JIT_PGXP_M_GL_2x` | 319.0 | 439.3 | **417.8** | **+31.0%** |
| **Crash Bandicoot 2** | `JIT_PGXP_C_GL_2x` | 254.8 | 345.8 | **330.3** | **+29.6%** |
| **Crash Bandicoot 2** | `JIT_PGXP_C_VK_2x` | 194.2 | 256.2 | **251.4** | **+29.5%** |
| **Dragon Ball GT: Final Bout** | `JIT_MMap_SW_1x` | 439.9 | 429.7 | **462.9** | **+5.2%** |
| **Dragon Ball GT: Final Bout** | `JIT_MMap_GL_1x` | 380.4 | 380.1 | **404.5** | **+6.3%** |
| **Dragon Ball GT: Final Bout** | `JIT_PGXP_M_GL_2x` | 223.7 | 272.3 | **284.4** | **+27.1%** |
| **Dragon Ball GT: Final Bout** | `JIT_PGXP_C_GL_2x` | 189.7 | 222.5 | **230.6** | **+21.6%** |
| **Dragon Ball GT: Final Bout** | `JIT_PGXP_C_VK_2x` | 153.7 | 175.5 | **196.7** | **+28.0%** |
| **Tekken 3** | `JIT_MMap_SW_1x` | 635.0 | 621.8 | **655.2** | **+3.2%** |
| **Tekken 3** | `JIT_MMap_GL_2x` | 578.5 | 564.1 | **596.5** | **+3.1%** |
| **Tekken 3** | `JIT_PGXP_M_GL_2x` | 277.6 | 347.3 | **342.7** | **+23.5%** |
| **Tekken 3** | `JIT_PGXP_C_GL_2x` | 211.3 | 250.5 | **260.8** | **+23.4%** |
| **Tekken 3** | `JIT_PGXP_C_VK_2x` | 209.0 | 251.6 | **260.8** | **+24.8%** |
