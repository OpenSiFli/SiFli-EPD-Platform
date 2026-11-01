/* SPDX-License-Identifier: Apache-2.0 */
#include "image_internal.h"
typedef ri_bitmap epd_svg_bitmap_t;
typedef int (*epd_svg_load_image_t)(void *, const char *, unsigned, unsigned, ri_bitmap *);



#include <ctype.h>
#include <math.h>
#include <string.h>
#include "../../SiFli-SDK/external/lvgl_v9/lvgl.h"

#if LV_USE_SVG && LV_USE_VECTOR_GRAPHIC && LV_USE_THORVG && LV_USE_DRAW_SW
#include "../../SiFli-SDK/external/lvgl_v9/src/libs/svg/lv_svg.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/libs/svg/lv_svg_token.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/libs/svg/lv_svg_render.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/draw/lv_draw_private.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/draw/sw/lv_draw_sw.h"
#include "../../SiFli-SDK/external/lvgl_v9/src/misc/cache/instance/lv_image_cache.h"

#define SVG_INPUT_LIMIT (256U * 1024U)
#define SVG_WIDTH_LIMIT 684U
#define SVG_HEIGHT_LIMIT 1216U
#define SVG_IMAGE_WIDTH 384U
#define SVG_IMAGE_HEIGHT 608U
/* ThorVG copies every pattern occurrence into the LVGL heap. */
#define SVG_IMAGE_BYTES (SVG_IMAGE_WIDTH * SVG_IMAGE_HEIGHT * 4U)
#define SVG_NODE_LIMIT 256U
#define SVG_DEPTH_LIMIT 32U
#define SVG_GEOMETRY_LIMIT (32U * 1024U)

enum { SVG_OK, SVG_INVALID = -1, SVG_CANCEL = -2, SVG_NOMEM = -3,
       SVG_UNSUPPORTED = -4, SVG_IO = -5 };

typedef struct svg_image {
    struct svg_image *next;
    lv_image_dsc_t image;
} svg_image_t;

typedef struct {
    epd_svg_load_image_t load;
    bool (*cancel)(void *);
    void *user;
    svg_image_t *images;
    size_t image_bytes;
    int status;
    const char *source;
    const char *names[SVG_DEPTH_LIMIT];
    size_t name_lengths[SVG_DEPTH_LIMIT];
    size_t href_offsets[SVG_NODE_LIMIT];
    unsigned href_count, depth, nodes;
    size_t geometry_bytes;
    unsigned image_max_width, image_max_height;
    size_t root_start, root_end;
    bool width_percent, height_percent, root_seen, root_flat;
} svg_context_t;

static bool svg_hal_ready;
static svg_context_t *svg_current;
static bool scalable_started;
static void svg_draw_vector(lv_draw_task_t *task, lv_draw_vector_dsc_t *vector)
{ lv_draw_sw_vector(task, vector); }

static bool svg_cancelled(svg_context_t *ctx)
{
    if (ctx->cancel && ctx->cancel(ctx->user)) ctx->status = SVG_CANCEL;
    return ctx->status == SVG_CANCEL;
}

static bool svg_equal(const char *begin, const char *end, const char *text)
{
    size_t size = strlen(text);
    return begin && end && (size_t)(end - begin) == size &&
           memcmp(begin, text, size) == 0;
}

/* Use the SDK tokenizer for bounds, units and SVG2 href adaptation. */
static bool svg_token(_lv_svg_token_t *token, void *data)
{
    static const char *const tags[] = {
        "svg", "g", "path", "rect", "circle", "ellipse", "line", "polyline",
        "polygon", "defs", "linearGradient", "radialGradient", "stop",
        "solidColor", "image", "title", "desc", "metadata"
    };
    svg_context_t *ctx = data;
    if (svg_cancelled(ctx)) return false;
    if (token->type == LV_SVG_TOKEN_CONTENT)
    {
        if (!ctx->depth)
            for (const char *p = token->start; p < token->end; p++)
                if (!isspace((unsigned char)*p)) goto invalid;
        return true;
    }
    if (token->type == LV_SVG_TOKEN_END)
    {
        if (!ctx->depth || (size_t)(token->end - token->start) !=
            ctx->name_lengths[ctx->depth - 1] ||
            memcmp(token->start, ctx->names[ctx->depth - 1],
                   ctx->name_lengths[ctx->depth - 1]))
            goto invalid;
        ctx->depth--;
        if (!ctx->depth)
        {
            const char *end = token->end;
            while (*end && *end != '>') end++;
            if (*end != '>') goto invalid;
            ctx->root_end = (size_t)(end + 1 - ctx->source);
        }
        return true;
    }
    bool root = !ctx->root_seen;
    bool is_svg = svg_equal(token->start, token->end, "svg");
    bool image = svg_equal(token->start, token->end, "image");
    if (root)
    {
        if (!is_svg) goto invalid;
        ctx->root_seen = true;
        ctx->root_start = (size_t)(token->start - ctx->source) - 1;
        ctx->root_flat = token->flat;
    }
    else if (!ctx->depth) goto invalid;
    else if (is_svg) goto unsupported;
    if (++ctx->nodes > SVG_NODE_LIMIT || ctx->depth == SVG_DEPTH_LIMIT)
        goto unsupported;
    bool supported = false;
    for (unsigned i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
        if (svg_equal(token->start, token->end, tags[i])) supported = true;
    if (!supported) goto unsupported;
    if (!token->flat)
    {
        ctx->names[ctx->depth] = token->start;
        ctx->name_lengths[ctx->depth++] = (size_t)(token->end - token->start);
    }
    bool href = false;
    for (uint32_t i = 0; i < lv_array_size(&token->attrs); i++)
    {
        const _lv_svg_token_attr_t *a = lv_array_at(&token->attrs, i);
        if (!a->name_start || !a->name_end || !a->value_start || !a->value_end)
            goto invalid;
        const char *end = a->value_end;
        while (end > a->value_start && isspace((unsigned char)end[-1])) end--;
        bool percent = end > a->value_start && end[-1] == '%';
        bool width = svg_equal(a->name_start, a->name_end, "width");
        bool height = svg_equal(a->name_start, a->name_end, "height");
        if (root && width) ctx->width_percent = percent;
        if (root && height) ctx->height_percent = percent;
        if (!root && percent && (width || height ||
            svg_equal(a->name_start, a->name_end, "x") ||
            svg_equal(a->name_start, a->name_end, "y"))) goto unsupported;
        if (svg_equal(a->name_start, a->name_end, "style") ||
            svg_equal(a->name_start, a->name_end, "clip-path") ||
            svg_equal(a->name_start, a->name_end, "mask") ||
            svg_equal(a->name_start, a->name_end, "filter")) goto unsupported;
        if (svg_equal(a->name_start, a->name_end, "d") ||
            svg_equal(a->name_start, a->name_end, "points"))
        {
            ctx->geometry_bytes += (size_t)(end - a->value_start);
            if (ctx->geometry_bytes > SVG_GEOMETRY_LIMIT) goto unsupported;
        }
        bool plain_href = svg_equal(a->name_start, a->name_end, "href");
        if (plain_href || svg_equal(a->name_start, a->name_end, "xlink:href"))
        {
            if (!image) goto unsupported;
            if (href || end == a->value_start) goto invalid;
            href = true;
            if (plain_href)
                ctx->href_offsets[ctx->href_count++] =
                    (size_t)(a->name_start - ctx->source);
        }
    }
    if (image && !href) goto invalid;
    if (root && token->flat)
    {
        const char *end = token->end;
        if (lv_array_size(&token->attrs))
        {
            const _lv_svg_token_attr_t *last =
                lv_array_at(&token->attrs, lv_array_size(&token->attrs) - 1);
            end = last->value_end + 1;
        }
        while (*end && *end != '>') end++;
        if (*end != '>' || end[-1] != '/') goto invalid;
        ctx->root_end = (size_t)(end + 1 - ctx->source);
    }
    return true;
invalid:
    ctx->status = SVG_INVALID;
    return false;
unsupported:
    ctx->status = SVG_UNSUPPORTED;
    return false;
}

static lv_svg_attr_t *svg_attr(lv_svg_node_t *node, lv_svg_attr_type_t id)
{
    for (uint32_t i = 0; i < lv_array_size(&node->attrs); i++)
    {
        lv_svg_attr_t *attr = lv_array_at(&node->attrs, i);
        if (attr->id == id) return attr;
    }
    return NULL;
}

static int svg_set_dimension(lv_svg_node_t *node, lv_svg_attr_type_t id, float value)
{
    lv_svg_attr_t *attr = svg_attr(node, id);
    if (!attr)
    {
        if (node->attrs.size == node->attrs.capacity &&
            !lv_array_resize(&node->attrs, node->attrs.capacity + 2))
            return SVG_NOMEM;
        node->attrs.size++;
        attr = lv_array_at(&node->attrs, node->attrs.size - 1);
    }
    memset(attr, 0, sizeof(*attr));
    attr->id = id;
    attr->class_type = LV_SVG_ATTR_VALUE_INITIAL;
    attr->val_type = LV_SVG_ATTR_VALUE_DATA;
    attr->value.fval = value;
    return SVG_OK;
}

static int svg_viewport(svg_context_t *ctx, lv_svg_node_t *doc,
                        unsigned max_width, unsigned max_height,
                        unsigned *width, unsigned *height, lv_matrix_t *matrix)
{
    lv_svg_attr_t *wa = svg_attr(doc, LV_SVG_ATTR_WIDTH);
    lv_svg_attr_t *ha = svg_attr(doc, LV_SVG_ATTR_HEIGHT);
    lv_svg_attr_t *box = svg_attr(doc, LV_SVG_ATTR_VIEWBOX);
    float w = wa ? wa->value.fval : 0.0f;
    float h = ha ? ha->value.fval : 0.0f;
    float bounds[4] = {0};
    if (box)
    {
        if (box->val_type != LV_SVG_ATTR_VALUE_PTR || !box->value.val)
            return SVG_INVALID;
        memcpy(bounds, box->value.val, sizeof(bounds));
        for (unsigned i = 0; i < 4; i++)
            if (!isfinite(bounds[i]) || fabsf(bounds[i]) > 1000000.0f)
                return SVG_INVALID;
        if (bounds[2] <= 0 || bounds[3] <= 0) return SVG_INVALID;
    }
    if ((!wa || ctx->width_percent) && box) w = bounds[2];
    if ((!ha || ctx->height_percent) && box) h = bounds[3];
    if ((ctx->width_percent || ctx->height_percent) && !box) return SVG_UNSUPPORTED;
    if (!isfinite(w) || !isfinite(h) || w <= 0 || h <= 0 ||
        w > 1000000.0f || h > 1000000.0f) return SVG_INVALID;

    float scale = fminf(1.0f, fminf(max_width / w, max_height / h));
    *width = (unsigned)fmaxf(1.0f, floorf(w * scale + 0.0001f));
    *height = (unsigned)fmaxf(1.0f, floorf(h * scale + 0.0001f));
    if (*width > max_width) *width = max_width;
    if (*height > max_height) *height = max_height;
    scale = fminf(scale, fminf(*width / w, *height / h));
    lv_matrix_identity(matrix);
    lv_matrix_scale(matrix, scale, scale);
    if (box)
    {
        float sx = w / bounds[2], sy = h / bounds[3];
        lv_svg_attr_t *aspect = svg_attr(doc, LV_SVG_ATTR_PRESERVE_ASPECT_RATIO);
        uint32_t ratio = aspect ? aspect->value.uval : LV_SVG_ASPECT_RATIO_XMID_YMID;
        uint32_t align = (ratio & ~1U) >> 1;
        if (align > 9) return SVG_INVALID;
        if (align)
        {
            float s = (ratio & 1) ? fmaxf(sx, sy) : fminf(sx, sy);
            lv_matrix_translate(matrix, (w - bounds[2] * s) * ((align - 1) % 3) * 0.5f,
                                (h - bounds[3] * s) * ((align - 1) / 3) * 0.5f);
            sx = sy = s;
        }
        lv_matrix_scale(matrix, sx, sy);
        lv_matrix_translate(matrix, -bounds[0], -bounds[1]);
        /* Retain the allocation for DOM deletion, bypass order-dependent SDK
         * viewport processing and install the completed matrix after create. */
        box->id = LV_SVG_ATTR_INVALID;
    }
    int result = svg_set_dimension(doc, LV_SVG_ATTR_WIDTH, w);
    if (!result) result = svg_set_dimension(doc, LV_SVG_ATTR_HEIGHT, h);
    return result;
}

static void svg_load_image(const char *url, lv_draw_image_dsc_t *dsc)
{
    svg_context_t *ctx = svg_current;
    if (!ctx || ctx->status || svg_cancelled(ctx)) return;
    if (!ctx->load)
    {
        ctx->status = SVG_UNSUPPORTED;
        return;
    }
    epd_svg_bitmap_t gray = {0};
    unsigned width = ctx->image_max_width, height = ctx->image_max_height;
    const ri_io *io = ctx->user;
    if (io->available) {
        size_t remaining = io->available(io->user);
        /* Leave space for the canvas and the pattern copy made by ThorVG.
         * Decoder row/Adam7 sums and gray-to-ARGB conversion also coexist. */
        size_t capacity = remaining > 131072u ? (remaining-131072u)/12u : 0;
        if (!capacity) { ctx->status=SVG_NOMEM; return; }
        if ((uint64_t)width*height > capacity) {
            float scale = sqrtf((float)capacity/((float)width*height));
            width = (unsigned)fmaxf(1.0f, floorf(width*scale));
            height = (unsigned)fmaxf(1.0f, floorf(height*scale));
        }
    }
    scalable_started = true;
    int result = ctx->load(ctx->user, url, width, height, &gray);
    if (result)
        ctx->status = result <= SVG_INVALID && result >= SVG_IO ? result : SVG_IO;
    if (svg_cancelled(ctx) || ctx->status) goto done;
    if (!gray.pixels || !gray.width || !gray.height ||
        gray.width > SVG_IMAGE_WIDTH || gray.height > SVG_IMAGE_HEIGHT)
    {
        ctx->status = SVG_INVALID;
        goto done;
    }
    size_t bytes = (size_t)gray.width * gray.height * 4;
    if (bytes > SVG_IMAGE_BYTES - ctx->image_bytes)
    {
        ctx->status = SVG_NOMEM;
        goto done;
    }
    svg_image_t *image = ri_work_alloc(sizeof(*image) + bytes + LV_DRAW_BUF_ALIGN - 1);
    if (!image)
    {
        ctx->status = SVG_NOMEM;
        goto done;
    }
    memset(image, 0, sizeof(*image));
    image->next = ctx->images;
    ctx->images = image;
    ctx->image_bytes += bytes;
    uint32_t *argb = lv_draw_buf_align(image + 1, LV_COLOR_FORMAT_ARGB8888);
    for (unsigned y = 0; y < gray.height; y++)
    {
        if (svg_cancelled(ctx)) goto done;
        for (unsigned x = 0; x < gray.width; x++)
        {
            size_t i = (size_t)y * gray.width + x;
            argb[i] = 0xff000000U | (uint32_t)gray.pixels[i] * 0x010101U;
        }
    }
    image->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image->image.header.cf = LV_COLOR_FORMAT_ARGB8888;
    image->image.header.w = gray.width;
    image->image.header.h = gray.height;
    image->image.header.stride = gray.width * 4;
    /* Opaque gray is already premultiplied. The bin decoder uses this memory
     * directly, so it neither duplicates nor caches the temporary descriptor. */
    image->image.header.flags = LV_IMAGE_FLAGS_PREMULTIPLIED;
    image->image.data_size = bytes;
    image->image.data = (const uint8_t *)argb;
    lv_image_cache_drop(&image->image);
    dsc->src = &image->image;
    dsc->header = image->image.header;
done:
    ri_work_free(gray.pixels);
}

static void svg_free_images(svg_context_t *ctx)
{
    while (ctx->images)
    {
        svg_image_t *image = ctx->images;
        ctx->images = image->next;
        /* Cache drop acquires the SDK's own cache mutex; do not nest that lock. */
        if (image->image.data) lv_image_cache_drop(&image->image);
        ri_work_free(image);
    }
}

static int render_svg(const char *svg, size_t length,
                   unsigned max_width, unsigned max_height,
                   epd_svg_load_image_t load, void *user,
                   bool (*cancel)(void *), epd_svg_bitmap_t *out)
{
    if (!out) return SVG_INVALID;
    memset(out, 0, sizeof(*out));
    if (!svg || !length || length > SVG_INPUT_LIMIT || !max_width || !max_height ||
        memchr(svg, 0, length)) return SVG_INVALID;
    if (length >= 3 && memcmp(svg, "\xef\xbb\xbf", 3) == 0)
    {
        svg += 3;
        length -= 3;
        if (!length) return SVG_INVALID;
    }
    if (cancel && cancel(user)) return SVG_CANCEL;
    if (max_width > SVG_WIDTH_LIMIT) max_width = SVG_WIDTH_LIMIT;
    if (max_height > SVG_HEIGHT_LIMIT) max_height = SVG_HEIGHT_LIMIT;
    int result;
    /* The renderer owns a single private SVG HAL context. */
    if (svg_current)
    {
        return SVG_UNSUPPORTED;
    }
    svg_context_t *ctx = ri_work_alloc(sizeof(*ctx));
    if (!ctx)
    {
        return SVG_NOMEM;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->load = load;
    ctx->cancel = cancel;
    ctx->user = user;
    ctx->image_max_width = max_width < SVG_IMAGE_WIDTH ? max_width : SVG_IMAGE_WIDTH;
    ctx->image_max_height = max_height < SVG_IMAGE_HEIGHT ? max_height : SVG_IMAGE_HEIGHT;
    svg_current = ctx;
    if (!svg_hal_ready)
    {
        const lv_svg_render_hal_t hal = {svg_load_image, NULL};
        lv_svg_render_init(&hal);
        svg_hal_ready = true;
    }

    char *source = NULL, *normalized = NULL;
    uint8_t *argb = NULL, *argb_allocation = NULL, *gray = NULL;
    lv_svg_node_t *doc = NULL;
    lv_svg_render_obj_t *render = NULL;
    lv_draw_vector_dsc_t *vector = NULL;
    unsigned width = 0, height = 0;
    lv_matrix_t matrix;
    lv_draw_buf_t buffer;
    lv_layer_t layer = {0};
    lv_draw_task_t task = {0};

    /* The SDK tokenizer reads a sentinel after some XML constructs. */
    source = ri_work_alloc(length + 16);
    if (!source) { ctx->status = SVG_NOMEM; goto done; }
    memcpy(source, svg, length);
    memset(source + length, 0, 16);
    ctx->source = source;
    /* Bound tokenizer allocations before it collects a whole tag's attributes. */
    unsigned delimiters = 0;
    for (size_t i = 0; i < length; i++)
        if ((source[i] == '=' || source[i] == '<') && ++delimiters > 2048)
        { ctx->status = SVG_UNSUPPORTED; goto done; }
    if (!_lv_svg_tokenizer(source, length, svg_token, ctx) ||
        !ctx->root_seen || !ctx->root_end || ctx->depth)
    {
        if (!ctx->status) ctx->status = SVG_INVALID;
        goto done;
    }
    if (ctx->href_count || ctx->root_flat)
    {
        size_t total = ctx->root_end + 6 * ctx->href_count + (ctx->root_flat ? 6 : 0);
        normalized = ri_work_alloc(total + 16);
        if (!normalized) { ctx->status = SVG_NOMEM; goto done; }
        size_t input = 0, output = 0;
        for (unsigned i = 0; i < ctx->href_count; i++)
        {
            size_t count = ctx->href_offsets[i] - input;
            memcpy(normalized + output, source + input, count);
            output += count;
            memcpy(normalized + output, "xlink:", 6);
            output += 6;
            input += count;
        }
        memcpy(normalized + output, source + input, ctx->root_end - input);
        if (ctx->root_flat)
        {
            normalized[total - 8] = ' ';
            memcpy(normalized + total - 6, "</svg>", 6);
        }
        memset(normalized + total, 0, 16);
        length = total;
    }
    else length = ctx->root_end;
    if (svg_cancelled(ctx)) goto done;
    doc = lv_svg_load_data((normalized ? normalized : source) + ctx->root_start,
                           length - ctx->root_start);
    if (!doc || doc->type != LV_SVG_TAG_SVG)
    { ctx->status = SVG_INVALID; goto done; }
    ctx->status = svg_viewport(ctx, doc, max_width, max_height, &width, &height, &matrix);
    if (ctx->status || svg_cancelled(ctx)) goto done;
    render = lv_svg_render_create(doc);
    if (!render && !ctx->status) ctx->status = SVG_NOMEM;
    if (ctx->status || svg_cancelled(ctx)) goto done;
    /* _prepare_render copies paint state, not ctx->matrix. Root viewport
     * rendering multiplies this matrix exactly once, before child transforms. */
    render->matrix = matrix;
    lv_svg_node_delete(doc);
    doc = NULL;
    ri_work_free(normalized);
    normalized = NULL;
    ri_work_free(source);
    source = NULL;

    /* The parsed representation and decoded image resources are already
     * charged. Reserve duplicated patterns and vector/raster working space. */
    const ri_io *io = user;
    if (io->available) {
        size_t remaining = io->available(io->user);
        size_t reserve = 65536u + ctx->image_bytes + ctx->geometry_bytes * 8u;
        size_t capacity = remaining > reserve ? (remaining - reserve) / 5u : 0;
        if (!capacity) { ctx->status = SVG_NOMEM; goto done; }
        if ((uint64_t)width * height > capacity) {
            float scale = sqrtf((float)capacity / ((float)width * height));
            unsigned nw = (unsigned)fmaxf(1.0f, floorf(width * scale));
            unsigned nh = (unsigned)fmaxf(1.0f, floorf(height * scale));
            scale = fminf((float)nw / width, (float)nh / height);
            lv_matrix_t shrink;
            lv_matrix_identity(&shrink); lv_matrix_scale(&shrink, scale, scale);
            lv_matrix_multiply(&shrink, &render->matrix); render->matrix = shrink;
            width = nw; height = nh;
        }
    }
    scalable_started = true;
    size_t pixels = (size_t)width * height;
    argb_allocation = ri_work_alloc(pixels * 4 + LV_DRAW_BUF_ALIGN - 1);
    if (!argb_allocation) { ctx->status = SVG_NOMEM; goto done; }
    argb = lv_draw_buf_align(argb_allocation, LV_COLOR_FORMAT_ARGB8888);
    memset(argb, 0xff, pixels * 4);
    if (lv_draw_buf_init(&buffer, width, height, LV_COLOR_FORMAT_ARGB8888,
                         width * 4, argb, pixels * 4) != LV_RESULT_OK)
    { ctx->status = SVG_INVALID; goto done; }
    layer.draw_buf = &buffer;
    layer.buf_area = (lv_area_t){0, 0, (int32_t)width - 1, (int32_t)height - 1};
    layer._clip_area = layer.buf_area;
    layer.color_format = LV_COLOR_FORMAT_ARGB8888;
    task.target_layer = &layer;
    task.clip_area = layer.buf_area;
    task.opa = LV_OPA_COVER;
    vector = lv_draw_vector_dsc_create(&layer);
    if (!vector) { ctx->status = SVG_NOMEM; goto done; }
    lv_draw_svg_render(vector, render);
    if (svg_cancelled(ctx)) goto done;
    size_t copied_bytes = 0;
    if (vector->task_list)
    {
        lv_draw_vector_subtask_t *sub = lv_ll_get_head(vector->task_list);
        while (sub)
        {
            if (sub->ctx.fill_dsc.style == LV_VECTOR_DRAW_STYLE_PATTERN)
            {
                const lv_image_dsc_t *img = sub->ctx.fill_dsc.img_dsc.src;
                if (!img || img->data_size > SVG_IMAGE_BYTES - copied_bytes)
                { ctx->status = SVG_NOMEM; goto done; }
                copied_bytes += img->data_size;
            }
            sub = lv_ll_get_next(vector->task_list, sub);
        }
    }
    /* This consumes the private vector tasks and synchronizes ThorVG. */
    svg_draw_vector(&task, vector);
    lv_draw_vector_dsc_delete(vector);
    vector = NULL;
    lv_svg_render_delete(render);
    render = NULL;
    svg_free_images(ctx);
    if (svg_cancelled(ctx)) goto done;
    gray = ri_output_alloc(pixels);
    if (!gray) { ctx->status = SVG_NOMEM; goto done; }
    for (unsigned y = 0; y < height; y++)
    {
        if (svg_cancelled(ctx)) goto done;
        const lv_color32_t *row = (const lv_color32_t *)argb + (size_t)y * width;
        for (unsigned x = 0; x < width; x++)
            gray[(size_t)y * width + x] = (uint8_t)((77U * row[x].red +
                150U * row[x].green + 29U * row[x].blue + 128U) >> 8);
    }
    out->pixels = gray;
    out->width = width;
    out->height = height;
    gray = NULL;
done:
    if (vector) lv_draw_vector_dsc_delete(vector);
    if (render) lv_svg_render_delete(render);
    if (doc) lv_svg_node_delete(doc);
    svg_free_images(ctx);
    ri_work_free(gray);
    ri_work_free(argb_allocation);
    ri_work_free(normalized);
    ri_work_free(source);
    result = ctx->status;
    svg_current = NULL;
    ri_work_free(ctx);
    return result;
}

static bool cancelled(void *p) { const ri_io *io = p; return io->cancel && io->cancel(io->user); }
static int load_image(void *p, const char *key, unsigned w, unsigned h, ri_bitmap *out)
{ return ri_decode_active(p, key, w, h, out); }
bool ri_svg_may_shrink(void) { return scalable_started; }
void ri_svg_adapter_reset(void) { svg_current = NULL; svg_hal_ready = false; scalable_started = false; }
int ri_svg_render_active(const ri_io *io, const char *resource, unsigned w, unsigned h, ri_bitmap *out)
{
    ri_raster_decoder_t d = {0}; d.io = io; d.resource = resource;
    int status = RI_IO;
    char *source = NULL;
    if (cancelled((void *)io)) return RI_CANCELLED;
    if (io->read(io->user, resource, 0, NULL, 0, &d.file_size)) return RI_IO;
    if (!d.file_size || d.file_size > SVG_INPUT_LIMIT) return RI_INVALID;
    source = ri_work_alloc(d.file_size);
    if (!ri_raster_read(&d, source, d.file_size)) { status = d.status; goto done; }
    status = render_svg(source, d.file_size, w, h, load_image, (void *)io, cancelled, out);
done:
    ri_work_free(source);
    return status;
}

#endif
