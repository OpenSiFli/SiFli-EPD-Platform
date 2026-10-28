#include <rtthread.h>
#include <rtm.h>

#ifdef AUDIO_USING_MANAGER
#include <audio_server.h>
RTM_EXPORT(audio_open);
RTM_EXPORT(audio_close);
RTM_EXPORT(audio_write);
RTM_EXPORT(audio_ioctl);
#endif

#ifdef AUDIO_LOCAL_MUSIC
#include <audio_mp3ctrl.h>
#include "mp3dec.h"
RTM_EXPORT(MP3FindSyncWord);

RTM_EXPORT(mp3ctrl_open);
RTM_EXPORT(mp3ctrl_close);
RTM_EXPORT(mp3ctrl_play);
RTM_EXPORT(mp3ctrl_resume);
RTM_EXPORT(mp3ctrl_pause);
RTM_EXPORT(mp3ctrl_seek);
RTM_EXPORT(mp3ctrl_getinfo);
RTM_EXPORT(mp3_get_id3_start);
RTM_EXPORT(mp3_get_id3_end);
#endif

#ifdef PKG_LIB_FLAC
#include <FLAC/stream_decoder.h>
RTM_EXPORT(FLAC__stream_decoder_new);
RTM_EXPORT(FLAC__stream_decoder_delete);
RTM_EXPORT(FLAC__stream_decoder_finish);
RTM_EXPORT(FLAC__stream_decoder_init_stream);
RTM_EXPORT(FLAC__stream_decoder_process_until_end_of_metadata);
RTM_EXPORT(FLAC__stream_decoder_process_single);
RTM_EXPORT(FLAC__stream_decoder_get_state);
RTM_EXPORT(FLAC__stream_decoder_seek_absolute);
#endif
