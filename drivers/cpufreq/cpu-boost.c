/*
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 *
 * Dynamic SchedTune Integration
 * Copyright (c) 2020, Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu-boost: " fmt

#include <linux/fb.h>
#include <linux/input.h>
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/cpu-boost.h>
#include <linux/sched.h>
#include <linux/sched/sysctl.h>

struct boost_val {
	bool state;
	struct work_struct enable;
	struct delayed_work disable;
	unsigned short duration, stored_duration_ms;
	unsigned int stored_val;
	int slot;
};

/* Boost value structures */
static struct boost_val input, kick;

/* Framebuffer state notifier */
static struct notifier_block fb_notifier;
static bool fb_state;

/* Workqueue structures and state work_struct */
static struct workqueue_struct *update_state_wq, *input_boost_wq,
	*kick_boost_wq;
static struct work_struct update_work;

/*
 * global_state - Controls the state of the driver depending on fb_state and
 * disable_dsboost. Can not be changed by user.
 */
static bool global_state;
module_param_named(dsboost_global_state, global_state, bool, 0444);

/*
 * Modifiable Variables
 */
static bool disable_dsboost;
module_param(disable_dsboost, bool, 0644);

static unsigned int __read_mostly input_sched_boost = 
	CONFIG_INPUT_SCHED_BOOST;
static unsigned int __read_mostly kick_sched_boost = 
	CONFIG_KICK_SCHED_BOOST;
static unsigned short __read_mostly input_duration_ms = 
	CONFIG_INPUT_DURATION;
static unsigned short __read_mostly kick_duration_ms = 
	CONFIG_KICK_DURATION;

module_param(input_sched_boost, uint, 0644);
module_param(kick_sched_boost, uint, 0644);
module_param(input_duration_ms, ushort, 0644);
module_param(kick_duration_ms, ushort, 0644);

static inline bool set_input_boost(bool enable)
{
	if (input.state == enable)
		return enable;

	/*
	 * Only allow boost and prefer_idle to function without bias in order to properly
	 * assess the capacity of cpus and choose the proper idle cpu for the task.
	 */
	do_prefer_idle("top-app", enable);
	do_prefer_idle("foreground", enable);

	return enable ? !do_stune_boost("top-app", input.stored_val, &input.slot)
		: reset_stune_boost("top-app", input.slot);
}

static inline bool set_kick_boost(bool enable)
{
	if (kick.state == enable)
		return enable;

	/*
	 * Use idle cpus with high original capacity and bias to big cluster when it
	 * comes to app launches and transitions in order to speed up the process
	 * and efficiently consume power.
	 */
	sysctl_sched_cpu_schedtune_bias = enable;
	do_crucial("top-app", enable);

	return enable ? !do_stune_boost("top-app", kick.stored_val, &kick.slot)
		: reset_stune_boost("top-app", kick.slot);
}

static inline bool check_state(void)
{
	bool ret;

	/* Update if disable_dsboost changes */
	ret = disable_dsboost == global_state;
	if (ret) {
		if (!work_pending(&update_work))
			queue_work(update_state_wq, &update_work);
		return ret;
	}
	ret = !global_state || kick.state;
	return ret;
}

static void trigger_input(struct work_struct *work)
{
	if (input_duration_ms != input.stored_duration_ms) {
		input.stored_duration_ms = input_duration_ms;
		input.duration = msecs_to_jiffies(input_duration_ms);
	}

	if (!input.duration)
		return;

	mod_delayed_work(input_boost_wq, &input.disable, input.duration);

	if (input_sched_boost != input.stored_val) {
		if (input_sched_boost <= 0 || input_sched_boost > 100)
			input_sched_boost = CONFIG_INPUT_SCHED_BOOST;

		input.stored_val = input_sched_boost;
		/* If boost is already active, let's just update boost value */
		if (input.state) {
			reset_stune_boost("top-app", input.slot);
			do_stune_boost("top-app", input_sched_boost, &input.slot);
			return;
		}
	}

	input.state = set_input_boost(1);
}

static void trigger_kick(struct work_struct *work)
{
	if (kick_duration_ms != kick.stored_duration_ms) {
		kick.stored_duration_ms = kick_duration_ms;
		kick.duration = msecs_to_jiffies(kick_duration_ms);
	}

	if (!kick.duration)
		return;

	mod_delayed_work(kick_boost_wq, &kick.disable, kick.duration);

	if (kick_sched_boost != kick.stored_val) {
		if (kick_sched_boost <= 0 || kick_sched_boost > 100)
			kick_sched_boost = CONFIG_KICK_SCHED_BOOST;

		kick.stored_val = kick_sched_boost;
		/* If boost is already active, let's just update boost value */
		if (kick.state) {
			reset_stune_boost("top-app", kick.slot);
			do_stune_boost("top-app", kick_sched_boost, &kick.slot);
			return;
		}
	}

	kick.state = set_kick_boost(1);
}

static void input_remove(struct work_struct *work)
{
	input.state = set_input_boost(0);
}

static void kick_remove(struct work_struct *work)
{
	kick.state = set_kick_boost(0);
}

static void update_state(struct work_struct *work)
{
	bool stored_fbs = fb_state;

	/*
	 * Set dsboost and schedtune state according to fb_state
	 * or stored_boost value.
	 */
	global_state = (disable_dsboost || !stored_fbs) ? 0 : 1;
	if (!global_state) {
		/* Drain workqueues if dsboost_state is off */
		if (input.state) {
			input.state = set_input_boost(0);
			drain_workqueue(input_boost_wq);
		}
		if (kick.state) {
			kick.state = set_kick_boost(0);
			drain_workqueue(kick_boost_wq);
		}
	}
	disable_schedtune_boost(!global_state);
}

void cpuboost_kick(void)
{
	if (!check_state() && !work_pending(&kick.enable))
		queue_work(kick_boost_wq, &kick.enable);
}

static void cpuboost_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	if (!check_state() && !work_pending(&input.enable))
		queue_work(input_boost_wq, &input.enable);
}

static int cpuboost_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void cpuboost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpuboost_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler cpuboost_input_handler = {
	.event          = cpuboost_input_event,
	.connect        = cpuboost_input_connect,
	.disconnect     = cpuboost_input_disconnect,
	.name           = "cpu-boost",
	.id_table       = cpuboost_ids,
};

static int fb_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	int *blank = ((struct fb_event *) data)->data;
	bool new_state = (*blank == FB_BLANK_UNBLANK) ? 1 : 0;

	if (action != FB_EARLY_EVENT_BLANK || new_state == fb_state)
		return NOTIFY_OK;

	fb_state = new_state;

	if (!work_pending(&update_work))
		queue_work(update_state_wq, &update_work);

	return NOTIFY_OK;
}

static void destroy_boost_workqueues(void)
{
	destroy_workqueue(input_boost_wq);
	destroy_workqueue(kick_boost_wq);
	destroy_workqueue(update_state_wq);
}

static void __exit cpu_boost_exit(void)
{
	input_unregister_handler(&cpuboost_input_handler);
	fb_unregister_client(&fb_notifier);
	destroy_boost_workqueues();
}

static int __init cpu_boost_init(void)
{
	int ret;

	input_boost_wq = alloc_ordered_workqueue("input_boost_wq", WQ_FREEZABLE);
	kick_boost_wq = alloc_ordered_workqueue("kick_boost_wq", WQ_FREEZABLE);
	update_state_wq = alloc_ordered_workqueue("update_state_wq", 0);
	if (!input_boost_wq || !kick_boost_wq || !update_state_wq)
		return -ENOMEM;

	INIT_WORK(&input.enable, trigger_input);
	INIT_WORK(&kick.enable, trigger_kick);
	INIT_DELAYED_WORK(&input.disable, input_remove);
	INIT_DELAYED_WORK(&kick.disable, kick_remove);
	INIT_WORK(&update_work, update_state);

	ret = input_register_handler(&cpuboost_input_handler);
	if (ret)
		goto err_wq;

	fb_notifier.notifier_call = fb_notifier_cb;
	fb_notifier.priority = INT_MAX;
	ret = fb_register_client(&fb_notifier);
	if (ret)
		goto err_input;

	return 0;
err_input:
	input_unregister_handler(&cpuboost_input_handler);
err_wq:
	destroy_boost_workqueues();

	return ret;
}
late_initcall(cpu_boost_init);
module_exit(cpu_boost_exit);
