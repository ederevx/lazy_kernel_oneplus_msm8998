// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */

#include <linux/fb.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/dynamic_stune.h>

#include "tune.h"

/*
 * Configurable variables
 */
#define INPUT_DURATION msecs_to_jiffies(CONFIG_INPUT_STUNE_DURATION)
#define KICK_DURATION msecs_to_jiffies(CONFIG_KICK_STUNE_DURATION)

/*
 * Driver variables and structures
 */
struct stune_val {
	bool curr_state;
	struct workqueue_struct *wq;
	struct work_struct enable;
	struct delayed_work disable;
};

/* Boost value structures */
static struct stune_val input, kick;

/* Framebuffer state notifier and stored blank boolean */
static struct notifier_block fb_notifier;
static bool stored_blank;

static inline void set_stune(struct stune_val *stune, bool enable)
{
	if (stune->curr_state == enable)
		return;

	stune->curr_state = enable;

	if (stune == &input) {
		/*
		 * Enable stune and prefer_idle with bias function in order to bias 
		 * migrating top-app (also for foreground) tasks to idle big cluster cores.
		 */
		do_prefer_idle("top-app", enable);
		do_prefer_idle("foreground", enable);
	} else {
		/*
		 * Use idle cpus with the highest original capacity for top-app when it
		 * comes to app launches and transitions in order to speed up 
		 * the process and efficiently consume power.
		 */
		do_crucial("top-app", enable);
	}
}

static inline void trigger_stune(struct stune_val *stune, unsigned short duration)
{
	mod_delayed_work(stune->wq, &stune->disable, duration);
	set_stune(stune, true);
}

static void trigger_input(struct work_struct *work)
{
	trigger_stune(&input, INPUT_DURATION);
}

static void trigger_kick(struct work_struct *work)
{
	trigger_stune(&kick, KICK_DURATION);
}

static void input_remove(struct work_struct *work)
{
	set_stune(&input, false);
}

static void kick_remove(struct work_struct *work)
{
	set_stune(&kick, false);
}

static inline void trigger_event(struct stune_val *stune)
{
	/* Disable stune if state or stored_blank is false */
	if (!stored_blank) {
		if (stune->curr_state)
			mod_delayed_work(stune->wq, &stune->disable, 0);
		return;
	}

	if (!work_pending(&stune->enable))
		queue_work(stune->wq, &stune->enable);
}

void dynstune_kick(void)
{
	trigger_event(&kick);
}

static void dynstune_input(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	trigger_event(&input);
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
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	{ }
};

static struct input_handler dynstune_input_handler = {
	.event          = dynstune_input,
	.connect        = dynstune_input_connect,
	.disconnect     = dynstune_input_disconnect,
	.name           = "dynamic_stune",
	.id_table       = dynstune_ids,
};

static int fb_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	int *blank = ((struct fb_event *) data)->data;
	bool suspend_state = *blank != FB_BLANK_UNBLANK;

	if (action != FB_EARLY_EVENT_BLANK || suspend_state != stored_blank)
		return NOTIFY_OK;

	stored_blank = !suspend_state;

	/* Trigger stunes whenever blank state changes */
	trigger_event(&input);
	trigger_event(&kick);

	return NOTIFY_OK;
}

static void destroy_stune_workqueues(void)
{
	if (input.wq)
		destroy_workqueue(input.wq);
	if (kick.wq)
		destroy_workqueue(kick.wq);
}

static int init_stune_workqueues(void)
{
	input.wq = alloc_workqueue("input_stune_wq", WQ_HIGHPRI, 1);
	if (!input.wq)
		return -ENOMEM;

	INIT_WORK(&input.enable, trigger_input);	
	INIT_DELAYED_WORK(&input.disable, input_remove);

	kick.wq = alloc_workqueue("kick_stune_wq", WQ_HIGHPRI, 1);
	if (!kick.wq)
		return -ENOMEM;

	INIT_WORK(&kick.enable, trigger_kick);
	INIT_DELAYED_WORK(&kick.disable, kick_remove);

	return 0;
}

static int __init dynamic_stune_init(void)
{
	int ret;

	ret = init_stune_workqueues();
	if (ret)
		goto err_wq;

	ret = input_register_handler(&dynstune_input_handler);
	if (ret)
		goto err_wq;

	fb_notifier.notifier_call = fb_notifier_cb;
	fb_notifier.priority = INT_MAX;
	ret = fb_register_client(&fb_notifier);
	if (ret)
		goto err_input;

	return 0;
err_input:
	input_unregister_handler(&dynstune_input_handler);
err_wq:
	destroy_stune_workqueues();

	return ret;
}
late_initcall(dynamic_stune_init);