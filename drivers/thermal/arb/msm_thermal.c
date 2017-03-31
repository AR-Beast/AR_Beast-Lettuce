/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/thermal.h>
#include <linux/regulator/rpm-smd-regulator.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/msm_thermal_ioctl.h>
#include <soc/qcom/rpm-smd.h>
#include <soc/qcom/scm.h>
#include <linux/sched/rt.h>

#define CREATE_TRACE_POINTS
#define TRACE_MSM_THERMAL
#include <trace/trace_thermal.h>

#define MAX_CURRENT_UA 100000
#define MAX_RAILS 5
#define MAX_THRESHOLD 2
#define MONITOR_ALL_TSENS -1
#define TSENS_NAME_MAX 20
#define TSENS_NAME_FORMAT "tsens_tz_sensor%d"
#define THERM_SECURE_BITE_CMD 8
#define SENSOR_SCALING_FACTOR 1
#define CPU_DEVICE "cpu%d"

/* Throttle CPU when it reaches a certain temp*/
unsigned int temp_threshold = 60;
module_param(temp_threshold, int, 0644);

static struct thermal_info {
	uint32_t cpuinfo_max_freq;
	uint32_t limited_max_freq;
	unsigned int safe_diff;
	bool throttling;
	bool pending_change;
} info = {
	.cpuinfo_max_freq = LONG_MAX,
	.limited_max_freq = LONG_MAX,
	.safe_diff = 5,
	.throttling = false,
	.pending_change = false,
};

/* throttle points in MHz */
enum thermal_freqs {
	FREQ_SHIT        = 200000,
	FREQ_HELL		 = 600000,
	FREQ_VERY_HOT		 = 800000,
	FREQ_HOT		 = 1000000,
	FREQ_WARM		 = 1200000,
};

enum threshold_levels {
	LEVEL_SHIT      = 20,
	LEVEL_HELL		= 15,
	LEVEL_VERY_HOT		= 10,
	LEVEL_HOT		= 5,
};

static struct msm_thermal_data msm_thermal_info;
static struct delayed_work check_temp_work;
static struct workqueue_struct *thermal_wq;

static int msm_thermal_cpufreq_callback(struct notifier_block *nfb,
		unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;

	if (event != CPUFREQ_ADJUST && !info.pending_change)
		return 0;

	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
		info.limited_max_freq);

	return 0;
}

static struct notifier_block msm_thermal_cpufreq_notifier = {
	.notifier_call = msm_thermal_cpufreq_callback,
};

static void limit_cpu_freqs(uint32_t max_freq)
{
	unsigned int cpu;

	if (info.limited_max_freq == max_freq)
		return;

	info.limited_max_freq = max_freq;
	info.pending_change = true;

	get_online_cpus();
	for_each_online_cpu(cpu) {
		cpufreq_update_policy(cpu);
		pr_info("%s: Setting cpu%d max frequency to %d\n",
				KBUILD_MODNAME, cpu, info.limited_max_freq);
	}
	put_online_cpus();

	info.pending_change = false;
}

static void check_temp(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	uint32_t freq = 0;
	long temp = 0;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	tsens_get_temp(&tsens_dev, &temp);

	if (info.throttling) {
		if (temp < (temp_threshold - info.safe_diff)) {
			limit_cpu_freqs(info.cpuinfo_max_freq);
			info.throttling = false;
			goto reschedule;
		}
	}

	if (temp >= temp_threshold + LEVEL_SHIT)
		freq = FREQ_SHIT;
	else if (temp >= temp_threshold + LEVEL_HELL)
		freq = FREQ_HELL;
	else if (temp >= temp_threshold + LEVEL_VERY_HOT)
		freq = FREQ_VERY_HOT;
	else if (temp >= temp_threshold + LEVEL_HOT)
		freq = FREQ_HOT;
	else if (temp > temp_threshold)
		freq = FREQ_WARM;

	if (freq) {
		limit_cpu_freqs(freq);

		if (!info.throttling)
			info.throttling = true;
	}

reschedule:
	queue_delayed_work(thermal_wq, &check_temp_work, msecs_to_jiffies(250));
}

static int msm_thermal_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device_node *node = pdev->dev.of_node;
	struct msm_thermal_data data;

	memset(&data, 0, sizeof(struct msm_thermal_data));

	ret = of_property_read_u32(node, "qcom,sensor-id", &data.sensor_id);
	if (ret)
		return ret;

	WARN_ON(data.sensor_id >= TSENS_MAX_SENSORS);

        memcpy(&msm_thermal_info, &data, sizeof(struct msm_thermal_data));

	ret = cpufreq_register_notifier(&msm_thermal_cpufreq_notifier,
		CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		pr_err("thermals: well, if this fails here, we're fucked\n");

	thermal_wq = alloc_workqueue("thermal_wq", WQ_HIGHPRI, 0);
	if (!thermal_wq) {
		pr_err("thermals: don't worry, if this fails we're also bananas\n");
		goto err;
	}

	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	queue_delayed_work(thermal_wq, &check_temp_work, 5);

err:
	return ret;
}

static int msm_thermal_dev_remove(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&check_temp_work);
	destroy_workqueue(thermal_wq);
	cpufreq_unregister_notifier(&msm_thermal_cpufreq_notifier,
                        CPUFREQ_POLICY_NOTIFIER);
	return 0;
}

static struct of_device_id msm_thermal_match_table[] = {
	{.compatible = "qcom,msm-thermal"},
	{},
};

static struct platform_driver msm_thermal_device_driver = {
	.probe = msm_thermal_dev_probe,
	.remove = msm_thermal_dev_remove,
	.driver = {
		.name = "msm-thermal",
		.owner = THIS_MODULE,
		.of_match_table = msm_thermal_match_table,
	},
};

static int __init msm_thermal_device_init(void)
{
	return platform_driver_register(&msm_thermal_device_driver);
}

static void __exit msm_thermal_device_exit(void)
{
	platform_driver_unregister(&msm_thermal_device_driver);
}

late_initcall(msm_thermal_device_init);
module_exit(msm_thermal_device_exit);
