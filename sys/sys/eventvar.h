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

#define KQDIR_ACTIVE (0)
#define KQDIR_INACTIVE (1)

struct kevq {
	/* 1st cacheline */
	/* Sched stats */
	uint64_t kevq_avg_lat;
	uint64_t kevq_avg_ev;
	uint64_t kevq_tot_ev;
	uint64_t kevq_tot_time;
	uint64_t kevq_tot_syscall;
	uint64_t kevq_last_kev;
	uint32_t kevq_last_nkev;
#define KEVQ_SLEEP	0x01
#define KEVQ_CLOSING  0x02
#define KEVQ_ACTIVE	0x04
#define KEVQ_WS 0x08 /* the kevq is work stealing */
	int		kevq_state;
	int		kn_count;				/* number of pending knotes */
	int		kn_rt_count;			/* number of runtime knotes */

	/* 2nd cacheline */
	uint64_t kevq_tot_ws;
	/* TODO: maybe these should be in kqdomain or global */
	uint64_t kevq_tot_fallback;
	uint64_t kevq_tot_kqd_mismatch;
	uint64_t kevq_tot_sched;
	uint64_t kevq_tot_realtime;

	LIST_ENTRY(kevq)	kevq_th_e; /* entry into kevq_thred's hashtable */
	LIST_ENTRY(kevq)	kq_e; /* entry into kq */
	LIST_ENTRY(kevq)	kevq_th_tqe; /* entry into kevq_thred's kevq_list */
	struct		kqueue	*kq;     /* the kq that the kevq belongs to */
	struct		kqdom	*kevq_kqd; /* the kq domain the kevq is on */
	/* XXX: Make kevq contain a struct thread ptr instead of this dude */
	struct		kevq_thred *kevq_th; /* the thread that the kevq belongs to */
	struct		mtx lock;		/* the lock for the kevq */
	struct 		ktailq kn_head;	/* list of pending knotes */
	struct 		knote kn_marker;	
	struct		ktailq kn_rt_head; /* list of pending knotes with runtime priority */
	struct		knote kn_marker_rt;	
	int		kevq_refcnt;
};

/* TODO: assumed that threads don't get rescheduled across cores */
struct kqdom {
	/* static */
	int id;
	struct rwlock kqd_lock;
	struct kqdom *parent;
	cpuset_t cpu_mask;
	struct veclist children; /* child kqdoms */

	/* statistics. Atomically updated, doesn't require the lock*/
	uint64_t avg_lat;

	/* dynamic members*/
	struct veclist kqd_activelist; /* active child kqdoms */
	struct veclist kqd_kevqs; /* kevqs for this kqdom */
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

	/* scheduler flags for the KQ, set by IOCTL */
	int			kq_sfeat;
	int			kq_ssargs;
	int 		kq_ssched;
	int			kq_sfargs;
	
	/* tuneables for the KQ, set by IOCTL */
	int		    kq_tfreq;
	int			kq_rtshare;

	/* Default */
	struct		rwlock  kevq_vlist_lk;
	struct		veclist kevq_vlist;

	/* CPU queue */
	struct		kqdom 	*kq_kqd; /* root domain */
};

#endif /* !_SYS_EVENTVAR_H_ */
