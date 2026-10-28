/**
 * @file board_service.h
 * @brief 板级服务（纯 C）：文件系统挂载、SD 卡电源、开关机流程
 *
 * 由原 Board/SF32Paper C++ 类抽取而来，去掉多板卡抽象层。
 */
#ifndef __BOARD_SERVICE_H__
#define __BOARD_SERVICE_H__

#include <rtthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 开机流程：处理唤醒来源（冷启动/休眠唤醒）
 */
void board_power_up(void);

/**
 * @brief Mount internal Flash at /flash and start asynchronous SD card management.
 */
void board_start_filesystem(void);

typedef enum
{
    BOARD_POWER_ERROR_NONE = 0,
    BOARD_POWER_ERROR_SAVE,     /* Persistence barrier is pending or failed; services remain running. */
    BOARD_POWER_ERROR_BUSY,     /* Installer, card transition or a writable file is still active. */
    BOARD_POWER_ERROR_START,    /* Finalization thread could not be created/started. */
    BOARD_POWER_ERROR_WAKEUP,   /* SDK could not configure the power-key wake source. */
    BOARD_POWER_ERROR_RETURNED, /* Reset/shutdown returned without terminating execution. */
    BOARD_POWER_ERROR_FRONTLIGHT, /* Frontlight could not confirm zero output. */
    BOARD_POWER_ERROR_DISPLAY,  /* Shutdown wallpaper could not be prepared/refreshed. */
    BOARD_POWER_ERROR_TOUCH,    /* Touch controller could not stop scanning. */
} board_power_error_t;

/* UI thread: accept a shutdown (false) or restart (true) request. No I/O here.
 * False means busy/rejected; an accepted request can still fail during process.
 * While pending, block new application/installer actions and background discovery.
 * Keep the UI event loop alive; do not wait for a worker or a thread to exit. */
bool board_power_request(bool restart);

/* UI thread, after LVGL drawing has completed. Flush first, then reuse the
 * catalog's cooperative page/service release. The final storage barrier and
 * SDK power operation run on a worker. The new EPD board waits for the real
 * shutdown wallpaper's full refresh, panel/rail completion and zero frontlight
 * output, then enters Hibernate. Restart does not display the wallpaper.
 * No forced stop or timeout power-cut.
 * A SAVE error has not stopped any application; normal operation can resume. */
void board_power_process(void);
bool board_power_pending(void);
/* Sticky until the next accepted request; display an error when pending is false. */
board_power_error_t board_power_error(void);

/** Legacy entry: enqueue shutdown; the UI must continue board_power_process(). */
void board_prepare_to_sleep(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_SERVICE_H__ */
