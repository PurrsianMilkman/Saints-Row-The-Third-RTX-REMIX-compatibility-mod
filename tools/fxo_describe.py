"""Write one plain-text description per disassembled .fxo_pc shader container.

Reads the facts file produced by tools/fxo_batch_disasm.py, classifies every
shader blob inside every container by the render pass it serves, and emits
<name>.txt describing what the effect is and how it works.

Everything structural in the output (samplers, registers, vertex streams,
instruction counts, pass inventory) is measured from the disassembly. The prose
comes from a knowledge base built by reading representative shaders of each
family; where a family has no bespoke entry the text is composed from the
filename tokens, which in this library are systematic.

Usage:  python fxo_describe.py <facts.json> <asm_dir> <out_dir>
"""
import json
import os
import re
import sys
import textwrap

WRAP = textwrap.TextWrapper(width=96, initial_indent="  ", subsequent_indent="  ")
WRAP_I = textwrap.TextWrapper(width=96, initial_indent="      ", subsequent_indent="      ")

# ---------------------------------------------------------------------------
# Engine-wide background, reproduced at the top of every material description.
# ---------------------------------------------------------------------------

INFERRED_LIGHTING = """\
Saints Row: The Third renders with Volition's Inferred Lighting (the `IR_` prefix on the shared \
constants). A frame goes: (1) a geometry prepass writes view-space normal, linear depth, specular \
power and a per-object "DSF" discontinuity id into a multiple-render-target G-buffer; (2) every \
light draws a volume that reads that G-buffer and accumulates into a *lower resolution* light \
buffer (the L-buffer); (3) each material redraws its geometry and resolves its own shading by \
reading the L-buffer back through a discontinuity-sensitive filter (DSF) that rejects L-buffer \
taps whose id or depth disagrees with the pixel being shaded. That is why nearly every material \
container here holds two families of pixel shader: short ones that write the G-buffer MRT, and \
long ones that sample IR_LBufferSampler (s12) and IR_GBuffer_DSF_DataSampler (s9)."""

SHARED_REGISTERS = """\
Register assignment is invariant across the whole library: projTM (c28, 4 regs) is the fused \
VIEW*PROJECTION matrix, objTM (c32, 3 regs) is the object-to-world affine, IR_World2View (c48, \
3 regs) is the pure view matrix, eyePos is c41, the 64-bone skinning palette is c52 (3 registers \
per bone), Object_instance_params is c35 and Object_instance_params_2 is c36. Texture coordinates \
arrive as 16-bit integers and are scaled by 1/1024 in the vertex shader, so the UV formula is \
uv = raw * tiling / 1024, with the tiling uniforms living at different registers in different \
shaders."""

# ---------------------------------------------------------------------------
# Geometry variant suffixes. Confirmed from the dcl_ declarations of the
# corresponding vertex shaders (see ir_bbsimple1_* which has all eight).
# ---------------------------------------------------------------------------

VARIANTS = {
    "s": (
        "static / rigid",
        "Plain rigid geometry: no skinning, no morphing, no instancing. The world transform is the "
        "uniform objTM at c32 and there is no per-vertex transform data. The streams this "
        "particular effect declares are listed below - typically position, texcoord, normal and "
        "tangent, fewer where the effect needs less.",
    ),
    "bs": (
        "batched / hardware-instanced static",
        "Instanced static geometry. On top of the rigid streams the vertex declaration adds "
        "position2/position3/position4 (v4/v5/v6) and color2 (v7). The three extra POSITION "
        "streams are the rows of a per-instance 4x3 world matrix - the shader does "
        "dp4 r1.xyz, v4/v5/v6, pos instead of reading objTM at c32 - and color2.x carries the "
        "per-instance DSF id. This is the variant most of the city is drawn with, and it is the "
        "one where the object's world matrix is NOT in a constant register.",
    ),
    "ms": (
        "morphed static",
        "Rigid geometry with vertex morphing. Adds position1 (v4), color (v5) and normal1 (v6): a "
        "second position/normal target that is blended against the base by a per-vertex weight "
        "before the object transform.",
    ),
    "c": (
        "skinned character",
        "Smooth-skinned geometry. Adds blendweight (v4) and blendindices (v5); the vertex shader "
        "does mova a0.x and indexes the 64-bone palette at c52 (three float4 registers per bone), "
        "accumulating the weighted bone transforms before applying objTM. All animation lives in "
        "those constants - the vertex buffer holds the bind pose only.",
    ),
    "mc": (
        "morphed skinned character",
        "Smooth skinning plus morph targets: position1 (v4) and normal1 (v5) are blended in before "
        "the bone palette at c52 is applied. This is what the character customisation meshes use, "
        "where body shape is a morph and animation is the skinning palette.",
    ),
    "v": (
        "rigid-bone / vehicle",
        "One-bone rigid attachment. The declaration has blendindices (v4) but no blendweight: each "
        "vertex names exactly one bone in the c52 palette and is transformed by it, then by objTM. "
        "This is how vehicle parts, doors, wheels and other hierarchies of rigid pieces are drawn.",
    ),
    "mv": (
        "morphed rigid-bone / vehicle",
        "Rigid one-bone attachment (blendindices, no weights) combined with morph targets "
        "position1/normal1 - vehicle parts that also deform, most obviously crash damage.",
    ),
    "fd": (
        "pre-transformed / world-space batch",
        "Vertices arrive already in world space: the shader feeds position straight into projTM "
        "with no objTM and no bone palette at all, and takes an extra color1 (v4) stream that "
        "carries the per-vertex DSF id and fade. Used for geometry that has been baked into large "
        "shared buffers.",
    ),
}

# ---------------------------------------------------------------------------
# Sampler roles. Stage numbers are measured per file; the text is the role.
# ---------------------------------------------------------------------------

SAMPLER_ROLES = {
    "Diffuse_Map": "base albedo/diffuse texture; its alpha is the opacity or cutout channel",
    "Diffuse_map": "base albedo/diffuse texture",
    "Diffuse_mapSampler": "base albedo/diffuse texture",
    "Diffuse_Map_1": "first albedo layer",
    "Diffuse_Map_2": "second albedo layer, blended against the first",
    "Normal_Map": "tangent-space normal map; the shader reconstructs z and scales the xy tilt by "
                  "Normal_Map_Height",
    "Normal_map": "tangent-space normal map",
    "Normal_mapSampler": "tangent-space normal map",
    "Normal_Map_2": "second normal map for the blended/second material layer",
    "Detail_Normal_Map": "high-frequency detail normal, tiled far tighter than the base normal",
    "Det_Normal_Map": "detail normal map",
    "Specular_Map": "specular intensity/colour mask",
    "Specular_map": "specular intensity/colour mask",
    "Specular_mapSampler": "specular intensity/colour mask",
    "Specular_Map_2": "specular mask for the second material layer",
    "Decal_Map": "decal layer composited over the base albedo",
    "Decal_Map_2": "second decal layer",
    "Decal2_Map": "second decal layer",
    "Decal_Map_1": "first decal layer",
    "Decal_diffuse_map": "the projected dynamic-decal texture (bullet holes, blood, sprays)",
    "Decal_Normal_Map": "normal map belonging to the decal layer",
    "Damage_Normal_Map": "vehicle damage normal map - the dents deform the paint's normal only",
    "Blend_Map": "per-texel blend weight between the two material layers",
    "Mask_Map": "generic mask",
    "Mask_Map_1": "particle mask/erosion texture",
    "Alpha_Map": "separate opacity mask, kept out of the albedo's alpha",
    "Alpha_Mask": "separate opacity mask",
    "Pattern_Map": "clothing pattern mask - the three channels select between Diffuse_Color_a/b/c",
    "Sphere_Map": "sphere/matcap environment map indexed by the view-space normal",
    "Sphere_Map_1": "first sphere/matcap environment map",
    "Sphere_Map_2": "second sphere/matcap environment map",
    "Viewsphere_Map": "view-sphere (matcap) map used instead of a real reflection probe",
    "Reflection_Map": "static reflection texture (cube or 2D) for the material",
    "Reflection_Mask": "per-texel mask controlling how reflective each texel is",
    "Reflection_Mask_Map": "per-texel reflectivity mask",
    "ref_Mask_Map": "per-texel reflectivity mask",
    "Planar_Reflection_Map": "the planar (mirror) reflection render target, sampled in screen space",
    "Dual_Paraboloid_Map_Front": "front half of the dual-paraboloid environment probe",
    "Dual_Paraboloid_Map_Back": "back half of the dual-paraboloid environment probe",
    "Single_Paraboloid_Map": "single-paraboloid environment probe",
    "Environment_Map": "environment reflection map",
    "Illumination_Map": "emissive/self-illumination texture, usually scrolled by a UV offset",
    "Night_Additive_Map": "time-of-day additive layer - the lit windows and signage that appear "
                          "only at night, scaled by Night_Additive_Color",
    "Glow_Map": "glow/emissive layer",
    "Glow_Map_1": "first glow layer",
    "Glow_Map_2": "second glow layer",
    "Glow_Mask_Map": "mask selecting which texels glow",
    "glow_Mask_Map": "mask selecting which texels glow",
    "Glow_Color_Map": "per-texel glow colour",
    "grime_map": "grime/dirt overlay, tiled independently of the albedo",
    "Grime_Map": "grime/dirt overlay, tiled independently of the albedo",
    "Dirt_Map": "dirt overlay",
    "Dirt_Rust_Map": "dirt and rust overlay for damaged vehicle surfaces",
    "Dirt_Reveal_Map": "mask that reveals the dirt layer",
    "Distortion_Map": "screen distortion offsets (heat haze, shockwaves)",
    "Distortion_Map_1": "screen distortion offsets",
    "Gradient_Map": "1D gradient lookup",
    "Detail_Map": "detail albedo tiled over the base",
    "Dob_Map": "hair depth/occlusion-of-bangs map used to sort and shade the hair cards",
    "Y_tex": "luma plane of a Bink video frame",
    "Cr_tex": "Cr chroma plane of a Bink video frame",
    "Cb_tex": "Cb chroma plane of a Bink video frame",
    "Orbital_map": "the orbital body (moon/planet) texture in the sky dome",
    "Layer01_map": "cloud layers 0 and 1, packed two per texture",
    "Layer23_map": "cloud layers 2 and 3, packed two per texture",
    "Frame_Buffer": "the back buffer read back for a screen-space effect",
    "framebuffer_map": "the back buffer read back as an input",
    "IR_LBuffer": "the low-resolution inferred-lighting light buffer (rgb = diffuse light, "
                  "a = specular)",
    "Lbuffer": "the inferred-lighting light buffer",
    "IR_GBuffer_DSF_Data": "the DSF id/depth buffer used to reject L-buffer taps across "
                           "discontinuities",
    "IR_GBuffer_Normals": "G-buffer view-space normals",
    "Gbuffer_Normals": "G-buffer view-space normals",
    "IR_GBuffer_Depth": "G-buffer linear depth",
    "Gbuffer_Depth": "G-buffer linear depth",
    "IR_GBuffer_Lighting": "the accumulated lighting target the light volumes write into",
    "IR_Stipple_Pattern_2D": "the stipple/dither pattern atlas used for screen-door fades",
    "ir_shadow_map": "the light's shadow map",
    "ir_ambient_occlusion": "the ambient-occlusion buffer folded into the ambient term",
    "Projection_texture": "the light's projected cookie/gobo texture",
    "Depth_buffer": "the scene depth buffer, read for soft-particle depth fade",
    "Depth_map": "scene depth",
    "depth_sampler": "scene depth",
    "Depth_bufferSampler": "scene depth",
    "Shadow_map": "the shadow depth map being projected",
    "base_sampler": "the source image for this post-processing pass",
    "Base_sampler": "the source image for this post-processing pass",
    "backbuffer_sampler": "the back buffer",
    "backbuffer_texture": "the back buffer",
    "base_texture": "the source image",
    "Base_texture": "the source image",
    "ssao_tex": "the screen-space ambient occlusion buffer",
    "ssao_sampler": "the SSAO buffer being blurred",
    "ssao_prev_tex": "last frame's SSAO buffer, reprojected for temporal accumulation",
    "rao_tex": "the ray-traced/analytic ambient-occlusion buffer",
    "particle_sampler": "the offscreen particle buffer being composited back",
    "edgemap_sampler": "the edge mask marking where the low-res particle buffer needs fixing up",
    "bloom_sampler": "the bloom buffer",
    "Lum_adapt_sampler": "the 1x1 adapted-luminance texture carried between frames",
    "Lut_sampler_2d": "the colour-grading lookup table",
    "rgb_texture": "the RGB source being converted to YUV",
    "texture_sampler": "the source texture being resampled",
    "damping_map": "per-texel damping for the water simulation",
    "frame_n_tex": "the water height field at frame n",
    "frame_n1_tex": "the water height field at frame n-1",
    "Normal_Map_A": "wave normal layer A",
    "Normal_Map_B": "wave normal layer B",
    "Normal_Map_C": "wave normal layer C",
    "Normal_Map_D": "wave normal layer D",
    "comp_normal_map": "the composited character normal map being written",
    "skin_map": "skin tone source for the character compositor",
    "mask_map": "region mask for the character compositor",
    "body_female": "female body morph normal source",
    "body_male": "male body morph normal source",
    "skinny": "skinny body morph normal source",
    "fat": "fat body morph normal source",
    "muscle": "muscular body morph normal source",
    "body_age": "body ageing normal source",
    "face_age": "face ageing normal source",
    "base": "the base normal map the character morphs are blended onto",
    "diffuse_map": "the diffuse source for the character compositor",
    "low_coc_sampler": "the far/low circle-of-confusion blur",
    "high_coc_sampler": "the near/high circle-of-confusion blur",
    "distortion_sampler": "the distortion offset buffer",
    "blurred_sampler": "the blurred copy of the scene",
    "Bloom_stage_0_sampler": "bloom pyramid level 0",
    "Bloom_stage_1_sampler": "bloom pyramid level 1",
    "Bloom_stage_2_sampler": "bloom pyramid level 2",
    "Bloom_add_texture_0_sampler": "an extra buffer added into the bloom",
    "Bloom_add_texture_1_sampler": "an extra buffer added into the bloom",
    "Light_shafts_brightpass_sampler": "the bright-pass buffer the light shafts are traced through",
    "Blending_lut_sampler_0": "colour-grading LUT 0",
    "Blending_lut_sampler_1": "colour-grading LUT 1",
    "Blending_lut_sampler_2": "colour-grading LUT 2",
    "Blending_lut_sampler_3": "colour-grading LUT 3",
    "Depth_buffer_sampler": "the depth buffer the ray-cast reads",
    "composite_texture": "the simulated water field being composited in",
    "0": "the first unnamed source texture",
    "1": "the second unnamed source texture",
}

# ---------------------------------------------------------------------------
# Constant roles. Register numbers are measured per file.
# ---------------------------------------------------------------------------

CONST_ROLES = {
    "projTM": "fused VIEW*PROJECTION matrix (not a pure projection despite the name)",
    "objTM": "object-to-world affine transform, 4x3",
    "IR_World2View": "world-to-view affine transform, 4x3",
    "eyePos": "camera position in world space",
    "Bone_weights": "64-bone skinning palette, three float4 registers per bone",
    "Object_instance_params": "per-instance colour/parameter multiplier applied to Diffuse_Color",
    "Object_instance_params_2": ".x is the per-instance DSF id packed into the G-buffer",
    "Fog_dist": "fog parameters: height falloff, distance scale and start offset",
    "Fog_color": "fog colour the final result is lerped towards by the vertex fog factor",
    "Tint_color": "global tint multiplied into the final colour of every material pass",
    "Target_dimensions": "render-target size in pixels, used to build the half-texel offset",
    "IR_Pixel_Steps": "L-buffer texel size (.xy) and L-buffer resolution (.zw) for the DSF fetch",
    "IR_Similarity_Data": "thresholds and scale/bias for the DSF similarity comparison",
    "IR_Stipple_Pattern_Offset": "offset into the stipple pattern atlas and its reciprocal count",
    "IR_Stipple_Repeat_Info": "screen-space repeat/scale of the stipple pattern",
    "Alpha_Threshold": "cutout threshold; texkill(alpha - threshold) does the alpha test",
    "Alpha_test_use_dsf": "boolean: fold the alpha test through the DSF coverage instead of a "
                          "hard cut",
    "Specular_Power": "specular exponent, written into the G-buffer for the lighting pass",
    "Specular_Color": "specular tint",
    "Specular_Alpha": "how much of the L-buffer specular term is applied",
    "Specular_Map_Amount": "scales the specular map's contribution",
    "Self_Illumination": "constant emissive term added as albedo * Self_Illumination",
    "Normal_Map_Height": "scales the tangent-space normal's xy, i.e. bump strength",
    "Normal_Height": "scales the tangent-space normal's xy, i.e. bump strength",
    "Diffuse_Color": "material albedo tint, multiplied by Object_instance_params per instance",
    "Diffuse_Color_2": "albedo tint of the second material layer or two-tone half",
    "Normal_Map_TilingU": "U tiling for the normal map; uv = raw * tiling / 1024",
    "Normal_Map_TilingV": "V tiling for the normal map; uv = raw * tiling / 1024",
    "Decal_Map_TilingU": "U tiling for the decal layer",
    "Decal_Map_TilingV": "V tiling for the decal layer",
    "Decal_Map_Opacity": "decal blend strength",
    "Decal_Color": "the colour a distance-field decal is tinted with",
    "Decal_Clamp_UV_1": "min/max UV the decal is clamped to so it does not tile",
    "Decal_Clamp_UV_2": "min/max UV for the second decal",
    "Reflection_Plane_Height": "the mirror plane the planar-reflection pass reflects about and "
                               "clips against",
    "Dual_Paraboloid_Transform": "the paraboloid warp applied when rendering into an environment "
                                  "probe",
    "V_ambient_render": "sky ambient colour for the cheap forward-lit pass",
    "V_back_ambient_render": "ground ambient colour for the cheap forward-lit pass",
    "lightPos": "key light direction for the cheap forward-lit pass",
    "V_light_color": "key light colour for the cheap forward-lit pass",
    "Time": "engine time, used to drive scrolling and pulsing",
    "Base_Paint_Color": "the vehicle's body paint colour",
    "Fresnel_Color": "colour added at grazing angles",
    "Fresnel_Min_Cos_Angle": "cosine at which the Fresnel term starts",
    "Fresnel_Inverse_Cos_Angle_Range": "reciprocal width of the Fresnel ramp",
    "Fresnel_Strength": "strength of the rim/Fresnel term",
    "Reflection_Cos_Min_Angles": "cosine at which the reflection term starts",
    "Reflection_Inv_Range_Cos_Angles": "reciprocal width of the reflection ramp",
    "Ambient_Reflection_Amount": "how much of the environment probe survives into ambient",
    "Reflection_Amount": "overall reflection strength",
    "Reflection_Map_Opacity": "reflection blend strength",
    "Spec_Reflect": "how strongly the reflection tracks the specular mask",
    "Glass_Color": "glass tint",
    "Glass_Opacity": "glass base opacity",
    "Opacity_fade": "global fade applied to the whole surface, used for pop-in and dissolve",
    "grime_amount": "grime blend strength",
    "Grime_Amount": "grime blend strength",
    "grime_override": "forces the grime layer on or off",
    "grime_tiling_u": "U tiling of the grime layer",
    "grime_tiling_v": "V tiling of the grime layer",
    "Grime_Tiling_U": "U tiling of the grime layer",
    "Grime_Tiling_V": "V tiling of the grime layer",
    "Viewsphere_Amount": "how much of the view-sphere matcap is blended in",
    "Sphere_Map_Amount": "how much of the sphere/matcap map is blended in",
    "Diffuse_Color_a": "clothing colour A, selected by the pattern mask's first channel",
    "Diffuse_Color_b": "clothing colour B, selected by the pattern mask's second channel",
    "Diffuse_Color_c": "clothing colour C, selected by the pattern mask's third channel",
    "Pattern_Map_TilingU": "U tiling of the clothing pattern",
    "Pattern_Map_TilingV": "V tiling of the clothing pattern",
    "Night_Additive_Color": "colour and strength of the night-time additive layer",
    "TOD_window_tint": "time-of-day tint applied to window glass",
    "TOD_Light_Dir": "time-of-day sun direction used by the sky shaders",
    "TOD_Light_Color_Front": "time-of-day light colour on the sun-facing side of a cloud layer",
    "TOD_Light_Color_Rear": "time-of-day light colour on the shaded side of a cloud layer",
    "Illumination_Map_Amount": "emissive strength",
    "Illumination_Map_Scroll": "UV scroll rate of the emissive layer",
    "Diffuse_Map_Scroll": "UV scroll rate of the albedo",
    "Normal_Map_Scroll": "UV scroll rate of the normal map",
    "Ground_reflection_coef": "global wetness/ground-reflection coefficient set by the weather",
    "Diffuse_scale_wet": "how much darker the albedo goes when wet",
    "Spec_power_wet": "specular exponent to use when wet",
    "Wet_mask_levels": "remap of the wetness mask",
    "Normal_map_refl_offset": "how far the normal map perturbs the planar-reflection lookup",
    "Render_offset": "sub-pixel offset of the render target, needed to line screen-space "
                      "lookups up",
    "Dyn_decal_pos": "world position of the projected dynamic decal",
    "Dyn_decal_rvec": "right axis of the decal projector",
    "Dyn_decal_uvec": "up axis of the decal projector",
    "Dyn_decal_fvec": "forward axis of the decal projector",
    "Dyn_decal_scale": "extent of the decal projector box",
    "Dyn_decal_tint": "decal tint",
    "Dyn_decal_params_1": "decal fade/angle parameters",
    "Dyn_decal_params_2": "decal fade/angle parameters",
    "IR_Light_Pos": "light position in view space - or the light direction, for directional lights",
    "IR_Light_Dir": "light direction in view space",
    "IR_Light_Color": "light colour",
    "IR_light_back_color": "the colour used on surfaces facing away, a cheap translucency term",
    "IR_Light_Info": "(falloff exponent, inner radius, outer radius, -); attenuation is "
                     "pow(saturate(1 - (d - inner)/(outer - inner)), exponent)",
    "IR_Spot_Info": ".y is cos(outer cone angle) and .z is 1/(cos inner - cos outer)",
    "IR_Light_Inv_Proj_TM": "inverse projection used to rebuild view-space position from depth",
    "Link_categories": "which lighting categories this light is allowed to affect",
    "Shadow_map_enabled": "boolean gate around the shadow lookup",
    "Shadow_map_projTM": "the shadow map's view-projection",
    "Shadow_map_size": "shadow map resolution, for the filter kernel",
    "Shadow_map_fade_params": "distance over which the shadow fades out",
    "Projection_texture_xform": "transform mapping view-space position into the cookie texture",
    "Tree_wind_dir": "wind direction for the tree animation",
    "Tree_wind_times": "phase offsets of the wind oscillators",
    "Tree_wind_distances": "amplitude of the wind displacement per level",
    "Tree_wind_gust": "current gust strength",
    "Tree_wind_gust_hints": "per-vertex gust response scaling",
    "Tree_wind_leaves": "leaf flutter amplitude",
    "Tree_wind_frond_ripple": "frond ripple amplitude",
    "Sampling_offsets": "the tap offsets of this filter kernel",
    "Separated_axis": "which axis this separable blur pass runs along",
    "Blur_scale": "blur strength",
    "Blur_clamp": "maximum blur displacement",
    "Depth_map_scale": "scale converting the sampled depth back to view depth",
    "Near_clip_params": "near-plane constants for depth reconstruction",
    "Focal_params": "focal distance and range for depth of field",
    "Coc_range_params": "circle-of-confusion range mapping",
    "Curr_to_prev": "current-frame to previous-frame reprojection matrix",
    "Radial_blur_position": "screen-space centre of the radial blur",
    "Radial_blur_radius": "radius of the radial blur",
    "Luminance_conversion": "RGB to luminance weights",
    "Adapt_time": "eye-adaptation time constant",
    "Desired_brightness": "target brightness the auto-exposure aims for",
    "Exposure_min": "exposure clamp, low end",
    "Exposure_max": "exposure clamp, high end",
    "Bloom_amount": "bloom blend strength",
    "Bloom_curve_values": "the bloom threshold/knee curve",
    "Light_shafts_light_pos": "screen-space sun position for the light-shaft pass",
    "Color_correct_matrix": "3x4 colour-correction matrix applied to particles",
    "Inv_proj_matrix": "inverse projection, for rebuilding view position from the depth buffer",
    "Z_dimensions": "near/far constants for linearising the depth buffer",
    "Effect_opacity": "the effect instance's overall opacity",
    "VFX_material_tint": "per-effect tint",
    "UV_anim_tiling": "flipbook tiling and animation rate",
    "Parametric_particle_constants": "the particle's parametric size/rotation/colour curve inputs",
    "Eye_orient": "camera orientation, used to face billboards and orient sky elements",
    "Ambient_color": "ambient colour applied to the particle",
    "Light_vec": "the key light direction used to shade lit particles",
    "Diffuse_color": "particle diffuse tint",
    "ssao_inv_proj": "inverse projection for rebuilding view position in the SSAO pass",
    "ssao_sample_radius_reference": "world-space radius the SSAO samples span",
    "ssao_blue_noise_offsets": "the blue-noise rotation offsets of the sample kernel",
    "ssao_interleave_offsets": "per-pixel interleaved sampling offsets",
    "ssao_temporal_falloff": "how fast the temporal SSAO history decays",
    "ssao_curr_to_prev_xform": "reprojection into the previous frame's SSAO buffer",
    "rao_inv_proj": "inverse projection for the analytic ambient-occlusion pass",
    "rao_strength": "strength of the analytic occlusion",
    "rao_offset_radii": "radii of the analytic occluder",
    "Prim_resolution_scale": "scale mapping 2D primitive coordinates onto the render target",
    "Prim_tex_texel_size": "texel size of the primitive's texture, for the half-texel offset",
    "Draw_Color": "flat colour this editor primitive is drawn in",
    "Hair_Parameters": "hair shift/exponent parameters for the anisotropic highlight",
    "Hair_Spec_Alpha": "hair specular strength",
    "Hair_Spec_Color1": "colour of the primary anisotropic hair highlight",
    "Hair_Spec_Color2": "colour of the secondary anisotropic hair highlight",
    "Rim_Color": "rim-light colour",
    "Rim_Start": "cosine at which the rim light starts",
    "Rim_End": "cosine at which the rim light saturates",
    "Rim_Brightness": "rim-light strength",
    "Room_Depth": "apparent depth of the faux interior parallax box",
    "ClampU": "UV clamp bound",
    "ClampV": "UV clamp bound",
    "ClampU1": "UV clamp bound",
    "ClampV1": "UV clamp bound",
    "ClampU2": "UV clamp bound",
    "ClampV2": "UV clamp bound",
    "Base_Reflectivity": "reflectivity at normal incidence",
    "Max_Fog_Depth": "depth at which the water fog saturates",
    "Crest_Color": "colour of the wave crests",
    "Crest_Threshold": "height above which a wave counts as a crest",
    "Wave_speeds": "scroll speeds of the four wave normal layers",
    "Normal_map_strengths": "per-layer weights of the four wave normal layers",
    "Meteor_strength": "brightness of the meteor streaks",
    "Star_strength": "brightness of the star layer",
    "Draw_distance": "distance the sky element is placed at",
    "Hemisphere_colors": "sky and ground hemisphere colours of the gradient dome",
    "Cloud_U_Offset": "cloud layer scroll offset",
    "Cloud_Fade_Height": "height at which the cloud layer starts to fade",
    "Cloud_Full_Height": "height at which the cloud layer is fully opaque",
    "Layer_strengths": "per-layer cloud opacity",
    "Rimlight_scale": "strength of the cloud rim light",
    "Rimlight_power": "exponent of the cloud rim light",
    "Mountain_fog_color": "colour the distant matte is fogged towards",
    "Mountain_fog_density": "how quickly the distant matte fogs out",
    "Tread_mask": "mask selecting the tread band that scrolls",
    "Invert_Blend_Map": "flips the sense of the terrain blend mask",
    "Flip_Amount": "how far along the window-flip animation this surface is",
    "night_time_intensity": "night-time brightness of the lit windows",
    "TOD_on": "boolean-ish switch enabling the time-of-day window behaviour",
    "Glow_Intensity": "glow strength",
    "Glow_Scroll_Speed": "glow layer scroll rate",
    "Main_Opacity": "overall opacity of the VFX layer",
    "Fade_Distance": "distance over which the effect fades against the depth buffer",
    "Soft_Fade_Alpha": "soft-particle fade strength against scene depth",
    "Distortion_Amount": "screen distortion strength",
    "ShimmerColor": "colour of the pickup shimmer sweep",
    "ScrollRate": "how fast the shimmer sweeps along the model",
    "Narrowness": "how tight the shimmer band is",
    "Frequency": "shimmer repetition frequency",
    "Lighting": "the tree lighting model's ambient/direct balance",
    "Tree_enable_leaf_wind": "boolean enabling the leaf-flutter oscillator",
    "Tree_enable_frond_wind": "boolean enabling the frond-ripple oscillator",
    "Lod_profile": "the imposter's level-of-detail transition profile",
    "Horiz_fade_value": "crossfade between the side billboards and the overhead view",
    "Camera_azimuth_trig": "sin/cos of the camera azimuth, used to pick the billboard view",
    "Camera_angles": "the camera angles the imposter was authored against",
    "Num_billboards": "how many pre-rendered billboard views this imposter has",
    "Billboard_tangents": "the tangent frames of the pre-rendered billboard views",
    "Billboard_texcoords": "where each billboard view sits in the imposter atlas",
    "Horiz_texcoords": "where the overhead view sits in the imposter atlas",
    "Camera_pos": "the shadow projector's camera position",
    "Camera_rvec": "the shadow projector's right axis",
    "Camera_uvec": "the shadow projector's up axis",
    "Camera_fvec": "the shadow projector's forward axis",
    "Shadow_percent_params": "how dark the projected shadow gets",
    "Shadow_fade_params": "the distance over which the projected shadow fades out",
    "near_far_depth_params": "near/far constants for reconstructing depth in the projector",
    "Bbox_size": "size of the bounding box the cheap projector extrudes",
    "Extrusion_info": "how far and in which direction the cheap projector extrudes the box",
    "ClipScissorMin": "lower corner of the editor pick rectangle",
    "ClipScissorMax": "upper corner of the editor pick rectangle",
    "t0": "start of the time-of-day window transition",
    "t2": "end of the time-of-day window transition",
    "Base_Color": "the base colour of the light cover",
    "Self_Illumination_Artist": "the artist-facing emissive control, separate from the runtime one",
    "Paint_Color": "body colour of the distant vehicle imposter",
    "Accent_Color": "trim colour of the distant vehicle imposter",
    "Dirt_Opacity": "how strongly the dirt layer covers the glass",
    "Decal_gen_alpha": "generated alpha for the projected decal",
    "Decal_gen_tint": "generated tint for the projected decal",
    "Scroll": "scroll rate of the projected cookie texture",
    "Refle_Amount": "reflection strength",
    "Illum_Amount": "emissive strength",
    "Base_Opacity": "the surface's base opacity",
    "Diffuse_Map_Amount": "how much of the second albedo layer is mixed in",
    "Diffuse_Map_Saturation": "saturation applied to the mixed albedo",
    "Diffuse_Light_Amount": "how much scene light the animated layer receives",
    "Angle_Power": "exponent of the view-angle falloff",
    "Glow_Falloff_Power": "exponent of the glow falloff",
    "Glow_Falloff_Inversion": "flips the sense of the glow falloff",
    "Alpha_Falloff_Amount": "strength of the alpha falloff",
    "Alpha_Falloff_Power": "exponent of the alpha falloff",
    "Diffuse_Distortion_Amount": "how far the distortion map displaces the diffuse layer",
    "Pulse_Rate": "how fast the colour pulses",
    "Pulse_Base": "the pulse's low value",
    "Pulse_Max": "the pulse's high value",
    "Fresnel_Falloff_Contrast_Amount": "contrast of the legacy Fresnel falloff",
    "Fresnel_Falloff_Brightness_Amount": "brightness of the legacy Fresnel falloff",
    "Reflection_Falloff_Contrast_Amount": "contrast of the legacy reflection falloff",
    "Reflection_Falloff_Brightness_Amount": "brightness of the legacy reflection falloff",
    "Dirt_Reveal_Brightness_Amount": "brightness of the revealed dirt",
    "Detail_Strength": "strength of the detail albedo",
    "Det_Normal_Height": "bump strength of the detail normal map",
    "Normal_map_detail_height": "bump strength of the detail normal map",
    "High_Detail_Cutoff_Dist": "distance beyond which the water detail normal is dropped",
    "Min_Opacity": "the water's opacity looking straight down",
    "Max_Fresnel": "the water's opacity at grazing angles",
    "TOD_Light_Direction": "time-of-day sun direction",
    "TOD_Light_Color": "time-of-day sun colour",
    "Wave_information": "the water simulation's wave propagation constants",
    "Wave_ambient_information": "ambient disturbance fed into the water simulation",
    "Wave_shift": "how far the water field shifts per step",
    "Composite_UV_XForm": "where the simulated field lands in the world water surface",
    "Composite_information": "blend controls for the water composite",
    "Blending_lut_weight_0": "weight of grading LUT 0",
    "Blending_lut_weight_1": "weight of grading LUT 1",
    "Blending_lut_weight_2": "weight of grading LUT 2",
    "Blending_lut_weight_3": "weight of grading LUT 3",
    "Tint_saturation": "saturation applied with the HDR tint",
    "Override_blur_percent": "forces a fixed depth-of-field blur amount",
    "Signal_noise": "noise added with the distortion, the analogue-signal look",
    "component_mask": "which YUV component this pass writes",
    "texture_dimensions": "the source texture's size, for the bicubic weights",
    "uv_xform": "scale/offset applied to the sampling coordinates",
    "dummy_var": "unused - the null shader needs a constant table to be valid",
    "unused_var": "unused - this pass does no shading",
    "projection_info": "near/far constants for linearising the visualised depth",
    "IR_light_debug_flags": "which debug overlay the light shader should draw",
    "blend_weights_1": "first four character-morph blend weights",
    "blend_weights_2": "second four character-morph blend weights",
    "hsb_offset": "hue/saturation/brightness offset applied inside the mask",
    "unmasked_hsb_offset": "hue/saturation/brightness offset applied outside the mask",
    "framebuffer_info": "size and offset of the frame buffer being read back",
    "target_dimension": "the size of the target being composited into",
    "Rain_params": "rain density and length",
    "Size_params": "rain streak size",
    "Speed": "rain fall speed",
    "Prev_cam_position": "last frame's camera position, used to shear the rain streaks",
    "Current_time": "engine time driving the rain animation",
    "Raycast_translation": "where in the depth buffer the ray-cast queries land",
    "Points_target_dimensions": "size of the ray-cast result target",
    "Near_clip": "near clip distance",
    "Far_clip": "far clip distance",
    "Scale": "scale placing this element on the sky dome",
    "Offset": "offset placing this element on the sky dome",
    "Layer_strengths2": "opacity of the second pair of cloud layers",
    "Mountain_color_front": "colour of the sunlit face of the distant matte",
    "Mountain_color_back": "colour of the shaded face of the distant matte",
    "Orbital_tint": "tint applied to the orbital body",
    "Room_Depth ": "apparent depth of the parallax interior box",
    "Flip_Amount ": "progress of the window flip animation",
    "World_xform": "the world transform of the visibility-query box",
    "Blur_scale ": "blur strength",
    "blur_falloff": "spatial falloff of the bilateral SSAO blur",
    "blur_sharpness": "how hard the bilateral SSAO blur stops at depth edges",
    "ssao_uv_scale_offset": "maps screen coordinates into the SSAO buffer",
    "ssao_fade_parameter": "distance over which SSAO fades out",
    "ssao_projection_scales": "projection scales used to size the SSAO kernel in screen space",
    "rao_box_min": "lower corner of the analytic occluder box",
    "rao_box_max": "upper corner of the analytic occluder box",
    "rao_view2box": "view space to occluder-box space",
    "rao_ellipsoid2view": "ellipsoid space to view space",
    "rao_view2ellipsoid": "view space to ellipsoid space",
    "rao_occluder_sphere": "the occluding sphere's centre and radius",
    "Parametric_particle_constants": "the particle system's size, rotation, colour and alpha "
                                     "curves, evaluated on the GPU",
    "Meteor_strength ": "brightness of the meteor streaks",
    "Tread_mask ": "which texels scroll with the track",
    "Detail_Normal_Map_Tiling": "tiling of the water detail normal map",
    "Normal_Map_Refl_Offset": "how far the normal map perturbs the reflection lookup",
    "Distortion_scale": "how far the distortion buffer displaces the image",
    "Shadow_map_resolution": "shadow map size, for the filter kernel",
    "Wave_ambient_information2": "second set of ambient disturbance constants for the water "
                                 "simulation",
    "uv_to_select": "the texture-space point the paint cursor is currently over",
    "uv_offset": "offset applied to the paint cursor's texture-space position",
}

# Applied when a constant has no exact entry. The name pattern in this library is
# systematic enough that these produce accurate text.
CONST_PATTERNS = [
    (r"Tiling_?U$|Tile_U$|TilingU(_\d)?$", "U tiling of {0}; uv = raw * tiling / 1024"),
    (r"Tiling_?V$|Tile_V$|TilingV(_\d)?$", "V tiling of {0}; uv = raw * tiling / 1024"),
    (r"Scroll_?U(_\d)?$", "U scroll rate of {0}"),
    (r"Scroll_?V(_\d)?$", "V scroll rate of {0}"),
    (r"Offset_?U(_\d)?$", "U offset of {0}"),
    (r"Offset_?V(_\d)?$", "V offset of {0}"),
    (r"_Opacity(_\d)?$", "blend strength of {0}"),
    (r"_Amount(_\d)?$", "strength of {0}"),
    (r"_Color(_\d)?$|_color(_\d)?$", "colour of {0}"),
    (r"_Height\d?$|Height(_\d)?$", "bump strength of {0}"),
    (r"_Power(_\d)?$", "exponent of {0}"),
    (r"_Strength(_\d)?$", "strength of {0}"),
    (r"_Alpha(_\d)?$", "alpha of {0}"),
    (r"_Threshold$", "threshold of {0}"),
    (r"_params?$|_Params?$", "parameters of {0}"),
    (r"_matrix$|_xform$|_XForm$|TM$", "transform for {0}"),
    (r"_dimensions?$", "the size of {0}"),
    (r"^Clamp[UV]", "UV clamp bound"),
]

WORDS = {
    "Decal": "the decal layer", "Decal2": "the second decal layer",
    "Diffuse": "the albedo", "Normal": "the normal map", "Spec": "the specular map",
    "Specular": "the specular term", "Glow": "the glow layer", "Grime": "the grime layer",
    "grime": "the grime layer", "Dirt": "the dirt layer", "Alpha": "the alpha mask",
    "Illumination": "the emissive layer", "Reflection": "the reflection",
    "Distortion": "the distortion layer", "Detail": "the detail layer",
    "Pattern": "the pattern mask", "Blend": "the blend mask", "Sphere": "the sphere map",
    "Viewsphere": "the view-sphere map", "Fresnel": "the Fresnel term",
    "Cloud": "the cloud layer", "Bloom": "the bloom", "Blur": "the blur",
    "Shadow": "the shadow", "Wave": "the wave simulation", "Tree": "the tree animation",
}


def const_role(name):
    role = CONST_ROLES.get(name)
    if role:
        return role
    second = ""
    ordinal = re.search(r"_?([12])$", name)
    if ordinal:
        base = name[: ordinal.start()]
        word = "first" if ordinal.group(1) == "1" else "second"
        if base in CONST_ROLES:
            return "%s layer: %s" % (word, CONST_ROLES[base])
        second = "the %s " % word
    head = re.split(r"[_ ]", name)[0]
    subject = WORDS.get(head, "this layer")
    if second and subject.startswith("the "):
        subject = second + subject[4:]
    for pattern, template in CONST_PATTERNS:
        if re.search(pattern, name):
            return template.format(subject)
    return "-"

# ---------------------------------------------------------------------------
# Filename token glossary. Applied in order; used to compose a description for
# families with no bespoke entry.
# ---------------------------------------------------------------------------

TOKENS = [
    (r"^ir_at_", "alpha-tested (cutout): a texkill against Alpha_Threshold discards texels, "
                 "including in the depth and G-buffer passes"),
    (r"^ir_", "an inferred-lighting material: it writes the G-buffer and later resolves its "
              "shading from the L-buffer"),
    (r"^rl_", "a render-layer shader - a full-screen pass, a utility draw, or a system that does "
              "not go through the normal material path"),
    (r"^rfg-", "part of the sky system, drawn on a dome around the camera"),
    (r"_nn$|_nn_", "no normal map: the tangent frame is still built but the surface normal comes "
                   "straight from the interpolated vertex normal"),
    (r"_tod", "time-of-day aware: it carries a Night_Additive layer or a TOD tint that is driven "
              "by the clock"),
    (r"decal", "composites one or more decal layers over the base albedo"),
    (r"illum", "adds a scrolling self-illumination layer"),
    (r"scroll", "animates its UVs over time"),
    (r"glow", "carries a glow/emissive layer"),
    (r"reflect|refl", "samples a reflection source and blends it in by a Fresnel-style ramp"),
    (r"window", "window glass: reflection plus a time-of-day interior/night tint"),
    (r"glass", "transparent glass: reflection and a tint, blended rather than written opaque"),
    (r"terrain", "blends two complete material layers by a per-texel blend map"),
    (r"twotone", "two albedo tints selected per texel"),
    (r"cloth", "character clothing: the colour is computed per texel from a pattern mask and "
               "three colour constants"),
    (r"skin", "character skin, with sphere-map based subsurface-ish response and a rim term"),
    (r"hair", "character hair, with an anisotropic double highlight"),
    (r"car(paint|glass|viewsphere|lightcover)|distantvehicle|tanktread",
     "vehicle surface, with damage normals and dirt/grime layers"),
    (r"water|pool", "water: animated normals, depth-based fog and a reflection"),
    (r"foliage|grass|tree", "vegetation, with wind animation in the vertex shader"),
    (r"particle", "particle geometry expanded and shaded in the VFX path"),
    (r"debug", "a debug visualisation, not used in normal rendering"),
    (r"editor", "an editor-only draw, not used in the shipping renderer"),
]

# ---------------------------------------------------------------------------
# Bespoke family notes, keyed on the base name with the variant suffix removed.
# ---------------------------------------------------------------------------

FAMILY = {

# ---- the plain world materials -------------------------------------------
"ir_bbsimple1": "The workhorse world material: albedo, normal map and specular map, nothing else. "
    "Most of the city's buildings and props are drawn with some variant of this. The G-buffer pass "
    "samples only the normal map; the material pass samples albedo and specular and multiplies them "
    "against the L-buffer.",
"ir_bbsimple2": "The same world material as ir_bbsimple1 with the specular map dropped - albedo and "
    "normal map only, with a constant specular colour.",
"ir_bbsimple3": "The cheapest world material: albedo only. No normal map, so the G-buffer normal is "
    "the interpolated vertex normal, and no specular map.",
"ir_bbsimple2_nodiffmap": "Normal map only, with the albedo coming entirely from Diffuse_Color. Used "
    "for painted flat surfaces.",
"ir_bbsimple2_decal": "ir_bbsimple2 plus one decal layer with its own tiling and opacity.",
"ir_bbsimple2_2decals": "ir_bbsimple2 plus two independently tiled decal layers.",
"ir_bbsimple_1uv_decal": "A decal layer that reuses the base UV set rather than a second one.",
"ir_bbsimpledecal1": "ir_bbsimple1 (albedo/normal/specular) plus a decal layer.",
"ir_bbsimplediffusedecal": "Albedo plus a decal layer, no normal or specular map.",
"ir_bbstandard": "The full standard world material: albedo, normal, specular and two independently "
    "tiled decal layers, with separate tiling for the specular map.",
"ir_bbblend1": "Two decal layers plus a blend map that chooses between two albedo tints - the "
    "variant used where two surface treatments meet.",
"ir_bbterrain1": "Terrain: two complete material layers (albedo, normal, specular each) blended per "
    "texel by Blend_Map, with a separate specular power and colour for the second layer.",
"ir_bbterrain1_donly": "Terrain with the layers reduced to albedo only.",
"ir_bbkennygrass1": "A grass-card material - albedo only, alpha cut, drawn on flat cards.",
"ir_bb_tod_window": "Building window glass with a paraboloid reflection probe and a time-of-day "
    "tint, so windows read as lit interiors at night and reflective sky by day.",
"ir_bbsimple1_diffusewet": "ir_bbsimple1 with the wet-weather path: Ground_reflection_coef darkens "
    "the albedo by Diffuse_scale_wet and sharpens the specular when it is raining.",
"ir_bbsimple2_diffusewet": "ir_bbsimple2 with the wet-weather albedo darkening.",
"ir_bbsimple3_diffusewet": "ir_bbsimple3 with the wet-weather albedo darkening.",
"ir_demowall": "A destructible-demo wall material with no textures at all - it exists to write "
    "geometry into the G-buffer and take its colour from constants.",

# ---- legacy / simple ------------------------------------------------------
"ir_legacy": "The compatibility material carried over from the previous engine: albedo, normal and "
    "specular with a single Normal_Height control.",
"ir_legacy_no_normal": "Legacy material without a normal map.",
"ir_legacy_no_tangent": "Legacy material for meshes that carry no tangent stream, so no "
    "tangent-space normal mapping is possible.",
"ir_nospec_nonormal": "Albedo only, with its own U/V tiling constants and no normal or specular map.",
"ir_nospec_nonormal_blend": "Two albedo layers blended by a blend map, no normal or specular.",
"ir_simple1_nospecmap": "Albedo and normal map with a constant specular and a wetness mask.",
"ir_simpledecal1_nospecmap": "Albedo, normal map and a decal layer, constant specular, wetness mask.",
"ir_vertexcoloring1": "Fully vertex-coloured surface: no textures at all, the albedo comes from the "
    "interpolated vertex colour.",
"ir_tod_vertexcoloring1": "Vertex-coloured surface with a time-of-day window tint applied.",
"ir_grndrfl_vertexcoloring1": "Vertex-coloured surface that also takes the wet-ground reflection "
    "treatment.",
"ir_unlit_diffcolor_mask": "Unlit flat colour gated by a mask texture - it never reads the L-buffer.",
"ir_mesh_depth_only": "Depth-only draw. The pixel shader writes nothing meaningful; the container "
    "exists so that geometry of every skinning variant can be laid into the depth buffer.",

# ---- decals ---------------------------------------------------------------
"ir_decal": "Projected dynamic decals - bullet holes, blood, tags. The vertex shader builds a "
    "projector basis from Dyn_decal_pos/rvec/uvec/fvec and Dyn_decal_scale, projects the mesh's "
    "world position into that box, and the pixel shader kills anything outside it.",
"ir_decal_screenspace": "The screen-space form of the dynamic decal: instead of re-drawing the "
    "receiving geometry it draws the projector box, rebuilds world position from the G-buffer depth "
    "with IR_Light_Inv_Proj_TM, and stamps the decal wherever that position lands inside the box.",
"ir_blood_pool": "A spreading blood pool - a dynamic decal projector that also samples the planar "
    "reflection map so the pool reflects.",
"ir_blood_pool_screenspace": "The screen-space form of the blood pool decal, reconstructing position "
    "from the G-buffer depth.",
"ir_sr3decalonly": "A decal with no base material underneath: the decal map is the whole surface, "
    "tinted by Decal_Color.",
"ir_decalonly_cuberef": "A decal-only surface that also picks up a cube reflection.",
"ir_sr3simple_distfield_decal": "A distance-field decal: the decal texture stores a signed distance, "
    "which is remapped to a hard edge in the pixel shader and tinted by Decal_Map_Color, so the "
    "decal stays sharp at any magnification.",
"ir_sr3diffcol_normal_decal": "Flat albedo colour plus a normal map and one decal layer.",
"ir_item_shimmer_decal": "The sweep of light that runs over collectible items. A sincos-driven band "
    "defined by ScrollRate, Frequency and Narrowness is projected onto the mesh through a decal "
    "projector and added in ShimmerColor.",
"ir_paint_cursor": "The cursor decal that shows where the player is about to spray, using "
    "uv_to_select/uv_offset to place a highlight in texture space.",

# ---- reflections / windows / glass ---------------------------------------
"ir_floor_reflect": "An interior floor that samples the planar reflection render target in screen "
    "space, offset by the normal map, and masks it with a per-texel reflectivity map.",
"ir_floor_reflect_illum": "The reflective floor with an added self-illumination amount.",
"ir_ground_reflect": "Wet ground: a detail normal map perturbs the planar reflection lookup, the "
    "wetness comes from the global Ground_reflection_coef and a per-texel wet mask, and the albedo "
    "darkens and the specular sharpens as it gets wetter.",
"ir_window_reflectmask": "Window glass lit by a single-paraboloid probe, with a per-texel reflection "
    "mask, a night additive layer and a glow layer.",
"ir_window_reflectmask_scraper": "The skyscraper window variant, which uses an environment map with "
    "an explicit LOD (texldl) so distant towers do not alias.",
"ir_window_simple": "The cheap window: albedo plus a reflection map at a constant opacity.",
"ir_windowwall_production": "A whole facade of windows drawn as one surface: two normal maps and two "
    "decal layers let individual panes differ, Flip_Amount animates panes between states, and "
    "TOD_on/night_time_intensity drive the day/night interior.",
"ir_windowwall_simple": "The reduced facade material: albedo, two normal maps and two decal layers, "
    "no reflection.",
"ir_windowwall_nodecal": "The facade material without the second decal layer.",
"ir_sr3wallwindow": "A wall-with-windows material: two normal maps, a decal layer, a reflection map "
    "and mask, and the TOD flip animation.",
"ir_sr3wallwindow2": "The extended wall-with-windows material with a second decal layer.",
"ir_sr3fauxinterior": "Fake interiors seen through windows. Room_Depth and the clamped UVs make the "
    "decal map parallax inside a virtual box, so the room appears to have depth without geometry.",
"ir_sr3_tod_window_fi": "Faux interior with the time-of-day window tint and a night additive layer.",
"ir_sr3glass1": "Glass with a dirt layer and a reflection map, blended by Glass_Opacity and tinted "
    "by Glass_Color.",
"ir_sr3glass_diffonly": "Glass reduced to an albedo and a tint - no reflection.",
"ir_sr3glass_diffuse_reflect": "Glass with an albedo and a reflection map.",
"ir_sr3decalcube": "A surface with a second material layer applied through a decal and its own "
    "normal map, reflecting a single-paraboloid probe.",
"ir_sr3decalcube_no_decal_ns": "The decal-cube material with the decal normal map dropped.",
"ir_sr3ground1": "Ground with a reflection map masked by the specular map.",
"ir_sr3ground2": "Ground with a reflection map and a separate reflection mask.",
"ir_sr3metal_dns": "Metal: albedo, normal, specular, a reflection map and a reflection mask.",
"ir_sr3metal_dn_ns": "Metal without the specular map.",
"ir_sr3metal_dn_exteriortod": "Exterior metal whose reflection tracks the time of day.",
"ir_shaderball_detail": "An art-test material: base albedo and normal plus a detail albedo and "
    "detail normal at their own tiling, and a Fresnel term.",
"ir_sr3shaderball": "The material-preview ball, used to author and compare shading settings.",
"ir_sr3shaderballadd": "The additive-blended variant of the material-preview ball.",

# ---- emissive / animated -------------------------------------------------
"ir_sr2illumscroll1": "Albedo, normal and an emissive illumination map, all three able to scroll "
    "independently - neon, conveyor belts, animated signage.",
"ir_sr3illumscroll_nonormal": "The scrolling emissive material without a normal map.",
"ir_sr_tod_illum_scroll": "A scrolling emissive material with a time-of-day night additive layer, so "
    "the sign only lights up after dark.",
"ir_sr_tod_illum_scroll_simple": "The reduced time-of-day scrolling emissive: albedo, night additive "
    "and illumination map only.",
"ir_sr_tod_illum_ns": "Time-of-day emissive with a normal map and no specular.",
"ir_at_sr_tod_illum": "Alpha-tested time-of-day emissive signage.",
"ir_glowmask_dns": "Albedo, normal and specular plus a mask that selects which texels glow.",
"ir_scrollglowmask_dns": "The glow-mask material with the glow scrolling over time and its colour "
    "coming from a second texture.",
"ir_blendscroll": "Two albedo layers and two glow layers, each with its own scroll rate, falloff "
    "power and colour - the most elaborate animated-sign material in the library.",
"ir_sr3megatv": "A large screen surface: the decal map is the video content, offset per frame by "
    "Decal_Map_OffsetU/V.",
"ir_sr3television": "A television screen with a scanline/roll animation driven by sincos and two "
    "content layers.",
"ir_sr3vr_pixel": "A screen material with a reflection and offset content layers, used for the VR "
    "and computer displays.",
"ir_bink": "Bink video playback as a surface. The three samplers are the Y, Cr and Cb planes; the "
    "pixel shader does the YUV to RGB conversion itself.",

# ---- characters -----------------------------------------------------------
"ir_sr3npcclothfull": "NPC clothing. The albedo is not a texture: Pattern_Map's channels are "
    "weights that mix Diffuse_Color_a, _b and _c per texel, so one garment mesh can be recoloured "
    "arbitrarily. A sphere map adds the sheen and Fresnel_Strength the rim.",
"ir_sr3npcclothpulse": "NPC clothing whose colour pulses between Pulse_Base and Pulse_Max at "
    "Pulse_Rate, driven by sincos on Time.",
"ir_sr3npccloth_glow": "NPC clothing with a scrolling illumination map on top of the pattern-mixed "
    "colour.",
"ir_sr3pccloth": "Player clothing. Like the NPC recipe - pattern mask mixing three colours - but the "
    "pattern is sampled on its own tiling and clamped UV range, and there is a real Diffuse_Map "
    "underneath it.",
"ir_at_sr3pccloth": "Alpha-tested player clothing, for garments with cut edges, adding a decal layer "
    "on a second clamped UV range.",
"ir_sr3npcskinfull": "NPC skin: albedo and normal plus two sphere maps blended by a blend map (one "
    "for the waxy highlight, one for the softer subsurface response), each with its own specular "
    "power and Fresnel strength.",
"ir_sr3pcskinfull": "Player skin, the same two-sphere-map recipe with a single Fresnel strength.",
"ir_sr3skintest2": "A skin development material with an explicit rim-light ramp "
    "(Rim_Start/Rim_End/Rim_Brightness/Rim_Color) and a gradient lookup.",
"ir_sr3pchair": "Player hair. Dob_Map carries the hair depth/occlusion used to sort the cards, and "
    "Hair_Spec_Color1/2 with Hair_Parameters produce the two shifted anisotropic highlights that "
    "make hair read as strands.",
"ir_sr3eyeball": "Eyes: albedo and normal plus a sphere map for the corneal highlight and a Fresnel "
    "term for the wet rim.",
"cust_normal_map_blend": "The character-customisation normal compositor. It is not a scene material: "
    "it renders a new normal map into a texture by blending the base against body_male, body_female, "
    "skinny, fat, muscle, body_age and face_age by blend_weights_1/2, and applies an HSB offset to "
    "the diffuse. The result is the character's baked face and body maps.",

# ---- vehicles -------------------------------------------------------------
"ir_sr3carpaint": "Vehicle body paint. Base_Paint_Color is the customisable colour; "
    "Damage_Normal_Map dents the surface; a Fresnel ramp "
    "(Fresnel_Min_Cos_Angle/Fresnel_Inverse_Cos_Angle_Range/Fresnel_Color) and a reflection ramp "
    "(Reflection_Cos_Min_Angles/Reflection_Inv_Range_Cos_Angles) shape the clearcoat. The name "
    "codes the layer stack: g = grime map, di = dirt/rust map, r = dual-paraboloid reflection "
    "probe, d1/d2 = decal layers 1 and 2, df1/df2 = distance-field decals tinted by Decal_Color.",
"ir_sr3carglass": "Vehicle glass. Reflects the dual-paraboloid probe through a Fresnel-shaped ramp, "
    "tinted by Glass_Color and faded by Opacity_fade. The suffix codes the same layer stack as the "
    "paint: g = grime, di = dirt/rust, d1 = decal, df1 = distance-field decal.",
"ir_sr3carviewsphere": "The cheap vehicle paint: instead of a real environment probe it looks up a "
    "Viewsphere_Map (a matcap) by the view-space normal, blended in by Viewsphere_Amount. Used at "
    "distance and on lower settings; the same g/di/d1/df1 layer codes apply.",
"ir_sr3cardiffusespec": "A plain vehicle surface - albedo and specular - for parts that are not "
    "painted bodywork.",
"ir_sr3carlightcover": "Vehicle light covers: a translucent coloured shell over the lamp.",
"ir_sr3distantvehicle": "The distant-vehicle imposter material. A single Pattern_Map picks between "
    "Paint_Color and Accent_Color, which is all the detail a car needs at range.",
"ir_sr3tanktread": "Tank and heavy-vehicle treads: Tread_mask selects the band of texels that scroll "
    "with the vehicle's motion so the track appears to run.",
"ir_sr2carpaint1": "The previous game's car paint recipe, kept for legacy content: decals with "
    "clamped UVs, a dirt map with a reveal mask, and Fresnel/reflection falloff controls expressed "
    "as brightness and contrast amounts rather than cosine ramps.",

# ---- vegetation and water ------------------------------------------------
"ir_foliage_grass1": "Grass cards. Alpha-tested albedo with the alpha test allowed to run through "
    "the DSF coverage (Alpha_test_use_dsf) so the cut edges do not shimmer.",
"ir_foliage_litter1": "Ground litter and small debris cards with a full albedo/normal/specular set.",
"ir_at_bbkennygrass1": "Alpha-tested grass cards.",
"tree": "SpeedTree-style trunk and branch geometry. The vertex shader animates the mesh with a "
    "hierarchy of wind oscillators - Tree_wind_dir, Tree_wind_times, Tree_wind_distances, plus a "
    "gust term and separate leaf and frond ripple - gated by the Tree_enable_leaf_wind and "
    "Tree_enable_frond_wind booleans.",
"tree_fade": "Tree geometry with an added distance fade, so a tree can dissolve into its imposter.",
"tree_cards": "The leaf and frond cards of a tree, wind-animated and alpha cut.",
"tree_cards_fade": "Leaf cards with the distance fade to the imposter.",
"tree_impostors": "The billboard imposter a tree becomes at distance. Camera_azimuth_trig and "
    "Num_billboards pick which of the pre-rendered billboard views to show, Billboard_tangents and "
    "Billboard_texcoords place it, and Horiz_fade_value crossfades to the overhead view.",
"ir_sr3dynamic_water1": "Dynamic water: a scrolling detail normal map, a reflection map, "
    "depth-based fogging against the G-buffer depth (Max_Fog_Depth), a Fresnel opacity ramp between "
    "Min_Opacity and Max_Fresnel, and a crest colour applied above Crest_Threshold.",
"ir_sr3standingwater": "Standing water - puddles and flooded ground - blending a planar reflection "
    "with depth fog.",
"ir_sr3_swimmingpool": "Pool water, which samples its normal map at an explicit LOD (texldl) so the "
    "ripples do not alias against the tile pattern.",
"water_physics": "The water height-field simulation step. It reads the field at frame n and n-1 and "
    "the damping map and writes the next state - a discrete wave equation run on the GPU.",
"water_simulation_composite": "Composites the simulated height field into the world water surface "
    "under Composite_UV_XForm.",
"water_normal_map_composite": "Sums four scrolling wave normal layers, each with its own speed and "
    "strength, into the single normal map the water material reads.",
"water_wavekillers": "Draws the volumes that damp the water simulation - the shapes that stop waves, "
    "such as harbour walls and boats.",

# ---- VFX ------------------------------------------------------------------
"ir_vfxmask": "The general VFX surface: a glow layer, a diffuse layer, a distortion layer and an "
    "alpha mask, each with its own scroll rate, plus a soft depth fade against the G-buffer depth "
    "and an angle-based falloff.",
"ir_vfxmasktod": "The VFX surface with time-of-day response.",
"ir_vfxdistort": "A screen-distorting VFX surface: the distortion map offsets the background, the "
    "diffuse layer scrolls over it, and Fade_Distance softens the intersection with geometry.",
"ir_sr3demofence1": "An animated demolition/force fence, driven by sincos.",
"privacy_bar": "The censorship bar. It reads the frame buffer back at s14 and re-renders it "
    "pixelated over the region it covers.",

# ---- inferred-lighting lights --------------------------------------------
"ir_light_point": "A point light in the inferred-lighting pass. The shader draws the light's volume, "
    "rebuilds view-space position from IR_GBuffer_Depth with IR_Light_Inv_Proj_TM, computes "
    "L = IR_Light_Pos - surfacePos, attenuates by "
    "pow(saturate(1 - (d - Info.y)/(Info.z - Info.y)), Info.x) and accumulates diffuse and specular "
    "into the L-buffer. IR_Light_Info.z is therefore the light's range.",
"ir_light_point_tex": "A point light that projects a cookie texture, transformed into the light's "
    "space by Projection_texture_xform and scrolled by Scroll.",
"ir_light_spot": "A spot light with shadows. The cone falloff is "
    "saturate((cos(angle) - IR_Spot_Info.y) * IR_Spot_Info.z), so IR_Spot_Info.y is cos(outer "
    "angle) and .z is 1/(cos inner - cos outer). Shadow_map_projTM projects into the shadow map, "
    "faded out by Shadow_map_fade_params.",
"ir_light_spot_noshadows": "The spot light with the shadow map removed.",
"ir_light_spot_tex": "A shadowed spot light that also projects a cookie texture.",
"ir_light_spot_tex_noshadows": "A cookie-projecting spot light with no shadow map.",
"ir_light_tube": "A tube/area light: the same attenuation as a point light, but the light vector is "
    "taken to the nearest point on a segment rather than to a single position.",
"ir_light_tube_tex": "A tube light projecting a cookie texture.",
"ir_light_directional": "The sun. IR_Light_Pos is used directly as an N.L direction with no position "
    "subtraction and no distance term. It folds in the shadow map (gated by Shadow_map_enabled), "
    "the ambient-occlusion buffer, and IR_light_back_color as a cheap translucency on backfacing "
    "surfaces. This is the single most valuable light to reproduce.",
"ir_light_directional_amb_only": "The sun shader reduced to its ambient and occlusion contribution.",
"ir_light_directional_direct_only": "The sun shader reduced to its direct contribution.",
"ir_light_local_ambient": "A local ambient probe: a bounded volume that adds a directionally biased "
    "ambient term (IR_Light_Color towards IR_Light_Dir, IR_light_back_color away from it).",
"ir_light_overdraw_debug": "Counts light volumes per pixel so light overdraw can be visualised.",
"rl_light_sampling_geompass": "The geometry pass of the light-sampling system: it lays down the "
    "surfaces that will be probed for light sampling.",
"rl_light_sampling_stenciltest": "The stencil test that limits light sampling to the marked region.",
"rl_reflected_light": "Accumulates bounced/reflected light back into the scene.",

# ---- debug views ----------------------------------------------------------
"ir_debug_depth": "Visualises the G-buffer depth, linearised with projection_info.",
"ir_debug_normals": "Visualises the G-buffer view-space normals.",
"ir_debug_dsf": "Visualises the DSF ids so discontinuity artefacts can be traced.",
"ir_debug_glbuffer": "Visualises the raw L-buffer contents.",
"ir_debug_specular": "Visualises the specular channel of the L-buffer.",
"ir_debug_light_overdraw": "Visualises how many light volumes touched each pixel.",
"ir_debug_ssao": "Visualises the SSAO buffer.",
"ir_debug_singleframe_ssao": "Visualises the single-frame SSAO buffer.",
"debug_diffuse_only": "A debug material that outputs a flat colour, used to isolate geometry from "
    "shading. The vertex shader still builds the full tangent frame, so it can stand in for a real "
    "material without changing vertex processing.",
"rl_debug_highlight": "Draws a mesh in a flat highlight colour - the selection highlight.",
"rl_debug_display_morph_weight": "Colours a mesh by its morph weights so blending can be checked.",
"bb-norender": "A material that is compiled and bound but deliberately draws nothing visible - the "
    "'no render' placeholder that keeps a mesh in the scene without shading it.",
"stencil_opt": "A stencil-only draw used to mark regions for later passes; its single unused_var "
    "constant confirms it has no shading work to do.",
"rl_null_shader": "The null shader: a valid vertex/pixel pair that does nothing, bound when a stage "
    "must exist but must not draw.",

# ---- editor ---------------------------------------------------------------
"editor_wireframe": "Editor wireframe drawing.",
"editor_filled": "Editor solid-fill drawing.",
"editor_const_color": "Draws geometry in the single constant colour Draw_Color.",
"rl_picking_clip_test": "Tests whether geometry falls inside the editor's pick region.",
"rl_picking_depth_render": "Renders depth for editor picking.",
"rl_picking_downsample_min": "Reduces the pick buffer by taking the minimum, so the nearest hit wins.",
"rl_picking_draw_tint": "Tints the picked object.",
"rl_vis_query": "The visibility-query draw: a cheap box rendered under an occlusion query to decide "
    "whether the real object is visible.",

# ---- post-processing and full-screen passes ------------------------------
"rl_hdr": "The HDR resolve and tone-map chain. It holds the luminance downsample, the eye-adaptation "
    "step towards Desired_brightness over Adapt_time (clamped between Exposure_min and "
    "Exposure_max), the bloom bright-pass and accumulation shaped by Bloom_curve_values, the "
    "light-shaft pass around Light_shafts_light_pos, and the final tone-map with the colour-grading "
    "LUT. Fourteen shader blobs, one per stage.",
"rl_gaussian_blur": "A separable Gaussian blur; Separated_axis selects which pass this is and "
    "Sampling_offsets carries the taps.",
"rl_vertical_blur": "The vertical half of a separable blur.",
"rl_downsample": "A box downsample with the taps in Sampling_offsets.",
"rl_downsample_2x2": "A 2x2 box downsample.",
"rl_downsample_fast": "A downsample that relies on bilinear filtering to average four texels in one "
    "fetch.",
"rl_bicubic": "A bicubic upsample, reconstructing from texture_dimensions.",
"rl_simple_filter": "A single-tap resample under uv_xform.",
"rl_simple_texture": "A straight texture copy.",
"rl_combine_textures": "Combines two textures into one target.",
"rl_depth_of_field": "Depth of field. It computes a circle of confusion from the depth buffer "
    "against Focal_params and Coc_range_params and blends between the sharp image and the near and "
    "far blur buffers.",
"rl_bokeh_vs": "Part of the bokeh depth-of-field system. This file is a DXBC (Shader Model 5) blob "
    "sitting in the DX9 directory, so it belongs to the DX11 renderer and cannot be disassembled "
    "with the DX9 disassembler.",
"rl_bokeh_cs": "The bokeh compute shader. Like rl_bokeh_vs this is a DXBC (Shader Model 5) blob and "
    "belongs to the DX11 renderer, not the DX9 path.",
"rl_motion_blur": "Motion blur. Curr_to_prev reprojects each pixel into the previous frame to get a "
    "velocity, and Radial_blur_position/Radial_blur_radius add the radial component used for speed "
    "effects.",
"rl_distortion": "Applies the accumulated screen-distortion buffer to the back buffer, mixing in a "
    "blurred copy and Signal_noise.",
"rl_lut_blend": "Blends up to four colour-grading LUTs by their weights, so grading can crossfade "
    "between zones.",
"rl_yuv_convert": "Converts RGB to YUV planes, the inverse of the Bink path.",
"rl_filter_invalid_hdr": "Scrubs NaN and infinite values out of the HDR buffer before it is filtered.",
"rl_4xmsaa_restore": "Resolves a 4x MSAA surface back into a single-sampled one.",
"rl_4xmsaa_restore_qres": "The quarter-resolution MSAA resolve.",
"rl_4xmsaa_restore_z": "Resolves the MSAA depth buffer.",
"rl_particle_swizzle_msaa": "Rearranges MSAA samples for the particle path.",
"rl_restore_depth": "Writes a stored depth buffer back into the depth target.",
"rl_restore_depth_and_backbuffer": "Restores both the depth buffer and the back buffer in one pass.",
"rl_raycast": "A GPU ray-cast against the depth buffer: it reads depth at requested points and "
    "writes back world positions, which the game uses for queries it does not want to do on the CPU.",
"rl_corona": "Light coronas and lens flares - camera-facing quads oriented by Eye_orient with a "
    "colour-correction matrix applied.",
"rl_rain": "Rain streaks. Prev_cam_position and Speed shear the streaks against camera motion, and "
    "Rain_params/Size_params control density and length.",
"rl_mesh_distort": "Distorts a mesh's screen-space contribution into the distortion buffer, which "
    "rl_distortion later applies.",
"rl_near_projected": "Projects a texture onto near geometry - the effect used for things that must "
    "hug surfaces close to the camera.",
"rl_ground_reflect": "The ground-reflection material in its render-layer form: it reads the planar "
    "reflection map at s15 directly.",
"rl_ir_diffuse_color": "Writes flat Diffuse_Color and Specular_Power into the G-buffer with no "
    "textures at all - the fallback material for untextured geometry.",
"rl_ir_envmap": "Renders geometry into a dual-paraboloid environment probe.",
"rl_ir_floating_decal": "A decal that floats above the surface it covers, with its own albedo and "
    "normal map.",
"rl_prim_2d": "Untextured 2D primitives - the lines, rectangles and triangles the UI and debug "
    "systems draw. Prim_resolution_scale maps primitive coordinates onto the render target.",
"rl_prim_2d_tex": "Textured 2D primitives; this is the path most of the HUD goes through.",
"rl_prim_2d_bink": "2D primitives whose source is a Bink video frame, converting Y/Cr/Cb to RGB in "
    "the pixel shader - the full-screen movie path.",
"rl_prim_3d": "Untextured 3D primitives - debug lines and shapes in world space.",
"rl_prim_3d_tex": "Textured 3D primitives.",
"rl_ssao_singleframe_calculate": "Single-frame SSAO. It rebuilds view position and normal from the "
    "G-buffer, samples a kernel whose orientation comes from ssao_blue_noise_offsets at a world "
    "radius of ssao_sample_radius_reference, and writes an occlusion factor.",
"rl_ssao_singleframe_blur": "The bilateral blur over the SSAO buffer, controlled by blur_falloff and "
    "blur_sharpness so occlusion does not bleed across edges.",
"rl_ssao_singleframe_apply": "Applies the SSAO buffer to the scene.",
"rl_ssao_multiframe_calculate": "Temporal SSAO: the same kernel, but reprojected into last frame's "
    "result through ssao_curr_to_prev_xform and accumulated with ssao_temporal_falloff, which lets "
    "each frame take fewer samples.",
"rl_ssao_multiframe_apply": "Applies the temporally accumulated SSAO, modulating "
    "IR_GBuffer_Lighting directly.",
"rl_rao_calculate_box": "Analytic ambient occlusion from a box occluder: rao_view2box takes the "
    "reconstructed view position into the box's space and the occlusion is solved in closed form.",
"rl_rao_calculate_ellipsoid": "Analytic ambient occlusion from an ellipsoid occluder - the cheap "
    "grounded shadow under characters and vehicles.",
"rl_rao_apply": "Applies the analytic occlusion buffer.",
"rl_particle_downsample_depth": "Downsamples the depth buffer for the low-resolution particle pass.",
"rl_particle_brightpass": "Extracts the bright parts of the particle buffer for bloom.",
"rl_particle_composite": "Composites the offscreen particle buffer back over the scene, using the "
    "edge map to fix up pixels where the low-resolution buffer disagrees with full-resolution depth.",
"rl_particle_composite_low_res": "The low-resolution particle composite.",
"rl_particle_composite_offscreen": "Composites the offscreen particle buffer together with the back "
    "buffer and the bloom buffer.",
"clb_projector": "The character/light blob shadow projector: it projects a shadow map onto receiving "
    "geometry, with Shadow_percent_params and Shadow_fade_params controlling the softness and fade.",
"clb_projector_cheap": "The cheap blob-shadow projector, which extrudes a bounding box "
    "(Bbox_size/Extrusion_info) instead of sampling a real shadow map.",

# ---- sky ------------------------------------------------------------------
"rfg-skybox": "The base sky dome, and it is all done in the vertex shader. From the dome vertex's "
    "world-space up component the shader computes an elevation angle with a polynomial arcsine "
    "(the constants 1.5707963, 0.3183098 and 2.546479 are pi/2, 1/pi and 8/pi), scales it to the "
    "range 0..10, does mova a0.x and linearly interpolates between two adjacent stops of the "
    "ten-entry Hemisphere_colors ramp at c0. The pixel shader is two instructions: take the "
    "interpolated colour and multiply by Tint_color. Scale and Offset place the dome around the "
    "camera. This is the family the RTX Remix shim currently passes through rather than "
    "converting, which is why the sky is missing.",
"rfg-skybox-simple": "A textured sky dome with two tiled decal layers over the base map.",
"rfg-skybox-clouds": "A scrolling cloud layer lit by the time-of-day sun: TOD_Light_Color_Front and "
    "_Rear give the lit and shaded sides, Rimlight_scale/Rimlight_power add the silver lining, and "
    "Cloud_Fade_Height/Cloud_Full_Height fade the layer towards the horizon.",
"rfg-skybox-clouds-2": "The two-texture cloud variant, packing four cloud layers into two textures.",
"rfg-skybox-overhead": "The overhead cloud layer, the same recipe applied to the dome's cap.",
"rfg-skybox-matte": "The distant matte painting - mountains and skyline - with its own fog colour "
    "and density so it recedes correctly.",
"rfg-skybox-stars": "The star layer, faded by Star_strength and placed at Draw_distance, oriented "
    "by Eye_orient so the sky does not swim with the camera.",
"rfg-skybox-meteors": "Meteor streaks, animated from Time and scaled by Meteor_strength.",
"rfg-orbital": "The moon or other orbital body, drawn as a tinted quad on the sky dome.",

# ---- two-tone and the remaining plain materials --------------------------
"ir_srtwotonediffuse1": "Two-tone shading. The albedo texture is not used as colour: its alpha "
    "picks between Diffuse_Color and Diffuse_Color_2 per texel, and its green channel is the "
    "greyscale detail that both tints are multiplied by. One texture therefore serves any pair of "
    "colours, which is how the engine recolours signage, uniforms and props without new art.",
"ir_srtwotonediffuse_simple": "The two-tone recipe reduced to albedo only - no normal or specular "
    "map.",
"ir_srtwotonediffuse_no_spec": "Two-tone with a normal map but no specular map.",
"ir_srtwotonediffusedecal1": "Two-tone with a decal layer on a clamped UV range, plus separate "
    "tiling for the albedo and the decal.",
"ir_srtwotonediffusedecal2": "Two-tone with a decal layer, without the specular map.",
"ir_twotonediffuse1_nospecmap": "Two-tone with a normal map, a constant specular and a wetness "
    "mask.",
"ir_twotone_diff_de1_nospecmap": "Two-tone with a decal layer, a constant specular and a wetness "
    "mask.",
"ir_srviewsphere1": "A full material - albedo, specular, normal - with a sphere/matcap map blended "
    "in by Sphere_Map_Amount instead of a real environment probe.",
"ir_sr3diffspec": "The plainest textured material in the SR3 set: an albedo map and a specular map, "
    "with the surface normal coming from the vertex normal alone.",
"ir_sr3diffspec_decal": "Albedo and specular plus a decal layer, each with its own tiling.",
"ir_sr3diffspec_nospecmap": "Albedo only, with a constant specular and a wetness mask.",
"ir_sr3diffcolonly": "No textures at all: the surface is a flat Diffuse_Color with an Opacity_fade, "
    "still lit through the L-buffer like any other material.",
"ir_blend2simple1": "Two albedo layers combined by Diffuse_Map_Amount with a saturation control, "
    "over a shared normal and specular map.",
"ir_sr3simple1decalmask": "A masked overlay surface: albedo and specular are shaded normally, but "
    "the final alpha comes from a separate Alpha_Map, so the surface can be blended over whatever "
    "is underneath instead of replacing it. It also carries the wet-ground path.",
"ir_sr3simple2decalmask": "The masked overlay surface without the specular map.",
"ir_sr3cardiffusespec_g": "The plain vehicle surface with a grime layer on its own tiling.",

# ---- particles ------------------------------------------------------------
"rl_particle_standard": "A particle system. Almost all of the work is in the vertex shader (200-380 "
    "instruction slots): the vertex streams carry the particle's state - position, colours, size, "
    "rotation, age and flipbook frame - and the shader expands each particle into its quad, "
    "orients it, animates the flipbook through UV_anim_tiling, applies Effect_opacity and "
    "VFX_material_tint, and computes the fog. The pixel shader shades the sprite, applies "
    "Color_correct_matrix (a 3x3 held in c57-c59) and, in the soft-particle variants, fades the "
    "sprite where it approaches scene geometry by comparing against Depth_buffer through "
    "Inv_proj_matrix and Z_dimensions. The 'standard' family samples one diffuse texture and "
    "nothing else. Blobs that read Parametric_particle_constants (c70, 22 registers) are the "
    "parametric variants, which evaluate the particle's size/colour/rotation curves on the GPU "
    "instead of being fed per-vertex values.",
"rl_particle_mask": "A particle system whose sprite is eroded by a second Mask_Map_1 texture, and "
    "in the billboard and ribbon forms also offset by a distortion map. Everything else matches "
    "the standard particle path: the whole expansion and animation is done in the vertex shader "
    "and the pixel shader does the colour correction and the soft-particle depth fade.",
"rl_particle_cramp": "A colour-ramp particle. The base texture's green channel becomes the U "
    "coordinate and a per-particle parameter the V coordinate of Diffuse_Map_2, which is a 2D "
    "gradient - so a single greyscale sprite can be tinted along an arbitrary colour curve as the "
    "particle ages. The result is desaturated towards its own luminance by a per-vertex factor and "
    "then run through Color_correct_matrix. Fire, smoke and explosion sprites use this.",
"rl_particle_billboard": "Camera-facing sprites: the quad is built against Eye_orient so it always "
    "faces the viewer.",
"rl_particle_oriented": "Sprites aligned to a per-particle direction rather than to the camera.",
"rl_particle_radial": "Sprites expanded radially from the emitter, for shockwaves and rings.",
"rl_particle_ribbon": "Trail ribbons: consecutive particles are stitched into a continuous strip.",
"rl_particle_drop": "Stretched drop sprites - rain, sparks, and anything that should smear along "
    "its velocity.",
}

# Particle families are named <system>_<shape>; the note is composed from both.
PARTICLE_SYSTEM = {"standard": "rl_particle_standard", "mask": "rl_particle_mask",
                   "cramp": "rl_particle_cramp"}
PARTICLE_SHAPE = {"billboard": "rl_particle_billboard", "oriented": "rl_particle_oriented",
                  "radial": "rl_particle_radial", "ribbon": "rl_particle_ribbon",
                  "drop": "rl_particle_drop"}

# Regex fallbacks, tried in order, for families with no explicit entry.
GROUP_RULES = [
    (r"^ir_sr3carpaint", "ir_sr3carpaint"),
    (r"^ir_sr3carglass", "ir_sr3carglass"),
    (r"^ir_sr3carviewsphere", "ir_sr3carviewsphere"),
    (r"^ir_at_bbsimple1", "ir_bbsimple1"),
    (r"^ir_at_bbsimple2", "ir_bbsimple2"),
    (r"^ir_at_bbsimple3", "ir_bbsimple3"),
    (r"^ir_at_bbsimpledecal", "ir_bbsimpledecal1"),
    (r"^ir_at_bbstandard", "ir_bbstandard"),
    (r"^ir_at_bb_tod_window", "ir_bb_tod_window"),
    (r"^ir_at_sr2illumscroll", "ir_sr2illumscroll1"),
    (r"^ir_at_sr3decalonly", "ir_sr3decalonly"),
    (r"^ir_at_decalonly_cuberef", "ir_decalonly_cuberef"),
    (r"^ir_at_window_reflectmask", "ir_window_reflectmask"),
    (r"^ir_at_sr_tod_illum", "ir_at_sr_tod_illum"),
    (r"^ir_bbsimple1", "ir_bbsimple1"),
    (r"^ir_bbsimple2", "ir_bbsimple2"),
    (r"^ir_bbsimple3", "ir_bbsimple3"),
    (r"^ir_bbstandard", "ir_bbstandard"),
    (r"^ir_bbterrain", "ir_bbterrain1"),
    (r"^ir_bbblend", "ir_bbblend1"),
    (r"^ir_bb_tod_window", "ir_bb_tod_window"),
    (r"^ir_window_reflectmask", "ir_window_reflectmask"),
    (r"^ir_floor_reflect", "ir_floor_reflect"),
    (r"^ir_ground_reflect", "ir_ground_reflect"),
    (r"^ir_sr2illumscroll", "ir_sr2illumscroll1"),
    (r"^ir_sr_tod_illum_scroll", "ir_sr_tod_illum_scroll"),
    (r"^ir_sr3glass", "ir_sr3glass1"),
    (r"^ir_sr3ground", "ir_sr3ground1"),
    (r"^ir_sr3metal", "ir_sr3metal_dns"),
    (r"^ir_sr3shaderball", "ir_sr3shaderball"),
    (r"^ir_sr3npccloth", "ir_sr3npcclothfull"),
    (r"^ir_sr3pccloth", "ir_sr3pccloth"),
    (r"^ir_sr3npcskin", "ir_sr3npcskinfull"),
    (r"^ir_sr3pcskin", "ir_sr3pcskinfull"),
    (r"^ir_sr3skintest", "ir_sr3skintest2"),
    (r"^ir_sr3decalcube", "ir_sr3decalcube"),
    (r"^ir_sr3wallwindow", "ir_sr3wallwindow"),
    (r"^ir_windowwall", "ir_windowwall_production"),
    (r"^ir_legacy", "ir_legacy"),
    (r"^ir_decal", "ir_decal"),
    (r"^ir_blood_pool", "ir_blood_pool"),
    (r"^tree_cards", "tree_cards"),
    (r"^tree", "tree"),
    (r"^rl_ssao_multiframe", "rl_ssao_multiframe_calculate"),
    (r"^rl_ssao_singleframe", "rl_ssao_singleframe_calculate"),
    (r"^rl_picking", "rl_picking_depth_render"),
    (r"^rl_prim_2d", "rl_prim_2d"),
    (r"^rl_prim_3d", "rl_prim_3d"),
    (r"^clb_projector", "clb_projector"),
    (r"^cust_normal_map_blend", "cust_normal_map_blend"),
    (r"^privacy_bar", "privacy_bar"),
    (r"^editor_wireframe", "editor_wireframe"),
    (r"^editor_filled", "editor_filled"),
    (r"^debug_diffuse_only", "debug_diffuse_only"),
    (r"^bb-norender", "bb-norender"),
    (r"^stencil_opt", "stencil_opt"),
]

SUFFIX_RE = re.compile(r"_(s|bs|ms|c|mc|mv|v|fd)$")


# ---------------------------------------------------------------------------
# Derivation from the measured facts
# ---------------------------------------------------------------------------

def names(params, sampler=None):
    out = {}
    for p in params:
        is_s = p["reg"].startswith("s")
        if sampler is None or is_s == sampler:
            out[p["name"].replace("Sampler", "")] = p["reg"]
    return out


def classify_vs(sh):
    c = names(sh["params"], sampler=False)
    ins = {i["semantic"] for i in sh["inputs"]}
    if "IR_Light_Inv_Proj_TM" in c:
        return ("light-volume VS",
                "Transforms the light's bounding volume into clip space and emits the screen "
                "position and the view ray the pixel shader needs to rebuild each covered pixel's "
                "view-space position from the G-buffer depth.")
    if "Dual_Paraboloid_Transform" in c:
        return ("env-probe VS",
                "Transforms into a dual-paraboloid environment probe, so this geometry can appear "
                "in other surfaces' reflections.")
    if "Reflection_Plane_Height" in c:
        return ("planar-reflection VS",
                "Mirrors the geometry about Reflection_Plane_Height and emits a clip distance, "
                "producing the planar reflection texture that reflective floors and wet ground "
                "sample.")
    if "eyePos" in c or "Fog_dist" in c:
        return ("material-pass VS",
                "Feeds the shading resolve: it emits the UVs, the clip position again as a "
                "texcoord (the pixel shader divides by w to address the L-buffer in screen space), "
                "the per-instance albedo tint Diffuse_Color * Object_instance_params, the packed "
                "DSF id, and the exponential distance/height fog factor from Fog_dist.")
    if "IR_World2View" in c:
        return ("G-buffer VS",
                "Feeds the geometry prepass: it emits the UVs, the tangent frame rotated into view "
                "space, the view-space position, and the DSF id packed as "
                "(normal.w*0.5+0.5)*255 + instanceId*32640, all divided by 65535.")
    if "projTM" not in c and "objTM" not in c and len(sh["outputs"]) <= 3:
        return ("full-screen pass VS",
                "No transform matrices at all: it passes a screen-aligned quad straight through "
                "and emits the sampling coordinates the pixel shader needs. This is a full-screen "
                "post-processing or utility pass.")
    if len(sh["outputs"]) <= 2 and "position" in ins:
        return ("depth-only VS", "Position and UV only - the shape written into the depth buffer, "
                                 "or into a shadow map.")
    return ("VS", "Vertex processing for one of this effect's passes.")


def classify_ps(sh):
    s = names(sh["params"], sampler=True)
    c = names(sh["params"], sampler=False)
    outs = sh["color_outputs"]
    has_lbuf = "IR_LBuffer" in s or "Lbuffer" in s
    stipple = "IR_Stipple_Pattern_2D" in s

    if "IR_Light_Pos" in c:
        parts = ["Shades one light into the L-buffer. It rebuilds the covered pixel's view-space "
                 "position from IR_GBuffer_Depth through IR_Light_Inv_Proj_TM, reads its normal "
                 "from IR_GBuffer_Normals, evaluates the light, and accumulates diffuse into rgb "
                 "and specular into alpha."]
        if "IR_Spot_Info" in c:
            parts.append("The cone term is saturate((dot(-L, IR_Light_Dir) - IR_Spot_Info.y) * "
                         "IR_Spot_Info.z).")
        if "IR_Light_Info" in c:
            parts.append("Distance attenuation is pow(saturate(1 - (d - IR_Light_Info.y) / "
                         "(IR_Light_Info.z - IR_Light_Info.y)), IR_Light_Info.x), so "
                         "IR_Light_Info.z is the light's range.")
        if "ir_shadow_map" in s:
            parts.append("It also projects into the shadow map through Shadow_map_projTM.")
        if "Projection_texture" in s:
            parts.append("A cookie texture is projected through Projection_texture_xform.")
        return ("light-volume PS", " ".join(parts))
    if len(outs) >= 3 or "oC2" in outs:
        role = "G-buffer PS"
        text = ("Writes the inferred-lighting G-buffer as three render targets: oC0.xy is the "
                "view-space normal encoded as n*0.5+0.5, oC1.x is linear depth, oC2.x is the "
                "specular power, and the yzw channels of oC1 and oC2 carry the two halves of the "
                "DSF id. No lighting happens here.")
        if stipple:
            text += (" This is the stipple variant: it samples the dither pattern at "
                     "IR_Stipple_Pattern_2D and texkills the texels that lose, which is how the "
                     "engine dissolves objects in and out without alpha blending.")
        return (role, text)
    if has_lbuf:
        if "IR_Similarity_Data" in c:
            return ("material resolve PS (full DSF)",
                    "The high-quality shading resolve. It projects the pixel into the L-buffer, "
                    "takes a neighbourhood of DSF taps, compares each tap's id and depth against "
                    "this pixel using the IR_Similarity_Data thresholds, picks the taps that "
                    "belong to this surface, fetches the L-buffer there, then applies the "
                    "material: albedo * light + specular, plus Self_Illumination, then the fog "
                    "lerp towards Fog_color and the global Tint_color.")
        return ("material resolve PS",
                "The shading resolve. It divides the interpolated clip position by w to find its "
                "place in the low-resolution L-buffer, takes four DSF taps around it and rejects "
                "the ones whose id or depth disagree, samples the L-buffer at the surviving tap, "
                "and applies the material: albedo * light + specular, plus Self_Illumination, then "
                "the fog lerp towards Fog_color and the global Tint_color.")
    if "V_ambient_render" in c or "lightPos" in c:
        return ("forward-lit PS",
                "A cheap forward-lit shading path with no L-buffer at all: a hemisphere ambient "
                "between V_ambient_render and V_back_ambient_render plus one directional term from "
                "lightPos/V_light_color. This is what runs when the surface is being rendered into "
                "a reflection or an environment probe, where the deferred machinery is not "
                "available.")
    if "IR_GBuffer_Depth" in s or "IR_GBuffer_Normals" in s:
        return ("screen-space PS",
                "Reads the G-buffer directly and works in screen space, rebuilding view-space "
                "position from depth rather than from its own geometry.")
    if not s and not c and (sh["instr_slots"] or 0) <= 4:
        return ("null/depth PS",
                "Writes a constant and nothing else - the pixel shader of a depth-only or "
                "stencil-only pass.")
    if not s and ("Tint_color" in c or "Fog_color" in c):
        return ("vertex-shaded PS",
                "Samples nothing. All the shading was done per vertex and interpolated; this shader "
                "only applies the global tint and fog and writes the result.")
    if "texkill" in sh["flags"] and len(s) == 1 and "Alpha_Threshold" in c:
        return ("alpha-test depth PS",
                "Samples only the albedo's alpha and texkills against Alpha_Threshold. This is the "
                "cutout shape as the depth buffer and the shadow map need to see it.")
    if s:
        return ("post-process / utility PS",
                "Reads its inputs as flat textures rather than through the material path - a "
                "full-screen filter, a composite, or a utility pass.")
    return ("PS", "Pixel processing for one of this effect's passes.")


def family_note(base):
    if base in FAMILY:
        return FAMILY[base]
    match = re.match(r"^rl_particle_(\w+?)_(\w+)$", base)
    if match and match.group(1) in PARTICLE_SYSTEM and match.group(2) in PARTICLE_SHAPE:
        return (FAMILY[PARTICLE_SYSTEM[match.group(1)]] + " "
                + FAMILY[PARTICLE_SHAPE[match.group(2)]])
    for pattern, key in GROUP_RULES:
        if re.search(pattern, base):
            return FAMILY[key]
    return None


def composed_note(base):
    parts = []
    for pattern, phrase in TOKENS:
        if re.search(pattern, base):
            parts.append(phrase)
    if not parts:
        return "A shader whose role is not covered by a bespoke note; see the measured pass "\
               "breakdown below."
    head = "By its name and its measured inputs this is " + parts[0]
    rest = parts[1:]
    if rest:
        head += ". It also " + "; it also ".join(rest)
    return head + "."


TABLE = textwrap.TextWrapper(width=96, initial_indent="", subsequent_indent=" " * 41)


def entry_lines(reg, name, role):
    """One table row, wrapped so the role column stays inside 96 columns."""
    return TABLE.fill("  %-5s %-32s %s" % (reg, name, role))


def texkill_kind(sh, all_c):
    """What a texkill in this particular shader is discarding."""
    c = names(sh["params"], sampler=False)
    s = names(sh["params"], sampler=True)
    if "Alpha_Threshold" in c:
        return "texkill (alpha cutout against Alpha_Threshold)"
    if "IR_Stipple_Pattern_2D" in s:
        return "texkill (stipple dissolve)"
    return ("texkill (clip against a vertex-shader-computed distance - the mirror plane in the "
            "planar-reflection pass, the hemisphere boundary in the paraboloid pass)")


def describe(stem, info, asm_dir):
    base = SUFFIX_RE.sub("", stem)
    suffix = stem[len(base):].lstrip("_")
    shaders = info["shaders"]
    vs = [s for s in shaders if s["target"].startswith("vs")]
    ps = [s for s in shaders if s["target"].startswith("ps")]

    all_s, all_c, vs_c, ps_c = {}, {}, {}, {}
    flags = set()
    for sh in shaders:
        flags.update(sh["flags"])
        for name, reg in names(sh["params"], sampler=True).items():
            all_s.setdefault(name, reg)
        target = vs_c if sh["target"].startswith("vs") else ps_c
        for name, reg in names(sh["params"], sampler=False).items():
            target.setdefault(name, reg)
            all_c.setdefault(name, reg)

    out = []
    add = out.append
    bar = "=" * 96

    note = family_note(base)
    add(bar)
    add(" %s" % info["file"])
    add(bar)
    add("Container     : re/shaders/DX9/%s  (%d bytes)" % (info["file"], info["file_bytes"]))
    add("Disassembly   : re/shaders/DX9_disasm/%s.asm  (%d shader blobs)" % (stem, info["blob_count"]))
    add("Shader model  : %s" % (", ".join(sorted({s["target"] for s in shaders})) or "n/a"))
    add("Family        : %s" % base)
    if suffix:
        add("Geometry class: _%s - %s" % (suffix, VARIANTS[suffix][0]))
    add("Produced by   : tools/fxo_batch_disasm.py + tools/fxo_describe.py "
        "(D3DXDisassembleShader, d3dx9_43.dll)")
    add("")

    if not shaders:
        add("1. WHAT IT IS")
        add("")
        add(WRAP.fill(note or composed_note(base)))
        add("")
        add("2. WHY THERE IS NO DISASSEMBLY")
        add("")
        add(WRAP.fill(
            "This container holds no DX9 (SM 1-3) shader tokens. Its first four bytes are 'DXBC', "
            "the Direct3D 10/11 bytecode container, so it is a Shader Model 5 blob that belongs to "
            "the DX11 renderer and has been shipped into the DX9 directory. D3DXDisassembleShader "
            "cannot decode it; it would need the D3DCompiler D3DDisassemble entry point instead."))
        add("")
        add(bar)
        return "\n".join(out)

    add("1. WHAT IT IS")
    add("")
    add(WRAP.fill(note or composed_note(base)))
    add("")
    if stem.startswith(("ir_", "rl_ir_", "bb-norender", "tree", "water_", "cust_", "privacy")):
        add(WRAP.fill(INFERRED_LIGHTING))
        add("")

    if suffix:
        add("2. GEOMETRY CLASS  (_%s, %s)" % (suffix, VARIANTS[suffix][0]))
        add("")
        add(WRAP.fill(VARIANTS[suffix][1]))
        add("")
        streams = []
        for sh in vs:
            key = tuple(i["semantic"] for i in sh["inputs"])
            if key and key not in streams:
                streams.append(key)
        add(WRAP.fill(
            "Vertex declarations measured from this file's vertex shaders (one line per distinct "
            "declaration; the passes differ in how much of the frame they need):"))
        for key in streams:
            add(WRAP.fill("  " + ", ".join(key)))
        add("")
        section = 3
    else:
        section = 2

    add("%d. TEXTURES IT BINDS" % section)
    add("")
    add("  Sampler stages are the same in every pass that uses a given map.")
    add("")
    if all_s:
        for name, reg in sorted(all_s.items(), key=lambda kv: (int(kv[1][1:]), kv[0])):
            role = SAMPLER_ROLES.get(name, "texture input")
            add(entry_lines(reg, name, role))
    else:
        add("  (none - this effect samples no textures)")
    add("")
    section += 1

    add("%d. CONSTANTS IT READS" % section)
    add("")
    add(WRAP.fill(
        "Vertex-shader and pixel-shader constants live in separate register files, so the same "
        "number means different things on either side - c28 is projTM in a vertex shader and the "
        "ambient colour in a pixel shader. They are listed apart for that reason."))
    add("")
    for label, table in (("vertex-shader constants", vs_c), ("pixel-shader constants", ps_c)):
        if not table:
            continue
        add("  -- %s --" % label)
        for name, reg in sorted(table.items(), key=lambda kv: (kv[1][0], int(kv[1][1:]), kv[0])):
            add(entry_lines(reg, name, const_role(name)))
        add("")
    if not all_c:
        add("  (none)")
        add("")
    section += 1

    add("%d. PASSES INSIDE THIS CONTAINER" % section)
    add("")
    add(WRAP.fill(
        "A .fxo_pc is not one shader: it is every technique variant of one effect, concatenated. "
        "Each blob below is classified by the render pass it serves, from what it actually reads "
        "and writes."))
    add("")
    for sh in shaders:
        role, text = (classify_vs(sh) if sh["target"].startswith("vs") else classify_ps(sh))
        add("  [%d] %-7s  %-28s %s instruction slots"
            % (sh["index"], sh["target"], role, sh["instr_slots"] if sh["instr_slots"] else "?"))
        if sh["inputs"]:
            add(WRAP_I.fill("in : " + ", ".join(
                "%s(%s)" % (i["reg"], i["semantic"]) for i in sh["inputs"])))
        if sh["outputs"]:
            add(WRAP_I.fill("out: " + ", ".join(
                "%s(%s)" % (o["reg"], o["semantic"]) for o in sh["outputs"])))
        if sh["color_outputs"]:
            add(WRAP_I.fill("targets: " + ", ".join(sh["color_outputs"])))
        smp = names(sh["params"], sampler=True)
        if smp:
            add(WRAP_I.fill("samplers: " + ", ".join(
                "%s=%s" % (r, n) for n, r in sorted(smp.items(), key=lambda kv: int(kv[1][1:])))))
        cst = names(sh["params"], sampler=False)
        if cst:
            add(WRAP_I.fill("constants: " + ", ".join(
                "%s@%s" % (n, r) for n, r in sorted(cst.items(), key=lambda kv: kv[1]))))
        extra = [f for f in sh["flags"] if f not in ("pp_precision", "centroid")]
        if extra:
            extra = [texkill_kind(sh, all_c) if f == "texkill" else f for f in extra]
            add(WRAP_I.fill("notable: " + ", ".join(extra)))
        add(WRAP_I.fill(text))
        add("")
    section += 1

    add("%d. HOW THE SHADING WORKS" % section)
    add("")
    notes = []
    if any(classify_ps(p)[0].startswith("G-buffer") for p in ps):
        notes.append(
            "Geometry prepass. The G-buffer pixel shader unpacks the normal map (the two stored "
            "channels are rescaled by 2x-1, z is reconstructed, and the xy tilt is scaled by "
            "Normal_Map_Height), rotates it through the interpolated tangent/binormal/normal basis "
            "which the vertex shader already put in view space, and writes it to oC0.xy. Linear "
            "depth goes to oC1.x and Specular_Power to oC2.x. The remaining channels carry the DSF "
            "id, which is what lets the resolve pass tell this surface apart from whatever is "
            "behind it.")
    if any("material resolve" in classify_ps(p)[0] for p in ps):
        notes.append(
            "Lighting resolve. Because the L-buffer is at a lower resolution than the frame, a "
            "straight bilinear fetch would bleed light across silhouettes. The resolve instead "
            "reads IR_GBuffer_DSF_DataSampler at the four L-buffer texels around the pixel, "
            "compares each tap's stored id and depth against this pixel's own, and uses cmp to "
            "select the taps that match. IR_Pixel_Steps carries the L-buffer texel size and "
            "resolution needed to build those addresses. The surviving L-buffer sample gives rgb = "
            "diffuse light and a = specular.")
    if "Alpha_Threshold" in all_c:
        notes.append(
            "Cutouts. Transparency here is not D3DRS_ALPHATESTENABLE: the shader issues texkill on "
            "(alpha - Alpha_Threshold), so the discard is invisible to render state. A render-state "
            "rule cannot see these cutouts, which matters to anything trying to reconstruct the "
            "material from outside the shader. Alpha_test_use_dsf, when present, switches the test "
            "from a hard cut to one folded through the DSF coverage so the cut edge does not "
            "shimmer.")
    elif "texkill" in flags:
        notes.append(
            "The texkills in this container are not an alpha test - there is no Alpha_Threshold. "
            "They discard against a distance the vertex shader computed: the signed distance to "
            "Reflection_Plane_Height in the planar-reflection pass, and the paraboloid-space z in "
            "the environment-probe pass, so each reflection render only keeps the half of the "
            "world it should.")
    if "IR_Stipple_Pattern_2D" in all_s:
        notes.append(
            "Fades. Objects do not fade with alpha blending; they fade with a screen-door stipple. "
            "The shader samples a dither pattern at IR_Stipple_Pattern_2D, offset by "
            "IR_Stipple_Pattern_Offset and repeated by IR_Stipple_Repeat_Info, and texkills the "
            "texels below the current fade level.")
    if "Bone_weights" in all_c:
        notes.append(
            "Skinning happens entirely in the vertex shader against the 64-bone palette at c52. "
            "The vertex buffer holds the bind pose and never changes, so the animation exists only "
            "in those constants.")
    if any(n.startswith("Dual_Paraboloid") for n in all_s):
        notes.append(
            "Reflections come from a dual-paraboloid probe: two hemispherical maps, front at s10 "
            "and back at s11, selected by the sign of the reflection vector's z. That is why the "
            "container also contains a vertex shader carrying Dual_Paraboloid_Transform - the same "
            "effect must be able to render *into* a probe as well as read one.")
    if "Planar_Reflection_Map" in all_s:
        notes.append(
            "Reflections come from a planar mirror render target sampled in screen space, which is "
            "why the container also holds a vertex shader with Reflection_Plane_Height: that pass "
            "renders the world mirrored about the plane to fill the target.")
    if "Fog_color" in all_c:
        notes.append(
            "Every material pass ends the same way: lerp towards Fog_color by the vertex fog "
            "factor, then multiply by the global Tint_color.")
    if not notes:
        notes.append(
            "This container does not follow the material skeleton - see the per-pass notes above "
            "for what each blob does.")
    for i, n in enumerate(notes):
        add(WRAP.fill(n))
        if i != len(notes) - 1:
            add("")
    add("")
    section += 1

    add("%d. NOTES FOR THE RTX REMIX SHIM" % section)
    add("")
    hints = []
    albedo = None
    for candidate in ("Diffuse_Map", "Diffuse_map", "Diffuse_Map_1", "Pattern_Map", "Decal_Map",
                      "Y_tex", "Mask_Map", "Orbital_map"):
        if candidate in all_s:
            albedo = (candidate, all_s[candidate])
            break
    if albedo:
        hints.append("Albedo to bind at stage 0 for a fixed-function re-issue: %s, which this "
                     "effect samples at %s." % (albedo[0], albedo[1]))
    else:
        hints.append("There is no albedo texture to bind: this effect's colour comes from "
                     "constants, from vertex colour, or from a buffer it reads back.")
    tiling = {n: r for n, r in all_c.items() if "Tiling" in n or "TilingU" in n or "TilingV" in n}
    if tiling:
        hints.append("UV tiling lives at " + ", ".join(
            "%s=%s" % (n, r) for n, r in sorted(tiling.items())) +
            ". The UV formula is uv = raw * tiling / 1024, and these registers differ between "
            "shaders, so they have to be read from this file's CTAB rather than assumed.")
    mat = []
    if "projTM" in all_c:
        mat.append("projTM at %s (VIEW*PROJ)" % all_c["projTM"])
    if "objTM" in all_c:
        mat.append("objTM at %s (world)" % all_c["objTM"])
    if "IR_World2View" in all_c:
        mat.append("IR_World2View at %s (view)" % all_c["IR_World2View"])
    if "Bone_weights" in all_c:
        mat.append("the bone palette at %s" % all_c["Bone_weights"])
    if mat:
        hints.append("Matrices: " + ", ".join(mat) + ".")
    if suffix == "bs":
        hints.append("This is the instanced variant: the world matrix is NOT in a constant "
                     "register, it arrives per instance in vertex streams v4/v5/v6. Anything "
                     "recovering a world transform from constants will get the wrong answer here.")
    if suffix == "fd":
        hints.append("This is the pre-transformed variant: vertices are already in world space and "
                     "there is no objTM at all, so the object transform is the identity.")
    if suffix in ("c", "mc"):
        hints.append("Skinned: the vertex buffer holds the bind pose and every frame of animation "
                     "lives in the c52 constants, so geometry submitted as-is will be frozen in "
                     "the bind pose unless it is skinned first.")
    if suffix in ("v", "mv"):
        hints.append("Rigid-bone: each vertex names one bone by index with no weight, so the mesh "
                     "is a hierarchy of rigid pieces posed from the c52 palette.")
    if "Alpha_Threshold" in all_c:
        hints.append("Cutouts are done with texkill, not D3DRS_ALPHATESTENABLE, so alpha testing "
                     "cannot be detected from render state for this effect - it has to be read "
                     "out of the shader.")
    if "IR_Stipple_Pattern_2D" in all_s:
        hints.append("Fades are stippled with texkill rather than alpha blended.")
    if any("material resolve" in classify_ps(p)[0] for p in ps):
        hints.append("The material passes read the L-buffer, an offscreen render target. Draws "
                     "using them are the ones a path tracer sees as 'non-primary render target' "
                     "work.")
    for i, h in enumerate(hints):
        add(WRAP.fill(h))
        if i != len(hints) - 1:
            add("")
    add("")
    add(WRAP.fill(SHARED_REGISTERS))
    add("")
    add(bar)
    return "\n".join(out)


def first_sentence(text):
    match = re.search(r"^(.+?\.)(\s|$)", text.replace("\n", " "))
    return match.group(1) if match else text[:120]


def write_overview(out_dir, facts):
    bar = "=" * 96
    out = [bar, " SR3 DX9 shader library - overview", bar, ""]
    add = out.append
    total_shaders = sum(len(i["shaders"]) for i in facts.values())
    add("Containers          : %d" % len(facts))
    add("Compiled shaders    : %d (Shader Model 3.0)" % total_shaders)
    add("Disassembly         : re/shaders/DX9_disasm/<name>.asm")
    add("Per-file write-ups  : re/shaders/DX9_analysis/<name>.txt")
    add("Regenerate with     : python tools/fxo_batch_disasm.py re/shaders/DX9 "
        "re/shaders/DX9_disasm re/shaders/dx9_facts.json")
    add("                      python tools/fxo_describe.py re/shaders/dx9_facts.json "
        "re/shaders/DX9_disasm re/shaders/DX9_analysis")
    add("")
    add("-" * 96)
    add(" 1. HOW A FRAME IS PUT TOGETHER")
    add("-" * 96)
    add("")
    add(WRAP.fill(INFERRED_LIGHTING))
    add("")
    add(WRAP.fill(SHARED_REGISTERS))
    add("")
    add("-" * 96)
    add(" 2. WHAT IS INSIDE ONE .fxo_pc")
    add("-" * 96)
    add("")
    add(WRAP.fill(
        "A .fxo_pc is a container: the magic EE A1 42 4B, then every compiled technique variant of "
        "one effect concatenated, each keeping an intact CTAB so constant names survive. A typical "
        "world material holds eleven blobs, and they line up like this:"))
    add("")
    for line in (
        "vs  G-buffer VS              -> feeds the geometry prepass",
        "vs  G-buffer VS (stipple)    -> same, with one extra varying for the dither lookup",
        "vs  material-pass VS         -> feeds the shading resolve",
        "vs  material-pass VS         -> second resolve variant",
        "vs  planar-reflection VS     -> renders the mirror image, clipped at the mirror plane",
        "vs  env-probe VS             -> renders into a dual-paraboloid reflection probe",
        "ps  G-buffer PS              -> writes normal / depth / specular power / DSF id (MRT)",
        "ps  G-buffer PS (stipple)    -> same, plus the screen-door texkill",
        "ps  material resolve PS      -> 4-tap DSF fetch of the L-buffer, then the material",
        "ps  material resolve PS      -> full DSF filter with IR_Similarity_Data",
        "ps  forward-lit PS           -> hemisphere ambient + one light, for reflection renders",
    ):
        add("    " + line)
    add("")
    add(WRAP.fill(
        "Alpha-tested (ir_at_*) effects add a depth-only vertex/pixel pair at the front, so the "
        "cutout shape can be laid into the depth buffer and the shadow map."))
    add("")
    add("-" * 96)
    add(" 3. THE FILENAME SUFFIX IS THE GEOMETRY CLASS")
    add("-" * 96)
    add("")
    add(WRAP.fill(
        "The same effect is compiled once per kind of geometry it can be applied to. The suffix "
        "says which, and it is the single most useful thing to read off a filename. All eight "
        "were confirmed from the vertex declarations of ir_bbsimple1_*, which exists in every "
        "variant."))
    add("")
    for suffix in ("s", "bs", "ms", "c", "mc", "v", "mv", "fd"):
        short, long = VARIANTS[suffix]
        add("  _%-3s %s" % (suffix, short))
        add(WRAP_I.fill(long))
        add("")
    add("-" * 96)
    add(" 4. FAMILIES")
    add("-" * 96)
    add("")
    bases = {}
    for stem in facts:
        bases.setdefault(SUFFIX_RE.sub("", stem), []).append(stem)
    for base in sorted(bases):
        note = family_note(base) or composed_note(base)
        variants = sorted(s[len(base):].lstrip("_") or "-" for s in bases[base])
        add("  %s   [%s]" % (base, ",".join(variants)))
        add(WRAP_I.fill(first_sentence(note)))
        add("")
    add(bar)
    path = os.path.join(out_dir, "_OVERVIEW.txt")
    with open(path, "w", encoding="utf-8", newline=chr(10)) as handle:
        handle.write("\n".join(out) + "\n")
    return path


def write_index(out_dir, facts):
    rows = []
    for stem, info in sorted(facts.items()):
        base = SUFFIX_RE.sub("", stem)
        suffix = stem[len(base):].lstrip("_")
        roles = []
        for sh in info["shaders"]:
            role = (classify_vs(sh) if sh["target"].startswith("vs") else classify_ps(sh))[0]
            if role not in roles:
                roles.append(role)
        rows.append((stem, len(info["shaders"]),
                     VARIANTS[suffix][0] if suffix else "-", ", ".join(roles)))
    width = max(len(r[0]) for r in rows) + 2
    out = ["=" * 120,
           " SR3 DX9 shader library - index of %d containers" % len(rows),
           " columns: file | shader blobs | geometry class | passes present",
           "=" * 120, ""]
    for stem, n, cls, roles in rows:
        out.append("%-*s %3d  %-34s %s" % (width, stem + ".txt", n, cls, roles))
    path = os.path.join(out_dir, "_INDEX.txt")
    with open(path, "w", encoding="utf-8", newline=chr(10)) as handle:
        handle.write("\n".join(out) + "\n")
    return path


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    facts_path, asm_dir, out_dir = argv[0], argv[1], argv[2]
    os.makedirs(out_dir, exist_ok=True)
    facts = json.load(open(facts_path, encoding="utf-8"))

    written = 0
    no_note = []
    for stem, info in sorted(facts.items()):
        text = describe(stem, info, asm_dir)
        path = os.path.join(out_dir, stem + ".txt")
        with open(path, "w", encoding="utf-8", newline=chr(10)) as handle:
            handle.write(text + chr(10))
        written += 1
        if family_note(SUFFIX_RE.sub("", stem)) is None:
            no_note.append(stem)

    print("descriptions written: %d -> %s" % (written, out_dir))
    print("overview: %s" % write_overview(out_dir, facts))
    print("index:    %s" % write_index(out_dir, facts))
    if no_note:
        bases = sorted({SUFFIX_RE.sub("", s) for s in no_note})
        print("families falling back to composed text: %d (%d files)"
              % (len(bases), len(no_note)))
        for b in bases:
            print("   " + b)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
