/* Per-source cache paths keep warm chapter handoffs independent. */
typedef struct { char binary[768], index[768]; } context_cache_t;
static unsigned context_caches;
document_cache_t *document_cache_create_source(const bookshelf_item_t *f, const char *a, const char *d, const char *identity)
{
    (void)f; (void)a; (void)d;
    context_cache_t *c = calloc(1, sizeof(*c)); assert(c); ++context_caches;
    snprintf(c->binary, sizeof(c->binary), "%s/%s.bin", test_root, identity);
    snprintf(c->index, sizeof(c->index), "%s/%s.idx", test_root, identity);
    return (document_cache_t *)c;
}
document_cache_t *document_cache_create(const bookshelf_item_t *f)
{ return document_cache_create_source(f, NULL, NULL, "whole"); }
bool document_cache_temporary_number(const char *d, const char *e, uint32_t *n, bool (*stop)(void *), void *u)
{ (void)d; (void)e; (void)n; (void)stop; (void)u; return false; }
const char *document_cache_bucket(const document_cache_t *c) { return c ? test_root : NULL; }
const char *document_cache_binary(const document_cache_t *c) { return ((context_cache_t *)c)->binary; }
const char *document_cache_index(const document_cache_t *c) { return ((context_cache_t *)c)->index; }
uint32_t document_cache_identity(const document_cache_t *c) { return c ? 1234 : 0; }
bool document_cache_lookup(document_cache_t *c, char *title, size_t n, uint32_t *end)
{
    struct stat st;
    if (stat(document_cache_binary(c), &st)) return false;
    *end = (uint32_t)st.st_size; snprintf(title, n, "Context fixture"); return true;
}
bool document_cache_prepare(document_cache_t *c) { _unlink(document_cache_binary(c)); return true; }
bool document_cache_invalidate(document_cache_t *c) { _unlink(document_cache_binary(c)); return true; }
bool document_cache_observe_begin(document_cache_t *c)
{ (void)c; ++observers; ++observe_begins; return true; }
void document_cache_observe_end(document_cache_t *c)
{ (void)c; assert(observers); --observers; ++observe_ends; }
void document_cache_observe_resource(document_cache_t *c, const char *p, int e) { (void)c; (void)p; (void)e; }
bool document_cache_commit(document_cache_t *c, const char *title, uint32_t end)
{ (void)c; (void)title; (void)end; ++cache_commits; return true; }
bool document_cache_touch(document_cache_t *c) { (void)c; return true; }
bool document_cache_has_space(const document_cache_t *c, size_t needed) { (void)c; (void)needed; return true; }
bool document_cache_reclaim(document_cache_t *c) { (void)c; return false; }
void document_cache_destroy(document_cache_t *c) { if (c) { --context_caches; free(c); } }
void document_cache_release_memory(void) { if (cache_retired_attempts) --cache_retired_attempts; }
bool document_cache_cleanup_pending(void) { return cache_retired_attempts != 0; }
void document_cache_cancel(document_cache_t *c) { (void)c; }
