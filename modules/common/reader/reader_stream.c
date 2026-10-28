/* SPDX-License-Identifier: Apache-2.0 */
/* Keep the existing file/ZIP stream implementation. Resource dispatch here
 * also accepts absolute URLs; fetching is owned by resources.open. */
#define bf_picture epd_reader_local_picture
#include "../../books/formats/book_stream.c"
#undef bf_picture
#include "reader_resource.h"

static int resource_result(const book_sink_t *sink, int result)
{
    if (result != READER_RESOURCE_MISSING) return result;
    result = bf_cancel(sink);
    if (result != BOOK_OK) return result;
    return sink->image ? sink->image(sink->user, NULL, NULL) : BOOK_OK;
}

int bf_picture(const book_sink_t *sink, const book_resources_t *resources,
               const char *base, const char *reference)
{
    if (!reference || !*reference) return BOOK_OK;
    if (!bf_external(reference)) return resource_result(sink, epd_reader_local_picture(sink, resources, base, reference));
    int svg = bf_suffix(reference, ".svg");
    if ((!svg && !sink->image) || !resources || !resources->open) return BOOK_OK;
    if (svg && !sink->svg) return BOOK_UNSUPPORTED;
    int result = bf_cancel(sink);
    if (result != BOOK_OK) return result;
    book_stream_t stream = {0};
    result = resources->open(resources->user, base, reference, &stream);
    if (result == BOOK_OK && !stream.read) result = BOOK_IO_ERROR;
    if (result == BOOK_OK) result = svg ? bf_svg_stream(&stream, sink, reference, resources) :
        sink->image(sink->user, reference, &stream);
    return resource_result(sink, bf_finish(&stream, result));
}
