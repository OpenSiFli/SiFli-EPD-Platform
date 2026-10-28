#include "netif_port.h"

extern int __real_aicwf_rx_eth_data_process(uint8_t *, uint16_t, net_if_t,
                                           void *, uint8_t);

int __wrap_aicwf_rx_eth_data_process(uint8_t *data, uint16_t length,
                                    net_if_t netif, void *message, uint8_t reordered)
{
    /* The official RT route copies the frame into a new protocol-owned pbuf.
     * Its reorder/zero-copy input is not transferred to that protocol. Release
     * it only after the synchronous report has finished reading the payload.
     * The host library's forward path relinquishes this reference to us. */
    int result = __real_aicwf_rx_eth_data_process(data, length, netif, message, reordered);
    if (message)
        aicwf_platform_net_buf_rx_free(message);
    return result;
}
