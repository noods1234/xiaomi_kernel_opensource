/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal stub for linux/soc/qcom/socinfo.h — used by KGSL upstream path.
 *
 * The full socinfo header and SMEM_DDR_BUILD_ID are Qualcomm BSP additions
 * not yet present in this 6.1.25 tree.  Leaving SMEM_DDR_BUILD_ID undefined
 * causes kgsl_util.c to compile the fallback kgsl_get_ddrtype() → -ENOENT,
 * which is correct behaviour when DDR type info is unavailable.
 *
 * Replace this stub once linux/soc/qcom/socinfo.h lands upstream (6.6+).
 */
#ifndef __LINUX_SOC_QCOM_SOCINFO_H
#define __LINUX_SOC_QCOM_SOCINFO_H

/* SMEM_DDR_BUILD_ID intentionally not defined — see kgsl_util.c */

struct ddrinfo {
	u32 device_type;
};

#endif /* __LINUX_SOC_QCOM_SOCINFO_H */
