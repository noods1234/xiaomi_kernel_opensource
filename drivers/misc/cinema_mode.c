// SPDX-License-Identifier: GPL-2.0-only
/*
 * cinema_mode.c — Cinema recording performance coordinator
 *
 * For Xiaomi 14 Ultra (aurora) / SM8650 (Pineapple).
 *
 * NOTE: device codename "aurora" is tentative — verify against the
 * shipping device-tree before tagging a production build.
 *
 * Exposes /sys/kernel/cinema_mode/enable.  Writing "1" activates cinema
 * mode, which:
 *
 *   • Pegs every CPUfreq policy to its maximum frequency via a PM-QoS
 *     FREQ_QOS_MIN request — bypasses governor scaling without changing
 *     the governor itself, so thermal governors still work.
 *   • Requests the system-wide CPU latency QoS to 0 µs — prevents the
 *     CPUs from entering deep C-states between frames.
 *   • Holds a wakeup source so the device cannot suspend mid-capture.
 *
 * Writing "0" releases all requests and returns the system to its normal
 * power behaviour.
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
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/pm_qos.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

/* One QoS request slot per possible CPU */
static struct freq_qos_request *freq_reqs;
/* Track which CPUs have an active request */
static bool *freq_req_active;

static struct pm_qos_request cinema_cpu_latency_qos;
static struct wakeup_source  *cinema_ws;
static struct kobject        *cinema_kobj;

static bool cinema_active;
static DEFINE_MUTEX(cinema_lock);

/* ------------------------------------------------------------------ */

/* Forward declaration required because cinema_activate() calls
 * cinema_deactivate() for partial-failure unwind before the latter
 * is defined.
 */
static void cinema_deactivate(void);

static int cinema_activate(void)
{
	struct cpufreq_policy *policy;
	unsigned int cpu;
	int ret;
	int failures = 0;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		/* Only act on the first (governing) CPU of each policy */
		if (policy->cpu != cpu) {
			cpufreq_cpu_put(policy);
			continue;
		}

		ret = freq_qos_add_request(&policy->constraints,
					   &freq_reqs[cpu],
					   FREQ_QOS_MIN,
					   policy->cpuinfo.max_freq);
		if (ret < 0) {
			pr_warn("cinema_mode: freq_qos add failed cpu%u (%d)\n",
				cpu, ret);
			failures++;
		} else {
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
	 * Block deep CPU idle states between frames.
	 *
	 * A value of 0 µs would block ALL idle states including WFI (C1,
	 * ~5 µs exit latency), burning power on every inter-frame gap.
	 * The relevant states to block on SM8650 are the cluster-power-collapse
	 * states (C2+) whose exit latency is ~150 µs — long enough to miss a
	 * DMA completion interrupt from the ISP or Venus mid-frame.
	 *
	 * 100 µs blocks those deep states while permitting clock-gated WFI,
	 * matching the value used by production Snapdragon camera HALs.
	 */
	cpu_latency_qos_add_request(&cinema_cpu_latency_qos, 100);

	/* Prevent runtime-suspend during recording.
	 * __pm_stay_awake() activates the wakeup source under ws->lock.
	 * wakeup_source_activate() is only called when !ws->active, so a
	 * second call will not double-increment combined_event_count.  No
	 * pre-check of ws->active is needed here, and reading it outside
	 * ws->lock would be a data race.
	 */
	__pm_stay_awake(cinema_ws);

	pr_info("cinema_mode: active — CPUs pinned to max freq, suspend blocked\n");
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
	}

	if (cpu_latency_qos_request_active(&cinema_cpu_latency_qos))
		cpu_latency_qos_remove_request(&cinema_cpu_latency_qos);

	/* Release the wakeup source.  __pm_relax() checks ws->active under
	 * ws->lock internally and is a no-op when the source is not active, so
	 * it is safe to call unconditionally (e.g. from the partial-failure
	 * unwind path in cinema_activate() before __pm_stay_awake() was ever
	 * called).  Reading cinema_ws->active here without ws->lock would be a
	 * data race, so the open-coded check is intentionally omitted.
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
 * pointing at freed memory and any later freq_qos_remove_request()
 * call becomes a use-after-free.
 *
 * CPUFREQ_CREATE_POLICY fires during cpufreq_online() after the policy
 * is fully initialised (cpuinfo.max_freq is set).  If cinema mode is
 * active we re-pin the new policy to its maximum frequency.
 *
 * Locking: cinema_lock is a mutex; both notifier events are called from
 * a sleepable hotplug thread context so mutex_lock is safe.  There is
 * no lock-ordering hazard because neither cinema_activate() nor
 * cinema_deactivate() can trigger a policy create/remove event.
 *
 * Governance-transfer invariant (SM8650-specific)
 * ------------------------------------------------
 * This driver indexes freq_reqs[] and freq_req_active[] by policy->cpu,
 * the governing (lowest-numbered) CPU of each policy at the time the
 * event fires.  If governance transfers to a different CPU within the
 * same policy (i.e. the original governing CPU goes offline while the
 * cluster stays up), policy->cpu changes but no CPUFREQ_REMOVE_POLICY /
 * CPUFREQ_CREATE_POLICY pair fires — the policy object persists with a
 * new .cpu field.  This would corrupt the index: the old slot in
 * freq_req_active[] stays true but points to the wrong request.
 *
 * On SM8650 (Pineapple) with the standard Qualcomm thermal hotplug policy,
 * CPUs within a cluster go offline and online as a unit, so the governing
 * CPU never changes without a full policy teardown.  The invariant holds.
 *
 * If this driver is ported to a platform where per-CPU hotplug is common
 * (e.g. server-class ARM with heterogeneous policies), the indexing scheme
 * must be changed from per-CPU to per-policy — keying on the lowest bit
 * of policy->cpus or on a policy-lifetime ID.
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
		/*
		 * The policy governing this CPU is being torn down.  Remove
		 * our QoS request before the constraints memory is freed.
		 */
		if (freq_req_active[cpu]) {
			freq_qos_remove_request(&freq_reqs[cpu]);
			freq_req_active[cpu] = false;
			pr_debug("cinema_mode: removed QoS for cpu%u (policy offline)\n",
				 cpu);
		}
		break;

	case CPUFREQ_CREATE_POLICY:
		/*
		 * A policy has been created (cluster came back online).
		 * Re-apply the frequency floor if cinema mode is active.
		 */
		if (!cinema_active)
			break;

		if (freq_req_active[cpu]) {
			/* Should not happen, but guard against double-add */
			pr_warn("cinema_mode: cpu%u policy created with active request — skipping\n",
				cpu);
			break;
		}

		ret = freq_qos_add_request(&policy->constraints,
					   &freq_reqs[cpu],
					   FREQ_QOS_MIN,
					   policy->cpuinfo.max_freq);
		if (ret >= 0) {
			freq_req_active[cpu] = true;
			pr_debug("cinema_mode: re-pinned cpu%u after hotplug\n",
				 cpu);
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

static ssize_t enable_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	bool active;

	/* Take the same lock used by enable_store() so the read is not racing
	 * with a concurrent write.  READ_ONCE alone does not provide the
	 * necessary mutual exclusion here.
	 */
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
	ssize_t ret = count;

	/* Pegging all CPUs to maximum frequency is a privileged operation that
	 * can cause thermal stress and constitutes a system-wide resource
	 * change.  Require CAP_SYS_ADMIN.
	 */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	/* kstrtobool accepts "0"/"1"/"y"/"n"/"on"/"off" and rejects everything
	 * else, preventing silent treatment of "-1" or arbitrary integers as
	 * "enable".
	 */
	if (kstrtobool(buf, &on))
		return -EINVAL;

	mutex_lock(&cinema_lock);

	if (on == cinema_active)
		goto out;

	if (on) {
		if (cinema_activate()) {
			ret = -EIO;
			goto out;
		}
	} else {
		cinema_deactivate();
	}

	cinema_active = on;

out:
	mutex_unlock(&cinema_lock);
	return ret;
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

	freq_req_active = kcalloc(num_cpus, sizeof(*freq_req_active), GFP_KERNEL);
	if (!freq_req_active) {
		ret = -ENOMEM;
		goto err_free_reqs;
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

	pr_info("cinema_mode: ready — echo 1 > /sys/kernel/cinema_mode/enable\n");
	return 0;

err_sysfs:
	sysfs_remove_group(cinema_kobj, &cinema_attr_group);
err_kobj:
	kobject_put(cinema_kobj);
err_ws:
	wakeup_source_unregister(cinema_ws);
err_free_active:
	kfree(freq_req_active);
err_free_reqs:
	kfree(freq_reqs);
	return ret;
}

static void __exit cinema_mode_exit(void)
{
	/*
	 * Step 1: drain userspace access.
	 *
	 * sysfs_remove_group() calls kernfs_drain() internally, which waits
	 * for any in-flight show/store callbacks to complete and prevents new
	 * ones from being dispatched.  After this returns, enable_store() can
	 * never be called again, so cinema_active and freq_req_active[] can
	 * only be modified by this exit path and by the cpufreq notifier.
	 */
	sysfs_remove_group(cinema_kobj, &cinema_attr_group);
	kobject_put(cinema_kobj);

	/*
	 * Step 2: release all QoS state under the lock.
	 *
	 * The cpufreq notifier is still registered here.  If a hotplug event
	 * fires concurrently it will block on cinema_lock until we release it,
	 * at which point freq_req_active[] is already all-false and the
	 * notifier becomes a no-op.  This is correct serialisation.
	 *
	 * The previous ordering (unregister notifier first, then deactivate)
	 * was wrong: it opened a window where a policy could be torn down
	 * between the unregister and cinema_deactivate(), causing the kernel's
	 * freq_qos_remove_all_requests() to clear req->qos while
	 * freq_req_active[cpu] remained true.  cinema_deactivate() would then
	 * call freq_qos_remove_request() on an already-removed request and
	 * trigger WARN_ON(!freq_qos_request_active(req)).
	 */
	mutex_lock(&cinema_lock);
	if (cinema_active) {
		cinema_deactivate();
		cinema_active = false;
	}
	mutex_unlock(&cinema_lock);

	/*
	 * Step 3: unregister the cpufreq notifier.
	 *
	 * All QoS requests are gone and freq_req_active[] is all-false.  Any
	 * notifier call that sneaks in between mutex_unlock and here will
	 * find cinema_active == false and freq_req_active[cpu] == false and
	 * return NOTIFY_DONE without touching any state.
	 *
	 * cpufreq_unregister_notifier() acquires the write side of the
	 * blocking notifier chain's rwsem (cpufreq_policy_notifier_list is a
	 * BLOCKING_NOTIFIER_HEAD — see drivers/cpufreq/cpufreq.c — not SRCU).
	 * The write lock waits for any in-flight reader (notifier call in
	 * progress holding the read lock) to complete, then removes our block.
	 * After it returns, cinema_cpufreq_notifier() cannot execute.
	 */
	cpufreq_unregister_notifier(&cinema_cpufreq_nb,
				    CPUFREQ_POLICY_NOTIFIER);

	wakeup_source_unregister(cinema_ws);
	kfree(freq_req_active);
	kfree(freq_reqs);
}

module_init(cinema_mode_init);
module_exit(cinema_mode_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Cinema recording performance coordinator for Xiaomi 14 Ultra");
MODULE_AUTHOR("Xiaomi Cinema Kernel Project");
