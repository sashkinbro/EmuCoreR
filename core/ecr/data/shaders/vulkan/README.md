# Vulkan shader sources (pre-baked SPIR-V)

This directory holds the GLSL sources for shaders used by the Vulkan backend
that have been moved off the runtime `ShaderGen` path. Each `.glsl` file here
has a matching pre-compiled SPIR-V blob checked in to
`src/common/vulkan/embedded_spirv/`.

## Why this exists

Historically the Vulkan backend generated GLSL at runtime via
`src/core/shadergen.cpp` and `src/core/gpu_hw_shadergen.cpp`, then handed the
text to glslang inside `src/common/vulkan/shader_compiler.cpp` to produce
SPIR-V. Both `dep/glslang/` (4.4 MB of vendored source, 99 translation units)
and the disk-backed runtime shader cache exist solely to support that path.

Pre-baking the SPIR-V removes:

* the glslang dependency from the libretro core build,
* the per-launch shader compilation stall, and
* a class of "stale cache after glslang version bump" runtime failures.

The OpenGL / OpenGL ES / D3D11 / D3D12 backends still use the runtime
`ShaderGen` path; only the Vulkan backend reads from the embedded SPIR-V.

## Build-time constraint

The core build (`Makefile.libretro` and `src/common/CMakeLists.txt`) must
never invoke an external shader compiler. The `.inc` files under
`src/common/vulkan/embedded_spirv/` are checked into the repository and
compiled like any other C++ source. The `tools/regen_vulkan_spirv.py` helper
exists for contributors who edit the `.glsl` sources in this directory; it is
not part of the build.

## Regenerating the SPIR-V blobs

After editing a `.glsl` file here, run:

    python3 tools/regen_vulkan_spirv.py

This invokes `glslangValidator --target-env vulkan1.0` on each `.glsl` file
and rewrites the matching `.inc` under `src/common/vulkan/embedded_spirv/`.
Both the `.glsl` and the regenerated `.inc` must be committed together so the
two stay in sync.

`glslangValidator` is part of the Vulkan SDK (or the `glslang-tools` package
on Debian / Ubuntu). It is **not** invoked by the core build.
