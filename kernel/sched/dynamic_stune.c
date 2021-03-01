// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */
#include <linux/hrtimer.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/slab.h>

#include <linux/dynamic_stune.h>

#include "tune.h"

struct dynstune dss = {
	.update = ATOMIC_INIT(0), .state = ATOMIC_INIT(0),
	.waitq = __WAIT_QUEUE_HEAD_INITIALIZER(dss.waitq)
};

struct dynstune_priv {
	char *name;
	struct dynstune *ds;
	void (*set_func)(bool state);

	atomic_t state;
	struct hrtimer input_timer;

	unsigned long duration[2];
};

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
		duration = priv_state ? dsp->duration[0] : MAX_SCHEDULE_TIMEOUT;
		priv_state = !!wait_event_timeout(ds->waitq, (atomic_read(&ds->update) && 
							atomic_read(&ds->state)), duration);

		atomic_cmpxchg_release(&ds->update, 1, 0);

		if ((duration != MAX_SCHEDULE_TIMEOUT) != priv_state) {
			atomic_set(&dsp->state, priv_state);
			dsp->set_func(priv_state);
		}
	}

	return 0;
}

static enum hrtimer_restart input_timer_func(struct hrtimer *timer)
{
	struct dynstune_priv *dsp = container_of(timer,
						struct dynstune_priv, input_timer);

	atomic_set_release(&dsp->ds->state, 0);
    return HRTIMER_NORESTART;
}

static void dynstune_input(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	struct dynstune_priv *dsp = handle->handler->private;
	struct dynstune *ds = dsp->ds;

	hrtimer_start(&dsp->input_timer, dsp->duration[1], HRTIMER_MODE_REL);
	atomic_cmpxchg_acquire(&ds->state, 0, 1);

	if (!atomic_read(&dsp->state) && atomic_read(&ds->update))
		wake_up(&ds->waitq);
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
	struct task_struct *thread;
	int ret = 0;

	static const struct dynstune_priv dsp_init = {
		.name = "dynstune_d", .ds = &dss,
		.set_func = &dynamic_schedtune_set,
		.state = ATOMIC_INIT(0)
	};

	ds_priv = kzalloc(sizeof(dsp_init), GFP_KERNEL);
	if (!ds_priv)
		return -ENOMEM;

	*ds_priv = dsp_init;

	ds_priv->duration[0] = msecs_to_jiffies(CONFIG_DYNSTUNE_CORE_DURATION);
	ds_priv->duration[1] = ms_to_ktime(CONFIG_DYNSTUNE_INPUT_TIME_FRAME);

	thread = kthread_run_perf_critical(dynstune_thread, ds_priv, ds_priv->name);
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start stune thread, err: %d\n", ret);
		goto err;
	}

	hrtimer_init(&ds_priv->input_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ds_priv->input_timer.function = &input_timer_func;

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