// SPDX-License-Identifier: GPL-2.0+
/*
 * IBM Power Systems Virtual Management Channel Support.
 *
 * Copyright (c) 2018 IBM Corp.
 *   Bryant G. Ly <bryantly@linux.vnet.ibm.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/device.h>

#include <asm/hvcall.h>
#include <asm/vio.h>

#include "ibmvsm.h"

#define IBMVSM_DRIVER_VERSION "0.1"
#define MSG_HI	0
#define MSG_LOW	1
#define MAX_VTERM 2
#define MAX_VIO_PUT_CHARS	16
#define SIZE_VIO_GET_CHARS	16

static const char ibmvsm_driver_name[] = "ibmvsm";

static struct ibmvsm_struct ibmvsm;
static struct ibmvsm_vterm vterms[MAX_VTERM];
static struct crq_server_adapter ibmvsm_adapter;
static dev_t ibmvsm_chrdev;

static struct class *ibmvsm_class;
struct device *ibmvsm_dev;

enum crq_entry_header {
	CRQ_FREE = 0x00,
	CRQ_CMD_RSP = 0x80,
	CRQ_INIT_MSG = 0xC0
};

enum crq_init_formats {
	CRQ_INIT = 0x01,
	CRQ_INIT_COMPLETE = 0x02
};

/**
 * crq_queue_next_crq: - Returns the next entry in message queue
 * @queue:      crq_queue to use
 *
 * Returns pointer to next entry in queue, or NULL if there are no new
 * entried in the CRQ.
 */
static struct ibmvsm_crq_msg *crq_queue_next_crq(struct crq_queue *queue)
{
	struct ibmvsm_crq_msg *crq;
	unsigned long flags;

	spin_lock_irqsave(&queue->lock, flags);
	crq = &queue->msgs[queue->cur];
	if (crq->valid & 0x80) {
		if (++queue->cur == queue->size)
			queue->cur = 0;

		/* Ensure the read of the valid bit occurs before reading any
		 * other bits of the CRQ entry
		 */
		dma_rmb();
	} else {
		crq = NULL;
	}

	spin_unlock_irqrestore(&queue->lock, flags);

	return crq;
}

/**
 * ibmvsm_send_init_message() - send initialization message to the client
 */
static long ibmvsm_send_init_msg(struct crq_server_adapter *adapter, u8 type)
{
	struct ibmvsm_crq_msg *crq;
	struct vio_dev *vdev = to_vio_dev(adapter->dev);
	u64 buffer[2] = { 0 , 0 };
	long rc;

	crq = (struct ibmvsm_crq_msg *)&buffer;
	crq->valid = CRQ_INIT_MSG;
	crq->type = type;
	rc = h_send_crq(vdev->unit_address,
			cpu_to_be64(buffer[MSG_HI]),
			cpu_to_be64(buffer[MSG_LOW]));

	return rc;
}

/**
 * ibmvsm_get_chars - retrieve characters from firmware for denoted vterm adapter
 * @adapter: point to the crq server adapter
 * @buf: The character buffer into which to put the character data fetched from
 *	firmware.
 */
static long ibmvsm_get_chars(struct crq_server_adapter *adapter, u64 tok, char *buf)
{
	struct vio_dev *vdev = to_vio_dev(adapter->dev);
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];
	unsigned long *lbuf = (unsigned long *)buf;
	long rc;

	rc = h_get_term_char_lp(retbuf, vdev->unit_address, tok);
	lbuf[MSG_HI] = be64_to_cpu(retbuf[1]);
	lbuf[MSG_LOW] = be64_to_cpu(retbuf[2]);

	if (rc == H_SUCCESS)
		return retbuf[0];

	return 0;
}

/**
 * ibmvsm_put_chars: send characters to firmware for denoted vterm adapter
 * @adapter: point to the crq server adapter
 * @buf: The character buffer that contains the character data to send to
 *	firmware.
 * @count: Send this number of characters.
 */
static long ibmvsm_put_chars(struct crq_server_adapter *adapter, u64 tok,
			     const char *buf, int count)
{
	struct vio_dev *vdev = to_vio_dev(adapter->dev);
	unsigned long *lbuf = (unsigned long *) buf;
	long rc;


	/* hcall will ret H_PARAMETER if 'count' exceeds firmware max.*/
	if (count > MAX_VIO_PUT_CHARS)
		count = MAX_VIO_PUT_CHARS;

	rc = h_put_term_char_lp(vdev->unit_address, tok, count,
				cpu_to_be64(lbuf[MSG_HI]),
				cpu_to_be64(lbuf[MSG_LOW]));

	if (rc == H_SUCCESS)
		return count;
	if (rc == H_BUSY)
		return -EAGAIN;
	return -EIO;
}

/**
 * ibmvsm_read - Read
 *
 * @file:	file struct
 * @buf:	character buffer
 * @nbytes:	size in bytes
 * @ppos:	offset
 *
 * Return:
 *	0 - Success
 *	Non-zero - Failure
 */
static ssize_t ibmvsm_read(struct file *file, char *buf, size_t nbytes,
			   loff_t *ppos)
{
	return 0;
}

/**
 * ibmvsm_write - Write
 *
 * @file:	file struct
 * @buf:	character buffer
 * @count:	count field
 * @ppos:	offset
 *
 * Return:
 *	0 - Success
 *	Non-zero - Failure
 */
static ssize_t ibmvsm_write(struct file *file, const char *buffer,
			    size_t count, loff_t *ppos)
{
	return 0;
}

/**
 * ibmvsm_poll - Poll
 *
 * @file:	file struct
 * @wait:	Poll Table
 *
 * Return:
 *	poll.h return values
 */
static unsigned int ibmvsm_poll(struct file *file, poll_table *wait)
{
	return 0;
}

/**
 * ibmvsm_ioctl_setid - IOCTL set HMC ID
 *
 * @session: ibmvsm_file_session struct
 * @new_hmc_id: HMC id field
 *
 * IOCTL command to setup the hmc id
 *
 * Return:
 * 	0 - Success
 * 	Non-zero - Failure
 */
static long ibmvsm_ioctl_setid(struct ibmvsm_file_session *session,
			       unsigned char __user *new_hmc_id)
{
	/* Reserve HMC session */

	/* Make sure Version exchange is done first */

	/* Send H_OPEN_VTERM_LP */
	return 0;
}

/**
 * ibmvsm_ioctl - IOCTL
 *
 * @session:	ibmvsm_file_session struct
 * @cmd:	cmd field
 * @arg:	Argument field
 *
 * Return:
 *	0 - Success
 *	Non-zero - Failure
 */
static long ibmvsm_ioctl(struct file *file,
			 unsigned int cmd, unsigned long arg)
{
	struct ibmvsm_file_session *session = file->private_data;

	pr_debug("ibmvsm: ioctl file=0x%lx, cmd=0x%x, arg=0x%lx, ses=0x%lx\n",
		 (unsigned long)file, cmd, arg,
		 (unsigned long)session);

	if (!session) {
		pr_warn("ibmvsm: ioctl: no session\n");
		return -EIO;
	}

	switch (cmd) {
	case VSM_IOCTL_SETID:
		return ibmvsm_ioctl_setid(session,
				(unsigned char __user *)arg);
	default:
		pr_warn("ibmvsm: unknown ioctl 0x%x\n", cmd);
		return -EINVAL;
	}
}

/**
 * ibmvsm_open - Open Session
 *
 * @inode:	inode struct
 * @file:	file struct
 *
 * Return:
 *	0 - Success
 */
static int ibmvsm_open(struct inode *inode, struct file *file)
{
	struct ibmvsm_file_session *session;
	int rc = 0;

	pr_debug("%s: inode = 0x%lx, file = 0x%lx, state = 0x%x\n", __func__,
		 (unsigned long)inode, (unsigned long)file,
		 ibmvsm.state);

	session = kzalloc(sizeof(*session), GFP_KERNEL);
	session->file = file;
	file->private_data = session;

	return rc;
}

/**
 * ibmvsm_close - Close Session
 *
 * @inode:	inode struct
 * @file:	file struct
 *
 * Return:
 *	0 - Success
 *	Non-zero - Failure
 */
static int ibmvsm_close(struct inode *inode, struct file *file)
{
	struct ibmvsm_file_session *session;
	int rc = 0;

	pr_debug("%s: file = 0x%lx, state = 0x%x\n", __func__,
		 (unsigned long)file, ibmvsm.state);

	session = file->private_data;
	if (!session)
		return -EIO;

	/* Do ibmvsm session specific stuff like check if vsm adapter
	 * available if not return -EIO, check if state is failed.
	 * if failed state then return -EIO. Then check if vsm state
	 * is trying to open again if so then close it.
	 */

	kzfree(session);

	return rc;
}

static const struct file_operations ibmvsm_fops = {
	.owner		= THIS_MODULE,
	.read		= ibmvsm_read,
	.write		= ibmvsm_write,
	.poll		= ibmvsm_poll,
	.unlocked_ioctl	= ibmvsm_ioctl,
	.open           = ibmvsm_open,
	.release        = ibmvsm_close,
};

/* routines for managing a command/response queue */
/**
 * ibmvsm_handle_event: - Interrupt handler for crq events
 * @irq:        number of irq to handle, not used
 * @dev_instance: crq_server_adapter that received interrupt
 *
 * Disables interrupts and schedules ibmvsm_task
 *
 * Always returns IRQ_HANDLED
 */
static irqreturn_t ibmvsm_handle_event(int irq, void *dev_instance)
{
	struct crq_server_adapter *adapter =
		(struct crq_server_adapter *)dev_instance;

	vio_disable_interrupts(to_vio_dev(adapter->dev));
	tasklet_schedule(&adapter->work_task);

	return IRQ_HANDLED;
}

/**
 * ibmvsm_release_crq_queue - Release CRQ Queue
 *
 * @adapter:	crq_server_adapter struct
 *
 * Return:
 *	0 - Success
 *	Non-Zero - Failure
 */
static void ibmvsm_release_crq_queue(struct crq_server_adapter *adapter)
{
	struct vio_dev *vdev = to_vio_dev(adapter->dev);
	struct crq_queue *queue = &adapter->queue;

	free_irq(vdev->irq, (void *)adapter);
	tasklet_kill(&adapter->work_task);

	if (adapter->reset_task)
		kthread_stop(adapter->reset_task);

	h_free_crq(vdev->unit_address);
	dma_unmap_single(adapter->dev,
			 queue->msg_token,
			 queue->size * sizeof(*queue->msgs), DMA_BIDIRECTIONAL);
	free_page((unsigned long)queue->msgs);
}

/**
 * ibmvsm_reset_crq_queue - Reset CRQ Queue
 *
 * @adapter:	crq_server_adapter struct
 *
 * This function calls h_free_crq and then calls H_REG_CRQ and does all the
 * bookkeeping to get us back to where we can communicate.
 *
 * Return:
 *	0 - Success
 *	Non-Zero - Failure
 */
static int ibmvsm_reset_crq_queue(struct crq_server_adapter *adapter)
{
	struct vio_dev *vdev = to_vio_dev(adapter->dev);
	struct crq_queue *queue = &adapter->queue;
	int rc = 0;

	/* Close the CRQ */
	h_free_crq(vdev->unit_address);

	/* Clean out the queue */
	memset(queue->msgs, 0x00, PAGE_SIZE);
	queue->cur = 0;

	/* And re-open it again */
	rc = h_reg_crq(vdev->unit_address,
		       queue->msg_token, PAGE_SIZE);
	if (rc == 2)
		/* Adapter is good, but other end is not ready */
		dev_warn(adapter->dev, "Partner adapter not ready\n");
	else if (rc != 0)
		dev_err(adapter->dev, "couldn't register crq--rc 0x%x\n", rc);

	return rc;
}

/**
 * ibmvsm_crq_process - Process CRQ
 *
 * @adapter:    crq_server_adapter struct
 * @crq:	ibmvsm_crq_msg struct
 *
 * Process the CRQ message based upon the type of message received.
 *
 */
static void ibmvsm_crq_process(struct crq_server_adapter *adapter,
			       struct ibmvsm_crq_msg *crq)
{
	switch (crq->type) {
	case VSM_MSG_VER_EXCH:
	case VSM_MSG_VTERM_INT:
	case VSM_MSG_VERSION_EXCH_RSP:
	case VSM_MSG_SIG_VTERM_INT:
	case VSM_MSG_ERR:
		dev_warn(adapter->dev, "CRQ recv: unexpected msg (0x%x)\n",
			 crq->type);
		break;
	default:
		dev_warn(adapter->dev, "CRQ recv: unknown msg (0x%x)\n",
			 crq->type);
		break;
	}
}

/**
 * ibmvsm_reset - Reset
 *
 * @adapter:	crq_server_adapter struct
 * @xport_event:	export_event field
 *
 * Closes all vterm sessions and conditionally schedules a CRQ reset.
 * @xport_event: If true, the partner closed their CRQ; we don't need to reset.
 *               If false, we need to schedule a CRQ reset.
 */
static void ibmvsm_reset(struct crq_server_adapter *adapter, bool xport_event)
{
	if (ibmvsm.state != ibmvsm_state_sched_reset) {
		dev_info(adapter->dev, "*** Reset to initial state.\n");

		/* TODO: return the vterm used back to init state to re-use*/

		if (xport_event) {
			/* CRQ was closed by the partner.  We don't need to do
			 * anything except set ourself to the correct state to
			 * handle init msgs.
			 */
			ibmvsm.state = ibmvsm_state_crqinit;
		} else {
			/* The partner did not close their CRQ - instead, we're
			 * closing the CRQ on our end. Need to schedule this
			 * for process context, because CRQ reset may require a
			 * sleep.
			 *
			 * Setting ibmvsm.state here immediately prevents
			 * ibmvsm_open from completing until the reset
			 * completes in process context.
			 */
			ibmvsm.state = ibmvsm_state_sched_reset;
			dev_dbg(adapter->dev, "Device reset scheduled");
			wake_up_interruptible(&adapter->reset_wait_queue);
		}
	}
}

/**
 * ibmvsm_reset_task - Reset Task
 *
 * @data:	Data field
 *
 * Performs a CRQ reset of the VSM device in process context.
 * NOTE: This function should not be called directly, use ibmvsm_reset.
 */
static int ibmvsm_reset_task(void *data)
{
	struct crq_server_adapter *adapter = data;
	int rc;

	set_user_nice(current, -20);

	while (!kthread_should_stop()) {
		wait_event_interruptible(adapter->reset_wait_queue,
			(ibmvsm.state == ibmvsm_state_sched_reset) ||
			kthread_should_stop());

		if (kthread_should_stop())
			break;

		dev_dbg(adapter->dev, "CRQ resetting in process context");
		tasklet_disable(&adapter->work_task);

		rc = ibmvsm_reset_crq_queue(adapter);

		if (rc != H_SUCCESS && rc != H_RESOURCE) {
			dev_err(adapter->dev, "Error initializing CRQ.  rc = 0x%x\n",
				rc);
			ibmvsm.state = ibmvsm_state_failed;
		} else {
			ibmvsm.state = ibmvsm_state_crqinit;

			if (ibmvsm_send_init_msg(adapter, CRQ_INIT) != 0 &&
			    rc != H_RESOURCE)
				dev_warn(adapter->dev, "Failed to send initialize CRQ message\n");
		}

		vio_enable_interrupts(to_vio_dev(adapter->dev));
		tasklet_enable(&adapter->work_task);
	}

	return 0;
}

/**
 * ibmvsm_handle_crq_init - Handle CRQ Init
 *
 * @crq:	ibmvsm_crq_msg struct
 * @adapter:	crq_server_adapter struct
 *
 * Handle the type of crq initialization based on whether
 * it is a message or a response.
 *
 */
static void ibmvsm_handle_crq_init(struct ibmvsm_crq_msg *crq,
				   struct crq_server_adapter *adapter)
{
	switch (crq->type) {
	case 0x01:	/* Initialization message */
		dev_dbg(adapter->dev, "CRQ recv: CRQ init msg - state 0x%x\n",
			ibmvsm.state);
		if (ibmvsm.state == ibmvsm_state_crqinit) {
			if (ibmvsm_send_init_msg(adapter, CRQ_INIT_COMPLETE) == 0) {
				/* Do Version Exchange */
			} else {
				dev_err(adapter->dev, " Unable to send init rsp\n");
			}
		} else {
			dev_err(adapter->dev, "Invalid state 0x%x\n",
				ibmvsm.state);
		}

		break;
	case 0x02:	/* Initialization response */
		dev_dbg(adapter->dev, "CRQ recv: initialization resp msg - state 0x%x\n",
			ibmvsm.state);
		if (ibmvsm.state == ibmvsm_state_crqinit)
			/* Do Version Exchange */
		break;
	default:
		dev_warn(adapter->dev, "Unknown crq message type 0x%lx\n",
			 (unsigned long)crq->type);
	}
	return 0;
}

/**
 * ibmvsm_handle_crq - Handle CRQ
 *
 * @crq:	ibmvsm_crq_msg struct
 * @adapter:	crq_server_adapter struct
 *
 * Read the command elements from the command queue and execute the
 * requests based upon the type of crq message.
 *
 */
static void ibmvsm_handle_crq(struct ibmvsm_crq_msg *crq,
			      struct crq_server_adapter *adapter)
{
	switch (crq->valid) {
	case 0xC0:		/* initialization */
		ibmvsm_handle_crq_init(crq, adapter);
		break;
	case 0xFF:	/* Hypervisor telling us the connection is closed */
		dev_warn(adapter->dev, "CRQ recv: virtual adapter failed - resetting.\n");
		ibmvsm_reset(adapter, true);
		break;
	case 0x80:	/* real payload */
		ibmvsm_crq_process(adapter, crq);
		break;
	default:
		dev_warn(adapter->dev, "CRQ recv: unknown msg 0x%02x.\n",
			 crq->valid);
		break;
	}
}

static void ibmvsm_task(unsigned long data)
{
	struct crq_server_adapter *adapter =
		(struct crq_server_adapter *)data;
	struct vio_dev *vdev = to_vio_dev(adapter->dev);
	struct ibmvsm_crq_msg *crq;
	int done = 0;

	while (!done) {
		/* Pull all the valid messages off the CRQ */
		while ((crq = crq_queue_next_crq(&adapter->queue)) != NULL) {
			ibmvsm_handle_crq(crq, adapter);
			crq->valid = 0x00;
			/* CRQ reset was requested, stop processing CRQs.
			 * Interrupts will be re-enabled by the reset task.
			 */
			if (ibmvsm.state == ibmvsm_state_sched_reset)
				return;
		}

		vio_enable_interrupts(vdev);
		crq = crq_queue_next_crq(&adapter->queue);
		if (crq) {
			vio_disable_interrupts(vdev);
			ibmvsm_handle_crq(crq, adapter);
			crq->valid = 0x00;
			/* CRQ reset was requested, stop processing CRQs.
			 * Interrupts will be re-enabled by the reset task.
			 */
			if (ibmvsm.state == ibmvsm_state_sched_reset)
				return;
		} else {
			done = 1;
		}
	}
}

/**
 * ibmvsm_init_crq_queue - Init CRQ Queue
 *
 * @adapter:	crq_server_adapter struct
 *
 * Return:
 *	0 - Success
 *	Non-zero - Failure
 */
static int ibmvsm_init_crq_queue(struct crq_server_adapter *adapter)
{
	struct vio_dev *vdev = to_vio_dev(adapter->dev);
	struct crq_queue *queue = &adapter->queue;
	int rc = 0;
	int retrc = 0;

	queue->msgs = (struct ibmvsm_crq_msg *)get_zeroed_page(GFP_KERNEL);

	if (!queue->msgs)
		goto malloc_failed;

	queue->size = PAGE_SIZE / sizeof(*queue->msgs);

	queue->msg_token = dma_map_single(adapter->dev, queue->msgs,
					  queue->size * sizeof(*queue->msgs),
					  DMA_BIDIRECTIONAL);

	if (dma_mapping_error(adapter->dev, queue->msg_token))
		goto map_failed;

	retrc = plpar_hcall_norets(H_REG_CRQ,
				   vdev->unit_address,
				   queue->msg_token, PAGE_SIZE);
	rc = retrc;

	if (rc == H_RESOURCE)
		rc = ibmvsm_reset_crq_queue(adapter);

	if (rc == 2) {
		dev_warn(adapter->dev, "Partner adapter not ready\n");
		retrc = 0;
	} else if (rc != 0) {
		dev_err(adapter->dev, "Error %d opening adapter\n", rc);
		goto reg_crq_failed;
	}

	queue->cur = 0;
	spin_lock_init(&queue->lock);

	tasklet_init(&adapter->work_task, ibmvsm_task, (unsigned long)adapter);

	if (request_irq(vdev->irq,
			ibmvsm_handle_event,
			0, "ibmvsm", (void *)adapter) != 0) {
		dev_err(adapter->dev, "couldn't register irq 0x%x\n",
			vdev->irq);
		goto req_irq_failed;
	}

	rc = vio_enable_interrupts(vdev);
	if (rc != 0) {
		dev_err(adapter->dev, "Error %d enabling interrupts!!!\n", rc);
		goto req_irq_failed;
	}

	return retrc;

req_irq_failed:
	/* Cannot have any work since we either never got our IRQ registered,
	 * or never got interrupts enabled
	 */
	tasklet_kill(&adapter->work_task);
	h_free_crq(vdev->unit_address);
reg_crq_failed:
	dma_unmap_single(adapter->dev,
			 queue->msg_token,
			 queue->size * sizeof(*queue->msgs), DMA_BIDIRECTIONAL);
map_failed:
	free_page((unsigned long)queue->msgs);
malloc_failed:
	return -ENOMEM;
}

/* Fill in the liobn and riobn fields on the adapter */
static int read_dma_window(struct vio_dev *vdev,
			   struct crq_server_adapter *adapter)
{
	const __be32 *dma_window;
	const __be32 *prop;

	dma_window =
		(const __be32 *)vio_get_attribute(vdev, "ibm,my-dma-window",
						  NULL);
	if (!dma_window) {
		dev_warn(adapter->dev, "Couldn't find ibm,my-dma-window property\n");
		return -1;
	}

	adapter->liobn = be32_to_cpu(*dma_window);
	dma_window++;

	prop = (const __be32 *)vio_get_attribute(vdev, "ibm,#dma-address-cells",
						 NULL);
	if (!prop) {
		dev_warn(adapter->dev, "Couldn't find ibm,#dma-address-cells property\n");
		dma_window++;
	} else {
		dma_window += be32_to_cpu(*prop);
	}

	prop = (const __be32 *)vio_get_attribute(vdev, "ibm,#dma-size-cells",
						 NULL);
	if (!prop) {
		dev_warn(adapter->dev, "Couldn't find ibm,#dma-size-cells property\n");
		dma_window++;
	} else {
		dma_window += be32_to_cpu(*prop);
	}

	/* dma_window should point to the second window now */
	adapter->riobn = be32_to_cpu(*dma_window);

	return 0;
}

static int ibmvsm_probe(struct vio_dev *vdev, const struct vio_device_id *id)
{
	struct crq_server_adapter *adapter = &ibmvsm_adapter;
	int rc;

	dev_set_drvdata(&vdev->dev, NULL);
	memset(adapter, 0, sizeof(*adapter));
	adapter->dev = &vdev->dev;

	dev_info(adapter->dev, "Probe for UA 0x%x\n", vdev->unit_address);

	/* Read DMA Window */
	rc = read_dma_window(vdev, adapter);
	if (rc != 0) {
		ibmvsm.state = ibmvsm_state_failed;
		return -1;
	}

	dev_dbg(adapter->dev, "Probe: liobn 0x%x, riobn 0x%x\n",
		adapter->liobn, adapter->riobn);

	init_waitqueue_head(&adapter->reset_wait_queue);
	adapter->reset_task = kthread_run(ibmvsm_reset_task, adapter, "ibmvsm");
	if (IS_ERR(adapter->reset_task)) {
		dev_err(adapter->dev, "Failed to start reset thread\n");
		ibmvsm.state = ibmvsm_state_failed;
		rc = PTR_ERR(adapter->reset_task);
		adapter->reset_task = NULL;
		return rc;
	}

	/* Init CRQ */
	rc = ibmvsm_init_crq_queue(adapter);
	if (rc != 0 && rc != H_RESOURCE) {
		dev_err(adapter->dev, "Error initializing CRQ.  rc = 0x%x\n",
			rc);
		ibmvsm.state = ibmvsm_state_failed;
		goto crq_failed;
	}

	ibmvsm.state = ibmvsm_state_crqinit;

	if (ibmvsm_send_init_msg(adapter, CRQ_INIT) != 0 &&
	    rc != H_RESOURCE)
		dev_warn(adapter->dev, "Failed to send initialize CRQ message\n");

	dev_set_drvdata(&vdev->dev, adapter);

	return 0;
crq_failed:
	kthread_stop(adapter->reset_task);
	adapter->reset_task = NULL;
	return -EPERM;
}

static int ibmvsm_remove(struct vio_dev *vdev)
{
	struct crq_server_adapter *adapter = dev_get_drvdata(&vdev->dev);

	dev_info(adapter->dev, "Entering remove for UA 0x%x\n",
		 vdev->unit_address);

	/* Need to release the CRQ here */
	ibmvsm_release_crq_queue(adapter);

	return 0;
}

static struct vio_device_id ibmvsm_device_table[] = {
	{ "serial-multiplex", "IBM,serial-multiplex" },
	{ "", "" }
};
MODULE_DEVICE_TABLE(vio, ibmvsm_device_table);

static struct vio_driver ibmvsm_driver = {
	.name        = ibmvsm_driver_name,
	.id_table    = ibmvsm_device_table,
	.probe       = ibmvsm_probe,
	.remove      = ibmvsm_remove,
};

static int __init ibmvsm_module_init(void)
{
	int rc, i;

	ibmvsm.state = ibmvsm_state_initial;
	pr_info("ibmvsm: version %s\n", IBMVSM_DRIVER_VERSION);

	/* Dynamically allocate ibmvsm major number */
	if (alloc_chrdev_region(&ibmvsm_chrdev, 0, VSM_NUM_MINORS,
		ibmvsm_driver_name)) {
		pr_err("ibmvsm: unable to allocate a dev_t\n");
		rc = -EIO;
		goto alloc_chrdev_failed;
	}

	pr_info("ibmvsm: node %d:%d\n", MAJOR(ibmvsm_chrdev),
			MINOR(ibmvsm_chrdev));

	memset(vterms, 0, sizeof(struct ibmvsm_vterm) * MAX_VTERM);
	for (i = 0; i < MAX_VTERM; i++) {
		spin_lock_init(&vterms[i].lock);
		vterms[i].state = ibmvterm_state_free;
	}

	cdev_init(&ibmvsm.cdev, &ibmvsm_fops);
	ibmvsm.cdev.owner = THIS_MODULE;
	ibmvsm.cdev.ops = &ibmvsm_fops;

	rc = cdev_add(&ibmvsm.cdev, ibmvsm_chrdev, VSM_NUM_MINORS);
	if (rc) {
		pr_err("ibmvsm: unable to add cdev: %d\n", rc);
		goto cdev_add_failed;
	}

	ibmvsm_class = class_create(THIS_MODULE, "ibmvsm");
	if (IS_ERR(ibmvsm_class)) {
		rc = PTR_ERR(ibmvsm_class);
		pr_err("ibmvsm: class regiter ibmvsm failed");
		goto cdev_add_failed;
	}

	ibmvsm_dev = device_create(ibmvsm_class, NULL, ibmvsm_chrdev,
			NULL, "ibmvsm");
	if (IS_ERR(ibmvsm_dev)) {
		rc = PTR_ERR(ibmvsm_dev);
		pr_err("ibmvsm: device add failed");
		goto device_create_failed;
	}

	rc = vio_register_driver(&ibmvsm_driver);
	if (rc) {
		pr_err("ibmvsm: rc %d from vio_register_driver\n", rc);
		goto vio_reg_fail;
	}
	/* Init data structures */
	return 0;

vio_reg_fail:
	cdev_del(&ibmvsm.cdev);
device_create_failed:
	class_destroy(ibmvsm_class);
cdev_add_failed:
	unregister_chrdev_region(ibmvsm_chrdev, VSM_NUM_MINORS);
alloc_chrdev_failed:
	return rc;
}

static void __exit ibmvsm_module_exit(void)
{
	pr_info("ibmvsm: module exit\n");
	vio_unregister_driver(&ibmvsm_driver);
	cdev_del(&ibmvsm.cdev);
	device_destroy(ibmvsm_class, ibmvsm_chrdev);
	class_destroy(ibmvsm_class);
	unregister_chrdev_region(ibmvsm_chrdev, VSM_NUM_MINORS);
}

MODULE_AUTHOR("Bryant G. Ly <bryantly@linux.vnet.ibm.com>");
MODULE_DESCRIPTION("IBM VSM");
MODULE_VERSION(IBMVSM_DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
