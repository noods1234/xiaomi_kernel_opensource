// SPDX-License-Identifier: GPL-2.0-only
/*
 * fake_a750_hw.c — Fake Adreno 750 (SM8650) GMU/HFI hardware simulation
 *
 * Simulates the Adreno 750 GPU Management Unit for pre-device firmware-
 * contract testing under QEMU.  No real GPU hardware required.
 *
 * Five integrated layers:
 *   Layer 2 — Register file  : vmalloc-backed u32 arrays for GPU+GMU MMIO
 *   Layer 3 — GMU state machine: IDLE → BOOTING → FW_INIT_DONE → HFI_READY
 *   Layer 4 — HFI surrogate  : services H2F queue, enqueues F2H ACKs
 *   Layer 5 — MMIO access log: ring buffer exposed via debugfs
 *   Self-test: full 6-message GMU cold-boot sequence on module load
 *
 * Register offsets match a6xx_gmu.xml.h (word-indexed × 4 = byte offset):
 *   CM3_ITCM_START  0x0c00  CM3_DTCM_START   0x1c00
 *   CM3_SYSRESET    0x5000  CM3_BOOT_CONFIG  0x5001
 *   CM3_FW_INIT_RESULT 0x501c
 *   HFI_CTRL_STATUS 0x5180  HFI_QTBL_ADDR   0x5185
 *   HFI_CTRL_INIT   0x5186  GMU2HOST_INTR   0x5192
 *
 * Firmware naming (linux-firmware.git qcom/):
 *   SQE: gen70900_sqe.fw   GMU: gmu_gen70900.bin
 *   ZAP: sm8650/gen70900_zap.mbn  (signed, SoC-specific)
 *
 * NOT for production use.  Enable with CONFIG_FAKE_A750_HW=m.
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/atomic.h>

/* -------------------------------------------------------------------------
 * Size constants
 * ---------------------------------------------------------------------- */
#define GPU_REGS_BYTES	0x40000u
#define GMU_REGS_BYTES	0x30000u
#define GPU_REGS_WORDS	(GPU_REGS_BYTES / 4)
#define GMU_REGS_WORDS	(GMU_REGS_BYTES / 4)
#define HFI_MEM_BYTES	(16 * 1024u)
#define HFI_MEM_WORDS	(HFI_MEM_BYTES / 4)
#define HFI_QUEUE_WORDS	512u

/* -------------------------------------------------------------------------
 * GMU register word-indices  (byte_offset >> 2)
 * ---------------------------------------------------------------------- */
#define REG_CM3_ITCM_START	0x0c00u
#define REG_CM3_DTCM_START	0x1c00u
#define REG_CM3_SYSRESET	0x5000u
#define REG_CM3_BOOT_CONFIG	0x5001u
#define REG_CM3_FW_INIT_RESULT	0x501cu
#define REG_HFI_CTRL_STATUS	0x5180u
#define REG_HFI_QTBL_ADDR	0x5185u
#define REG_HFI_CTRL_INIT	0x5186u
#define REG_GMU2HOST_INTR_INFO	0x5192u

/* -------------------------------------------------------------------------
 * HFI constants
 * ---------------------------------------------------------------------- */
#define HFI_COMMAND_QUEUE	0
#define HFI_RESPONSE_QUEUE	1
#define HFI_F2H_MSG_ACK		126
#define HFI_F2H_MSG_ERROR	100

#define HFI_H2F_MSG_INIT	0
#define HFI_H2F_MSG_FW_VERSION	1
#define HFI_H2F_MSG_BW_TABLE	3
#define HFI_H2F_MSG_PERF_TABLE	4
#define HFI_H2F_MSG_TEST	5
#define HFI_H2F_MSG_START	10
#define HFI_H2F_MSG_CORE_FW_START 14

#define HFI_HEADER(id, sz, seq)	(((seq) << 20) | ((sz) << 8) | (id))
#define HFI_HEADER_ID(h)	((h) & 0xff)
#define HFI_HEADER_SIZE(h)	(((h) >> 8) & 0xff)
#define HFI_HEADER_SEQNUM(h)	(((h) >> 20) & 0xfff)

/* HFI shared memory layout (u32 word offsets) */
#define QTBL_HDR_OFF		0u	/* 6 words: queue table header   */
#define H2F_HDR_OFF		6u	/* 12 words: H2F queue header     */
#define F2H_HDR_OFF		18u	/* 12 words: F2H queue header     */
#define H2F_DATA_OFF		30u	/* 512 words: H2F queue ring      */
#define F2H_DATA_OFF		542u	/* 512 words: F2H queue ring      */

/* struct a6xx_hfi_queue_header field offsets within a 12-word block */
#define QHDR_STATUS	0
#define QHDR_IOVA	1
#define QHDR_TYPE	2
#define QHDR_SIZE	3
#define QHDR_MSG_SIZE	4
#define QHDR_DROPPED	5
#define QHDR_RX_WM	6
#define QHDR_TX_WM	7
#define QHDR_RX_REQ	8
#define QHDR_TX_REQ	9
#define QHDR_READ_IDX	10
#define QHDR_WRITE_IDX	11

/* -------------------------------------------------------------------------
 * MMIO access log  (Layer 5)
 * ---------------------------------------------------------------------- */
#define MMIO_LOG_SIZE	256

struct mmio_entry {
	char     op;		/* 'R' or 'W' */
	u32      off;		/* word offset */
	u32      val;
};

static struct mmio_entry mmio_log[MMIO_LOG_SIZE];
static unsigned int      mmio_log_head;	/* next write position */
static unsigned int      mmio_log_count;
static DEFINE_SPINLOCK(mmio_log_lock);

static void mmio_log_append(char op, u32 off, u32 val)
{
	unsigned long flags;

	spin_lock_irqsave(&mmio_log_lock, flags);
	mmio_log[mmio_log_head % MMIO_LOG_SIZE] = (struct mmio_entry){ op, off, val };
	mmio_log_head++;
	if (mmio_log_count < MMIO_LOG_SIZE)
		mmio_log_count++;
	spin_unlock_irqrestore(&mmio_log_lock, flags);
}

/* -------------------------------------------------------------------------
 * Register files  (Layer 2)
 * ---------------------------------------------------------------------- */
static u32 *gmu_regs;	/* [GMU_REGS_WORDS] */
static u32 *gpu_regs;	/* [GPU_REGS_WORDS] */
static u32 *hfi_mem;	/* [HFI_MEM_WORDS]  */

/* Waitqueues for inter-thread signalling */
static DECLARE_WAIT_QUEUE_HEAD(gmu_event_wq);
static DECLARE_WAIT_QUEUE_HEAD(hfi_event_wq);
static DECLARE_WAIT_QUEUE_HEAD(f2h_event_wq);

/* GMU boot state machine */
enum gmu_state {
	GMU_IDLE = 0,
	GMU_BOOTING,
	GMU_FW_INIT_DONE,
	GMU_HFI_WAITING,
	GMU_HFI_READY,
};
static atomic_t gmu_state = ATOMIC_INIT(GMU_IDLE);

/* HFI ACK counter for self-test */
static atomic_t hfi_ack_count = ATOMIC_INIT(0);

static u32 gmu_read(u32 word_off)
{
	u32 val;

	if (word_off >= GMU_REGS_WORDS)
		return 0;
	val = gmu_regs[word_off];
	mmio_log_append('R', word_off, val);
	return val;
}

static void gmu_write(u32 word_off, u32 val)
{
	if (word_off >= GMU_REGS_WORDS)
		return;
	gmu_regs[word_off] = val;
	mmio_log_append('W', word_off, val);

	if (word_off == REG_CM3_SYSRESET && val == 0)
		wake_up(&gmu_event_wq);
	if (word_off == REG_HFI_CTRL_INIT && val == 1)
		wake_up(&hfi_event_wq);
}

/* -------------------------------------------------------------------------
 * Layer 3 — GMU boot state machine kthread
 * ---------------------------------------------------------------------- */
static struct task_struct *gmu_sm_task;

static int gmu_sm_thread(void *unused)
{
	while (!kthread_should_stop()) {
		/* Wait for CM3_SYSRESET to be written 0 */
		wait_event_interruptible(gmu_event_wq,
			kthread_should_stop() ||
			(atomic_read(&gmu_state) == GMU_IDLE &&
			 gmu_regs[REG_CM3_SYSRESET] == 0));

		if (kthread_should_stop())
			break;
		if (atomic_read(&gmu_state) != GMU_IDLE)
			continue;

		atomic_set(&gmu_state, GMU_BOOTING);

		/* Simulate CM3 firmware init delay */
		msleep(5);

		/* Signal FW_INIT complete: (val & 0x1ff) == 0x100 */
		gmu_regs[REG_CM3_FW_INIT_RESULT] = 0x100;
		atomic_set(&gmu_state, GMU_FW_INIT_DONE);

		/* Wait for HFI_CTRL_INIT = 1 */
		wait_event_interruptible(hfi_event_wq,
			kthread_should_stop() ||
			gmu_regs[REG_HFI_CTRL_INIT] == 1);

		if (kthread_should_stop())
			break;

		atomic_set(&gmu_state, GMU_HFI_WAITING);

		/* HFI subsystem ready */
		gmu_regs[REG_HFI_CTRL_STATUS] = 1;
		atomic_set(&gmu_state, GMU_HFI_READY);

		/* Re-arm: wait for next reset cycle */
		wait_event_interruptible(gmu_event_wq,
			kthread_should_stop() ||
			gmu_regs[REG_CM3_SYSRESET] == 1);
		if (!kthread_should_stop())
			atomic_set(&gmu_state, GMU_IDLE);
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Layer 4 — HFI queue surrogate kthread
 * ---------------------------------------------------------------------- */
static struct task_struct *hfi_surr_task;

static void hfi_enqueue_ack(u32 ret_header)
{
	u32 wi, ri, avail;
	u32 *f2h_hdr = hfi_mem + F2H_HDR_OFF;
	u32 *f2h_data = hfi_mem + F2H_DATA_OFF;
	u32 seqnum = HFI_HEADER_SEQNUM(ret_header);
	u32 ack_hdr = HFI_HEADER(HFI_F2H_MSG_ACK, 3, seqnum);

	wi = f2h_hdr[QHDR_WRITE_IDX];
	ri = f2h_hdr[QHDR_READ_IDX];
	avail = HFI_QUEUE_WORDS - ((wi - ri) & (HFI_QUEUE_WORDS - 1));
	if (avail < 3)
		return; /* queue full — drop */

	f2h_data[wi % HFI_QUEUE_WORDS] = ack_hdr;
	f2h_data[(wi + 1) % HFI_QUEUE_WORDS] = ret_header;
	f2h_data[(wi + 2) % HFI_QUEUE_WORDS] = 0; /* error = 0 */
	/* publish: advance write index */
	f2h_hdr[QHDR_WRITE_IDX] = (wi + 3) & (HFI_QUEUE_WORDS - 1);

	atomic_inc(&hfi_ack_count);
	wake_up(&f2h_event_wq);
}

static int hfi_surrogate_thread(void *unused)
{
	while (!kthread_should_stop()) {
		u32 *h2f_hdr = hfi_mem + H2F_HDR_OFF;
		u32 *h2f_data = hfi_mem + H2F_DATA_OFF;
		u32 wi, ri, msg_hdr, msg_id, msg_sz;

		wait_event_interruptible(hfi_event_wq,
			kthread_should_stop() ||
			h2f_hdr[QHDR_WRITE_IDX] != h2f_hdr[QHDR_READ_IDX]);

		if (kthread_should_stop())
			break;

		/* Drain all pending H2F messages */
		wi = h2f_hdr[QHDR_WRITE_IDX];
		ri = h2f_hdr[QHDR_READ_IDX];

		while (ri != wi) {
			msg_hdr = h2f_data[ri % HFI_QUEUE_WORDS];
			msg_id  = HFI_HEADER_ID(msg_hdr);
			msg_sz  = HFI_HEADER_SIZE(msg_hdr);

			/* Validate known boot message IDs */
			switch (msg_id) {
			case HFI_H2F_MSG_INIT:
			case HFI_H2F_MSG_FW_VERSION:
			case HFI_H2F_MSG_BW_TABLE:
			case HFI_H2F_MSG_PERF_TABLE:
			case HFI_H2F_MSG_TEST:
			case HFI_H2F_MSG_START:
			case HFI_H2F_MSG_CORE_FW_START:
				hfi_enqueue_ack(msg_hdr);
				break;
			default:
				pr_warn("fake_a750: unknown HFI msg id=%u\n",
					msg_id);
				break;
			}

			if (msg_sz < 1)
				msg_sz = 1;
			ri = (ri + msg_sz) & (HFI_QUEUE_WORDS - 1);
		}
		h2f_hdr[QHDR_READ_IDX] = ri;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * HFI queue init helper
 * ---------------------------------------------------------------------- */
static void hfi_queue_init(void)
{
	u32 *qtbl = hfi_mem;
	u32 *h2f_hdr = hfi_mem + H2F_HDR_OFF;
	u32 *f2h_hdr = hfi_mem + F2H_HDR_OFF;

	memset(hfi_mem, 0, HFI_MEM_BYTES);

	/* Queue table header */
	qtbl[0] = 0;			/* version */
	qtbl[1] = HFI_MEM_WORDS;	/* size in dwords */
	qtbl[2] = H2F_HDR_OFF;		/* qhdr0_offset */
	qtbl[3] = 12;			/* qhdr_size */
	qtbl[4] = 2;			/* num_queues */
	qtbl[5] = 2;			/* active_queues */

	/* H2F (command) queue header */
	h2f_hdr[QHDR_STATUS]    = 1;
	h2f_hdr[QHDR_TYPE]      = HFI_COMMAND_QUEUE;
	h2f_hdr[QHDR_SIZE]      = HFI_QUEUE_WORDS;
	h2f_hdr[QHDR_RX_WM]     = 1;
	h2f_hdr[QHDR_TX_WM]     = 1;

	/* F2H (response) queue header */
	f2h_hdr[QHDR_STATUS]    = 1;
	f2h_hdr[QHDR_TYPE]      = HFI_RESPONSE_QUEUE;
	f2h_hdr[QHDR_SIZE]      = HFI_QUEUE_WORDS;
	f2h_hdr[QHDR_RX_WM]     = 1;
	f2h_hdr[QHDR_TX_WM]     = 1;
}

/* Send one HFI message and wait for ACK */
static int hfi_send_and_wait(u32 msg_id, u32 seqnum, unsigned int timeout_ms)
{
	u32 *h2f_hdr  = hfi_mem + H2F_HDR_OFF;
	u32 *h2f_data = hfi_mem + H2F_DATA_OFF;
	u32 wi, before;
	int ret;

	/* 1-dword minimal message (header only) */
	wi = h2f_hdr[QHDR_WRITE_IDX];
	h2f_data[wi % HFI_QUEUE_WORDS] = HFI_HEADER(msg_id, 1, seqnum);
	h2f_hdr[QHDR_WRITE_IDX] = (wi + 1) & (HFI_QUEUE_WORDS - 1);

	before = atomic_read(&hfi_ack_count);

	/* Kick the surrogate thread */
	wake_up(&hfi_event_wq);

	/* Wait for an ACK to appear */
	ret = wait_event_timeout(f2h_event_wq,
		atomic_read(&hfi_ack_count) != before,
		msecs_to_jiffies(timeout_ms));
	return ret ? 0 : -ETIMEDOUT;
}

/* -------------------------------------------------------------------------
 * Self-test kthread  (runs once on module load if selftest=1)
 * ---------------------------------------------------------------------- */
static struct task_struct *selftest_task;
static bool selftest = true;
module_param(selftest, bool, 0444);
MODULE_PARM_DESC(selftest, "Run GMU boot self-test on module load (default: 1)");

static const u8 boot_msg_ids[] = {
	HFI_H2F_MSG_INIT,
	HFI_H2F_MSG_FW_VERSION,
	HFI_H2F_MSG_BW_TABLE,
	HFI_H2F_MSG_PERF_TABLE,
	HFI_H2F_MSG_START,
	HFI_H2F_MSG_CORE_FW_START,
};

static int selftest_thread(void *unused)
{
	int i, rc;
	int acked = 0;
	unsigned long deadline;
	const int TOTAL = ARRAY_SIZE(boot_msg_ids);

	/* Step 1: reset state */
	memset(gmu_regs, 0, GMU_REGS_BYTES);
	memset(gpu_regs, 0, GPU_REGS_BYTES);
	atomic_set(&gmu_state, GMU_IDLE);
	atomic_set(&hfi_ack_count, 0);
	gmu_regs[REG_CM3_SYSRESET] = 1; /* held in reset */

	/* Step 2: fake firmware load — write sentinel to ITCM */
	gmu_regs[REG_CM3_ITCM_START] = 0xdeadbeef;
	gmu_regs[REG_CM3_DTCM_START] = 0xcafebabe;

	/* Step 3: release CM3 (SYSRESET 1 → 0) */
	gmu_write(REG_CM3_SYSRESET, 1);
	gmu_write(REG_CM3_SYSRESET, 0);

	/* Step 4: poll CM3_FW_INIT_RESULT for (val & 0x1ff) == 0x100 */
	deadline = jiffies + msecs_to_jiffies(100);
	while (time_before(jiffies, deadline)) {
		if ((gmu_read(REG_CM3_FW_INIT_RESULT) & 0x1ff) == 0x100)
			goto fw_init_ok;
		msleep(1);
	}
	pr_err("fake_a750: FAIL step 4 — CM3_FW_INIT_RESULT timeout\n");
	return -ETIMEDOUT;

fw_init_ok:
	/* Step 5: init HFI queues */
	hfi_queue_init();

	/* Step 6: write HFI_QTBL_ADDR, then HFI_CTRL_INIT = 1 */
	gmu_write(REG_HFI_QTBL_ADDR, 0); /* sim uses pointer-less in-kernel buf */
	gmu_write(REG_HFI_CTRL_INIT, 1);

	/* Step 7: poll HFI_CTRL_STATUS until bit 0 set */
	deadline = jiffies + msecs_to_jiffies(100);
	while (time_before(jiffies, deadline)) {
		if (gmu_read(REG_HFI_CTRL_STATUS) & 1)
			goto hfi_ready;
		msleep(1);
	}
	pr_err("fake_a750: FAIL step 7 — HFI_CTRL_STATUS timeout\n");
	return -ETIMEDOUT;

hfi_ready:
	/* Steps 8-9: send 6 boot messages, wait for each ACK */
	for (i = 0; i < TOTAL; i++) {
		rc = hfi_send_and_wait(boot_msg_ids[i], i + 1, 50);
		if (rc) {
			pr_err("fake_a750: FAIL step 9 — msg id=%u timed out (acked %d/%d)\n",
				boot_msg_ids[i], acked, TOTAL);
			return rc;
		}
		acked++;
	}

	pr_info("fake_a750: boot self-test PASS (%d/%d messages ACKed)\n",
		acked, TOTAL);
	return 0;
}

/* -------------------------------------------------------------------------
 * Layer 5 — debugfs MMIO log
 * ---------------------------------------------------------------------- */
static struct dentry *dbgfs_root;

static int mmio_log_show(struct seq_file *m, void *unused)
{
	unsigned long flags;
	unsigned int i, start, count;

	spin_lock_irqsave(&mmio_log_lock, flags);
	count = mmio_log_count;
	start = (mmio_log_head - count) % MMIO_LOG_SIZE;
	spin_unlock_irqrestore(&mmio_log_lock, flags);

	for (i = 0; i < count; i++) {
		struct mmio_entry *e = &mmio_log[(start + i) % MMIO_LOG_SIZE];
		/* Mark registers outside the known GMU control range */
		char pfx = (e->off >= REG_CM3_ITCM_START &&
			    e->off < GMU_REGS_WORDS) ? ' ' : '?';
		seq_printf(m, "%c%c %05x %08x\n",
			   pfx, e->op, e->off * 4, e->val);
	}
	return 0;
}

static int mmio_log_open(struct inode *inode, struct file *file)
{
	return single_open(file, mmio_log_show, NULL);
}

static const struct file_operations mmio_log_fops = {
	.owner   = THIS_MODULE,
	.open    = mmio_log_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* -------------------------------------------------------------------------
 * Module init / exit
 * ---------------------------------------------------------------------- */
static int __init fake_a750_init(void)
{
	int ret = -ENOMEM;

	gmu_regs = vzalloc(GMU_REGS_BYTES);
	if (!gmu_regs)
		goto err_gmu;

	gpu_regs = vzalloc(GPU_REGS_BYTES);
	if (!gpu_regs)
		goto err_gpu;

	hfi_mem = vzalloc(HFI_MEM_BYTES);
	if (!hfi_mem)
		goto err_hfi;

	/* debugfs */
	dbgfs_root = debugfs_create_dir("fake_a750", NULL);
	debugfs_create_file("mmio_log", 0444, dbgfs_root, NULL, &mmio_log_fops);

	/* Start GMU state machine */
	gmu_sm_task = kthread_run(gmu_sm_thread, NULL, "fake_a750_gmu");
	if (IS_ERR(gmu_sm_task)) {
		ret = PTR_ERR(gmu_sm_task);
		gmu_sm_task = NULL;
		goto err_gmu_task;
	}

	/* Start HFI surrogate */
	hfi_surr_task = kthread_run(hfi_surrogate_thread, NULL, "fake_a750_hfi");
	if (IS_ERR(hfi_surr_task)) {
		ret = PTR_ERR(hfi_surr_task);
		hfi_surr_task = NULL;
		goto err_hfi_task;
	}

	/* Optionally run self-test */
	if (selftest) {
		selftest_task = kthread_run(selftest_thread, NULL,
					    "fake_a750_test");
		if (IS_ERR(selftest_task)) {
			ret = PTR_ERR(selftest_task);
			selftest_task = NULL;
			goto err_test_task;
		}
	}

	pr_info("fake_a750: A750 GMU/HFI simulation loaded (selftest=%d)\n",
		selftest);
	return 0;

err_test_task:
	kthread_stop(hfi_surr_task);
	hfi_surr_task = NULL;
err_hfi_task:
	kthread_stop(gmu_sm_task);
	gmu_sm_task = NULL;
err_gmu_task:
	debugfs_remove_recursive(dbgfs_root);
	vfree(hfi_mem);
err_hfi:
	vfree(gpu_regs);
err_gpu:
	vfree(gmu_regs);
err_gmu:
	return ret;
}

static void __exit fake_a750_exit(void)
{
	if (selftest_task) {
		kthread_stop(selftest_task);
		selftest_task = NULL;
	}
	if (hfi_surr_task) {
		/* Unblock hfi thread before stopping */
		wake_up(&hfi_event_wq);
		kthread_stop(hfi_surr_task);
		hfi_surr_task = NULL;
	}
	if (gmu_sm_task) {
		wake_up(&gmu_event_wq);
		wake_up(&hfi_event_wq);
		kthread_stop(gmu_sm_task);
		gmu_sm_task = NULL;
	}

	debugfs_remove_recursive(dbgfs_root);

	vfree(hfi_mem);
	vfree(gpu_regs);
	vfree(gmu_regs);

	pr_info("fake_a750: unloaded\n");
}

module_init(fake_a750_init);
module_exit(fake_a750_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Xiaomi Cinema Kernel Project");
MODULE_DESCRIPTION("Fake Adreno 750 GMU/HFI hardware simulation for QEMU testing");
