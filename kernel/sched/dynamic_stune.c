// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/time.h>

#include <linux/dynamic_stune.h>

#include "sched.h"
#include "tune.h"

struct dynstune dss[DSS_MAX];

struct dynstune_priv {
	char *name;
	struct dynstune *ds;
	wait_queue_head_t waitq;
	unsigned long duration;
	void (*set_func)(bool state);
};

static void set_dynstune_core(bool state)
{
	/* Call schedtune counterpart */
	dynamic_schedtune_set(state);
}

static int dynstune_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct dynstune_priv *dsp = data;
	struct dynstune *ds = dsp->ds;
	unsigned long duration = 0;
	bool priv_state = false;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		duration = atomic_read(&ds->state) ? dsp->duration : MAX_SCHEDULE_TIMEOUT;
		priv_state = !!wait_event_timeout(dsp->waitq, atomic_read(&ds->update), duration);

		if (!priv_state)
			atomic_cmpxchg_acquire(&ds->update, 0, 1);

		if ((duration != MAX_SCHEDULE_TIMEOUT) != priv_state) {
			atomic_set(&ds->state, priv_state);
			if (dsp->set_func)
				dsp->set_func(priv_state);
		}

		atomic_set_release(&ds->update, 0);
	}

	return 0;
}

static void dynstune_input(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	struct dynstune_priv *ds_priv = handle->handler->private;
	struct dynstune *ds;

	ds = ds_priv[INPUT].ds;
	if (!atomic_cmpxchg_acquire(&ds->update, 0, 1))
		wake_up(&ds_priv[INPUT].waitq);

	ds = ds_priv[CORE].ds;
	if (!atomic_read(&ds->state) && atomic_read(&ds->update))
		wake_up(&ds_priv[CORE].waitq);
}

static int dynstune_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "dynamic_stune";

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

static void dynstune_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id dynstune_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler dynstune_input_handler = {
	.event          = dynstune_input,
	.connect        = dynstune_input_connect,
	.disconnect     = dynstune_input_disconnect,
	.name           = "dynstune_h",
	.id_table       = dynstune_ids,
};

static int __init dynamic_stune_init(void)
{
	struct dynstune_priv *ds_priv;
	enum dynstune_structs i;
	int ret = 0;

	static const struct dynstune_priv dsp_init[] = {
		{ "dynstune_core", NULL, { }, CONFIG_DYNSTUNE_CORE_DURATION, &set_dynstune_core },
		{ "dynstune_input", NULL, { }, CONFIG_DYNSTUNE_INPUT_TIME_FRAME, NULL },
		{ }
	};

	ds_priv = kzalloc(sizeof(dsp_init), GFP_KERNEL);
	if (!ds_priv)
		return -ENOMEM;

	for (i = 0; i < DSS_MAX; i++) {
		struct dynstune_priv *dsp = &ds_priv[i];
		struct dynstune *ds = &dss[i];
		struct task_struct *thread;

		struct dynstune ds_init = {
			.update = ATOMIC_INIT(0), .state = ATOMIC_INIT(0)
		};

		*ds = ds_init;
		*dsp = dsp_init[i];

		dsp->ds = ds;
		dsp->duration = msecs_to_jiffies(dsp->duration);

		init_waitqueue_head(&dsp->waitq);

		thread = kthread_run_perf_critical(dynstune_thread, dsp, dsp->name);
		if (IS_ERR(thread)) {
			ret = PTR_ERR(thread);
			pr_err("Failed to start stune thread, err: %d\n", ret);
			break;
		}
	}

	dynstune_input_handler.private = ds_priv;
	ret = input_register_handler(&dynstune_input_handler);
	if (ret)
		goto err;

	return 0;

err:
	kfree(ds_priv);
	return ret;
}
late_initcall(dynamic_stune_init);