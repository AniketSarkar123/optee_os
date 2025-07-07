/*
 * Texas Instruments K3 Mailbox Driver
 * Copyright (C) 2025 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <stddef.h>
#include <string.h>
#include <kernel/delay.h>
#include <string.h>
#include <io.h>
#include <ti_sci_transport.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <ti_sci_protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/*
 * TI Mailbox Register Offsets
 * These offsets are used to access the mailbox
 * registers for sending and receiving messages.
 */

#define TI_MAILBOX_SYSC             0x10UL
#define TI_MAILBOX_MSG              0x40UL
#define TI_MAILBOX_FIFO_STATUS      0x80UL
#define TI_MAILBOX_MSG_STATUS       0xc0UL
#define TI_MAILBOX_TX_BASE          0x44240000UL
#define TI_MAILBOX_RX_BASE          0x44250000UL
#define MAILBOX_TX_START_REGION     0x70814000UL
#define MAILBOX_RX_START_REGION     0x70815000UL
#define MAILBOX_MAX_MESSAGE_SIZE    56U
#define MAILBOX_TX_REGION_SIZE      0x7081413FUL

/*
 * TI Mailbox MMIO and SRAM base addresses
 * These are the base addresses for the mailbox MMIO and SRAM regions.
 * They are used to map the physical addresses to virtual addresses.
 */

static void *mailbox_rx_base;
static void *mailbox_tx_base;
static void *mailbox_tx_sram_va;
static void *mailbox_rx_sram_va;

/*
 * mailbox_init() - Initialize the mailbox MMIO and SRAM regions
 */

void mailbox_init(void)
{
	mailbox_rx_base = (void *)core_mmu_get_va(TI_MAILBOX_RX_BASE, MEM_AREA_IO_SEC, 0x1000);
	mailbox_tx_base = (void *)core_mmu_get_va(TI_MAILBOX_TX_BASE, MEM_AREA_IO_SEC, 0x1000);
	mailbox_tx_sram_va = (void *)core_mmu_get_va(MAILBOX_TX_START_REGION, MEM_AREA_IO_SEC, 0x1000);
	mailbox_rx_sram_va = (void *)core_mmu_get_va(MAILBOX_RX_START_REGION, MEM_AREA_IO_SEC, 0x1000);

	if (!mailbox_rx_base || !mailbox_tx_base || !mailbox_tx_sram_va || !mailbox_rx_sram_va) {
		EMSG("Failed to map mailbox MMIO or SRAM regions: rx=%p tx=%p tx_sram=%p rx_sram=%p",
		     mailbox_rx_base, mailbox_tx_base, mailbox_tx_sram_va, mailbox_rx_sram_va);
		return;
	}

	IMSG("Mailbox MMIO and SRAM regions mapped: rx=%p tx=%p tx_sram=%p rx_sram=%p",
	      mailbox_rx_base, mailbox_tx_base, mailbox_tx_sram_va, mailbox_rx_sram_va);
}

/*
 * Function to poll for mailbox rx messages
 */

static int8_t ti_mailbox_poll_rx_status(void)
{
	uint32_t num_messages_pending = 0U;
	uint32_t retry_count = 100U;

	while (num_messages_pending == 0U) {
		num_messages_pending = io_read32((vaddr_t)mailbox_rx_base + TI_MAILBOX_MSG_STATUS);

		if (retry_count-- == 0U) {
			EMSG("Mailbox RX status polling timed out");
			return -1;
		}
		udelay(10000);
	}

	return 0;
}

/*
 * ti_sci_transport_send() - Send data over a TISCI transport
 */

TEE_Result ti_sci_transport_send(const struct ti_sci_msg *msg)
{
	uint32_t num_bytes;
	void *dst_ptr = mailbox_tx_sram_va;
	paddr_t pa;

	if (!msg)
		return -1;

	num_bytes = msg->len;

	if (io_read32((vaddr_t)mailbox_tx_base + TI_MAILBOX_FIFO_STATUS) != 0U) {
		EMSG("Mailbox TX FIFO is not empty");
		return -1;
	}

	if (num_bytes > MAILBOX_MAX_MESSAGE_SIZE) {
		EMSG("Message size exceeds maximum allowed size");
		return -1;
	}

	memmove(dst_ptr, msg->buf, num_bytes);
	pa = virt_to_phys(dst_ptr);
	io_write32((vaddr_t)mailbox_tx_base + TI_MAILBOX_MSG, (uint32_t)pa);

	return 0;
}

/*
 * ti_sci_transport_recv() - Receive data from a TISCI transport
 */

TEE_Result ti_sci_transport_recv(const struct ti_sci_msg *msg)
{
	uint32_t num_bytes;
	uint64_t rcv_addr;
	char hex_str[20];
	uint32_t val;
	uint8_t *rcv_va;

	if (!msg)
		return -1;

	num_bytes = msg->len / sizeof(uint8_t);

	if (ti_mailbox_poll_rx_status() != 0) {
		EMSG("Mailbox RX status polling failed");
		return -1;
	}

	rcv_addr = io_read32((vaddr_t)mailbox_rx_base + TI_MAILBOX_MSG);
	snprintf(hex_str, sizeof(hex_str), "0x%lx", rcv_addr);
	IMSG("Hex value of rcv_addr is: %s", hex_str);

	val = (uint32_t)strtoul(hex_str, NULL, 0);

	if (val < MAILBOX_RX_START_REGION) {
		EMSG("Message address %lu is not valid\n", rcv_addr);
		return -1;
	}

	if (num_bytes > MAILBOX_MAX_MESSAGE_SIZE) {
		EMSG("Message length %lu > max msg size\n", rcv_addr);
		return -1;
	}

	rcv_va = (uint8_t *)core_mmu_get_va(rcv_addr, MEM_AREA_IO_SEC, 0x1000);
	if (!rcv_va) {
		EMSG("Failed to get virtual address for RX");
		return -1;
	}

	memmove(msg->buf, (uint8_t *)rcv_va, num_bytes);
	return 0;
}

/*
 * ti_sci_clear_init() - Clear the mailbox RX status and initialize the mailbox
 */

uint32_t ti_sci_clear_init(void)
{
	uint32_t try_count;

	mailbox_init();

	try_count = io_read32((vaddr_t)mailbox_rx_base + TI_MAILBOX_MSG_STATUS);
	while (io_read32((vaddr_t)mailbox_rx_base + TI_MAILBOX_MSG_STATUS) != 0U) {
		io_read32((vaddr_t)mailbox_rx_base + TI_MAILBOX_MSG);
		if (try_count == 0U) {
			EMSG("Mailbox RX status polling timed out");
			return -1;
		}
		try_count--;
	}

	IMSG("Mailbox RX status cleared after %u tries", try_count);
	return 0;
}