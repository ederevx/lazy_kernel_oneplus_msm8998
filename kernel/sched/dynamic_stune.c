// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */

#include <linux/workqueue.h>
#include <linux/dynamic_stune.h>

#include "tune.h"

/*
 * Driver variables and structures
 */
struct stune_val {
	struct workqueue_struct *wq;
	struct work_struct enable;
	struct delayed_work disable;
};

/* Boost value structures */
static struct stune_val boost, crucial;

/* Mutex locks */
struct mutex boost_lock, crucial_lock;

/* Last time an event occured */
unsigned long last_boost_time, last_crucial_time;

static __always_inline void set_stune_boost(bool enable)
{
	/*
	 * Enable boost and prefer_idle in order to bias migrating top-app 
	 * (also for foreground) tasks to idle big cluster cores.
	 */
	do_boost("top-app", enable);
	do_prefer_idle("top-app", enable);
	do_prefer_idle("foreground", enable);
}

static __always_inline void set_stune_crucial(bool enable)
{
	/*
	 * Use idle cpus with the highest original capacity for top-app when it
	 * comes to app launches and transitions in order to speed up 
	 * the process and efficiently consume power.
	 */
	do_crucial("top-app", enable);
}

static __always_inline
void trigger_event(struct stune_val *stune, struct mutex *stune_lock, 
	unsigned long *last_time, unsigned short duration)
{
	mutex_lock(stune_lock);

	if (!mod_delayed_work(stune->wq, &stune->disable, duration))
		queue_work(stune->wq, &stune->enable);

	/* Update time parameters after delaying and/or queueing work */
	*last_time = jiffies;

	mutex_unlock(stune_lock);
}

static void trigger_boost(struct work_struct *work)
{
	set_stune_boost(true);
}

static void trigger_crucial(struct work_struct *work)
{
	set_stune_crucial(true);
}

static void boost_remove(struct work_struct *work)
{
	set_stune_boost(false);
}

static void crucial_remove(struct work_struct *work)
{
	set_stune_crucial(false);
}

void dynstune_boost(void)
{
	trigger_event(&boost, &boost_lock, 
		&last_boost_time, BOOST_DURATION);
}

void dynstune_crucial(void)
{
	trigger_event(&crucial, &crucial_lock, 
		&last_crucial_time, CRUCIAL_DURATION);
}

static void destroy_stune_workqueues(void)
{
	if (boost.wq)
		destroy_workqueue(boost.wq);
	if (crucial.wq)
		destroy_workqueue(crucial.wq);
}

static int init_stune_workqueues(void)
{
	boost.wq = alloc_ordered_workqueue("boost_stune_wq", WQ_HIGHPRI);
	if (!boost.wq)
		return -ENOMEM;

	INIT_WORK(&boost.enable, trigger_boost);	
	INIT_DELAYED_WORK(&boost.disable, boost_remove);

	crucial.wq = alloc_ordered_workqueue("crucial_stune_wq", WQ_HIGHPRI);
	if (!crucial.wq)
		return -ENOMEM;

	INIT_WORK(&crucial.enable, trigger_crucial);
	INIT_DELAYED_WORK(&crucial.disable, crucial_remove);

	return 0;
}

static int __init dynamic_stune_init(void)
{
	int ret;

	ret = init_stune_workqueues();
	if (ret) {
		destroy_stune_workqueues();
		return ret;
	}

	mutex_init(&boost_lock);
	mutex_init(&crucial_lock);

	return 0;
}
late_initcall(dynamic_stune_init);