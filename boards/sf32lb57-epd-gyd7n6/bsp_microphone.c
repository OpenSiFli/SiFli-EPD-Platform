/* SPDX-License-Identifier: Apache-2.0 */
#include "bsp_board.h"
#include "bsp_microphone.h"

#if defined(SOC_BF0_HCPU) && defined(BSP_USING_PDM1) && !defined(CFG_BOOTLOADER)
#include <rtthread.h>
#include <rtdevice.h>
#include <stdint.h>
extern void BSP_GPIO_Set(int pin, int val, int is_porta);

void BSP_Microphone_PowerOn(void)
{
    /* U6 ETA5055 supplies MIC1 and MIC2; PA57 is its enable input. */
    BSP_GPIO_Set(57, 1, 1);
    rt_thread_mdelay(10);
}

void BSP_Microphone_PowerOff(void)
{
    BSP_GPIO_Set(57, 0, 1);
}

int BSP_Microphone_SetGain(int gain_half_db)
{
    rt_device_t device;
    if (gain_half_db < -30 || gain_half_db > 90)
        return -RT_EINVAL;
    device = rt_device_find("pdm1");
    if (!device)
        return -RT_ENOSYS;
    return rt_device_control(device, AUDIO_CTL_SETVOLUME,
                             (void *)(intptr_t)gain_half_db);
}
#endif
