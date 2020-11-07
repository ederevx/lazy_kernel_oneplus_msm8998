// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */

#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/workqueue.h>
#include <linux/dynamic_stune.h>

#include "tune.h"

/* State bit */
#define STATE_BIT BIT(0)

/* Boost value structures */
struct dstune_val {
	struct delayed_work disable;
	wait_queue_head_t waitq;
	struct mutex lock;
	void (*set_stune)(bool enable);
	unsigned long state;
};

static struct dstune_val boost, crucial;

/* *last_time variables */
unsigned long last_boost_time, last_crucial_time;

/* 
 * Enable/disable stune functions 
 */
static __always_inline
void enable_stune(struct dstune_val *ds, unsigned long *last_time, 
	unsigned short duration)
{
	/* Try to acquire the lock, if not possible do not proceed */
	if (!mutex_trylock(&ds->lock))
		return;

	if (!mod_delayed_work(system_unbound_wq, &ds->disable, duration)) {
		ds->state |= STATE_BIT;
		wake_up(&ds->waitq);
	}

	/* Update *last_time after updating delayed work duration */
	*last_time = jiffies;

	mutex_unlock(&ds->lock);
}

static __always_inline 
void disable_stune(struct dstune_val *ds)
{
	/* Try to acquire the lock, if not possible do not proceed */
	if (!mutex_trylock(&ds->lock))
		return;

	ds->state &= ~STATE_BIT;
	wake_up(&ds->waitq);

	mutex_unlock(&ds->lock);
}

/* 
 * Boost structure 
 */
static void set_stune_boost(bool enable)
{
	/*
	 * Enable boost and prefer_idle in order to bias migrating top-app 
	 * (also for foreground) tasks to idle big cluster cores.
	 */
	do_boost("top-app", enable);
	do_prefer_idle("top-app", enable);
	do_prefer_idle("foreground", enable);
}

static void boost_remove(struct work_struct *work)
{
	disable_stune(&boost);
}

static struct dstune_val boost = {
	.disable = __DELAYED_WORK_INITIALIZER(boost.disable,
						  boost_remove, 0),
	.waitq = __WAIT_QUEUE_HEAD_INITIALIZER(boost.waitq),
	.lock = __MUTEX_INITIALIZER(boost.lock),
	.set_stune = &set_stune_boost
};

/* 
 * Crucial structure
 */
static void set_stune_crucial(bool enable)
{
	/*
	 * Use idle cpus with the highest original capacity for top-app when it
	 * comes to app launches and transitions in order to speed up 
	 * the process and efficiently consume power.
	 */
	do_crucial("top-app", enable);
}

static void crucial_remove(struct work_struct *work)
{
	disable_stune(&crucial);
}

static struct dstune_val crucial = {
	.disable = __DELAYED_WORK_INITIALIZER(crucial.disable,
						  crucial_remove, 0),
	.waitq = __WAIT_QUEUE_HEAD_INITIALIZER(crucial.waitq),
	.lock = __MUTEX_INITIALIZER(crucial.lock),
	.set_stune = &set_stune_crucial
};

/* 
 * DStune Kthread 
 */
static int dstune_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct dstune_val *ds = data;
	bool old_state = false;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		bool should_stop = false, curr_state;

		wait_event_freezable(ds->waitq, (curr_state = 
			(READ_ONCE(ds->state) & STATE_BIT)) != old_state || 
			(should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		old_state = curr_state;
		ds->set_stune(curr_state);
	}

	return 0;
}

/* 
 * Driver hook functions 
 */
void dynstune_boost(void)
{
	enable_stune(&boost, &last_boost_time, BOOST_DURATION);
}

void dynstune_crucial(void)
{
	enable_stune(&crucial, &last_crucial_time, CRUCIAL_DURATION);
}

/*
 * Init functions
 */
static int dstune_kthread_init(struct dstune_val *ds, const char namefmt[])
{
	struct task_struct *thread;
	int ret = 0;

	thread = kthread_run_perf_critical(dstune_thread, ds, namefmt);
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start stune thread, err: %d\n", ret);
	}

	return ret;
}

static int __init dynamic_stune_init(void)
{
	int ret = 0;
	
	ret = dstune_kthread_init(&boost, "dstune_boostd");
	if (ret)
		goto err;

	ret = dstune_kthread_init(&crucial, "dstune_cruciald");
err:
	return ret;
}
late_initcall(dynamic_stune_init);