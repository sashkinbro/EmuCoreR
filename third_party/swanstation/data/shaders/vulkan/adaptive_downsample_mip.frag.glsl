// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateAdaptiveDownsampleMipFragmentShader(first_pass).
//
// Vulkan-only port. Originally "mipmap_energy.glsl" from parallel-rsx.
// Measures local "energy" (variance) across a 2x2 footprint and writes a
// scalar bias used downstream to choose mip levels.
//
// The 'first_pass' parameter that previously produced two distinct GLSL
// source strings (and thus two cached SPIR-V blobs) is now a single bool
// specialization constant at id=100. One SPIR-V blob serves both
// GetDownsampleFirstPassPipeline (Adaptive branch) and
// GetDownsampleMidPassPipeline; each binds a different
// VkSpecializationInfo at vkCreateGraphicsPipelines time.
//
// Spec-constant ID allocation (project-wide convention):
//   0-99 : common knobs (RESOLUTION_SCALE, MULTISAMPLES, ...) - not used here
//   100+ : shader-specific. FIRST_PASS lives at 100.

#version 450 core

layout(constant_id = 100) const bool FIRST_PASS = false;

layout(set = 0, binding = 0) uniform sampler2D samp0;

layout(push_constant) uniform PushConstants {
  vec2 u_uv_min;
  vec2 u_uv_max;
  vec2 u_rcp_resolution;
};

layout(location = 0) in VertexData {
  vec2 v_tex0;
};

layout(location = 0) out vec4 o_col0;

// Two overloads of get_bias - the rgb form is used by the first pass (input
// is the raw downsampled color so we have no prior alpha bias to carry),
// the rgba form is used by subsequent passes which feed back the bias in
// the alpha channel.
vec4 get_bias(vec3 c00, vec3 c01, vec3 c10, vec3 c11)
{
  vec3 avg = 0.25 * (c00 + c01 + c10 + c11);
  float s00 = dot(c00 - avg, c00 - avg);
  float s01 = dot(c01 - avg, c01 - avg);
  float s10 = dot(c10 - avg, c10 - avg);
  float s11 = dot(c11 - avg, c11 - avg);
  return vec4(avg, 1.0 - log2(1000.0 * (s00 + s01 + s10 + s11) + 1.0));
}

vec4 get_bias(vec4 c00, vec4 c01, vec4 c10, vec4 c11)
{
  float avg = 0.25 * (c00.a + c01.a + c10.a + c11.a);
  vec4 bias = get_bias(c00.rgb, c01.rgb, c10.rgb, c11.rgb);
  bias.a *= avg;
  return bias;
}

void main()
{
  vec2 uv = v_tex0 - (u_rcp_resolution * 0.25);
  // The spec-constant bool turns the runtime 'if' into a compile-time
  // selection at pipeline-creation; the dead branch is dropped by the
  // driver's optimizer.
  if (FIRST_PASS)
  {
    vec3 c00 = textureOffset(samp0, uv, ivec2(0, 0)).rgb;
    vec3 c01 = textureOffset(samp0, uv, ivec2(0, 1)).rgb;
    vec3 c10 = textureOffset(samp0, uv, ivec2(1, 0)).rgb;
    vec3 c11 = textureOffset(samp0, uv, ivec2(1, 1)).rgb;
    o_col0 = get_bias(c00, c01, c10, c11);
  }
  else
  {
    vec4 c00 = textureOffset(samp0, uv, ivec2(0, 0));
    vec4 c01 = textureOffset(samp0, uv, ivec2(0, 1));
    vec4 c10 = textureOffset(samp0, uv, ivec2(1, 0));
    vec4 c11 = textureOffset(samp0, uv, ivec2(1, 1));
    o_col0 = get_bias(c00, c01, c10, c11);
  }
}
