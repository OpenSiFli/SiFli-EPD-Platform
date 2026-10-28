#include "book_svg.h"
#include "epd_svg.h"

typedef struct {
    const char *base;
    const book_resources_t *resources;
    bool (*cancelled)(void *);
    void *user;
} svg_input_t;

static bool stopped(void *user)
{
    svg_input_t *input = user;
    return input->cancelled && input->cancelled(input->user);
}

static int load_image(void *user, const char *reference, unsigned max_width,
                      unsigned max_height, epd_svg_bitmap_t *image)
{
    svg_input_t *input = user;
    if (stopped(user)) return BOOK_CANCELLED;
    if (!reference || !*reference || !input->resources || !input->resources->open)
        return BOOK_UNSUPPORTED;
    book_stream_t stream = {0};
    int result = input->resources->open(input->resources->user, input->base, reference, &stream);
    if (result == BOOK_OK)
        result = book_image_decode(&stream, max_width, max_height, stopped, input,
                                   &image->pixels, &image->width, &image->height, NULL, 0);
    if (stream.close) stream.close(stream.user);
    return result;
}

int book_svg_decode(const char *xml, size_t length, const char *base,
                    const book_resources_t *resources, unsigned max_width, unsigned max_height,
                    bool (*cancelled)(void *), void *user,
                    uint8_t **gray, unsigned *width, unsigned *height)
{
    if (!gray || !width || !height) return BOOK_ERROR;
    *gray = NULL;
    *width = *height = 0;
    svg_input_t input = {base, resources, cancelled, user};
    epd_svg_bitmap_t image = {0};
    int result = epd_svg_render(xml, length, max_width, max_height,
                                load_image, &input, stopped, &image);
    if (result == BOOK_OK)
    {
        *gray = image.pixels;
        *width = image.width;
        *height = image.height;
    }
    return result;
}
