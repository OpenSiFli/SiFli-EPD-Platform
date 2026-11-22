#ifndef CONFIG_IO_H
#define CONFIG_IO_H
#include <stdbool.h>
#include <stdint.h>
typedef struct config_io config_io_t;
typedef void (*config_io_done_t)(bool saved, bool ok, uint32_t presses, void *context);
/* UI lane. Completion is asynchronous and ends the handle's lifetime. */
config_io_t *config_io_start(bool save, uint32_t presses, config_io_done_t done, void *context);
/* Detach the UI receiver. Reads cancel; accepted atomic writes finish. */
void config_io_close(config_io_t *io);
#endif
