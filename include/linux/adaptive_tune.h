// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Edrick Vince Sinsuan <sedrickvince@gmail.com>.
 */
#ifndef _ADAPTIVE_TUNE_H_
#define _ADAPTIVE_TUNE_H_

enum adaptune_states {
    CORE,
    INPUT,
    N_ATS
};

extern struct adaptune {
    struct {
        atomic_t state, pending;
    } priv[N_ATS];
} atx __cacheline_aligned_in_smp;

#define adaptune_read_state(_ats) atomic_read(&atx.priv[_ats].state)

/* Allow framebuffer to extend core duration up to 1 second */
#define MAX_UPDATE (1000 > CONFIG_ADAPTUNE_CORE_DURATION ?              \
    1 : (1000 / CONFIG_ADAPTUNE_CORE_DURATION))

/* Only for framebuffer and the like */
#define adaptune_acquire_update()                                       \
    do {                                                                \
        if (adaptune_read_state(INPUT) &&                               \
            atomic_read(&atx.priv[CORE].pending) < MAX_UPDATE)          \
            atomic_inc(&atx.priv[CORE].pending);                        \
    } while (0)

/* 
 * Allow external functions to extend durations up to 10 times 
 * their original value.
 */
#define SHARED_MAX_PENDING 10

/*
 * adaptune_acquire_pending: Allow external functions to acquire updates
 * to keep adaptune structures from deactivating to retain active tune
 * adjustments, while trying to minimize impact on the driver by atomicizing
 * all operations.
 */
#define adaptune_acquire_pending()                                      \
    do {                                                                \
        unsigned int i;                                                 \
                                                                        \
        for (i = 0; i < N_ATS; i++) {                                   \
            if (adaptune_read_state(i) &&                               \
                atomic_read(&atx.priv[i].pending) < SHARED_MAX_PENDING) \
                atomic_inc(&atx.priv[i].pending);                       \
        }                                                               \
    } while (0)

/* Internal Functions */
void schedtune_adaptive_write(bool state);
void schedutil_adaptive_limit_write(bool state);

#endif /* _ADAPTIVE_TUNE_H_ */