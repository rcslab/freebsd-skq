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

#define KQ_NEVENTS	8		/* minimize copy{in,out} calls */
#define KQEXTENT	256		/* linear growth by this amount */

struct kevq {
	SLIST_ENTRY(kevq)	kevq_th_e; /* entry into kevq_thred's hashtable */
	TAILQ_ENTRY(kevq)	kq_e;   /* entry into kqueue's list */
	TAILQ_ENTRY(kevq)	kevq_th_tqe; /* entry into kevq_thred's TAILQ */
	struct		kqueue *kq;     /* the kq that the kevq belongs to */
	struct		kevq_thred *kevq_th; /* the thread that the kevq belongs to */
	struct		mtx lock;		/* the lock for the kevq */
	TAILQ_HEAD(, knote) kn_head;	/* list of pending knotes */
	int		kn_count;				/* number of pending knotes */
#define KEVQ_SLEEP	0x01
#define KEVQ_CLOSING  0x02
#define KEVQ_RDY	0x04
	int		kevq_state;
	int		kevq_refcnt;
};

struct kqueue {
	struct		mtx kq_lock;
	int		kq_refcnt;
	struct		selinfo kq_sel;
	int		kq_state;
#define KQ_SEL		0x01
#define KQ_ASYNC	0x02
#define	KQ_TASKSCHED	0x04			/* task scheduled */
#define	KQ_TASKDRAIN	0x08			/* waiting for task to drain */
#define KQ_CLOSING	0x10
#define KQ_FLAG_INIT 0x20		/* kqueue has been initialized. this flag is set after the first kevent structure is processed */
#define KQ_FLAG_MULTI 0x40		/* Multi-threaded mode */
	TAILQ_ENTRY(kqueue)	kq_list;
	struct		sigio *kq_sigio;
	struct		filedesc *kq_fdp;
	int		kq_knlistsize;		/* size of knlist */
	struct		klist *kq_knlist;	/* list of knotes */
	u_long		kq_knhashmask;		/* size of knhash */
	struct		klist *kq_knhash;	/* hash table for knotes */
	/* only-set: in multithreaded mode */
	TAILQ_HEAD(, kevq)	kq_kevqlist; /* list of kevqs interested in the kqueue */
	struct		kevq	*kq_ckevq; /* current kevq for multithreaded kqueue */
	/* only-set: in single threaded mode */
	struct		kevq *kq_kevq;
	/* End only-set */
	struct		task kq_task;
	struct		ucred *kq_cred;
};

#endif /* !_SYS_EVENTVAR_H_ */
