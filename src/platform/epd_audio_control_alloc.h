#ifndef EPD_AUDIO_CONTROL_ALLOC_H
#define EPD_AUDIO_CONTROL_ALLOC_H

#include "epd_audio_alloc.h"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef audio_mem_malloc
#undef audio_mem_calloc
#undef audio_mem_realloc
#undef audio_mem_free

/* Scoped to audio_mp3ctrl.c and audio_server.c. Both select rt_calloc again
 * after including audio_mem.h. Their zero-filled objects are CPU-owned client,
 * command and ring-buffer storage. rt_free dispatches to the owning memheap. */
#define rt_calloc epd_audio_calloc

#ifdef EPD_AUDIO_CONTROLLER_ALLOC
/* The native controller's input/PCM workspaces are copied into Audio Manager.
 * Audio Manager's malloc path and the device drivers retain SRAM allocation. */
#define rt_malloc epd_audio_malloc
#endif

#endif
