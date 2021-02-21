// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */
#ifndef _DYNAMIC_STUNE_H_
#define _DYNAMIC_STUNE_H_

#define dynstune_read_state(_dsnum) atomic_read(&dss[_dsnum].state)
#define dynstune_acquire_update(_dsnum) atomic_cmpxchg_acquire(&dss[_dsnum].update, 0, 1)

enum dynstune_structs {
    CORE,
    INPUT,
    DSS_MAX
};

struct dynstune {
    atomic_t update, state;
};

extern struct dynstune dss[];

#endif /* _DYNAMIC_STUNE_H_ */