/* SPDX-License-Identifier: Apache-2.0 */
#ifndef COMMON_READER_I18N_H
#define COMMON_READER_I18N_H
#include "reader_error.h"
#include "platform/epd_i18n.h"

/* UI thread only. Error identity is retained independently of this text. */
static inline const char *reader_error_text(reader_error_t error)
{
    switch (error)
    {
    case READER_ERROR_INPUT: return epd_get_str(reader_error_input);
    case READER_ERROR_APP_ID: return epd_get_str(reader_error_app_id);
    case READER_ERROR_TYPE: return epd_get_str(reader_error_type);
    case READER_ERROR_STOPPING: return epd_get_str(reader_error_stopping);
    case READER_ERROR_SESSION_MEMORY: return epd_get_str(reader_error_session_memory);
    case READER_ERROR_CODEC_LOAD: return epd_get_str(reader_error_codec_load);
    case READER_ERROR_CODEC_ABI: return epd_get_str(reader_error_codec_abi);
    case READER_ERROR_CANCELLED: return epd_get_str(reader_error_cancelled);
    case READER_ERROR_CONVERT_MEMORY: return epd_get_str(reader_error_convert_memory);
    case READER_ERROR_NO_SPACE: return epd_get_str(reader_error_no_space);
    case READER_ERROR_UNSUPPORTED: return epd_get_str(reader_error_unsupported);
    case READER_ERROR_IO: return epd_get_str(reader_error_io);
    case READER_ERROR_PARSE: return epd_get_str(reader_error_parse);
    case READER_ERROR_CACHE_WRITE: return epd_get_str(reader_error_cache_write);
    case READER_ERROR_CACHE_RANGE: return epd_get_str(reader_error_cache_range);
    case READER_ERROR_TOC_RANGE: return epd_get_str(reader_error_toc_range);
    case READER_ERROR_TOC_MEMORY: return epd_get_str(reader_error_toc_memory);
    case READER_ERROR_LAYOUT_MEMORY: return epd_get_str(reader_error_layout_memory);
    case READER_ERROR_IMAGE_MEMORY: return epd_get_str(reader_error_image_memory);
    case READER_ERROR_IMAGE_READ: return epd_get_str(reader_error_image_read);
    case READER_ERROR_PAGE_MEMORY: return epd_get_str(reader_error_page_memory);
    case READER_ERROR_LINK_MISSING: return epd_get_str(reader_error_link_missing);
    case READER_ERROR_LINK_READ: return epd_get_str(reader_error_link_read);
    case READER_ERROR_PAGE_READ: return epd_get_str(reader_error_page_read);
    case READER_ERROR_LAYOUT: return epd_get_str(reader_error_layout);
    case READER_ERROR_WIDGET_MEMORY: return epd_get_str(reader_error_widget_memory);
    case READER_ERROR_UNAVAILABLE: return epd_get_str(reader_error_unavailable);
    case READER_ERROR_CURSOR_MEMORY: return epd_get_str(reader_error_cursor_memory);
    case READER_ERROR_FONT_MEMORY: return epd_get_str(reader_error_font_memory);
    case READER_ERROR_ENCODING: return epd_get_str(reader_error_encoding);
    case READER_ERROR_TEXT_READ: return epd_get_str(reader_error_text_read);
    case READER_ERROR_BOOK_CLOSED: return epd_get_str(reader_error_book_closed);
    default: return "";
    }
}
#endif

