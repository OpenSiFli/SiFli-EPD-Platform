#include "board_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void *allocate(size_t size, void *context)
{
    (void)context;
    return malloc(size);
}
static void release(void *p, void *context)
{
    (void)context;
    free(p);
}

static void quoted(const char *s)
{
    if (!s) { fputs("null", stdout); return; }
    putchar('"');
    for (; *s; ++s)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c < 32) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}
static bool emit(const board_command_t *c, void *context)
{
    (void)context;
    static const char *const kinds[] = {"back", "title", "panel", "heading", "body"};
    printf("{\"kind\":\"%s\",\"card\":%zu,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"font_size\":%u,\"text\":",
           kinds[c->kind], c->card, c->x, c->y, c->width, c->height, c->font_size);
    quoted(c->text);
    fputs(",\"summary\":", stdout); quoted(c->summary);
    fputs(",\"empty\":", stdout); quoted(c->empty);
    puts("}");
    return !ferror(stdout);
}

int main(int argc, char **argv)
{
    if (argc != 4) { fputs("usage: preview layout.json width height\n", stderr); return 2; }
    char *end;
    errno = 0;
    long width = strtol(argv[2], &end, 10);
    if (errno || *end || width <= 0 || (unsigned long)width > BOARD_COORD_MAX) return 2;
    long height = strtol(argv[3], &end, 10);
    if (errno || *end || height <= 0 || (unsigned long)height > BOARD_COORD_MAX) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) { perror("layout"); return 2; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 2; }
    long length = ftell(file);
    if (length < 0 || (uint64_t)length >= (uint64_t)PTRDIFF_MAX || fseek(file, 0, SEEK_SET))
    { fclose(file); return 2; }
    char *data = malloc((size_t)length + 1);
    if (!data) { fclose(file); return 2; }
    size_t got = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) { free(data); return 2; }
    const board_allocator_t allocator = {allocate, release, NULL};
    board_model_t *model = NULL;
    board_result_t result = board_model_parse(data, got, &allocator, &model);
    free(data);
    if (result == BOARD_OK) result = board_layout(model, (int32_t)width, (int32_t)height, emit, NULL);
    board_model_free(model);
    fprintf(stderr, "%s\n", board_result_text(result));
    return result == BOARD_OK ? 0 : 1;
}
