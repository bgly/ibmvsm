/* SPDX-License-Identifier: GPL-2.0+
 *
 * linux/drivers/misc/ibmvsm.h
 *
 * IBM Power Systems Virtual Serial Multiplex
 *
 * Copyright (c) 2018 IBM Corp.
 *   Bryant G. Ly <bryantly@linux.vnet.ibm.com>
 */
#ifndef IBMVSM_H
#define IBMVSM_H

#include <linux/cdev.h>

#define H_OPEN_VTERM_LP		0x408
#define H_GET_TERM_CHAR_LP	0x40C
#define H_PUT_TERM_CHAR_LP	0x410
#define H_CLOSE_VTERM_LP	0x414

#define VSM_MSG_VER_EXCH		0x01
#define VSM_MSG_VTERM_INT		0x02
#define VSM_MSG_ERR			0x03
#define VSM_MSG_VERSION_EXCH_RSP	0x81
#define VSM_MSG_SIG_VTERM_INT		0x82

/* ioctl info */
#define VSM_TYPE		0xCD
#define VSM_IOCTL_SETID		_IOW(VSM_TYPE, 0x00, unsigned char *)

#define VSM_NUM_MINORS 1

enum ibmvsm_states {
	ibmvsm_state_sched_reset  = -1,
	ibmvsm_state_initial      = 0,
	ibmvsm_state_crqinit      = 1,
	ibmvsm_state_capabilities = 2,
	ibmvsm_state_ready        = 3,
	ibmvsm_state_failed       = 4,
};

enum ibmvsm_vterm_states {
	/* HMC connection not established */
	ibmvterm_state_free    = 0,

	/* HMC connection established (open called) */
	ibmvterm_state_initial = 1,

	/* open msg sent to HV, due to ioctl(1) call */
	ibmvterm_state_opening = 2,

	/* HMC connection ready, open resp msg from HV */
	ibmvterm_state_ready   = 3,

	/* VTERM connection failure */
	ibmvterm_state_failed  = 4,
};

struct ibmvsm_crq_msg {
	u8 valid;		/* RPA Defined */
	u8 type;		/* ibmvsm msg type */
	u16 rsvd;
	u32 rsvd1;
	u64 console_token;	/* Console Token */
};

/* an RPA command/response transport queue */
struct crq_queue {
	struct ibmvsm_crq_msg *msgs;
	int size, cur;
	dma_addr_t msg_token;
	spinlock_t lock;
};

/* VSM server adapter settings */
struct crq_server_adapter {
	struct device *dev;
	struct crq_queue queue;
	u32 liobn;
	u32 riobn;
	struct tasklet_struct work_task;
};

struct ibmvsm_struct {
	u32 state;
	struct crq_server_adapter *adapter;
	struct cdev cdev;
};

struct ibmvmc_file_session;

struct ibmvsm_vterm {
	u64 console_token;
	u32 state;
	u32 rsvd;
	struct crq_server_adapter *adapter;
	struct ibmvmc_file_session *file_session;
	spinlock_t lock;
};

struct ibmvsm_file_session {
	struct file *file;
	bool valid;
};

#define h_reg_crq(ua, tok, sz) \
		  plpar_hcall_norets(H_REG_CRQ, ua, tok, sz)
#define h_free_crq(ua) \
		   plpar_hcall_norets(H_FREE_CRQ, ua)
#define h_send_crq(ua, d1, d2) \
		   plpar_hcall_norets(H_SEND_CRQ, ua, d1, d2)
#define h_get_term_char_lp(buf, ua, tok) \
		   plpar_hcall_norets(H_GET_TERM_CHAR_LP, buf, ua, tok)
#define h_put_term_char_lp(ua, tok, len, d1, d2) \
		   plpar_hcall_norets(H_PUT_TERM_CHAR_LP, ua, tok, len, d1, d2)
#define h_open_vterm_lp(ua, sid, pid) \
		   plpar_hcall_norets(H_OPEN_VTERM_LP, ua, sid, pid)
#define h_close_vterm_lp(ua, tok) \
		   plpar_hcall_norets(H_CLOSE_VTERM_LP, ua, tok)

#endif /* __IBMVSM_H */
