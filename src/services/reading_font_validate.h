#ifndef EPD_READING_FONT_VALIDATE_H
#define EPD_READING_FONT_VALIDATE_H
#include "reading_font.h"

typedef bool (*reading_font_cancel_t)(void *context);
/* 32-bit FNV-1a (offset 2166136261, prime 16777619) of a freshly written,
 * fsynced staging file. The producer must bind
 * the hash to the path/revision/session captured under storage_lock(), and
 * discard it if any other writer touched the file. Keep path and this object
 * immutable for the duration of validation. storage_session is zero for flash,
 * or storage_card_session() for SD. This is not a persistent cache. */
typedef struct
{
    const char *path;
    uint32_t size, signature, path_revision, storage_session;
} reading_font_signature_t;

/* Bounded-memory, seekable static TrueType validator. Runs on a worker.
 * Checks table/glyph references and the supported outline format before the
 * file can enter the catalog. Writes a UTF-8 family/style display name.
 * Cancellation is polled at I/O boundaries and every 256 parser work units. */
rt_err_t reading_font_validate(const char *path, reading_font_info_t *info,
                               reading_font_cancel_t cancelled, void *context);
/* NULL or a mismatched binding falls back to hashing the whole file. A matching
 * binding skips only that hash pass; all structural checks still run. Changes
 * during validation fail, including revision/session/writer/size changes.
 * info is written only on success. No storage lock may be held by the caller. */
rt_err_t reading_font_validate_with_signature(const char *path, reading_font_info_t *info,
                                             const reading_font_signature_t *signature,
                                             reading_font_cancel_t cancelled, void *context);
/* Full installation validation; proof is written only on success. The catalog
 * version binds this bounded header fingerprint to the validation rules. */
rt_err_t reading_font_validate_install(const char *path, reading_font_info_t *info,
    const reading_font_signature_t *binding, uint32_t *proof,
    reading_font_cancel_t cancelled, void *context);
/* Check saved size, table directory and head/hhea/maxp against an installation
 * proof, retaining info unchanged. No glyph/cmap traversal or whole-file hash.
 * Reuses the same cancellation, path/writer/session checks as full validation;
 * a mismatch fails without a full-validation fallback. Header fingerprints do
 * not authenticate arbitrary same-size modifications of glyph data. */
rt_err_t reading_font_probe_installed(const char *path, reading_font_info_t *info,
    uint32_t proof, reading_font_cancel_t cancelled, void *context);
#endif
