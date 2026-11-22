/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IDENTITY_DOCUMENT_RUNTIME_H
#define IDENTITY_DOCUMENT_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
void identity_runtime_start(const char *id, const char *peer_id);
void identity_runtime_pump(unsigned lane, uint32_t now);
void identity_runtime_admitted(uint64_t activity);
void identity_runtime_owned(const void *allocation);
void identity_runtime_finish(void);
void *identity_alloc(size_t bytes);
void identity_free(void *memory);
size_t identity_size(const void *memory);
size_t identity_available(void);
#endif
