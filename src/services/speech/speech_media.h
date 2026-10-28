/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPEECH_MEDIA_H
#define SPEECH_MEDIA_H

#include "speech_internal.h"
#include "../../ui/platform/epd_system.h"

/* Worker-only; waits at most 2 seconds for confirmed media/tone silence.
 * Cancellation is checked between waits of at most 10 ms. Failed acquisition
 * rolls back only its own lease. Core requests TTS auto-pause before ASR acquire;
 * ASR waits for any TTS acquisition and its explicit output-close publication.
 * The same 2-second deadline includes these prerequisites and media/tone ACKs.
 * One lease per kind; repeated acquisition returns BUSY. */
epd_speech_error_t speech_media_acquire(epd_speech_kind_t kind,
                                      speech_cancel_fn cancel, void *user);
/* Recheck before opening capture/output and while feeding it. A paused TTS
 * keeps its lease; ASR temporarily blocks TTS output. External media intent
 * invalidates permission and prevents restoration of the captured music.
 * Explicit user continuation after takeover uses reacquire_tts below. */
bool speech_media_can_start(epd_speech_kind_t kind);
/* Worker-only, exclusively for an explicit user resume of a paused TTS. Keep
 * output closed and core pause set until success; never use for ASR auto-resume.
 * A valid lease is retained (including its restore veto). Only an invalid lease
 * is retired with restore=false, then the current media is paused and ACKed.
 * No core text/PCM is touched. On failure retain the paused core session; a later
 * explicit resume may retry this API even if the old lease was already retired.
 * New-lease failure rolls back only the newly paused media, conditionally.
 * Before rolling back a CANCELLED new lease, handoff_to_asr(user), if present,
 * is queried outside the media lock. True keeps the paused TTS lease and pending
 * PAUSE/ACK intact for ASR acquire, without restoring music. The result remains
 * CANCELLED; core keeps TTS paused. False/NULL uses ordinary conditional rollback.
 * The hook is not queried for a retained lease or for other completion results.
 * ASR ownership, active output or another acquisition returns BUSY unchanged.
 * cancel must also detect a superseding user intent/ASR request. Core must
 * recheck that intent before unpausing after OK. Requires the session sleep hold. */
epd_speech_error_t speech_media_reacquire_tts(speech_cancel_fn cancel, void *user,
                                            speech_cancel_fn handoff_to_asr);
/* service is the resident speech_tts service; requester is the business app
 * whose copied identity the shell displays. No requester pointer is retained.
 * paused=true is an output-close acknowledgement, never a pause request. A live
 * TTS publisher records it even if an external takeover rejects the visible
 * publication (false), including before its first successful publication.
 * This acknowledgement must not replace that external media.
 * title=NULL withdraws the TTS publication after output has closed. */
bool speech_media_publish(epd_service_t *service, epd_service_t *requester, const char *title,
                          bool playing, bool paused);
/* Consumes PLAY/PAUSE/STOP using the service's normal sleep-hold contract. */
bool speech_media_poll(epd_service_t *service, epd_media_command_t *command);
/* Call only after the corresponding capture/output is closed. ASR release
 * lifts tone suppression. The final lease release conditionally restores
 * music; restore=false on either lease vetoes that restoration. */
void speech_media_release(epd_speech_kind_t kind, bool restore);

#endif
