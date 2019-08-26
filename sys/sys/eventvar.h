/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 1999,2000 Jonathan Lemon <jlemon@FreeBSD.org>
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
 *	$FreeBSD$
 */

#ifndef _SYS_EVENTVAR_H_
#define _SYS_EVENTVAR_H_

#ifndef _KERNEL
#error "no user-serviceable parts inside"
#endif

#include <sys/_task.h>
#include <sys/veclist.h>
#include <sys/stdint.h>
#include <sys/param.h>
#include <sys/lock.h>
#include <sys/rwlock.h>

#define KQ_NEVENTS	8		/* minimize copy{in,out} calls */
#define KQEXTENT	256		/* linear growth by this amount */

#define KQDOM_EXTENT 8 /* linear growth by this amount */
#define KQDIR_ACTIVE (0)
#define KQDIR_INACTIVE (1)

struct kevq {
	LIST_ENTRY(kevq)	kevq_th_e; /* entry into kevq_thred's hashtable */
	LIST_ENTRY(kevq)	kq_e; /* entry into kq */
	LIST_ENTRY(kevq)	kevq_th_tqe; /* entry into kevq_thred's kevq_list */
	struct		kqueue	*kq;     /* the kq that the kevq belongs to */
	struct		kqdom	*kevq_kqd; /* the kq domain the kevq is on */
	struct		kevq_thred *kevq_th; /* the thread that the kevq belongs to */
	struct		mtx lock;		/* the lock for the kevq */
	TAILQ_HEAD(, knote) kn_head;	/* list of pending knotes */
	int		kn_count;				/* number of pending knotes */
#define KEVQ_SLEEP	0x01
#define KEVQ_CLOSING  0x02
#define KEVQ_RDY	0x04
	int		kevq_state;
	int		kevq_refcnt;

	/* Used by the scheduler */
	unsigned long kevq_avg_lat;
	struct timespec kevq_last_kev;
	uint64_t kevq_last_nkev;
};

/* TODO: assumed that threads don't get rescheduled across cores */
struct kqdom {
	/* static */
	int id;
	struct mtx	kqd_lock;
	struct kqdom *parent;
	cpuset_t cpu_mask;
	struct veclist children; /* child kqdoms */

	/* statistics. Atomically updated, doesn't require the lock*/
	unsigned long avg_lat;

	/* dynamic members*/
	struct veclist kqd_activelist; /* active child kqdoms */
	struct veclist kqd_kevqs; /* kevqs for this kqdom */
	int kqd_ckevq; /* current kevq for round robbin. XXX: Remove round robbin it has literally no benefit but maintainance nightmares */
};

struct kqueue {
	struct		mtx kq_lock;
	struct		selinfo kq_sel;
	int			kq_state;
#define KQ_SEL		0x01
#define KQ_ASYNC	0x02
#define	KQ_TASKSCHED	0x04			/* task scheduled */
#define	KQ_TASKDRAIN	0x08			/* waiting for task to drain */
#define KQ_CLOSING	0x10
	int			kq_flags;
#define KQ_FLAG_INIT 0x01		/* kqueue has been initialized. this flag is set after the first kevent structure is processed */
#define KQ_FLAG_MULTI 0x02		/* Multi-threaded mode */
	TAILQ_ENTRY(kqueue)	kq_list;
	struct		sigio *kq_sigio;
	struct		filedesc *kq_fdp;
	int			kq_knlistsize;		/* size of knlist */
	struct		klist *kq_knlist;	/* list of knotes */
	u_long		kq_knhashmask;		/* size of knhash */
	struct		klist *kq_knhash;	/* hash table for knotes */
	struct		kevq *kq_kevq; /* the kevq for kq, always created, act as buffer queue in multithreaded mode */
	struct		task kq_task;
	struct		ucred *kq_cred;
	struct 		kevqlist  kq_kevqlist; /* list of kevqs */

	/* scheduling stuff */
	int			kq_sched_flags; /* Scheduler flag for the KQ */
	/* Round robbin (only as a fall back) */
	struct		kevq	*kq_ckevq; /* current kevq for multithreaded kqueue, used for round robbin */
	/* Best of two */
	struct		rwlock  sched_bot_lk;
	struct		veclist sched_bot_lst;
	/* CPU queue */
	struct		kqdom 	*kq_kqd; /* root domain */
};

#endif /* !_SYS_EVENTVAR_H_ */
