#ifndef TI_SCI_TRANSPORT_H
#define TI_SCI_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>


// enum k3_sec_proxy_chan_id {
// #if !K3_SEC_PROXY_LITE
// 	SP_NOTIFY = 0,
// 	SP_RESPONSE,
// 	SP_HIGH_PRIORITY,
// 	SP_LOW_PRIORITY,
// 	SP_NOTIFY_RESP,
// #else
// 	SP_RESPONSE = 8,
// 	/*
// 	 * Note: TISCI documentation indicates "low priority", but in reality
// 	 * with a single thread, there is no low or high priority.. This usage
// 	 * is more appropriate for TF-A since we can reduce the churn as a
// 	 * result.
// 	 */
// 	SP_HIGH_PRIORITY,
// #endif /* K3_SEC_PROXY_LITE */
// };



struct ti_sci_msg{
	size_t len;
	uint8_t *buf;
};

void mailbox_init(void);

uint32_t ti_sci_transport_send(const struct ti_sci_msg *msg);

uint32_t ti_sci_transport_recv(const struct ti_sci_msg *msg);

uint32_t ti_sci_clear_init(void);


#endif