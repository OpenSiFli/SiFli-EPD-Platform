#ifndef NOTES_AI_H
#define NOTES_AI_H
#include "notes_model.h"
#include "epd_ai.h"
typedef enum { AI_CLEAR, AI_CONCISE, AI_FORMAL, AI_TYPO, AI_SUMMARY, AI_TASKS, AI_MINUTES } notes_ai_mode_t;
epd_ai_error_t notes_ai_run(epd_service_t *owner, const note_t *source,
                          notes_ai_mode_t mode, epd_ai_cancel_t *cancel, note_t *result);
epd_ai_error_t notes_ai_profile(epd_service_t *owner, epd_ai_profile_t *profile, size_t *model);
bool notes_ai_apply(note_t *note, const note_t *preview);
bool notes_ai_pack(const note_t *source, const note_t *result, notes_ai_mode_t mode, note_buffer_t *out);
bool notes_ai_unpack(const void *data, size_t size, note_t *source, note_t *result, notes_ai_mode_t *mode);
#endif
