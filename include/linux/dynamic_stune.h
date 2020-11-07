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
#define INPUT_DURATION msecs_to_jiffies(CONFIG_INPUT_INTERVAL_DURATION)

/*
 * Time prohibitions for triggering boosts
 */
extern unsigned long last_input_time, last_boost_time, last_crucial_time;
#define INPUT_INTERVAL (last_input_time + INPUT_DURATION)
#define BOOST_CLEARANCE (last_boost_time + (BOOST_DURATION >> 1))
#define CRUCIAL_CLEARANCE (last_crucial_time + (CRUCIAL_DURATION >> 1))

void dynstune_boost(void);
void dynstune_crucial(void);

#endif /* _DYNAMIC_STUNE_H_ */