// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>

#include <linux/dynamic_stune.h>

enum {
	CURR,
	NEW
};

struct dynstune dss = {
	.update = ATOMIC_INIT(0), 
	.state = { ATOMIC_INIT(0) }
};

struct dynstune_priv {
	struct dynstune *ds;
    struct task_struct *thread;
	struct timer_list timer[MAX_DSS];
	unsigned long duration[MAX_DSS];
	int state[2];
};

static void wake_up_dynstune(struct dynstune_priv *dsp)
{
    struct task_struct *thread = dsp->thread;

	if ((thread->state & TASK_IDLE) != 0)
		wake_up_state(thread, TASK_IDLE);
}

static bool dynstune_cmpxchg_state(struct dynstune_priv *dsp, int req_curr)
{
	struct dynstune *ds = dsp->ds;
	int *state = dsp->state, ret = (state[NEW] != state[CURR]);

	/* Change atomics if curr state matches requirement */
	if (state[CURR] == req_curr) {
		state[NEW] = atomic_read(&ds->update);

		ret = (state[NEW] != state[CURR]);
		if (ret)
			atomic_set(&ds->state[CORE], state[NEW]);

		if (state[NEW]) {
			atomic_set_release(&ds->update, 0);
			mod_timer_pinned(&dsp->timer[CORE], jiffies + dsp->duration[CORE]);
		}
	}

	return ret;
}

static void dynstune_pm_qos(struct pm_qos_request *req, bool state)
{
	if (likely(atomic_read(&req->cpus_affine))) {
		if (likely(pm_qos_request_active(req)))
			pm_qos_update_request(req, state ? 100 : PM_QOS_DEFAULT_VALUE);
		else if (state)
			pm_qos_add_request(req, PM_QOS_CPU_DMA_LATENCY, 100);	
	} else {
		/* 
		 * Combined with prefer_idle, restricting idle to lp cluster creates an
		 * ideal condition where Case A prefer_idle path will always lead to
		 * a perf cluster CPU.
		 */
		atomic_set(&req->cpus_affine, BIT(*cpumask_bits(cpu_lp_mask)));
	}
}

static int dynstune_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct pm_qos_request req = {
		.type = PM_QOS_REQ_AFFINE_CORES,
		.cpus_affine = ATOMIC_INIT(0)
	};
	struct dynstune_priv *dsp = data;
	int *state = dsp->state;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		do {
			set_current_state(TASK_IDLE);
			schedule();
		} while (unlikely(!dynstune_cmpxchg_state(dsp, 0)));

		dynstune_pm_qos(&req, state[NEW]);
		pr_debug("dynstune: set stune = %d\n", state[NEW]);
		dynamic_schedtune_set(state[NEW]);
		state[CURR] = state[NEW];
	}

	return 0;
}

void dynstune_extend_timer(struct dynstune *ds)
{
	struct dynstune_priv *dsp = ds->priv_data;

	if (!dsp)
		return;

	if (!mod_timer(&dsp->timer[INPUT], jiffies + dsp->duration[INPUT]))
		atomic_set(&ds->state[INPUT], 1);
}

static void core_timeout(unsigned long data)
{
	struct dynstune_priv *dsp = (struct dynstune_priv *)data;

	if (dynstune_cmpxchg_state(dsp, 1))
		wake_up_dynstune(dsp);
}

static void input_timeout(unsigned long data)
{
	struct dynstune *ds = (struct dynstune *)data;
	atomic_set(&ds->state[INPUT], 0);
}

static void dynstune_input(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	struct dynstune_priv *dsp = handle->handler->private;
	struct dynstune *ds = dsp->ds;

	if (!mod_timer(&dsp->timer[INPUT], jiffies + dsp->duration[INPUT]))
		atomic_set(&ds->state[INPUT], 1);

	if (!atomic_read(&ds->state[CORE]) && atomic_read(&ds->update))
		wake_up_dynstune(dsp);
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
	int ret = 0;

	ds_priv = kzalloc(sizeof(*ds_priv), GFP_KERNEL);
	if (!ds_priv)
		return -ENOMEM;

	ds_priv->ds = &dss;

	ds_priv->duration[CORE] = msecs_to_jiffies(CONFIG_DYNSTUNE_CORE_DURATION);
	ds_priv->duration[INPUT] = msecs_to_jiffies(CONFIG_DYNSTUNE_INPUT_TIME_FRAME);

	setup_timer(&ds_priv->timer[CORE], core_timeout, (unsigned long)ds_priv);
	setup_timer(&ds_priv->timer[INPUT], input_timeout, (unsigned long)ds_priv->ds);

	ds_priv->thread = kthread_run_perf_critical(dynstune_thread, ds_priv, "dynstune_d");
	if (IS_ERR(ds_priv->thread)) {
		ret = PTR_ERR(ds_priv->thread);
		pr_err("Failed to start stune thread, err: %d\n", ret);
		goto err;
	}

	dynstune_input_handler.private = ds_priv;
	ret = input_register_handler(&dynstune_input_handler);
	if (ret)
		goto err;

	dss.priv_data = ds_priv;

	return 0;

err:
	kfree(ds_priv);
	return ret;
}
late_initcall(dynamic_stune_init);