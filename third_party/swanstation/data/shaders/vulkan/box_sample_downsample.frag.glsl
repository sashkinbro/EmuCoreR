// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateBoxSampleDownsampleFragmentShader().
//
// Vulkan-only port. Used when downsample mode = Box: averages a
// RESOLUTION_SCALE x RESOLUTION_SCALE block of upscaled texels back down
// to a single PSX-native-resolution pixel.
//
// Spec constant: RESOLUTION_SCALE at constant_id=0. Drives:
//   - the block origin scaling (base_coords)
//   - the two inner loop bounds (so the driver can unroll if it chooses)
//   - the final averaging divisor (RESOLUTION_SCALE * RESOLUTION_SCALE)
//
// All three uses become OpSpecConstantOp arithmetic in the SPIR-V; the
// driver folds them at pipeline-create time once the value is bound.
//
// Note on loop unrolling: at scale=16 the inner body runs 256 times.
// Whether the driver unrolls is a heuristic call - the original runtime
// path baked RESOLUTION_SCALE as a literal so the GLSL frontend often
// unrolled there too. Behaviour parity is the goal; perf is at worst
// identical, at best better because pipeline-create-time specialisation
// gives the driver a cleaner IR than runtime-shaderlang generation did.
//
// Descriptor binding 1 matches ShaderGen's "textures start at 1"
// convention for Vulkan (m_single_sampler_descriptor_set_layout).

#version 450 core

layout(constant_id = 0) const uint RESOLUTION_SCALE = 1u;

layout(set = 0, binding = 1) uniform sampler2D samp0;

layout(location = 0) in VertexData {
  vec2 v_tex0;
};

layout(location = 0) out vec4 o_col0;

void main()
{
  vec3 color = vec3(0.0, 0.0, 0.0);
  uvec2 base_coords = uvec2(gl_FragCoord.xy) * uvec2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  for (uint offset_x = 0u; offset_x < RESOLUTION_SCALE; offset_x++)
  {
    for (uint offset_y = 0u; offset_y < RESOLUTION_SCALE; offset_y++)
      color += texelFetch(samp0, ivec2(base_coords + uvec2(offset_x, offset_y)), 0).rgb;
  }
  color /= float(RESOLUTION_SCALE * RESOLUTION_SCALE);
  o_col0 = vec4(color, 1.0);
}
