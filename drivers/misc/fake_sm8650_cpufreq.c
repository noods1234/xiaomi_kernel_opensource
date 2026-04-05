// SPDX-License-Identifier: GPL-2.0-only
/*
 * fake_sm8650_cpufreq.c — Simulated SM8650 cpufreq topology for QEMU/virt
 *
 * Registers four cpufreq policies that mirror the Snapdragon 8 Gen 3
 * (SM8650-AB / Pineapple) cluster layout as seen in the Qualcomm BSP:
 *
 *   Policy 0  CPUs 0-1   Cortex-A520  efficiency  307.2 – 2265.6 MHz  (25 OPPs)
 *   Policy 2  CPUs 2-3   Cortex-A720  perf-lo     460.8 – 2956.8 MHz  (44 OPPs)
 *   Policy 4  CPUs 4-6   Cortex-A720  perf-hi     460.8 – 3148.8 MHz  (47 OPPs)
 *   Policy 7  CPU  7     Cortex-X4    prime         480  – 3302.4 MHz  (49 OPPs)
 *
 * OPP tables are taken verbatim from the upstream mainline sm8650.dtsi
 * (arch/arm64/boot/dts/qcom/sm8650.dtsi, torvalds/linux).  Max frequencies
 * are cross-validated against the in-tree KUnit reference in cinema_mode_kunit.c:
 *   A520=2265600, A720-lo=2956800, A720-hi=3148800, X4=3302400 (kHz)
 *
 * Topology note — OEM (Qualcomm BSP) vs. mainline difference:
 *   BSP   : policy2=CPUs2-3(A720-lo), policy4=CPUs4-6(A720-hi)  ← this file
 *   Mainline: policy2=CPUs2-4(A720-hi), policy5=CPUs5-6(A720-lo)
 *
 * No real frequency switching happens.  The driver exists solely to present
 * valid struct cpufreq_policy objects so that cinema_mode.c can exercise its
 * freq_qos_add/remove_request paths under QEMU.
 *
 * NOT for production use.  Enable with CONFIG_FAKE_SM8650_CPUFREQ=m.
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

/*
 * OPP tables — verbatim from sm8650.dtsi operating-points-v2 nodes.
 * All values in kHz.  Steps derive from the 19.2 MHz EPSS reference clock
 * (multiples of 19200 kHz).
 */

/* Policy 0: Cortex-A520 ×2 (CPUs 0-1), 25 OPPs, max 2265.6 MHz */
static struct cpufreq_frequency_table effic_freqs[] = {
	{ .frequency =  307200 },
	{ .frequency =  364800 },
	{ .frequency =  460800 },
	{ .frequency =  556800 },
	{ .frequency =  672000 },
	{ .frequency =  787200 },
	{ .frequency =  902400 },
	{ .frequency = 1017600 },
	{ .frequency = 1132800 },
	{ .frequency = 1248000 },
	{ .frequency = 1344000 },
	{ .frequency = 1440000 },
	{ .frequency = 1459200 },
	{ .frequency = 1536000 },
	{ .frequency = 1574400 },
	{ .frequency = 1651200 },
	{ .frequency = 1689600 },
	{ .frequency = 1747200 },
	{ .frequency = 1804800 },
	{ .frequency = 1843200 },
	{ .frequency = 1920000 },
	{ .frequency = 1939200 },
	{ .frequency = 2035200 },
	{ .frequency = 2150400 },
	{ .frequency = 2265600 },	/* 2265.6 MHz — SM8650-AB A520 max */
	{ .frequency = CPUFREQ_TABLE_END },
};

/*
 * Policy 2: Cortex-A720 ×2 perf-lo (CPUs 2-3), 44 OPPs, max 2956.8 MHz.
 * This is the first 44 entries of the full A720 table.
 */
static struct cpufreq_frequency_table perf_lo_freqs[] = {
	{ .frequency =  460800 },
	{ .frequency =  499200 },
	{ .frequency =  576000 },
	{ .frequency =  614400 },
	{ .frequency =  691200 },
	{ .frequency =  729600 },
	{ .frequency =  806400 },
	{ .frequency =  844800 },
	{ .frequency =  902400 },
	{ .frequency =  960000 },
	{ .frequency = 1036800 },
	{ .frequency = 1075200 },
	{ .frequency = 1152000 },
	{ .frequency = 1190400 },
	{ .frequency = 1267200 },
	{ .frequency = 1286400 },
	{ .frequency = 1382400 },
	{ .frequency = 1401600 },
	{ .frequency = 1497600 },
	{ .frequency = 1612800 },
	{ .frequency = 1708800 },
	{ .frequency = 1728000 },
	{ .frequency = 1824000 },
	{ .frequency = 1843200 },
	{ .frequency = 1920000 },
	{ .frequency = 1958400 },
	{ .frequency = 2035200 },
	{ .frequency = 2073600 },
	{ .frequency = 2131200 },
	{ .frequency = 2188800 },
	{ .frequency = 2246400 },
	{ .frequency = 2304000 },
	{ .frequency = 2323200 },
	{ .frequency = 2380800 },
	{ .frequency = 2400000 },
	{ .frequency = 2438400 },
	{ .frequency = 2515200 },
	{ .frequency = 2572800 },
	{ .frequency = 2630400 },
	{ .frequency = 2707200 },
	{ .frequency = 2764800 },
	{ .frequency = 2841600 },
	{ .frequency = 2899200 },
	{ .frequency = 2956800 },	/* 2956.8 MHz — SM8650-AB A720-lo max */
	{ .frequency = CPUFREQ_TABLE_END },
};

/*
 * Policy 4: Cortex-A720 ×3 perf-hi (CPUs 4-6), 47 OPPs, max 3148.8 MHz.
 * Full A720 table — 3 extra steps beyond perf-lo.
 */
static struct cpufreq_frequency_table perf_hi_freqs[] = {
	{ .frequency =  460800 },
	{ .frequency =  499200 },
	{ .frequency =  576000 },
	{ .frequency =  614400 },
	{ .frequency =  691200 },
	{ .frequency =  729600 },
	{ .frequency =  806400 },
	{ .frequency =  844800 },
	{ .frequency =  902400 },
	{ .frequency =  960000 },
	{ .frequency = 1036800 },
	{ .frequency = 1075200 },
	{ .frequency = 1152000 },
	{ .frequency = 1190400 },
	{ .frequency = 1267200 },
	{ .frequency = 1286400 },
	{ .frequency = 1382400 },
	{ .frequency = 1401600 },
	{ .frequency = 1497600 },
	{ .frequency = 1612800 },
	{ .frequency = 1708800 },
	{ .frequency = 1728000 },
	{ .frequency = 1824000 },
	{ .frequency = 1843200 },
	{ .frequency = 1920000 },
	{ .frequency = 1958400 },
	{ .frequency = 2035200 },
	{ .frequency = 2073600 },
	{ .frequency = 2131200 },
	{ .frequency = 2188800 },
	{ .frequency = 2246400 },
	{ .frequency = 2304000 },
	{ .frequency = 2323200 },
	{ .frequency = 2380800 },
	{ .frequency = 2400000 },
	{ .frequency = 2438400 },
	{ .frequency = 2515200 },
	{ .frequency = 2572800 },
	{ .frequency = 2630400 },
	{ .frequency = 2707200 },
	{ .frequency = 2764800 },
	{ .frequency = 2841600 },
	{ .frequency = 2899200 },
	{ .frequency = 2956800 },
	{ .frequency = 3014400 },
	{ .frequency = 3072000 },
	{ .frequency = 3148800 },	/* 3148.8 MHz — SM8650-AB A720-hi max */
	{ .frequency = CPUFREQ_TABLE_END },
};

/*
 * Policy 7: Cortex-X4 ×1 prime (CPU 7), 49 OPPs, max 3302.4 MHz.
 * X4 has its own independent step sequence from 480 MHz.
 */
static struct cpufreq_frequency_table prime_freqs[] = {
	{ .frequency =  480000 },
	{ .frequency =  499200 },
	{ .frequency =  576000 },
	{ .frequency =  614400 },
	{ .frequency =  672000 },
	{ .frequency =  729600 },
	{ .frequency =  787200 },
	{ .frequency =  844800 },
	{ .frequency =  902400 },
	{ .frequency =  940800 },
	{ .frequency = 1017600 },
	{ .frequency = 1075200 },
	{ .frequency = 1132800 },
	{ .frequency = 1190400 },
	{ .frequency = 1248000 },
	{ .frequency = 1305600 },
	{ .frequency = 1363200 },
	{ .frequency = 1420800 },
	{ .frequency = 1478400 },
	{ .frequency = 1555200 },
	{ .frequency = 1593600 },
	{ .frequency = 1670400 },
	{ .frequency = 1708800 },
	{ .frequency = 1804800 },
	{ .frequency = 1824000 },
	{ .frequency = 1939200 },
	{ .frequency = 2035200 },
	{ .frequency = 2073600 },
	{ .frequency = 2112000 },
	{ .frequency = 2169600 },
	{ .frequency = 2208000 },
	{ .frequency = 2246400 },
	{ .frequency = 2304000 },
	{ .frequency = 2342400 },
	{ .frequency = 2380800 },
	{ .frequency = 2438400 },
	{ .frequency = 2457600 },
	{ .frequency = 2496000 },
	{ .frequency = 2553600 },
	{ .frequency = 2630400 },
	{ .frequency = 2688000 },
	{ .frequency = 2745600 },
	{ .frequency = 2803200 },
	{ .frequency = 2880000 },
	{ .frequency = 2937600 },
	{ .frequency = 2995200 },
	{ .frequency = 3052800 },
	{ .frequency = 3187200 },
	{ .frequency = 3302400 },	/* 3302.4 MHz — SM8650-AB X4 max */
	{ .frequency = CPUFREQ_TABLE_END },
};

/* Which frequency table each CPU uses.  Index by CPU number.
 * The governing CPU of each cluster is the one cpufreq calls init() on. */
static struct cpufreq_frequency_table *cpu_freq_table[8] = {
	[0] = effic_freqs,	/* policy0 — governing CPU */
	[1] = effic_freqs,
	[2] = perf_lo_freqs,	/* policy2 — governing CPU */
	[3] = perf_lo_freqs,
	[4] = perf_hi_freqs,	/* policy4 — governing CPU */
	[5] = perf_hi_freqs,
	[6] = perf_hi_freqs,
	[7] = prime_freqs,	/* policy7 — governing CPU */
};

static cpumask_t policy_cpus[8]; /* populated at module init */

static int fake_cpufreq_init(struct cpufreq_policy *policy)
{
	unsigned int cpu = policy->cpu;
	int i;

	if (cpu >= 8 || !cpu_freq_table[cpu])
		return -EINVAL;

	policy->freq_table = cpu_freq_table[cpu];
	policy->cpuinfo.min_freq = cpu_freq_table[cpu][0].frequency;

	/* Walk to the last entry before CPUFREQ_TABLE_END */
	for (i = 0; cpu_freq_table[cpu][i].frequency != CPUFREQ_TABLE_END; i++)
		;
	policy->cpuinfo.max_freq = cpu_freq_table[cpu][i > 0 ? i - 1 : 0].frequency;

	policy->cur = policy->cpuinfo.max_freq;
	policy->min = policy->cpuinfo.min_freq;
	policy->max = policy->cpuinfo.max_freq;

	cpumask_copy(policy->cpus, &policy_cpus[cpu]);

	pr_info("fake_sm8650_cpufreq: policy cpu%u init: %u – %u kHz, cpus %*pbl\n",
		cpu, policy->cpuinfo.min_freq, policy->cpuinfo.max_freq,
		cpumask_pr_args(policy->cpus));

	return 0;
}

static int fake_cpufreq_target_index(struct cpufreq_policy *policy,
				     unsigned int index)
{
	/* No hardware — just update cur. */
	policy->cur = policy->freq_table[index].frequency;
	return 0;
}

static unsigned int fake_cpufreq_get(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	return policy ? policy->cur : 0;
}

static struct cpufreq_driver fake_sm8650_driver = {
	.name           = "fake-sm8650",
	.flags          = CPUFREQ_NEED_INITIAL_FREQ_CHECK |
			  CPUFREQ_IS_COOLING_DEV,
	.init           = fake_cpufreq_init,
	.verify         = cpufreq_generic_frequency_table_verify,
	.target_index   = fake_cpufreq_target_index,
	.get            = fake_cpufreq_get,
	.attr           = cpufreq_generic_attr,
};

static int __init fake_sm8650_cpufreq_init(void)
{
	unsigned int nr_cpus = num_possible_cpus();

	if (nr_cpus < 2)
		pr_warn("fake_sm8650_cpufreq: need >= 2 CPUs for topology sim, got %u\n",
			nr_cpus);

	/*
	 * Build cluster cpumasks for SM8650 BSP topology.
	 * If QEMU has fewer than 8 CPUs the absent clusters register no policy.
	 */
	cpumask_clear(&policy_cpus[0]);
	cpumask_clear(&policy_cpus[2]);
	cpumask_clear(&policy_cpus[4]);
	cpumask_clear(&policy_cpus[7]);

#define ADD_IF_PRESENT(cpu, idx) \
	if ((cpu) < nr_cpus) cpumask_set_cpu((cpu), &policy_cpus[(idx)])

	ADD_IF_PRESENT(0, 0);
	ADD_IF_PRESENT(1, 0);
	ADD_IF_PRESENT(2, 2);
	ADD_IF_PRESENT(3, 2);
	ADD_IF_PRESENT(4, 4);
	ADD_IF_PRESENT(5, 4);
	ADD_IF_PRESENT(6, 4);
	ADD_IF_PRESENT(7, 7);

#undef ADD_IF_PRESENT

	/* Alias non-governing CPUs to their governing CPU's mask */
	policy_cpus[1] = policy_cpus[0];
	policy_cpus[3] = policy_cpus[2];
	policy_cpus[5] = policy_cpus[4];
	policy_cpus[6] = policy_cpus[4];

	return cpufreq_register_driver(&fake_sm8650_driver);
}

static void __exit fake_sm8650_cpufreq_exit(void)
{
	cpufreq_unregister_driver(&fake_sm8650_driver);
}

module_init(fake_sm8650_cpufreq_init);
module_exit(fake_sm8650_cpufreq_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("SM8650 (Pineapple) cpufreq topology for QEMU cinema_mode testing");
MODULE_AUTHOR("Xiaomi Cinema Kernel Project");
