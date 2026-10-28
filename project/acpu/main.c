/* SPDX-License-Identifier: Apache-2.0 */
#include "acpu_ctrl.h"
#include "epd_compute_protocol.h"
#include <rtthread.h>

void acpu_main(uint8_t task, void *parameter)
{
    RT_ASSERT(task == ACPU_TASK_0);
    epd_compute_remote_dispatch(parameter);
    /* Component errors live in the shared result, not the IPC error field. */
    acpu_send_result(ACPU_ERR_OK, 0);
}
