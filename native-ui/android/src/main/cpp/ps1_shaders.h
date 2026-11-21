#pragma once

namespace ps1ui {

inline constexpr const char* kQuadVertexShader = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

uniform vec2 u_screen;

out vec2 v_uv;
out vec4 v_color;

void main() {
    vec2 ndc = vec2(
        (a_pos.x / u_screen.x) * 2.0 - 1.0,
        1.0 - (a_pos.y / u_screen.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
}
)";

inline constexpr const char* kQuadFragmentShader = R"(#version 300 es
precision highp float;

uniform sampler2D u_texture;

in vec2 v_uv;
in vec4 v_color;

out vec4 o_color;

void main() {
    vec4 tex = texture(u_texture, v_uv);
    o_color = tex * v_color;
}
)";

// Full-screen PS1 BIOS Background + 3D Blinn-Phong Spheres
inline constexpr const char* kBgVertexShader = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
out vec2 v_uv;

void main() {
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

inline constexpr const char* kBgFragmentShader = R"(#version 300 es
precision highp float;

in vec2 v_uv;
uniform vec2 u_screen;
uniform float u_time;
uniform float u_motion_scale;
uniform int u_theme;
uniform vec2 u_parallax;

out vec4 o_color;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Raycast analytic 3D sphere for ultra-crisp edges and Blinn-Phong shading at any DPI
vec4 evalSphere(vec2 px, vec2 center, float radius, vec3 baseCol, vec3 rimCol, vec3 specCol, vec3 lightDir) {
    vec2 d = px - center;
    float dist = length(d);
    float aa = 1.35;
    float mask = 1.0 - smoothstep(radius - aa, radius + aa, dist);
    if (mask <= 0.001) {
        return vec4(0.0);
    }
    vec2 nxy = clamp(d / radius, -1.0, 1.0);
    float r2 = dot(nxy, nxy);
    float nz = sqrt(max(0.0001, 1.0 - r2));
    // Invert Y because screen px has Y pointing down
    vec3 normal = normalize(vec3(nxy.x, -nxy.y, nz));

    float diff = max(dot(normal, lightDir), 0.0);
    float wrapDiff = max(dot(normal, lightDir) * 0.65 + 0.35, 0.0);
    vec3 viewDir = vec3(0.0, 0.0, 1.0);
    vec3 halfVec = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfVec), 0.0), 26.0);
    float specSoft = pow(max(dot(normal, halfVec), 0.0), 6.0);
    float fresnel = pow(1.0 - nz, 2.6);

    // Subtle quantized PS1-inspired soft shading contour without harsh pixel jaggedness
    vec3 shade = baseCol * (0.14 + 0.88 * pow(wrapDiff, 1.25));
    shade += specCol * (spec * 0.42 + specSoft * 0.22);
    shade += rimCol * (fresnel * 0.24 * (0.35 + 0.65 * diff));

    // Soft ambient occlusion darkening around sphere perimeter
    float edgeDarken = smoothstep(1.0, 0.72, r2);
    shade *= mix(0.55, 1.0, edgeDarken);

    return vec4(shade, mask);
}

void main() {
    vec2 px = vec2(v_uv.x * u_screen.x, (1.0 - v_uv.y) * u_screen.y);
    vec2 center = u_screen * 0.5;
    float minDim = min(u_screen.x, u_screen.y);

    // Radial PS1 BIOS backdrop
    vec2 rel = (px - center) / vec2(max(u_screen.x * 0.56, 1.0), max(u_screen.y * 0.56, 1.0));
    float rad = length(rel);
    float glow = exp(-rad * rad * 1.45);
    float softBloom = smoothstep(1.18, 0.08, rad);

    vec3 bg = vec3(0.0);
    vec3 sphereBase = vec3(0.32, 0.34, 0.88);
    vec3 sphereRim = vec3(0.52, 0.58, 1.0);
    vec3 sphereSpec = vec3(0.86, 0.88, 1.0);

    if (u_theme == 0) {
        // Theme 0: Classic Blue (SCPH-7001 / 5501)
        vec3 outerNavy = vec3(0.0, 0.0, 0.115);
        vec3 midCobalt = vec3(0.11, 0.12, 0.48);
        vec3 innerBlue = vec3(0.47, 0.49, 0.99);
        bg = mix(outerNavy, midCobalt, pow(softBloom, 1.35));
        bg = mix(bg, innerBlue, pow(glow, 1.55) * 0.92);
    } else if (u_theme == 1) {
        // Theme 1: PSone Silver (SCPH-101)
        vec3 outerSilver = vec3(0.76, 0.80, 0.88);
        vec3 midIce = vec3(0.87, 0.90, 0.96);
        vec3 innerPearl = vec3(0.97, 0.985, 1.0);
        bg = mix(outerSilver, midIce, softBloom);
        bg = mix(bg, innerPearl, glow * 0.88);
        sphereBase = vec3(0.52, 0.62, 0.88);
        sphereRim = vec3(0.78, 0.88, 1.0);
        sphereSpec = vec3(1.0, 1.0, 1.0);
    } else {
        // Theme 2: Original Slate (SCPH-1001) with subtle 1994 diagonal diamond grid
        vec3 outerDark = vec3(0.035, 0.045, 0.085);
        vec3 innerSlate = vec3(0.11, 0.15, 0.25);
        bg = mix(outerDark, innerSlate, softBloom * 0.88);
        vec2 gp = vec2(px.x + px.y, px.x - px.y) * 0.032;
        vec2 cell = abs(fract(gp) - 0.5);
        float line = 1.0 - smoothstep(0.455, 0.495, max(cell.x, cell.y));
        bg += vec3(0.03, 0.055, 0.10) * line * (0.4 + 0.6 * softBloom);
        sphereBase = vec3(0.16, 0.42, 0.64);
        sphereRim = vec3(0.32, 0.78, 0.95);
        sphereSpec = vec3(0.82, 0.95, 1.0);
    }

    float t = u_time * u_motion_scale;
    float w = u_screen.x;
    float h = u_screen.y;
    vec3 lightDir = normalize(vec3(-0.32, 0.36, 0.87));

    // Gentle floating motion matching authentic serene BIOS vibe
    vec2 par = u_parallax * 14.0;
    vec2 drift1 = vec2(sin(t * 0.45) * 6.0, cos(t * 0.37) * 5.0) * u_motion_scale;
    vec2 drift2 = vec2(cos(t * 0.52) * 4.5, sin(t * 0.41) * 4.5) * u_motion_scale;
    vec2 drift3 = vec2(sin(t * 0.39 + 1.2) * 6.5, cos(t * 0.48 + 0.7) * 5.5) * u_motion_scale;
    vec2 drift4 = vec2(cos(t * 0.56 + 2.1) * 4.0, sin(t * 0.44 + 1.5) * 4.0) * u_motion_scale;

    // Top-left sphere pair
    vec2 cTL1 = vec2(w * 0.235, h * 0.105) + drift1 + par;
    float rTL1 = minDim * 0.112;
    vec2 cTL2 = vec2(w * 0.332, h * 0.195) + drift2 + par * 1.35;
    float rTL2 = minDim * 0.082;

    // Bottom-right sphere pair
    vec2 cBR1 = vec2(w * 0.472, h * 0.855) + drift3 + par;
    float rBR1 = minDim * 0.132;
    vec2 cBR2 = vec2(w * 0.368, h * 0.770) + drift4 + par * 1.35;
    float rBR2 = minDim * 0.068;

    // Draw back-to-front per pair so smaller front sphere overlaps cleanly like BIOS
    vec4 s1 = evalSphere(px, cTL1, rTL1, sphereBase * 0.92, sphereRim, sphereSpec, lightDir);
    bg = mix(bg, s1.rgb, s1.a);
    vec4 s2 = evalSphere(px, cTL2, rTL2, sphereBase * 1.03, sphereRim, sphereSpec, lightDir);
    bg = mix(bg, s2.rgb, s2.a);

    vec4 s3 = evalSphere(px, cBR1, rBR1, sphereBase * 0.95, sphereRim, sphereSpec, lightDir);
    bg = mix(bg, s3.rgb, s3.a);
    vec4 s4 = evalSphere(px, cBR2, rBR2, sphereBase * 1.06, sphereRim, sphereSpec, lightDir);
    bg = mix(bg, s4.rgb, s4.a);

    // Subtle dither to eliminate 8-bit gradient banding on large tablet displays
    float dither = (hash21(gl_FragCoord.xy) - 0.5) / 255.0;
    o_color = vec4(clamp(bg + dither, 0.0, 1.0), 1.0);
}
)";

// Procedural PS1 Rainbow Graffiti Splash Shader
inline constexpr const char* kGraffitiVertexShader = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;

uniform vec2 u_screen;

out vec2 v_uv;

void main() {
    vec2 ndc = vec2(
        (a_pos.x / u_screen.x) * 2.0 - 1.0,
        1.0 - (a_pos.y / u_screen.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = a_uv;
}
)";

inline constexpr const char* kGraffitiFragmentShader = R"(#version 300 es
precision highp float;

in vec2 v_uv;

uniform float u_seed;
uniform float u_focus;
uniform float u_alpha;
uniform float u_time;
uniform int u_theme;

out vec4 o_color;

float hash2(vec2 p) {
    p = fract(p * vec2(234.34, 567.21) + u_seed * 0.137);
    p += dot(p, p + 34.45);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash2(i);
    float b = hash2(i + vec2(1.0, 0.0));
    float c = hash2(i + vec2(0.0, 1.0));
    float d = hash2(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.52;
    for (int i = 0; i < 4; ++i) {
        v += a * noise(p);
        p = p * 2.07 + vec2(13.1, 7.7);
        a *= 0.5;
    }
    return v;
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    // Stretch noise horizontally for authentic brush-stroke / splatter silhouette
    vec2 p = v_uv * 2.0 - 1.0;
    float edgeDist = length(vec2(p.x * 0.92, p.y * 1.18));

    // Pixel-fleck crispness blended with smooth organic paint cloud
    vec2 qUv = floor(v_uv * vec2(132.0, 44.0)) / vec2(132.0, 44.0);
    float nSmooth = fbm(vec2(v_uv.x * 5.2 + u_seed * 3.1, v_uv.y * 3.6 + u_seed * 1.7));
    float nFleck = hash2(floor(v_uv * vec2(118.0, 40.0)));
    float nStreak = fbm(vec2(qUv.x * 11.0 - u_seed, qUv.y * 5.0));

    float body = (1.0 - smoothstep(0.32, 0.96, edgeDist)) + (nSmooth - 0.45) * 0.68 + (nStreak - 0.5) * 0.26;
    float mask = smoothstep(0.34, 0.52, body);

    // Scattered crisp spray paint droplets outside the core splash
    float dropRegion = (1.0 - smoothstep(0.65, 1.05, edgeDist)) * step(0.84, nFleck) * step(0.46, nSmooth);
    float alpha = clamp(max(mask, dropRegion * 0.92), 0.0, 1.0);
    if (alpha <= 0.01) {
        discard;
    }

    // Vibrant PS1 rainbow color stops with subtle focus shimmer
    float flow = sin(u_time * 1.6 + v_uv.x * 4.5) * 0.03 * u_focus;
    float hueBase = v_uv.x * 0.85 + (nSmooth - 0.5) * 0.34 + u_seed * 0.29 + flow;
    float sat = (u_theme == 1) ? 0.84 : 0.92;
    float val = mix(0.76, 1.0, u_focus) * mix(0.85, 1.0, nSmooth);
    vec3 col = hsv2rgb(vec3(fract(hueBase), sat, val));

    // Extra vibrant warm orange/magenta/cyan punch typical of PS1 BIOS splashes
    col = pow(col, vec3(0.92));
    if (u_focus > 0.01) {
        float sweep = exp(-pow((v_uv.x - fract(u_time * 0.35 + u_seed * 0.2)) * 5.0, 2.0));
        col += vec3(0.18, 0.20, 0.25) * sweep * u_focus;
    }

    o_color = vec4(col, alpha * mix(0.82, 0.98, u_focus) * u_alpha);
}
)";

// Beveled UI Panel / Plaque / Button Shader
inline constexpr const char* kPanelVertexShader = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;

uniform vec2 u_screen;
out vec2 v_uv;

void main() {
    vec2 ndc = vec2(
        (a_pos.x / u_screen.x) * 2.0 - 1.0,
        1.0 - (a_pos.y / u_screen.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = a_uv;
}
)";

inline constexpr const char* kPanelFragmentShader = R"(#version 300 es
precision highp float;

in vec2 v_uv;

uniform vec2 u_size;
uniform float u_radius;
uniform float u_border_px;
uniform vec4 u_top_col;
uniform vec4 u_bot_col;
uniform vec4 u_border_col;
uniform vec4 u_glow_col; // xyz = glow color, w = glow strength (concentric plaque glow)

out vec4 o_color;

float sdRoundedBox(vec2 p, vec2 halfSize, float r) {
    vec2 q = abs(p) - halfSize + vec2(r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec2 px = v_uv * u_size;
    vec2 center = u_size * 0.5;
    float r = clamp(u_radius, 0.0, min(center.x, center.y) - 0.5);
    float d = sdRoundedBox(px - center, center, r);

    float aa = 1.0;
    float outerAlpha = 1.0 - smoothstep(-aa, aa, d);
    if (outerAlpha <= 0.001) {
        discard;
    }

    vec4 fill = mix(u_top_col, u_bot_col, v_uv.y);

    // Optional concentric PS1 BIOS header glow
    if (u_glow_col.w > 0.001) {
        vec2 ell = (v_uv - 0.5) * vec2(1.4, 2.0);
        float ellDist = length(ell);
        float steps = floor((1.0 - clamp(ellDist, 0.0, 1.0)) * 7.0) / 7.0;
        float smoothGlow = exp(-dot(ell, ell) * 2.2);
        fill.rgb = mix(fill.rgb, u_glow_col.rgb, mix(smoothGlow, steps, 0.32) * u_glow_col.w);
    }

    // Crisp subtle top bevel sheen
    float topSheen = (1.0 - smoothstep(0.0, 0.25, v_uv.y)) * 0.09;
    fill.rgb += vec3(topSheen);

    if (u_border_px > 0.05) {
        float innerMask = 1.0 - smoothstep(-u_border_px - aa, -u_border_px + aa, d);
        vec3 col = mix(u_border_col.rgb, fill.rgb, innerMask);
        float alpha = mix(u_border_col.a, fill.a, innerMask) * outerAlpha;
        o_color = vec4(col, alpha);
    } else {
        o_color = vec4(fill.rgb, fill.a * outerAlpha);
    }
}
)";

// Spinning Black-Bottom PS1 Optical Disc Shader
inline constexpr const char* kDiscVertexShader = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;

uniform vec2 u_screen;
out vec2 v_uv;

void main() {
    vec2 ndc = vec2(
        (a_pos.x / u_screen.x) * 2.0 - 1.0,
        1.0 - (a_pos.y / u_screen.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = a_uv;
}
)";

inline constexpr const char* kDiscFragmentShader = R"(#version 300 es
precision highp float;

in vec2 v_uv;
uniform float u_angle;
uniform float u_alpha;
uniform int u_theme;

out vec4 o_color;

void main() {
    vec2 p = v_uv * 2.0 - 1.0;
    float r = length(p);
    float aa = 0.014;

    if (r > 1.0 || r < 0.145) {
        discard;
    }

    float outerMask = 1.0 - smoothstep(1.0 - aa, 1.0, r);
    float holeMask = smoothstep(0.145, 0.145 + aa, r);
    float mask = outerMask * holeMask;

    float ang = atan(p.y, p.x) + u_angle;

    // Iconic PS1 obsidian black data surface with rotating rainbow diffraction bands
    vec3 col = vec3(0.035, 0.04, 0.075);

    if (r > 0.965) {
        // Metallic outer rim bevel
        col = vec3(0.68, 0.72, 0.80);
    } else if (r > 0.36) {
        // Black polycarbonate surface + iridescent rainbow light cones
        float cone1 = pow(max(0.0, cos(ang * 2.0)), 6.0);
        float cone2 = pow(max(0.0, cos(ang * 2.0 + 1.57)), 8.0);
        vec3 irid1 = vec3(0.26, 0.38, 0.95) * cone1 * 0.55;
        vec3 irid2 = vec3(0.75, 0.22, 0.88) * cone2 * 0.42;
        float trackRings = 0.04 * sin(r * 160.0);
        col = vec3(0.035, 0.040, 0.078) + irid1 + irid2 + vec3(trackRings * 0.25);
        // Specular wipe across disc
        float spec = pow(max(0.0, cos(ang - 0.7)), 14.0) * 0.38;
        col += vec3(0.55, 0.65, 1.0) * spec;
    } else if (r > 0.255) {
        // Inner silver/mirror clamping ring
        float ringSpec = 0.72 + 0.22 * cos(ang * 2.0);
        col = vec3(0.45, 0.50, 0.64) * ringSpec;
    } else {
        // Clear inner plastic hub
        col = vec3(0.24, 0.28, 0.42);
    }

    // Thin crisp separation lines between zones
    float rimLine = exp(-pow((r - 0.36) * 90.0, 2.0)) * 0.35;
    float hubLine = exp(-pow((r - 0.255) * 90.0, 2.0)) * 0.35;
    col += vec3(0.6, 0.7, 0.95) * (rimLine + hubLine);

    o_color = vec4(col, mask * u_alpha);
}
)";

// Optional Subtle CRT Scanlines Overlay
inline constexpr const char* kScanlineFragmentShader = R"(#version 300 es
precision highp float;

in vec2 v_uv;
uniform vec2 u_viewport_px;
uniform float u_strength;

out vec4 o_color;

void main() {
    float yPx = v_uv.y * u_viewport_px.y;
    // Period ~3-4 physical pixels so scanlines stay clean on 1080p & 2000p
    float line = 0.5 - 0.5 * cos(yPx * 3.14159265 * 0.65);
    float dark = line * u_strength;
    vec2 d = v_uv - 0.5;
    float vig = dot(d, d) * u_strength * 0.75;
    float alpha = clamp(dark + vig, 0.0, 0.55);
    o_color = vec4(0.0, 0.0, 0.05, alpha);
}
)";

} // namespace ps1ui
