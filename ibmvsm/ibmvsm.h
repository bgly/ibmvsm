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

enum ibmvsm_states {
	ibmvsm_state_sched_reset  = -1,
	ibmvsm_state_initial      = 0,
	ibmvsm_state_crqinit      = 1,
	ibmvsm_state_capabilities = 2,
	ibmvsm_state_ready        = 3,
	ibmvsm_state_failed       = 4,
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
};

/* VSM server adapter settings */
struct crq_server_adapter {
	struct device *dev;
	struct crq_queue queue;
	u32 liobn;
	u32 riobn;
};

struct ibmvsm_struct {
	u32 state;
	struct crq_server_adapter * adapter;
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

#endif /* __IBMVSM_H */
