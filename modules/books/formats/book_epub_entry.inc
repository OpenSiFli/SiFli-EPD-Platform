/* SPDX-License-Identifier: Apache-2.0 */
static epub_item *entry_navigation(epub *e)
{
    for (size_t i = 0; i < e->count; ++i)
        if (item_at(e, i)->nav) return item_at(e, i);
    for (size_t i = 0; i < e->count; ++i)
        if (item_at(e, i)->ncx && (!*e->toc || !strcmp(e->toc, item_at(e, i)->id))) return item_at(e, i);
    return NULL;
}
/* The central directory is already resident. These checks perform no source
 * reads, and include package/navigation CRCs so changed mappings fall back. */
static bool entry_file(epub *e, const char *path, book_entry_file_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!*path) return true;
    int index = mz_zip_reader_locate_file(&e->base.zip, path, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
    mz_zip_archive_file_stat st;
    if (index < 0 || !mz_zip_reader_file_stat(&e->base.zip, (mz_uint)index, &st) ||
        st.m_uncomp_size > UINT32_MAX || st.m_comp_size > UINT32_MAX || st.m_is_directory) return false;
    *out = (book_entry_file_t){st.m_crc32, (uint32_t)st.m_comp_size, (uint32_t)st.m_uncomp_size, st.m_method};
    return true;
}
static int entry_export(epub *e, const char *href, const book_compute_selection_t *selected, bool first)
{
    if (e->split || !e->observer || !e->observer->entry_buffer || !e->observer->entry_ready ||
        !(selected->flags & BOOK_COMPUTE_BODY_ONLY) || e->headings_incomplete ||
        e->base.zip.m_archive_size > UINT32_MAX) return BOOK_OK;
    epub_item *nav = entry_navigation(e);
    const char *anchor = first && e->reading_rank == 3 && strchr(e->reading_entry, '#') ? e->reading_entry : "";
    const char *fields[] = {e->title, e->author, href, e->opf, nav ? nav->href : "", anchor};
    book_entry_t *entry = e->observer->entry_buffer;
    size_t used = sizeof(*entry), prefix = strlen(href), heading_bytes = 0;
    memset(entry, 0, sizeof(*entry));
    for (unsigned i = 0; i < BOOK_ENTRY_FIELDS; ++i) {
        size_t n = strlen(fields[i]) + 1;
        if (n > BOOK_ENTRY_MAX - used) return BOOK_OK;
        memcpy((uint8_t *)entry + used, fields[i], n);
        entry->length[i] = (uint32_t)n; used += n;
    }
    for (size_t at = 0; at < e->heading_bytes;) {
        const char *p = e->heading_targets + at;
        size_t n = strlen(p) + 1; at += n;
        if (n <= prefix + 2 || strncmp(p, href, prefix) || p[prefix] != '#') continue;
        if (n > BOOK_ENTRY_MAX - used) return BOOK_OK;
        memcpy((uint8_t *)entry + used, p, n); used += n; heading_bytes += n;
    }
    const char *paths[] = {"META-INF/container.xml", e->opf, nav ? nav->href : "", href};
    for (unsigned i = 0; i < 4; ++i) {
        int rc = bf_cancel(e->sink);
        if (rc < 0) return rc;
        if (!entry_file(e, paths[i], &entry->file[i])) return BOOK_OK;
    }
    entry->magic = BOOK_ENTRY_MAGIC; entry->bytes = (uint32_t)used;
    entry->chapter = selected->spine; entry->chapters = e->chapter_total;
    entry->source_size = (uint32_t)e->base.zip.m_archive_size;
    entry->flags = first ? BOOK_ENTRY_FIRST | ((selected->flags & BOOK_COMPUTE_FIRST_BODY) ? BOOK_ENTRY_FIRST_EFFECTIVE : 0) : 0;
    entry->heading_bytes = (uint32_t)heading_bytes;
    entry->checksum = book_entry_checksum(entry, used);
    return book_entry_valid(entry, used) ? e->observer->entry_ready(e->observer->user, used) : BOOK_OK;
}
/* A false return emits no records and leaves the ordinary parser untouched. */
static bool entry_try(epub *e, const char *path, int *result)
{
    if (e->split || !e->selection || !(e->selection->flags & BOOK_COMPUTE_BODY_ONLY)) return false;
    const book_entry_t *entry = e->selection->entry;
    if (!entry || !book_entry_valid(entry, entry->bytes) || entry->source_size != e->base.zip.m_archive_size) return false;
    bool first = (e->selection->flags & BOOK_COMPUTE_FIRST_BODY) != 0;
    if (first ? !(entry->flags & BOOK_ENTRY_FIRST) : entry->chapter != e->selection->spine) return false;
    const char *href = book_entry_field(entry, BOOK_ENTRY_HREF);
    const char *paths[] = {"META-INF/container.xml", book_entry_field(entry, BOOK_ENTRY_OPF),
        book_entry_field(entry, BOOK_ENTRY_NAV), href};
    for (unsigned i = 0; i < 4; ++i) {
        if ((*result = bf_cancel(e->sink)) < 0) return true;
        book_entry_file_t current;
        if (!entry_file(e, paths[i], &current) || memcmp(&current, &entry->file[i], sizeof(current))) return false;
    }
    book_compute_selection_t selected = *e->selection;
    selected.spine = entry->chapter;
    if (first && !(entry->flags & BOOK_ENTRY_FIRST_EFFECTIVE)) selected.flags &= ~BOOK_COMPUTE_FIRST_BODY;
    e->selection = &selected; e->selected_index = entry->chapter; e->chapter_total = entry->chapters;
    e->heading_targets = (char *)book_entry_headings(entry); e->heading_bytes = entry->heading_bytes;
    e->heading_borrowed = true;
    const char *anchor = book_entry_field(entry, BOOK_ENTRY_ANCHOR);
    if (first && *anchor) { strcpy(e->reading_entry, anchor); e->reading_rank = 3; }
    *result = bf_metadata(e->sink, book_entry_field(entry, BOOK_ENTRY_TITLE), book_entry_field(entry, BOOK_ENTRY_AUTHOR));
    if (*result == BOOK_OK && e->observer && e->observer->chapter)
        *result = e->observer->chapter(e->observer->user, entry->chapter, entry->chapters, BOOK_FORMAT_EPUB, true, href, href);
    if (*result == BOOK_OK && e->observer && e->observer->entry_ready)
        *result = e->observer->entry_ready(e->observer->user, entry->bytes);
    if (*result == BOOK_OK) *result = render_part(e, path, href, NULL);
    return true;
}
