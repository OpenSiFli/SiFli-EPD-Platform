#include "epd_image.h"
#include <rtthread.h>
#include <rtm.h>
#include <rthw.h>
#include <limits.h>
#include <string.h>
#include "drv_epic.h"

bool epd_image_jpeg_layout(unsigned width, unsigned height, epd_jpeg_layout_t *layout)
{
#if defined(HAL_JPEGD_MODULE_ENABLED) && !defined(DRV_EPIC_NEW_API)
    if (!layout || !width || !height || width > INT16_MAX || height > INT16_MAX)
        return false;
    unsigned stride = RT_ALIGN(width, 16);
    if ((uint64_t)stride * RT_ALIGN(height, 16) > INT_MAX) return false;
    JPEGD_DecodeConfigTypeDef config = {0};
    config.output_mode = HAL_JPEGD_OUTPUT_AHB;
    config.width = width;
    config.height = height;
    /* These HAL queries only inspect config and do not touch hardware. */
    int work_size = HAL_JPEGD_GetBufferSize(NULL, &config);
    int y_size, u_size, v_size;
    if (work_size <= 0 || HAL_OK != HAL_JPEGD_GetOutputSize(NULL, &config, &y_size, &u_size, &v_size) ||
        y_size <= 0 || u_size <= 0 || v_size <= 0) return false;
    layout->width = width;
    layout->height = height;
    layout->stride = stride;
    layout->work_size = RT_ALIGN((size_t)work_size, 64);
    layout->y_size = RT_ALIGN((size_t)y_size, 64);
    layout->u_size = RT_ALIGN((size_t)u_size, 64);
    layout->v_size = RT_ALIGN((size_t)v_size, 64);
    uint64_t total = (uint64_t)layout->work_size + layout->y_size + layout->u_size + layout->v_size;
    if (total > UINT32_MAX - 63u) return false;
    layout->buffer_size = (size_t)total;
    return true;
#else
    (void)width; (void)height; (void)layout;
    return false;
#endif
}
RTM_EXPORT(epd_image_jpeg_layout);

#if defined(HAL_JPEGD_MODULE_ENABLED) && !defined(DRV_EPIC_NEW_API)
static void jpeg_complete(JPEGD_HandleTypeDef *jpegd, void *context)
{
    (void)jpegd;
    rt_sem_release(context);
}

/* The HAL has no abort API. Reset both shared decode blocks while still
 * owning the GPU lock, before the caller can release DMA buffers. */
static void jpeg_stop(JPEGD_HandleTypeDef *jpegd)
{
    HAL_RCC_EnableModule(RCC_MOD_JPEGD);
    HAL_RCC_EnableModule(RCC_MOD_EZIP);
    jpegd->Instance->INT_EN = 0;
    HAL_RCC_ResetModule(RCC_MOD_JPEGD);
    HAL_RCC_ResetModule(RCC_MOD_EZIP);
    HAL_NVIC_ClearPendingIRQ(JPEGD_IRQn);
    HAL_NVIC_ClearPendingIRQ(EZIP_IRQn);
    jpegd->State = HAL_JPEGD_STATE_READY;
    jpegd->ErrorCode = 0;
    HAL_RCC_DisableModule(RCC_MOD_JPEGD);
    HAL_RCC_DisableModule(RCC_MOD_EZIP);
}
#endif

bool epd_image_jpeg_decode(const uint8_t *input, size_t length,
                           const epd_jpeg_layout_t *layout, void *buffer,
                           bool (*cancel)(void *context), void *context)
{
#if defined(HAL_JPEGD_MODULE_ENABLED) && !defined(DRV_EPIC_NEW_API)
    if (!input || !layout || !buffer || !length || length > UINT32_MAX ||
        ((uintptr_t)input & 31) || ((uintptr_t)buffer & 63)) return false;
    int width, height;
    if (HAL_OK != HAL_JPEGD_GetDim((uint8_t *)input, length, &width, &height) ||
        width != (int)layout->width || height != (int)layout->height) return false;
    if (cancel && cancel(context)) return false;

    struct rt_semaphore done;
    if (rt_sem_init(&done, "imgjpg", 0, RT_IPC_FLAG_FIFO) != RT_EOK) return false;
    /* Busy hardware remains available to its owner; software can start before
     * this operation has produced any output. */
    if (drv_gpu_take(0) != RT_EOK)
    {
        rt_sem_detach(&done);
        return false;
    }
    JPEGD_HandleTypeDef *jpegd = drv_get_jpegd_handle();
    if (!jpegd || HAL_JPEGD_CheckReady(jpegd) != HAL_OK ||
        jpegd->Instance != jpegd->HwInstance || (cancel && cancel(context)))
    {
        drv_gpu_release();
        rt_sem_detach(&done);
        return false;
    }

    JPEGD_DecodeConfigTypeDef config = {0};
    config.input = (uint8_t *)input;
    config.input_data_size = length;
    config.output_mode = HAL_JPEGD_OUTPUT_AHB;
    config.width = width;
    config.height = height;
    config.work_buffer = buffer;
    config.output_y = (uint8_t *)buffer + layout->work_size;
    config.output_u = config.output_y + layout->y_size;
    config.output_v = config.output_u + layout->u_size;
#ifdef PSRAM_CACHE_WB
    mpu_dcache_clean((void *)input, length);
    mpu_dcache_clean(buffer, layout->buffer_size);
#endif
#ifdef BSP_USING_PM
    rt_pm_request(PM_SLEEP_MODE_IDLE);
    rt_pm_hw_device_start();
#endif
    JPEGD_CpltCallback previous_callback = jpegd->CpltCallback;
    void *previous_user = jpegd->UserData;
    jpegd->CpltCallback = jpeg_complete;
    jpegd->UserData = &done;
    jpegd->ErrorCode = 0;
    rt_tick_t start = rt_tick_get();
    bool ok = false;
    if (HAL_JPEGD_Decode_IT(jpegd, &config) == HAL_OK)
    {
        while (!(cancel && cancel(context)) && jpegd->State != HAL_JPEGD_STATE_ERROR &&
               rt_tick_get() - start < rt_tick_from_millisecond(500))
        {
            if (rt_sem_take(&done, rt_tick_from_millisecond(20)) == RT_EOK)
            {
                ok = jpegd->State == HAL_JPEGD_STATE_READY && jpegd->ErrorCode == 0;
                break;
            }
        }
    }
    uint32_t error = jpegd->ErrorCode;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!ok) jpeg_stop(jpegd);
    jpegd->CpltCallback = previous_callback;
    jpegd->UserData = previous_user;
    rt_hw_interrupt_enable(level);
#ifdef BSP_USING_PM
    rt_pm_hw_device_stop();
    rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
    drv_gpu_release();
    rt_sem_detach(&done);
#ifdef PSRAM_CACHE_WB
    mpu_dcache_invalidate(config.output_y, layout->y_size);
#endif
    if (error)
        rt_kprintf("[image] JPEGD failed: error=0x%x\n", (unsigned)error);
    return ok;
#else
    (void)input; (void)length; (void)layout; (void)buffer; (void)cancel; (void)context;
    return false;
#endif
}
RTM_EXPORT(epd_image_jpeg_decode);
