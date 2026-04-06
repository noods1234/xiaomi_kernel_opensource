// SPDX-License-Identifier: GPL-2.0-only
/*
 * cinema_mode.c — Cinema recording performance coordinator
 *
 * For Xiaomi 14 Ultra (aurora) / SM8650 (Pineapple).
 *
 * Exposes /sys/kernel/cinema_mode/enable.  Writing "1" activates cinema
 * mode, which:
 *
 *   • Raises every CPUfreq policy's minimum frequency to a recording floor
 *     computed as (cpuinfo.max_freq * floor_pct / 100).  The floor keeps
 *     all clusters fast enough for concurrent ISP/codec/display pipelines
 *     while leaving EAS free to select any OPP at or above the floor.
 *     Pinning to 100% of max_freq would destroy energy-aware scheduling;
 *     the default floor_pct of 85 retains the upper OPP band for EAS.
 *
 *   • Requests the system-wide CPU latency QoS to 100 µs — prevents
 *     cluster-power-collapse idle states (C2+, ~150 µs exit latency) that
 *     can stall DMA completion from the ISP or Venus mid-frame, while
 *     permitting clock-gated WFI (~5 µs) between frames.
 *
 *   • Holds a wakeup source so the device cannot suspend mid-capture.
 *
 * Writing "0" releases all requests and returns the system to its normal
 * power behaviour.
 *
 * Module parameter
 * ----------------
 * floor_pct (int, default 85):  percentage of each policy's max_freq used
 * as the FREQ_QOS_MIN recording floor.  Valid range 50–100.  Values below
 * 50 are rejected at activation time; 100 pegs every cluster to its
 * ceiling (equivalent to the original behaviour, but kills EAS).
 *
 * CPU hotplug handling
 * --------------------
 * When an entire cpufreq policy cluster goes offline its policy object is
 * destroyed, invalidating any freq_qos_request embedded in its constraints
 * list.  A cpufreq policy notifier removes our request before that happens
 * (CPUFREQ_REMOVE_POLICY) and re-pins the newly created policy when the
 * cluster comes back online (CPUFREQ_CREATE_POLICY), so the frequency floor
 * is maintained across thermal-driven hotplug events.
 *
 * Inter-module API
 * ----------------
 * cinema_mode_set_active(bool on) is exported for use by cinema_end.c.
 * Both modules share the same cinema_lock; cinema_end must not call this
 * from a context that already holds the lock.
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/pm_qos.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

/* ------------------------------------------------------------------ */
/* Module parameters                                                    */
/* ------------------------------------------------------------------ */

static int floor_pct = 85;
module_param(floor_pct, int, 0644);
MODULE_PARM_DESC(floor_pct,
	"Recording floor as %% of each policy's max_freq (50-100, default 85)");

/* ------------------------------------------------------------------ */
/* Per-CPU state                                                        */
/* ------------------------------------------------------------------ */

/* One QoS request slot per possible CPU */
static struct freq_qos_request *freq_reqs;

/*
 * freq_floor_hz[cpu]: the floor value (kHz) installed for each governing
 * CPU's policy.  Stored at activation time so the cpufreq notifier can
 * re-apply the same floor after a hotplug re-create without recomputing
 * from floor_pct.  This keeps the floor consistent even if floor_pct is
 * changed via sysctl between the original activation and a re-pin event.
 */
static unsigned int *freq_floor_hz;

/* Track which CPUs have an active request */
static bool *freq_req_active;

/* ------------------------------------------------------------------ */
/* Module-level state                                                   */
/* ------------------------------------------------------------------ */

static struct pm_qos_request cinema_cpu_latency_qos;
static struct wakeup_source  *cinema_ws;
static struct kobject        *cinema_kobj;

static bool cinema_active;
static DEFINE_MUTEX(cinema_lock);

/* ------------------------------------------------------------------ */

/*
 * cinema_snap_to_opp - round target_khz up to the nearest valid OPP.
 *
 * FREQ_QOS_MIN values that land between two OPP steps force the DVFS
 * to select the higher step on every governor decision, wasting a small
 * amount of energy continuously.  Snapping to an exact OPP boundary
 * eliminates that overhead.
 *
 * Iterates policy->freq_table (set by the platform cpufreq driver) for
 * the lowest entry >= target_khz.  Falls back to target_khz itself if
 * the table is absent (e.g. FAKE_SM8650_CPUFREQ under QEMU) or if no
 * entry is >= target_khz (impossible when target_khz <= max_freq, but
 * guarded defensively).
 *
 * Must be called with a policy reference held (cpufreq_cpu_get).
 */
static unsigned int cinema_snap_to_opp(struct cpufreq_policy *policy,
					unsigned int target_khz)
{
	struct cpufreq_frequency_table *pos;
	unsigned int best = policy->cpuinfo.max_freq;

	if (!policy->freq_table)
		return target_khz;

	cpufreq_for_each_valid_entry(pos, policy->freq_table) {
		if (pos->frequency >= target_khz && pos->frequency < best)
			best = pos->frequency;
	}

	/* If no entry >= target_khz was found, best still equals max_freq */
	return best;
}

/* Forward declaration required because cinema_activate() calls
 * cinema_deactivate() for partial-failure unwind before the latter
 * is defined.
 */
static void cinema_deactivate(void);

static int cinema_activate(void)
{
	struct cpufreq_policy *policy;
	unsigned int cpu;
	unsigned int floor;
	int ret;
	int failures = 0;

	if (floor_pct < 50 || floor_pct > 100) {
		pr_err("cinema_mode: floor_pct=%d out of range [50,100]\n",
		       floor_pct);
		return -EINVAL;
	}

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		/* Only act on the first (governing) CPU of each policy */
		if (policy->cpu != cpu) {
			cpufreq_cpu_put(policy);
			continue;
		}

		/*
		 * Compute the recording floor for this policy.
		 * floor_pct < 100 preserves the upper OPP band for EAS;
		 * the governor may still select higher OPPs based on demand.
		 * Do-it-in-kernel-integer: no fp, overflow-safe for u32 kHz.
		 *
		 * Snap to the nearest OPP at or above the computed floor so
		 * the FREQ_QOS_MIN value exactly matches a step the DVFS knows
		 * about.  A mid-step floor forces a round-up on every governor
		 * tick; an OPP-aligned floor costs nothing extra.
		 */
		floor = (unsigned int)(
			(u64)policy->cpuinfo.max_freq * floor_pct / 100);
		floor = cinema_snap_to_opp(policy, floor);

		ret = freq_qos_add_request(&policy->constraints,
					   &freq_reqs[cpu],
					   FREQ_QOS_MIN,
					   (s32)floor);
		if (ret < 0) {
			pr_warn("cinema_mode: freq_qos add failed cpu%u (%d)\n",
				cpu, ret);
			failures++;
		} else {
			freq_floor_hz[cpu]  = floor;
			freq_req_active[cpu] = true;
		}

		cpufreq_cpu_put(policy);
	}

	if (failures) {
		/* Unwind any partial requests before returning error */
		cinema_deactivate();
		return -EIO;
	}

	/*
	 * Block cluster-power-collapse idle states (C2+, ~150 µs exit latency)
	 * while permitting clock-gated WFI (~5 µs) between frames.
	 * 100 µs matches the value used by production Snapdragon camera HALs.
	 */
	cpu_latency_qos_add_request(&cinema_cpu_latency_qos, 100);

	/*
	 * Prevent runtime-suspend during recording.
	 * __pm_stay_awake() is safe to call unconditionally; it activates
	 * the wakeup source under ws->lock and is idempotent when called
	 * multiple times.
	 */
	__pm_stay_awake(cinema_ws);

	pr_info("cinema_mode: active — CPUs floored at %d%% of max, suspend blocked\n",
		floor_pct);
	return 0;
}

static void cinema_deactivate(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		if (!freq_req_active[cpu])
			continue;
		freq_qos_remove_request(&freq_reqs[cpu]);
		freq_req_active[cpu] = false;
		freq_floor_hz[cpu]   = 0;
	}

	if (cpu_latency_qos_request_active(&cinema_cpu_latency_qos))
		cpu_latency_qos_remove_request(&cinema_cpu_latency_qos);

	/*
	 * __pm_relax() checks ws->active internally and is a no-op when the
	 * source is not active, so it is safe to call from the partial-failure
	 * unwind path before __pm_stay_awake() was ever called.
	 */
	__pm_relax(cinema_ws);

	pr_info("cinema_mode: inactive — CPU scaling and suspend restored\n");
}

/* ------------------------------------------------------------------ */
/* CPU hotplug notifier                                                  */
/* ------------------------------------------------------------------ */

/*
 * cinema_cpufreq_notifier - maintain QoS floor across policy hotplug.
 *
 * CPUFREQ_REMOVE_POLICY fires inside cpufreq_policy_free(), before
 * kfree(policy), while policy->constraints are still valid.  We must
 * remove our request here; otherwise freq_reqs[cpu].qos is left
 * pointing at freed memory.
 *
 * CPUFREQ_CREATE_POLICY fires during cpufreq_online() after the policy
 * is fully initialised.  If cinema mode is active we re-pin the new
 * policy using the floor stored in freq_floor_hz[cpu].
 *
 * Locking: both notifier events are called from a sleepable hotplug
 * thread, so mutex_lock is safe.
 *
 * Governance-transfer note (SM8650-specific):
 * On SM8650 with the standard Qualcomm thermal hotplug policy, CPUs within
 * a cluster go offline and online as a unit, so policy->cpu never changes
 * without a full REMOVE/CREATE pair.  If ported to a platform with
 * per-CPU hotplug, the indexing must change from per-CPU to per-policy.
 */
static int cinema_cpufreq_notifier(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu;
	int ret;

	cpu = policy->cpu;
	if (cpu >= nr_cpu_ids)
		return NOTIFY_DONE;

	mutex_lock(&cinema_lock);

	switch (event) {
	case CPUFREQ_REMOVE_POLICY:
		if (freq_req_active[cpu]) {
			freq_qos_remove_request(&freq_reqs[cpu]);
			freq_req_active[cpu] = false;
			freq_floor_hz[cpu]   = 0;
			pr_debug("cinema_mode: removed QoS for cpu%u (policy offline)\n",
				 cpu);
		}
		break;

	case CPUFREQ_CREATE_POLICY:
		if (!cinema_active)
			break;

		if (freq_req_active[cpu]) {
			pr_warn("cinema_mode: cpu%u policy created with active request — skipping\n",
				cpu);
			break;
		}

		/*
		 * Use the floor stored at activation time.  If for some reason
		 * freq_floor_hz[cpu] is zero (should not happen if the module
		 * is correct), recompute from the new policy's max_freq.
		 */
		if (!freq_floor_hz[cpu]) {
			freq_floor_hz[cpu] = (unsigned int)(
				(u64)policy->cpuinfo.max_freq * floor_pct / 100);
		}

		ret = freq_qos_add_request(&policy->constraints,
					   &freq_reqs[cpu],
					   FREQ_QOS_MIN,
					   (s32)freq_floor_hz[cpu]);
		if (ret >= 0) {
			freq_req_active[cpu] = true;
			pr_debug("cinema_mode: re-pinned cpu%u after hotplug (floor %u kHz)\n",
				 cpu, freq_floor_hz[cpu]);
		} else {
			pr_warn("cinema_mode: re-pin failed for cpu%u after hotplug (%d)\n",
				cpu, ret);
		}
		break;

	default:
		break;
	}

	mutex_unlock(&cinema_lock);
	return NOTIFY_DONE;
}

static struct notifier_block cinema_cpufreq_nb = {
	.notifier_call = cinema_cpufreq_notifier,
};

/* ------------------------------------------------------------------ */
/* Exported inter-module API                                            */
/* ------------------------------------------------------------------ */

/**
 * cinema_mode_set_active - activate or deactivate cinema performance mode.
 * @on: true to activate, false to deactivate.
 *
 * May be called by cinema_end.c to couple eND enable with the performance
 * coordinator.  Must not be called with cinema_lock held.
 *
 * Returns 0 on success, negative errno on failure.
 */
int cinema_mode_set_active(bool on)
{
	int ret = 0;

	mutex_lock(&cinema_lock);

	if (on == cinema_active)
		goto out;

	if (on) {
		ret = cinema_activate();
		if (ret)
			goto out;
	} else {
		cinema_deactivate();
	}

	cinema_active = on;

out:
	mutex_unlock(&cinema_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(cinema_mode_set_active);

/* ------------------------------------------------------------------ */
/* Sysfs interface                                                       */
/* ------------------------------------------------------------------ */

static ssize_t enable_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	bool active;

	mutex_lock(&cinema_lock);
	active = cinema_active;
	mutex_unlock(&cinema_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", active ? 1 : 0);
}

static ssize_t enable_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	bool on;
	int ret;

	/*
	 * Pegging all CPUs to a high frequency floor is a privileged operation
	 * that can cause thermal stress.  Require CAP_SYS_ADMIN.
	 */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtobool(buf, &on))
		return -EINVAL;

	ret = cinema_mode_set_active(on);
	return ret ? ret : count;
}

static struct kobj_attribute enable_attr =
	__ATTR(enable, 0600, enable_show, enable_store);

static struct attribute *cinema_attrs[] = {
	&enable_attr.attr,
	NULL,
};

static const struct attribute_group cinema_attr_group = {
	.attrs = cinema_attrs,
};

/* ------------------------------------------------------------------ */

static int __init cinema_mode_init(void)
{
	int ret;
	unsigned int num_cpus = nr_cpu_ids;

	freq_reqs = kcalloc(num_cpus, sizeof(*freq_reqs), GFP_KERNEL);
	if (!freq_reqs)
		return -ENOMEM;

	freq_floor_hz = kcalloc(num_cpus, sizeof(*freq_floor_hz), GFP_KERNEL);
	if (!freq_floor_hz) {
		ret = -ENOMEM;
		goto err_free_reqs;
	}

	freq_req_active = kcalloc(num_cpus, sizeof(*freq_req_active), GFP_KERNEL);
	if (!freq_req_active) {
		ret = -ENOMEM;
		goto err_free_floor;
	}

	cinema_ws = wakeup_source_register(NULL, "cinema_mode");
	if (!cinema_ws) {
		ret = -ENOMEM;
		goto err_free_active;
	}

	cinema_kobj = kobject_create_and_add("cinema_mode", kernel_kobj);
	if (!cinema_kobj) {
		ret = -ENOMEM;
		goto err_ws;
	}

	ret = sysfs_create_group(cinema_kobj, &cinema_attr_group);
	if (ret)
		goto err_kobj;

	/*
	 * Register the cpufreq policy notifier last — after all other
	 * state is set up — so the handler can safely access freq_reqs[]
	 * and freq_req_active[] from the moment it is registered.
	 */
	ret = cpufreq_register_notifier(&cinema_cpufreq_nb,
					CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("cinema_mode: failed to register cpufreq notifier (%d)\n",
		       ret);
		goto err_sysfs;
	}

	pr_info("cinema_mode: ready (floor_pct=%d%%) — echo 1 > /sys/kernel/cinema_mode/enable\n",
		floor_pct);
	return 0;

err_sysfs:
	sysfs_remove_group(cinema_kobj, &cinema_attr_group);
err_kobj:
	kobject_put(cinema_kobj);
err_ws:
	wakeup_source_unregister(cinema_ws);
err_free_active:
	kfree(freq_req_active);
err_free_floor:
	kfree(freq_floor_hz);
err_free_reqs:
	kfree(freq_reqs);
	return ret;
}

static void __exit cinema_mode_exit(void)
{
	/*
	 * Step 1: drain userspace access.
	 * sysfs_remove_group() waits for in-flight show/store to complete and
	 * prevents new ones from being dispatched.
	 */
	sysfs_remove_group(cinema_kobj, &cinema_attr_group);
	kobject_put(cinema_kobj);

	/*
	 * Step 2: release all QoS state under the lock.
	 * The notifier is still registered here.  If a hotplug event fires
	 * concurrently it blocks on cinema_lock; by the time it acquires the
	 * lock freq_req_active[] is all-false and the handler is a no-op.
	 */
	mutex_lock(&cinema_lock);
	if (cinema_active) {
		cinema_deactivate();
		cinema_active = false;
	}
	mutex_unlock(&cinema_lock);

	/*
	 * Step 3: unregister the cpufreq notifier.
	 * All QoS requests are gone and freq_req_active[] is all-false.
	 * cpufreq_unregister_notifier() acquires the write side of the
	 * blocking notifier chain's rwsem and waits for any in-flight call to
	 * complete before removing our block.
	 */
	cpufreq_unregister_notifier(&cinema_cpufreq_nb,
				    CPUFREQ_POLICY_NOTIFIER);

	wakeup_source_unregister(cinema_ws);
	kfree(freq_req_active);
	kfree(freq_floor_hz);
	kfree(freq_reqs);
}

module_init(cinema_mode_init);
module_exit(cinema_mode_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Cinema recording performance coordinator for Xiaomi 14 Ultra");
MODULE_AUTHOR("Xiaomi Cinema Kernel Project");
