/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Texas Instruments SCI Transport Protocol Header
 *
 * Copyright (C) 2018-2025 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef TI_SCI_TRANSPORT_H
#define TI_SCI_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#include <tee_api_types.h>

/**
 * struct ti_sci_msg - Secure proxy message structure
 * @len: Length of data in the Buffer
 * @buf: Buffer pointer
 *
 * This is the structure for data used in ti_sci_transport_{send,recv}()
 */
struct ti_sci_msg {
	size_t len;
	uint8_t *buf;
};

/**
 * mailbox_init() - Initialize the mailbox MMIO and SRAM regions
 *
 * This function initializes the mailbox MMIO and SRAM regions used for
 * sending and receiving messages.
 */
void mailbox_init(void);

/**
 * ti_mailbox_poll_rx_status() - Poll the mailbox RX status
 *
 * This function polls the mailbox RX status to check if a message has
 * been received.
 *
 * Return: 0 on success, or an error code on failure.
 */
TEE_Result ti_mailbox_poll_rx_status(void);

/**
 * ti_sci_transport_send() - Send data over a TISCI transport
 * @msg: Pointer to ti_sci_msg
 *
 * This function sends a message over the TISCI transport.
 *
 * Return: 0 on success, or an error code on failure.
 */
TEE_Result ti_sci_transport_send(const struct ti_sci_msg *msg);

/**
 * ti_sci_transport_recv() - Receive data over a TISCI transport
 * @msg: Pointer to ti_sci_msg
 *
 * This function receives a message over the TISCI transport.
 *
 * Return: 0 on success, or an error code on failure.
 */
TEE_Result ti_sci_transport_recv(const struct ti_sci_msg *msg);

/**
 * ti_sci_clear_init() - Initialize the secure proxy threads
 *
 * This function initializes the secure proxy threads used for TISCI
 * communication.
 *
 * Return: 0 on success, or an error code on failure.
 */
TEE_Result ti_sci_clear_init(void);

#endif /* TI_SCI_TRANSPORT_H */
