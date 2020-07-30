/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */
#ifndef _CPU_BOOST_H_
#define _CPU_BOOST_H_

#ifdef CONFIG_CPU_BOOST
void cpuboost_kick(void);
#else
static inline void cpuboost_kick(void)
{
}
#endif /* CONFIG_CPU_BOOST */

#endif /* _CPU_BOOST_H_ */
