// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */
#ifndef _DYNAMIC_STUNE_H_
#define _DYNAMIC_STUNE_H_

enum dynstune_states {
    CORE,
    INPUT,
    MAX_DSS
};

struct dynstune {
    atomic_t update[MAX_DSS], state[MAX_DSS];
};

extern struct dynstune dss;

#define dynstune_read_state(_dss) atomic_read(&dss.state[_dss])
#define dynstune_acquire_update(_dss)                       \
({                                                          \
    if (dynstune_read_state(INPUT))                         \
        atomic_cmpxchg_acquire(&dss.update[_dss], 0, 1);    \
})

void dynamic_schedtune_set(bool state);

#endif /* _DYNAMIC_STUNE_H_ */