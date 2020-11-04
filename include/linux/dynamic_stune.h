// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */

#ifndef _DYNAMIC_STUNE_H_
#define _DYNAMIC_STUNE_H_

#include <linux/mutex.h>

/*
 * Configurable variables
 */
#define BOOST_DURATION msecs_to_jiffies(CONFIG_STUNE_BOOST_DURATION)
#define CRUCIAL_DURATION msecs_to_jiffies(CONFIG_STUNE_CRUCIAL_DURATION)
#define INPUT_INTERVAL msecs_to_jiffies(CONFIG_INPUT_INTERVAL_DURATION)

/*
 * Time prohibition for triggering boosts
 */
#define BOOST_CLEARANCE (BOOST_DURATION >> 1)
#define CRUCIAL_CLEARANCE (CRUCIAL_DURATION >> 1)

/*
 * Check mutex lock before allowing dynstune execution
 */
#define dynstune_allowed(_lock) !mutex_is_locked(_lock)

extern unsigned long last_input_time, last_boost_time, last_crucial_time;
extern struct mutex boost_lock, crucial_lock;

void dynstune_boost(void);
void dynstune_crucial(void);

#endif /* _DYNAMIC_STUNE_H_ */