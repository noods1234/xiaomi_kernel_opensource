// SPDX-License-Identifier: GPL-2.0-only
/*
 * fake_sm8650_cpufreq.c — Simulated SM8650 cpufreq topology for QEMU/virt
 *
 * Registers four cpufreq policies that mirror the Snapdragon 8 Gen 3
 * (SM8650 / Pineapple) cluster layout:
 *
 *   Policy 0  CPUs 0-1   A520 efficiency  300 – 2016 MHz
 *   Policy 2  CPUs 2-3   A720 perf-lo     300 – 2784 MHz
 *   Policy 4  CPUs 4-6   A720 perf-hi     300 – 2784 MHz
 *   Policy 7  CPU  7     X4 prime         300 – 3187 MHz
 *
 * No real frequency switching happens.  The driver exists solely to
 * present valid struct cpufreq_policy objects so that cinema_mode.c can
 * exercise its freq_qos_add/remove_request paths under QEMU.
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

/* SM8650 OPP tables (simplified — real table has more steps) */

static struct cpufreq_frequency_table effic_freqs[] = {
	{ .frequency =  300000 },
	{ .frequency =  576000 },
	{ .frequency =  768000 },
	{ .frequency =  1017600 },
	{ .frequency =  1344000 },
	{ .frequency =  1612800 },
	{ .frequency =  1804800 },
	{ .frequency =  2016000 },
	{ .frequency = CPUFREQ_TABLE_END },
};

static struct cpufreq_frequency_table perf_lo_freqs[] = {
	{ .frequency =  300000 },
	{ .frequency =  768000 },
	{ .frequency =  1075200 },
	{ .frequency =  1459200 },
	{ .frequency =  1843200 },
	{ .frequency =  2188800 },
	{ .frequency =  2496000 },
	{ .frequency =  2784000 },
	{ .frequency = CPUFREQ_TABLE_END },
};

static struct cpufreq_frequency_table perf_hi_freqs[] = {
	{ .frequency =  300000 },
	{ .frequency =  768000 },
	{ .frequency =  1075200 },
	{ .frequency =  1459200 },
	{ .frequency =  1843200 },
	{ .frequency =  2188800 },
	{ .frequency =  2496000 },
	{ .frequency =  2784000 },
	{ .frequency = CPUFREQ_TABLE_END },
};

static struct cpufreq_frequency_table prime_freqs[] = {
	{ .frequency =  300000 },
	{ .frequency =  768000 },
	{ .frequency =  1190400 },
	{ .frequency =  1612800 },
	{ .frequency =  2073600 },
	{ .frequency =  2457600 },
	{ .frequency =  2803200 },
	{ .frequency =  3187200 },
	{ .frequency = CPUFREQ_TABLE_END },
};

/* Which frequency table does each CPU use?
 * Index by CPU number; governing CPU of each cluster determines the policy. */
static struct cpufreq_frequency_table *cpu_freq_table[8] = {
	[0] = effic_freqs,    /* policy 0 — governing CPU */
	[1] = effic_freqs,
	[2] = perf_lo_freqs,  /* policy 2 — governing CPU */
	[3] = perf_lo_freqs,
	[4] = perf_hi_freqs,  /* policy 4 — governing CPU */
	[5] = perf_hi_freqs,
	[6] = perf_hi_freqs,
	[7] = prime_freqs,    /* policy 7 — governing CPU */
};

/* Cluster membership: which CPUs share a policy with cpu[i]? */
static const cpumask_t cluster_masks[8] = {
	[0] = { CPU_BITS_CPU0 },            /* will be filled in init */
};

static cpumask_t policy_cpus[8]; /* populated at init */

static int fake_cpufreq_init(struct cpufreq_policy *policy)
{
	unsigned int cpu = policy->cpu;

	if (cpu >= 8 || !cpu_freq_table[cpu])
		return -EINVAL;

	policy->freq_table = cpu_freq_table[cpu];
	policy->cpuinfo.min_freq = cpu_freq_table[cpu][0].frequency;

	/* Find max: last entry before CPUFREQ_TABLE_END */
	{
		int i;
		for (i = 0; cpu_freq_table[cpu][i].frequency != CPUFREQ_TABLE_END; i++)
			;
		policy->cpuinfo.max_freq = cpu_freq_table[cpu][i > 0 ? i-1 : 0].frequency;
	}

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
	/* No hardware to touch — update cur and return. */
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

	if (nr_cpus < 2) {
		pr_warn("fake_sm8650_cpufreq: need >= 2 CPUs for topology sim, got %u\n",
			nr_cpus);
	}

	/*
	 * Build cluster masks matching SM8650 topology.
	 * If QEMU is started with fewer than 8 CPUs, the higher clusters
	 * simply have no members (their governing CPUs are offline).
	 */
	cpumask_clear(&policy_cpus[0]);
	cpumask_clear(&policy_cpus[2]);
	cpumask_clear(&policy_cpus[4]);
	cpumask_clear(&policy_cpus[7]);

#define ADD_IF_PRESENT(cpu, policy_idx) \
	if (cpu < nr_cpus) cpumask_set_cpu(cpu, &policy_cpus[policy_idx])

	ADD_IF_PRESENT(0, 0);
	ADD_IF_PRESENT(1, 0);
	ADD_IF_PRESENT(2, 2);
	ADD_IF_PRESENT(3, 2);
	ADD_IF_PRESENT(4, 4);
	ADD_IF_PRESENT(5, 4);
	ADD_IF_PRESENT(6, 4);
	ADD_IF_PRESENT(7, 7);
#undef ADD_IF_PRESENT

	/*
	 * Point non-governing CPUs at their governing CPU's mask so that
	 * fake_cpufreq_init() sets policy->cpus correctly regardless of
	 * which CPU the core calls init() on.
	 */
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
MODULE_DESCRIPTION("Simulated SM8650 cpufreq topology for QEMU cinema_mode testing");
MODULE_AUTHOR("Xiaomi Cinema Kernel Project");
