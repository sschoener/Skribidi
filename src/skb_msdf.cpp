// SPDX-FileCopyrightText: 2025
// SPDX-License-Identifier: MIT

#include "skb_rasterizer.h"
#include "skb_font_collection_internal.h"
#include "skb_common.h"

#include <hb.h>
#include <msdfgen.h>
#include <cstring>

// Context for building msdfgen Shape from HarfBuzz glyph outline
struct MsdfBuildContext {
    msdfgen::Shape* shape;
    msdfgen::Contour* contour;
    msdfgen::Point2 lastPoint;
};

// HarfBuzz draw callbacks for MSDF - coordinates are in font units
static void msdf_move_to(hb_draw_funcs_t* dfuncs, void* draw_data, hb_draw_state_t* st,
                         float to_x, float to_y, void* user_data) {
    (void)dfuncs; (void)st; (void)user_data;
    MsdfBuildContext* ctx = (MsdfBuildContext*)draw_data;

    // Start a new contour
    ctx->contour = &ctx->shape->addContour();
    ctx->lastPoint = msdfgen::Point2(to_x, to_y);
}

static void msdf_line_to(hb_draw_funcs_t* dfuncs, void* draw_data, hb_draw_state_t* st,
                         float to_x, float to_y, void* user_data) {
    (void)dfuncs; (void)st; (void)user_data;
    MsdfBuildContext* ctx = (MsdfBuildContext*)draw_data;

    if (!ctx->contour) return;

    msdfgen::Point2 to(to_x, to_y);
    ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->lastPoint, to));
    ctx->lastPoint = to;
}

static void msdf_quadratic_to(hb_draw_funcs_t* dfuncs, void* draw_data, hb_draw_state_t* st,
                              float c1x, float c1y, float to_x, float to_y, void* user_data) {
    (void)dfuncs; (void)st; (void)user_data;
    MsdfBuildContext* ctx = (MsdfBuildContext*)draw_data;

    if (!ctx->contour) return;

    msdfgen::Point2 c1(c1x, c1y);
    msdfgen::Point2 to(to_x, to_y);
    ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->lastPoint, c1, to));
    ctx->lastPoint = to;
}

static void msdf_cubic_to(hb_draw_funcs_t* dfuncs, void* draw_data, hb_draw_state_t* st,
                          float c1x, float c1y, float c2x, float c2y,
                          float to_x, float to_y, void* user_data) {
    (void)dfuncs; (void)st; (void)user_data;
    MsdfBuildContext* ctx = (MsdfBuildContext*)draw_data;

    if (!ctx->contour) return;

    msdfgen::Point2 c1(c1x, c1y);
    msdfgen::Point2 c2(c2x, c2y);
    msdfgen::Point2 to(to_x, to_y);
    ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->lastPoint, c1, c2, to));
    ctx->lastPoint = to;
}

static void msdf_close_path(hb_draw_funcs_t* dfuncs, void* draw_data, hb_draw_state_t* st,
                            void* user_data) {
    (void)dfuncs; (void)st; (void)user_data;
    // Contour is implicitly closed in msdfgen
}

extern "C" bool skb_rasterizer_draw_msdf_glyph(
    skb_rasterizer_t* rasterizer,
    skb_temp_alloc_t* temp_alloc,
    uint32_t glyph_id,
    const skb_font_t* font,
    float font_size,
    float offset_x,
    float offset_y,
    skb_image_t* target)
{
    (void)rasterizer;
    (void)temp_alloc;

    if (!font || !target || target->bpp != 3) {
        return false;
    }

    // Create msdfgen shape
    msdfgen::Shape shape;

    // Set up HarfBuzz draw functions for MSDF
    hb_draw_funcs_t* draw_funcs = hb_draw_funcs_create();
    hb_draw_funcs_set_move_to_func(draw_funcs, msdf_move_to, nullptr, nullptr);
    hb_draw_funcs_set_line_to_func(draw_funcs, msdf_line_to, nullptr, nullptr);
    hb_draw_funcs_set_quadratic_to_func(draw_funcs, msdf_quadratic_to, nullptr, nullptr);
    hb_draw_funcs_set_cubic_to_func(draw_funcs, msdf_cubic_to, nullptr, nullptr);
    hb_draw_funcs_set_close_path_func(draw_funcs, msdf_close_path, nullptr, nullptr);
    hb_draw_funcs_make_immutable(draw_funcs);

    // Build context
    MsdfBuildContext ctx;
    ctx.shape = &shape;
    ctx.contour = nullptr;

    // Extract glyph outline (coordinates from HarfBuzz are in font units)
    hb_font_draw_glyph(font->hb_font, glyph_id, draw_funcs, &ctx);
    hb_draw_funcs_destroy(draw_funcs);

    // Check if shape is valid
    if (shape.contours.empty()) {
        // Empty glyph (like space) - fill with "outside" value (0 = outside)
        memset(target->buffer, 0, target->width * target->height * 3);
        return true;
    }

    // Normalize shape edges and apply edge coloring
    shape.normalize();
    msdfgen::edgeColoringSimple(shape, 3.0);

    // Create msdfgen bitmap
    msdfgen::Bitmap<float, 3> msdf(target->width, target->height);

    // Use the same scale as the regular SDF rasterizer:
    // font_size * upem_scale converts font units to pixels
    double scale = (double)(font_size * font->upem_scale);

    // The projection transforms shape coords (font units) to bitmap coords.
    // msdfgen Projection: output = scale * (coord + translate)
    // We want: pixel_x = font_x * scale + offset_x  (offset in pixels)
    //          pixel_y = font_y * (-scale) + offset_y
    // Therefore: translate_x = offset_x / scale
    //            translate_y = -offset_y / scale (negative because of Y flip)
    msdfgen::Projection projection(
        msdfgen::Vector2(scale, -scale),
        msdfgen::Vector2(offset_x / scale, -offset_y / scale)
    );

    // Pixel range for SDF - how many pixels of distance gradient around edges
    double pxRange = 4.0;

    // The range in shape units. Convert pixel range to font units.
    // pxRange pixels = pxRange / scale font units
    msdfgen::Range range(pxRange / scale);

    msdfgen::generateMSDF(msdf, shape, projection, range);

    // Copy to target buffer
    // MSDF output: distances normalized so ±(pxRange/scale) font units -> ±1
    // Map to 0.5 ± 0.5 for 8-bit output
    // Use stride_bytes for proper atlas rendering (may differ from width * bpp)
    int row_stride = target->stride_bytes > 0 ? target->stride_bytes : target->width * 3;
    for (int y = 0; y < target->height; y++) {
        for (int x = 0; x < target->width; x++) {
            uint8_t* dst = target->buffer + y * row_stride + x * 3;

            float r = (float)msdf(x, y)[0];
            float g = (float)msdf(x, y)[1];
            float b = (float)msdf(x, y)[2];

            // Due to Y-axis flip in projection, msdfgen sees inverted winding:
            // positive = inside, negative = outside (opposite of normal)
            // Standard MSDF: high values (>0.5) = inside, low (<0.5) = outside
            float nr = 0.5f + r * 0.5f;
            float ng = 0.5f + g * 0.5f;
            float nb = 0.5f + b * 0.5f;

            // Clamp and convert to 0-255
            dst[0] = (uint8_t)(skb_clampf(nr, 0.0f, 1.0f) * 255.0f + 0.5f);
            dst[1] = (uint8_t)(skb_clampf(ng, 0.0f, 1.0f) * 255.0f + 0.5f);
            dst[2] = (uint8_t)(skb_clampf(nb, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    return true;
}
