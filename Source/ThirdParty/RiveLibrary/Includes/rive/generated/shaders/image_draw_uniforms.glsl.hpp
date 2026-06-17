#pragma once

#include "image_draw_uniforms.glsl.exports.h"

namespace rive {
namespace gpu {
namespace glsl {
const char image_draw_uniforms[] = R"===(#ifndef DRAW_IMAGE_UNIFORMS_NAME
#define DRAW_IMAGE_UNIFORMS_NAME  EXPORTED_ImageDrawUniforms
#endif
UNIFORM_BLOCK_BEGIN(IMAGE_DRAW_UNIFORM_BUFFER_IDX, DRAW_IMAGE_UNIFORMS_NAME)
DECLARE_UNIFORM_FLOAT4(viewMatrix)
DECLARE_UNIFORM_FLOAT2(translate)
DECLARE_UNIFORM_FLOAT(opacity)
DECLARE_UNIFORM_FLOAT(padding)
// clipRectInverseMatrix transforms from pixel coordinates to a space where the
// clipRect is the normalized rectangle: [-1, -1, 1, 1].
DECLARE_UNIFORM_FLOAT4(clipRectInverseMatrix)
DECLARE_UNIFORM_FLOAT2(clipRectInverseTranslate)
DECLARE_UNIFORM_UINT(clipID)
DECLARE_UNIFORM_UINT(blendMode)
DECLARE_UNIFORM_UINT(zIndex)
UNIFORM_BLOCK_END(imageDrawUniforms)
)===";
} // namespace glsl
} // namespace gpu
} // namespace rive