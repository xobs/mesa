/*
 * Copyright (c) 2012-2015 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include "etnaviv_context.h"

#include "etnaviv_blend.h"
#include "etnaviv_clear_blit.h"
#include "etnaviv_compiler.h"
#include "etnaviv_debug.h"
#include "etnaviv_emit.h"
#include "etnaviv_fence.h"
#include "etnaviv_rasterizer.h"
#include "etnaviv_screen.h"
#include "etnaviv_shader.h"
#include "etnaviv_state.h"
#include "etnaviv_surface.h"
#include "etnaviv_texture.h"
#include "etnaviv_transfer.h"
#include "etnaviv_translate.h"
#include "etnaviv_zsa.h"

#include "pipe/p_context.h"
#include "util/u_blitter.h"
#include "util/u_memory.h"
#include "util/u_prim.h"

#include "hw/common.xml.h"

static void etna_context_destroy(struct pipe_context *pctx)
{
    struct etna_context *ctx = etna_context(pctx);

    if (ctx->primconvert)
        util_primconvert_destroy(ctx->primconvert);

    if (ctx->blitter)
        util_blitter_destroy(ctx->blitter);

    if (ctx->stream)
        etna_cmd_stream_del(ctx->stream);

    util_slab_destroy(&ctx->transfer_pool);

    FREE(pctx);
}

static void etna_draw_vbo(struct pipe_context *pctx,
                 const struct pipe_draw_info *info)
{
    struct etna_context *ctx = etna_context(pctx);
    uint32_t draw_mode;

    if (ctx->vertex_elements == NULL || ctx->vertex_elements->num_elements == 0)
        return; /* Nothing to do */

    if (!(ctx->prim_hwsupport & (1 << info->mode))) {
        struct primconvert_context *primconvert = ctx->primconvert;
        util_primconvert_save_index_buffer(primconvert, &ctx->index_buffer);
        util_primconvert_save_rasterizer_state(primconvert, ctx->rasterizer);
        util_primconvert_draw_vbo(primconvert, info);
        return;
    }

    int prims = u_decomposed_prims_for_vertices(info->mode, info->count);
    if (unlikely(prims <= 0))
    {
        DBG("Invalid draw primitive mode=%i or no primitives to be drawn", info->mode);
        return;
    }

    /* First, sync state, then emit DRAW_PRIMITIVES or DRAW_INDEXED_PRIMITIVES */
    etna_emit_state(ctx);

    if (ctx->vs && ctx->vertex_elements->num_elements != ctx->vs->num_inputs)
    {
        BUG("Number of elements %i does not match the number of VS inputs %i",
                ctx->vertex_elements->num_elements, ctx->vs->num_inputs);
        return;
    }

    draw_mode = translate_draw_mode(info->mode);
    if (draw_mode == ETNA_NO_MATCH) {
        BUG("Unsupported draw mode");
        return;
    }

    if (info->indexed)
    {
        etna_draw_indexed_primitives(ctx->stream, draw_mode, info->start,
                                     prims, info->index_bias);
    }
    else
    {
        etna_draw_primitives(ctx->stream, draw_mode, info->start, prims);
    }

    if (DBG_ENABLED(ETNA_DBG_FLUSH_ALL))
    {
        pctx->flush(pctx, NULL, 0);
    }
}

static void etna_flush(struct pipe_context *pctx,
             struct pipe_fence_handle **fence,
             enum pipe_flush_flags flags)
{
    if (fence)
        *fence = etna_fence_create(pctx);

    /* TODO */
}

static void etna_cmd_stream_reset_notify(struct etna_cmd_stream *stream, void *priv)
{
    struct etna_context *ctx = priv;

    etna_set_state(stream, VIVS_GL_API_MODE, VIVS_GL_API_MODE_OPENGL);
    etna_set_state(stream, VIVS_GL_VERTEX_ELEMENT_CONFIG , 0x00000001);
    etna_set_state(stream, VIVS_RA_EARLY_DEPTH, 0x00000031);
    etna_set_state(stream, VIVS_PA_W_CLIP_LIMIT, 0x34000001);

    ctx->dirty = ~0UL;
}

struct pipe_context *etna_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
    struct etna_context *ctx = CALLOC_STRUCT(etna_context);
    struct etna_screen *screen;
    struct pipe_context *pctx = NULL;

    if (ctx == NULL)
        return NULL;

    screen = etna_screen(pscreen);
    ctx->stream = etna_cmd_stream_new(screen->pipe, &etna_cmd_stream_reset_notify, ctx);
    if (ctx->stream == NULL)
        goto fail;

    pctx = &ctx->base;
    pctx->priv = ctx;
    pctx->screen = pscreen;

    /* context ctxate setup */
    ctx->specs = screen->specs;
    ctx->screen = screen;

    /*  Set sensible defaults for state */
    etna_cmd_stream_reset_notify(ctx->stream, ctx);

    pctx->destroy = etna_context_destroy;
    pctx->draw_vbo = etna_draw_vbo;
    pctx->flush = etna_flush;

    /* creation of compile states */
    pctx->create_blend_state = etna_blend_state_create;
    pctx->create_rasterizer_state = etna_rasterizer_state_create;
    pctx->create_depth_stencil_alpha_state = etna_zsa_state_create;

    etna_clear_blit_init(pctx);
    etna_state_init(pctx);
    etna_surface_init(pctx);
    etna_shader_init(pctx);
    etna_texture_init(pctx);
    etna_transfer_init(pctx);

    ctx->blitter = util_blitter_create(pctx);
    if (!ctx->blitter)
        goto fail;

    /* Generate the bitmask of supported draw primitives. GPUs
     * without the RECT_PRIMITIVE do not support PIPE_PRIM_QUADS.
     */
    ctx->prim_hwsupport = 1 << PIPE_PRIM_POINTS |
                          1 << PIPE_PRIM_LINES |
                          1 << PIPE_PRIM_LINE_LOOP |
                          1 << PIPE_PRIM_LINE_STRIP |
                          1 << PIPE_PRIM_TRIANGLES |
                          1 << PIPE_PRIM_TRIANGLE_STRIP |
                          1 << PIPE_PRIM_TRIANGLE_FAN;

    if (VIV_FEATURE(ctx->screen, chipMinorFeatures2, RECT_PRIMITIVE))
        ctx->prim_hwsupport |= 1 << PIPE_PRIM_QUADS;

    ctx->primconvert = util_primconvert_create(pctx, ctx->prim_hwsupport);
    if (!ctx->primconvert)
        goto fail;

    util_slab_create(&ctx->transfer_pool, sizeof(struct etna_transfer),
                     16, UTIL_SLAB_SINGLETHREADED);

    return pctx;

fail:
    pctx->destroy(pctx);

    return NULL;
}