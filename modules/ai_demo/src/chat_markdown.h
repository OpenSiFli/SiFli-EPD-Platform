#ifndef CHAT_MARKDOWN_H
#define CHAT_MARKDOWN_H

#include "platform/epd_app.h"

typedef struct chat_markdown chat_markdown_t;
void chat_markdown_set_link_handler(void (*handler)(const char *url));

/* UI-thread view. Deleting the root or any ancestor releases the view. */
chat_markdown_t *chat_markdown_create(lv_obj_t *parent);
lv_obj_t *chat_markdown_root(chat_markdown_t *view);
/* Original text is never rewritten. An unsuccessful update retains the old view. */
bool chat_markdown_update(chat_markdown_t *view, const char *text, size_t length, int width);
const char *chat_markdown_source(const chat_markdown_t *view);
size_t chat_markdown_block_count(const chat_markdown_t *view);
lv_obj_t *chat_markdown_block(const chat_markdown_t *view, size_t index);

#endif
