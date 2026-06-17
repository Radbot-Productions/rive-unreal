#pragma once

#include "draw_image_mesh.vert.exports.h"

namespace rive {
namespace gpu {
namespace glsl {
const char draw_image_mesh_vert[] = R"===(/*
 * Copyright 2023 Rive
 */

#ifdef EXPORTED_VERTEX
ATTR_BLOCK_BEGIN(PositionAttr)
ATTR(0, float2, EXPORTED_a_position);
ATTR_BLOCK_END

ATTR_BLOCK_BEGIN(UVAttr)
ATTR(1, float2, EXPORTED_a_texCoord);
ATTR_BLOCK_END
#endif

VARYING_BLOCK_BEGIN
NO_PERSPECTIVE VARYING(0, float2, v_texCoord);
#ifdef EXPORTED_ENABLE_CLIPPING
EXPORTED_OPTIONALLY_FLAT VARYING(1, half, v_clipID);
#endif
#if defined(EXPORTED_ENABLE_CLIP_RECT) && !defined(EXPORTED_RENDER_MODE_MSAA)
NO_PERSPECTIVE VARYING(2, float4, v_clipRect);
#endif
VARYING_BLOCK_END

#ifdef EXPORTED_VERTEX
VERTEX_TEXTURE_BLOCK_BEGIN
VERTEX_TEXTURE_BLOCK_END

IMAGE_MESH_VERTEX_MAIN(EXPORTED_drawVertexMain,
                       PositionAttr,
                       position,
                       UVAttr,
                       uv,
                       _vertexID)
{
    ATTR_UNPACK(_vertexID, position, EXPORTED_a_position, float2);
    ATTR_UNPACK(_vertexID, uv, EXPORTED_a_texCoord, float2);

    VARYING_INIT(v_texCoord, float2);
#ifdef EXPORTED_ENABLE_CLIPPING
    VARYING_INIT(v_clipID, half);
#endif
#if defined(EXPORTED_ENABLE_CLIP_RECT) && !defined(EXPORTED_RENDER_MODE_MSAA)
    VARYING_INIT(v_clipRect, float4);
#endif

    float2 vertexPosition =
        MUL(make_float2x2(imageDrawUniforms.viewMatrix), EXPORTED_a_position) +
        imageDrawUniforms.translate;
    v_texCoord = EXPORTED_a_texCoord;
#ifdef EXPORTED_ENABLE_CLIPPING
    if (EXPORTED_ENABLE_CLIPPING)
    {
        v_clipID = id_bits_to_f16(imageDrawUniforms.clipID,
                                  uniforms.pathIDGranularity);
    }
#endif
#ifdef EXPORTED_ENABLE_CLIP_RECT
    if (EXPORTED_ENABLE_CLIP_RECT)
    {
#ifndef EXPORTED_RENDER_MODE_MSAA
        v_clipRect = find_clip_rect_coverage_distances(
            make_float2x2(imageDrawUniforms.clipRectInverseMatrix),
            imageDrawUniforms.clipRectInverseTranslate,
            vertexPosition CLIP_CONTEXT_UNPACK);
#else
        set_clip_rect_plane_distances(
            make_float2x2(imageDrawUniforms.clipRectInverseMatrix),
            imageDrawUniforms.clipRectInverseTranslate,
            vertexPosition CLIP_CONTEXT_UNPACK);
#endif
    }
#endif // ENABLE_CLIP_RECT
    float4 pos = RENDER_TARGET_COORD_TO_CLIP_COORD(vertexPosition);
#ifdef EXPORTED_POST_INVERT_Y
    pos.y = -pos.y;
#endif
#ifdef EXPORTED_RENDER_MODE_MSAA
    pos.z = normalize_z_index(imageDrawUniforms.zIndex);
#endif

    VARYING_PACK(v_texCoord);
#ifdef EXPORTED_ENABLE_CLIPPING
    VARYING_PACK(v_clipID);
#endif
#if defined(EXPORTED_ENABLE_CLIP_RECT) && !defined(EXPORTED_RENDER_MODE_MSAA)
    VARYING_PACK(v_clipRect);
#endif
    EMIT_VERTEX(pos);
}
#endif
)===";
} // namespace glsl
} // namespace gpu
} // namespace rive