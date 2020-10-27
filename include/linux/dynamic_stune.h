// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */

#ifndef _DYNAMIC_STUNE_H_
#define _DYNAMIC_STUNE_H_

#ifdef CONFIG_DYNAMIC_STUNE
void dynstune_kick(void);
#else
static inline void dynstune_kick(void) {}
#endif /* CONFIG_DYNAMIC_STUNE */

#endif /* _DYNAMIC_STUNE_H_ */