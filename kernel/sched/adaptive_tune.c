// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */
#include <linux/fb.h>
#include <linux/input.h>

#include <linux/adaptive_tune.h>

/* Extend duration max value */
#define INTERNAL_MAX_PENDING 5
#define MAX_PENDING (INTERNAL_MAX_PENDING + SHARED_MAX_PENDING)

struct adaptune atx = {
	.priv = {{ ATOMIC_INIT(0) }}
};

struct adaptune_local {
	struct {
		struct timer_list timer;
		unsigned long duration[MAX_PENDING];
		unsigned int pending;
		bool state;
	} priv[N_ATS];
	struct notifier_block fb_notif;
	bool suspended;
};

#define adaptune_update(ats, val)							\
	do {													\
		switch (ats) {										\
			case CORE:										\
				schedtune_adaptive_write(val);				\
				schedutil_adaptive_limit_write(val);		\
				break;										\
			case INPUT:										\
				break;										\
		}													\
		atomic_set(&atx.priv[ats].state, val);				\
	} while (0)

static inline void adaptune_timeout(struct adaptune_local *atl,
		unsigned int ats)
{
	unsigned int pending;

	if (unlikely(READ_ONCE(atl->suspended)))
		return;

	if (unlikely(!atl->priv[ats].state))
		return;

	pending = atl->priv[ats].pending + atomic_read(&atx.priv[ats].pending);
	if (pending > 0) {
		mod_timer(&atl->priv[ats].timer, jiffies +
				atl->priv[ats].duration[pending - 1]);
	} else {
		adaptune_update(ats, 0);
		atl->priv[ats].state = false;
	}

	atomic_set(&atx.priv[ats].pending, 0);
	atl->priv[ats].pending = 0;
}

static inline void adaptune_wake(struct adaptune_local *atl)
{
	unsigned int i;

	for (i = 0; i < N_ATS; i++) {
		if (atl->priv[i].state) {
			if (atl->priv[i].pending < INTERNAL_MAX_PENDING)
				atl->priv[i].pending++;
		} else {
			atl->priv[i].state = true;
			adaptune_update(i, 1);
			mod_timer(&atl->priv[i].timer, jiffies +
					atl->priv[i].duration[0]);
		}
	}
}

static inline void adaptune_suspend(struct adaptune_local *atl)
{
	unsigned int i;

	for (i = 0; i < N_ATS; i++) {
		if (atl->priv[i].state) {
			del_timer(&atl->priv[i].timer);
			adaptune_update(i, 0);
			atl->priv[i].state = false;
		}
		atl->priv[i].pending = 0;
		atomic_set(&atx.priv[i].pending, 0);
	}
}

static void core_timeout(unsigned long data)
{
	adaptune_timeout((void *)data, CORE);
}

static void input_timeout(unsigned long data)
{
	adaptune_timeout((void *)data, INPUT);
}

static void adaptune_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	struct adaptune_local *atl = handle->handler->private;

	if (READ_ONCE(atl->suspended))
		return;

	adaptune_wake(atl);
}

static int adaptune_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "adaptive_tune";

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

static void adaptune_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id adaptune_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	{ }
};

static struct input_handler adaptune_input_handler = {
	.event		   = adaptune_input_event,
	.connect	   = adaptune_input_connect,
	.disconnect	   = adaptune_input_disconnect,
	.name		   = "adaptune_h",
	.id_table	   = adaptune_ids,
};

static int fb_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct adaptune_local *atl = container_of(nb, typeof(*atl), fb_notif);
	int *blank = ((struct fb_event *)data)->data, state;

	/* Notify the structures as soon as possible, do not allow if blank is NULL */
	if (action != FB_EARLY_EVENT_BLANK || !blank)
		return NOTIFY_OK;

	state = (*blank == FB_BLANK_UNBLANK);
	if (state == atl->suspended) {
		atl->suspended = !state;

		if (!state)
			adaptune_suspend(atl);
		else
			adaptune_wake(atl);
	}

	return NOTIFY_OK;
}

static int __init adaptive_tune_init(void)
{
	struct adaptune_local *atl;
	unsigned int i;
	int ret = 0;

	atl = kzalloc(sizeof(*atl), GFP_KERNEL);
	if (!atl)
		return -ENOMEM;

	for (i = 0; i < N_ATS; i++) {
		unsigned long duration;
		unsigned int j;
		void *timeout_func;

		switch (i) {
			case CORE:
				duration = CONFIG_ADAPTUNE_CORE_DURATION;
				timeout_func = core_timeout;
				break;
			case INPUT:
				duration = CONFIG_ADAPTUNE_INPUT_TIME_FRAME;
				timeout_func = input_timeout;
				break;
		}

		duration = msecs_to_jiffies(duration);
		for (j = 0; j < MAX_PENDING; j++)
			atl->priv[i].duration[j] = duration * (j + 1);

		setup_timer(&atl->priv[i].timer, timeout_func, (unsigned long)atl);
	}

	adaptune_input_handler.private = atl;
	ret = input_register_handler(&adaptune_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto free_main;
	}

	atl->fb_notif.notifier_call = fb_notifier_cb;
	atl->fb_notif.priority = INT_MAX;
	ret = fb_register_client(&atl->fb_notif);
	if (ret) {
		pr_err("Failed to register fb notifier, err: %d\n", ret);
		goto unregister_input;
	}

	return 0;

unregister_input:
	input_unregister_handler(&adaptune_input_handler);

free_main:
	kfree(atl);
	return ret;
}
late_initcall(adaptive_tune_init);