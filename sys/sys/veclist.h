/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c)2019 Reliable Computer Systems Lab, University of Waterloo
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * $FreeBSD$
 */

/* Vector list - insert/remove: O(n)
 *             - random access: O(1)
 *             - insert/remove tail: O(1)
 */

#ifndef _SYS_VECLIST_H_
#define	_SYS_VECLIST_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/errno.h>

struct veclist {
    size_t cap;
    size_t size;
    struct malloc_type *mtype;
    void **buf;
};

#define VECLIST_EXPAND_FACTOR (2)
#define VECLIST_INIT_SZ (8)

/* returns old buffer */
static inline int
veclist_expand(struct veclist *lst, size_t new_cap)
{
    void **new_buf;
    KASSERT(new_cap > lst->cap, ("veclist expand"));
    new_buf = (void **)malloc(new_cap * sizeof(void*), lst->mtype, M_NOWAIT);
    if (new_buf == NULL) {
        return ENOMEM;
    }
    memcpy(new_buf, lst->buf, lst->size * sizeof(void*));
    free(lst->buf, lst->mtype);
    lst->buf = new_buf;
    lst->cap = new_cap;
    return 0;
}

static inline int
veclist_init(struct veclist *lst, size_t init_cap, struct malloc_type *mtype)
{
    lst->cap = 0;
    lst->buf = NULL;
    lst->size = 0;
    lst->mtype = mtype;

    return init_cap ? veclist_expand(lst, init_cap) : 0;
}

static inline void * 
veclist_remove_at(struct veclist *lst, size_t idx)
{
    void *ret;
    KASSERT(lst->size > idx, ("veclist_remove_at index out of bound"));
    ret = lst->buf[idx];
    memmove(&lst->buf[idx], &lst->buf[idx+1], (lst->size - (idx + 1)) * sizeof(void*));
    lst->size--;
    return ret;
}

static inline void
veclist_destroy(struct veclist *lst)
{
    free(lst->buf, lst->mtype);
}

static inline void *
veclist_remove(struct veclist *lst, void *ele)
{
    int found;

    for(found = 0; found < lst->size; found++) {
        if(lst->buf[found] == ele) {
            break;
        }
    }

    return veclist_remove_at(lst, found);
}

/* inserts an element so that the index of the element after insertion is idx */
static inline int
veclist_insert_at(struct veclist *lst, void *ele, size_t idx)
{
    int err;
    KASSERT(idx <= lst->size, ("veclist idx overflow"));
    if (lst->size == lst->cap) {
        /* needs expansion */
        err = veclist_expand(lst, lst->cap == 0 ? VECLIST_INIT_SZ : lst->cap * VECLIST_EXPAND_FACTOR);
        if (err) {
            return err;
        }
    }
    memmove(&lst->buf[idx+1], &lst->buf[idx], (lst->size - idx) * sizeof(void*));
    lst->size++;
    lst->buf[idx] = ele;
    return 0;
}

static inline int
veclist_insert_tail(struct veclist *lst, void *ele)
{
    return veclist_insert_at(lst, ele, lst->size);
}

static inline int
veclist_insert_head(struct veclist *lst, void *ele)
{
    return veclist_insert_at(lst, ele, 0);
}

static inline void *
veclist_remove_head(struct veclist *lst)
{
    return veclist_remove_at(lst, 0);
}

static inline void *
veclist_remove_tail(struct veclist *lst)
{
    return veclist_remove_at(lst, lst->size - 1);
}

static inline int 
veclist_size(struct veclist *lst)
{
    return lst->size;
}

static inline void *
veclist_at(struct veclist *lst, size_t idx)
{
    KASSERT(lst->size > idx, ("veclist_at index out of bound"));
    return lst->buf[idx];
}


#endif
