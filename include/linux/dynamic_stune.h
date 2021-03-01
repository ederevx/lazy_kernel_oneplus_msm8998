// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */
#ifndef _DYNAMIC_STUNE_H_
#define _DYNAMIC_STUNE_H_

struct dynstune {
    wait_queue_head_t waitq;
    atomic_t update, state;
};

extern struct dynstune dss;

#define dynstune_read_state() atomic_read(&dss.state)
#define dynstune_acquire_update()                   \
({                                                  \
    if (waitqueue_active(&dss.waitq))               \
        atomic_cmpxchg_acquire(&dss.update, 0, 1);  \
})

#endif /* _DYNAMIC_STUNE_H_ */