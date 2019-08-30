/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 1999,2000,2001 Jonathan Lemon <jlemon@FreeBSD.org>
 * Copyright 2004 John-Mark Gurney <jmg@FreeBSD.org>
 * Copyright (c) 2009 Apple, Inc.
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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_ktrace.h"
#include "opt_kqueue.h"

#ifdef COMPAT_FREEBSD11
#define	_WANT_FREEBSD11_KEVENT
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/capsicum.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/proc.h>
#include <sys/malloc.h>
#include <sys/unistd.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/filio.h>
#include <sys/fcntl.h>
#include <sys/kthread.h>
#include <sys/selinfo.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/eventvar.h>
#include <sys/poll.h>
#include <sys/protosw.h>
#include <sys/resourcevar.h>
#include <sys/sigio.h>
#include <sys/signalvar.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/sysproto.h>
#include <sys/syscallsubr.h>
#include <sys/taskqueue.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/ktr.h>
#include <sys/smp.h>
#include <sys/veclist.h>
#include <sys/stdint.h>
#include <sys/libkern.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#ifdef KTRACE
#include <sys/ktrace.h>
#endif
#include <machine/atomic.h>
#include <machine/cpu.h>

#include <vm/uma.h>

static MALLOC_DEFINE(M_KQUEUE, "kqueue", "memory for kqueue system");

#define KQDOM_FLAGS (KQ_SCHED_CPU | KQ_SCHED_QUEUE)
#define KEVQ_LAT_FLAGS (KQ_SCHED_CPU | KQ_SCHED_QUEUE | KQ_SCHED_BEST)

/*
 * This lock is used if multiple kq locks are required.  This possibly
 * should be made into a per proc lock.
 */
static struct mtx	kq_global;
MTX_SYSINIT(kq_global, &kq_global, "kqueue order", MTX_DEF);
#define KQ_GLOBAL_LOCK(lck, haslck)	do {	\
	if (!haslck)				\
		mtx_lock(lck);			\
	haslck = 1;				\
} while (0)
#define KQ_GLOBAL_UNLOCK(lck, haslck)	do {	\
	if (haslck)				\
		mtx_unlock(lck);			\
	haslck = 0;				\
} while (0)

TASKQUEUE_DEFINE_THREAD(kqueue_ctx);

// TODO: only use it in SMP
extern struct cpu_group *cpu_top;
#define AVG_WEIGHT_FACTOR_OLD (4)
#define AVG_WEIGHT_FACTOR_NEW (1)

/* no need to handle overflow as long as the existing org/cur doesn't overflow */
#define CALC_OVERTIME_AVG(prev, cur) (((prev) * AVG_WEIGHT_FACTOR_OLD + (cur) * AVG_WEIGHT_FACTOR_NEW) / (AVG_WEIGHT_FACTOR_OLD + AVG_WEIGHT_FACTOR_NEW))

static struct kevq * kevqlist_find(struct kevqlist *kevq_list, struct kqueue *kq); 
static void kevq_thred_init(struct kevq_thred *kevq_th);
static void kevq_thred_destroy(struct kevq_thred *kevq_th);
static void kevq_wakeup(struct kevq* kevq);
static void kevq_init(struct kevq *kevq);
static void kevq_release(struct kevq* kevq, int locked);
static void kevq_destroy(struct kevq *kevq);
static int 	kevq_acquire(struct kevq *kevq, int locked);
static void kevq_worksteal(struct kevq *kevq);
void kevq_drain(struct kevq *kevq, struct thread *td);
static void kevq_activate(struct kevq *kevq, struct thread *td);


static int kqueue_acquire_kevq(struct file *fp, struct thread *td, struct kqueue **kqp, struct kevq **kevq);
static void kqueue_ensure_kqdom(struct kqueue *kq);
static int	kevent_copyout(void *arg, struct kevent *kevp, int count);
static int	kevent_copyin(void *arg, struct kevent *kevp, int count);
static int  kqueue_register(struct kqueue *kq, struct kevq *kevq, 
				struct kevent *kev, struct thread *td, int mflag);
static int	kqueue_obtain_kevq(struct kqueue *kq, struct thread *td, struct kevq **kevqp);
static void	kqueue_destroy(struct kqueue *kq);
static void	kqueue_drain(struct kqueue *kq, struct kevq *kevq, struct thread *td);
static int	kqueue_expand(struct kqueue *kq, struct filterops *fops,
		    uintptr_t ident, int mflag);
static void	kqueue_task(void *arg, int pending);
static int	kqueue_scan(struct kevq *kq, int maxevents,
		    struct kevent_copyops *k_ops,
		    const struct timespec *timeout,
		    struct kevent *keva, struct thread *td);
static void 	kqueue_wakeup(struct kqueue *kq);
static struct filterops *kqueue_fo_find(int filt);
static void	kqueue_fo_release(int filt);
struct g_kevent_args;
static int	kern_kevent_generic(struct thread *td,
		    struct g_kevent_args *uap,
		    struct kevent_copyops *k_ops, const char *struct_name);

static fo_ioctl_t	kqueue_ioctl;
static fo_poll_t	kqueue_poll;
static fo_kqfilter_t	kqueue_kqfilter;
static fo_stat_t	kqueue_stat;
static fo_close_t	kqueue_close;
static fo_fill_kinfo_t	kqueue_fill_kinfo;

static struct fileops kqueueops = {
	.fo_read = invfo_rdwr,
	.fo_write = invfo_rdwr,
	.fo_truncate = invfo_truncate,
	.fo_ioctl = kqueue_ioctl,
	.fo_poll = kqueue_poll,
	.fo_kqfilter = kqueue_kqfilter,
	.fo_stat = kqueue_stat,
	.fo_close = kqueue_close,
	.fo_chmod = invfo_chmod,
	.fo_chown = invfo_chown,
	.fo_sendfile = invfo_sendfile,
	.fo_fill_kinfo = kqueue_fill_kinfo,
};

static bool knote_leave_flux_ul(struct knote *kn);
static bool knote_leave_flux(struct knote *kn);
static void knote_enter_flux(struct knote *kn);
static void knote_enter_flux_ul(struct knote *kn);
static void knote_flux_wakeup_ul(struct knote *kn);
static void knote_flux_wakeup(struct knote *kn);
static void knote_activate(struct knote *kn);
static int 	knote_attach(struct knote *kn, struct kqueue *kq);
static void 	knote_drop(struct knote *kn, struct thread *td);
static void 	knote_drop_detached(struct knote *kn, struct thread *td);
static void 	knote_enqueue(struct knote *kn, struct kevq *kevq);
static void 	knote_dequeue(struct knote *kn);
static void 	knote_init(void);
static struct 	knote *knote_alloc(int mflag);
static void 	knote_free(struct knote *kn);
static void knote_sched(struct knote *kn);
static struct kevq *   knote_next_kevq(struct knote *kn);


static void kqdom_init(struct kqdom *kqd);
static void kqdom_update_lat(struct kqdom *leaf, unsigned long avg);
static void kqdom_update_parents(struct kqdom *leaf, int direction);
static void kqdom_insert(struct kqdom *kqd, struct kevq *kevq);
static void kqdom_remove(struct kqdom *kqd, struct kevq *kevq);
static void kqdom_destroy(struct kqdom *root);
static struct kevq * kqdom_random_kevq_locked(struct kqdom *kqd);
static void kqdom_build_internal(struct kqdom *kqd_cur, struct cpu_group *cg_cur, int *kqd_id);
static struct kqdom * kqdom_build(void);
static struct kqdom * kqdom_find(struct kqdom *root, int cpuid);
static void kqdom_dump(struct kqdom *kqd, int level, struct sbuf *buf);

static struct kevq *kevq_vec_select_kevq(struct veclist *lst, int num_rand);

static void	filt_kqdetach(struct knote *kn);
static int	filt_kqueue(struct knote *kn, long hint);
static int	filt_procattach(struct knote *kn);
static void	filt_procdetach(struct knote *kn);
static int	filt_proc(struct knote *kn, long hint);
static int	filt_fileattach(struct knote *kn);
static void	filt_timerexpire(void *knx);
static int	filt_timerattach(struct knote *kn);
static void	filt_timerdetach(struct knote *kn);
static void	filt_timerstart(struct knote *kn, sbintime_t to);
static void	filt_timertouch(struct knote *kn, struct kevent *kev,
		    u_long type);
static int	filt_timervalidate(struct knote *kn, sbintime_t *to);
static int	filt_timer(struct knote *kn, long hint);
static int	filt_userattach(struct knote *kn);
static void	filt_userdetach(struct knote *kn);
static int	filt_user(struct knote *kn, long hint);
static void	filt_usertouch(struct knote *kn, struct kevent *kev,
		    u_long type);

static struct filterops file_filtops = {
	.f_isfd = 1,
	.f_attach = filt_fileattach,
};
static struct filterops kqread_filtops = {
	.f_isfd = 1,
	.f_detach = filt_kqdetach,
	.f_event = filt_kqueue,
};
/* XXX - move to kern_proc.c?  */
static struct filterops proc_filtops = {
	.f_isfd = 0,
	.f_attach = filt_procattach,
	.f_detach = filt_procdetach,
	.f_event = filt_proc,
};
static struct filterops timer_filtops = {
	.f_isfd = 0,
	.f_attach = filt_timerattach,
	.f_detach = filt_timerdetach,
	.f_event = filt_timer,
	.f_touch = filt_timertouch,
};
static struct filterops user_filtops = {
	.f_attach = filt_userattach,
	.f_detach = filt_userdetach,
	.f_event = filt_user,
	.f_touch = filt_usertouch,
};

static uma_zone_t	knote_zone;
static unsigned int	kq_ncallouts = 0;
static unsigned int 	kq_calloutmax = 4 * 1024;
SYSCTL_UINT(_kern, OID_AUTO, kq_calloutmax, CTLFLAG_RW,
    &kq_calloutmax, 0, "Maximum number of callouts allocated for kqueue");

//#define ENABLE_SELECT

#define KTR_KQ (KTR_SPARE5)

#define KQ_LOCK(kq) do {						\
	mtx_lock(&(kq)->kq_lock);					\
} while (0)
#define KN_FLUX_LOCK(kn) do {						\
	mtx_lock(&(kn)->kn_fluxlock);					\
} while (0)
#define KEVQ_TH_LOCK(kevqth) do {						\
	mtx_lock(&(kevqth)->lock);					\
} while (0)
#define KEVQ_LOCK(kevq) do {						\
	mtx_lock(&(kevq)->lock);					\
} while (0)
#define KQ_UNLOCK(kq) do {						\
	mtx_unlock(&(kq)->kq_lock);					\
} while (0)
#define KN_FLUX_UNLOCK(kn) do {						\
	mtx_unlock(&(kn)->kn_fluxlock);					\
} while (0)
#define KN_LEAVE_FLUX_WAKEUP(kn) do {		\
	KN_FLUX_NOTOWNED((kn));						\
	KN_FLUX_LOCK((kn));							\
	knote_leave_flux((kn)); 						\
	knote_flux_wakeup((kn)); 					\
	KN_FLUX_UNLOCK((kn));						\
} while(0)
#define KEVQ_TH_UNLOCK(kevqth) do {						\
	mtx_unlock(&(kevqth)->lock);					\
} while (0)
#define KEVQ_UNLOCK(kevq) do {						\
	mtx_unlock(&(kevq)->lock);					\
} while (0)
#define KQ_OWNED(kq) do {						\
	mtx_assert(&(kq)->kq_lock, MA_OWNED);				\
} while (0)
#define KQ_NOTOWNED(kq) do {						\
	mtx_assert(&(kq)->kq_lock, MA_NOTOWNED);			\
} while (0)
#define KN_FLUX_OWNED(kn) do {						\
	mtx_assert(&(kn)->kn_fluxlock, MA_OWNED);				\
} while (0)
#define KN_FLUX_NOTOWNED(kn) do {						\
	mtx_assert(&(kn)->kn_fluxlock, MA_NOTOWNED);			\
} while (0)
#define KEVQ_OWNED(kevq) do {						\
	mtx_assert(&(kevq)->lock, MA_OWNED);				\
} while (0)
#define KEVQ_NOTOWNED(kevq) do {						\
	mtx_assert(&(kevq)->lock, MA_NOTOWNED);			\
} while (0)

#define KQD_ROWNED(kqd) do {						\
	rw_assert(&(kqd)->kqd_lock, RA_RLOCKED);				\
} while (0)
#define KQD_WOWNED(kqd) do {						\
	rw_assert(&(kqd)->kqd_lock, RA_WLOCKED);				\
} while (0)
#define KQD_RLOCK(kqd) do {						\
	rw_rlock(&kqd->kqd_lock);					\
} while (0)
#define KQD_WLOCK(kqd) do {						\
	rw_wlock(&kqd->kqd_lock);					\
} while (0)
#define KQD_WUNLOCK(kqd) do {						\
	rw_wunlock(&kqd->kqd_lock);					\
} while (0)
#define KQD_RUNLOCK(kqd) do {						\
	rw_runlock(&kqd->kqd_lock);					\
} while (0)

#define KVLST_ROWNED(kq) do {						\
	rw_assert(&(kq)->kevq_vlist_lk, RA_RLOCKED);				\
} while (0)
#define KVLST_WOWNED(kq) do {						\
	rw_assert(&(kq)->kevq_vlist_lk, RA_WLOCKED);				\
} while (0)
#define KVLST_RLOCK(kq) do {						\
	rw_rlock(&kq->kevq_vlist_lk);					\
} while (0)
#define KVLST_WLOCK(kq) do {						\
	rw_wlock(&kq->kevq_vlist_lk);					\
} while (0)
#define KVLST_WUNLOCK(kq) do {						\
	rw_wunlock(&kq->kevq_vlist_lk);					\
} while (0)
#define KVLST_RUNLOCK(kq) do {						\
	rw_runlock(&kq->kevq_vlist_lk);					\
} while (0)

#define KQSCHED_PARSE_SCHED(sf) ((sf) & 0xFF)
#define KQSCHED_PARSE_SARGS(sf) (((sf) >> 8) & 0xFF)
#define KQSCHED_PARSE_FARGS(sf) (((sf) >> 24) & 0xFF)
#define KQSCHED_PARSE_FLAGS(sf) (((sf) >> 16) & 0xFF)

#define KQSCHED_GET_SCHED(kq) (kq->kq_ssched)
#define KQSCHED_GET_SARGS(kq) (kq->kq_ssargs)
#define KQSCHED_GET_FARGS(kq) (kq->kq_sfargs)
#define KQSCHED_GET_FLAGS(kq) (kq->kq_sflags)

static inline int
kevq_lat_cmp(struct kevq *kevq1, struct kevq *kevq2)
{
	if (kevq1 == kevq2)
		return 0;

	return kevq1->kevq_avg_lat * kevq1->kn_count - kevq2->kevq_avg_lat * kevq2->kn_count;
}

static inline int
kevq_lat_wcmp(struct kevq *kevq1, struct kevq *kevq2, int pct1)
{
	if (kevq1 == kevq2)
		return 0;

	return kevq1->kevq_avg_lat * kevq1->kn_count * pct1 / 100 - kevq2->kevq_avg_lat * kevq2->kn_count;
}

static inline int
kevq_avail(struct kevq *kevq)
{
	return (kevq->kevq_state & KEVQ_CLOSING) == 0 && (kevq->kevq_state & KEVQ_ACTIVE);
}

static inline struct kevq * 
kevq_lock_check_avail(struct kevq *next_kevq)
{
	if (next_kevq != NULL) {
		KEVQ_NOTOWNED(next_kevq);
		KEVQ_LOCK(next_kevq);
		if (!kevq_avail(next_kevq)) {
			KEVQ_UNLOCK(next_kevq);
			next_kevq = NULL;
		}
	}
	return next_kevq;
}

static struct knlist *
kn_list_lock(struct knote *kn)
{
	struct knlist *knl;

	knl = kn->kn_knlist;
	if (knl != NULL)
		knl->kl_lock(knl->kl_lockarg);
	return (knl);
}

static void
kn_list_unlock(struct knlist *knl)
{
	bool do_free;

	if (knl == NULL)
		return;
	do_free = knl->kl_autodestroy && knlist_empty(knl);
	knl->kl_unlock(knl->kl_lockarg);
	if (do_free) {
		knlist_destroy(knl);
		free(knl, M_KQUEUE);
	}
}

static bool
kn_in_flux(struct knote *kn)
{

	return (kn->kn_influx > 0);
}

static void
knote_enter_flux_ul(struct knote *kn)
{
	KN_FLUX_NOTOWNED(kn);
	KN_FLUX_LOCK(kn);
	knote_enter_flux(kn);
	KN_FLUX_UNLOCK(kn);
}

static void
knote_enter_flux(struct knote *kn)
{
	/* CTR2(KTR_KQ, "knote_enter_flux: %p flux: %d", kn, kn->kn_influx); */
	KN_FLUX_OWNED(kn);
	MPASS(kn->kn_influx < INT_MAX);
	kn->kn_influx++;
}

static bool
knote_leave_flux_ul(struct knote *kn)
{
	bool ret;
	KN_FLUX_NOTOWNED(kn);
	KN_FLUX_LOCK(kn);
	ret = knote_leave_flux(kn);
	KN_FLUX_UNLOCK(kn);
	return ret;
}

static bool
knote_leave_flux(struct knote *kn)
{
	/* CTR2(KTR_KQ, "knote_leave_flux: %p flux: %d", kn, kn->kn_influx); */
		KN_FLUX_OWNED(kn);
	MPASS(kn->kn_influx > 0);
	kn->kn_influx--;

	return (kn->kn_influx == 0);
}

#define	KNL_ASSERT_LOCK(knl, islocked) do {				\
	if (islocked)							\
		KNL_ASSERT_LOCKED(knl);				\
	else								\
		KNL_ASSERT_UNLOCKED(knl);				\
} while (0)
#ifdef INVARIANTS
#define	KNL_ASSERT_LOCKED(knl) do {					\
	knl->kl_assert_locked((knl)->kl_lockarg);			\
} while (0)
#define	KNL_ASSERT_UNLOCKED(knl) do {					\
	knl->kl_assert_unlocked((knl)->kl_lockarg);			\
} while (0)
#else /* !INVARIANTS */
#define	KNL_ASSERT_LOCKED(knl) do {} while(0)
#define	KNL_ASSERT_UNLOCKED(knl) do {} while (0)
#endif /* INVARIANTS */

#ifndef	KN_HASHSIZE
#define	KN_HASHSIZE		64		/* XXX should be tunable */
#define KEVQ_HASHSIZE   128
#endif

#define KN_HASH(val, mask)	(((val) ^ (val >> 8)) & (mask))
#define KEVQ_HASH(val, mask) KN_HASH((val), (mask))

static int
filt_nullattach(struct knote *kn)
{
	
	return (ENXIO);
};

struct filterops null_filtops = {
	.f_isfd = 0,
	.f_attach = filt_nullattach,
};

/* XXX - make SYSINIT to add these, and move into respective modules. */
extern struct filterops sig_filtops;
extern struct filterops fs_filtops;

/*
 * Table for for all system-defined filters.
 */
static struct mtx	filterops_lock;
MTX_SYSINIT(kqueue_filterops, &filterops_lock, "protect sysfilt_ops",
	MTX_DEF);
static struct {
	struct filterops *for_fop;
	int for_nolock;
	int for_refcnt;
} sysfilt_ops[EVFILT_SYSCOUNT] = {
	{ &file_filtops, 1 },			/* EVFILT_READ */
	{ &file_filtops, 1 },			/* EVFILT_WRITE */
	{ &null_filtops },			/* EVFILT_AIO */
	{ &file_filtops, 1 },			/* EVFILT_VNODE */
	{ &proc_filtops, 1 },			/* EVFILT_PROC */
	{ &sig_filtops, 1 },			/* EVFILT_SIGNAL */
	{ &timer_filtops, 1 },			/* EVFILT_TIMER */
	{ &file_filtops, 1 },			/* EVFILT_PROCDESC */
	{ &fs_filtops, 1 },			/* EVFILT_FS */
	{ &null_filtops },			/* EVFILT_LIO */
	{ &user_filtops, 1 },			/* EVFILT_USER */
	{ &null_filtops },			/* EVFILT_SENDFILE */
	{ &file_filtops, 1 },                   /* EVFILT_EMPTY */
};

/*
 * Simple redirection for all cdevsw style objects to call their fo_kqfilter
 * method.
 */
static int
filt_fileattach(struct knote *kn)
{

	return (fo_kqfilter(kn->kn_fp, kn));
}

/*ARGSUSED*/
static int
kqueue_kqfilter(struct file *fp, struct knote *kn)
{
	CTR1(KTR_KQ, "kqueue_kqfilter called for kn %p", kn);

	struct kqueue *kq = kn->kn_fp->f_data;

	if (kn->kn_filter != EVFILT_READ)
		return (EINVAL);

	kn->kn_status |= KN_KQUEUE;
	kn->kn_fop = &kqread_filtops;
	knlist_add(&kq->kq_sel.si_note, kn, 0);

	return (0);
}

static void
filt_kqdetach(struct knote *kn)
{
	struct kqueue *kq = kn->kn_fp->f_data;

	knlist_remove(&kq->kq_sel.si_note, kn, 0);
}

/*ARGSUSED*/
static int
filt_kqueue(struct knote *kn, long hint)
{
	struct kqueue *kq = kn->kn_fp->f_data;
	struct kevq *kevq;

	CTR1(KTR_KQ, "filt_kqueue called for kn %p", kn);

	if ( (kq->kq_flags & KQ_FLAG_MULTI) == KQ_FLAG_MULTI) {
		return 0;
	}

	kevq = kq->kq_kevq;

	if (kevq == NULL) {
		return 0;
	} else {
		kn->kn_data = kevq->kn_count;
	return (kn->kn_data > 0);
}
}

/* XXX - move to kern_proc.c?  */
static int
filt_procattach(struct knote *kn)
{
	struct proc *p;
	int error;
	bool exiting, immediate;

	exiting = immediate = false;
	if (kn->kn_sfflags & NOTE_EXIT)
		p = pfind_any(kn->kn_id);
	else
		p = pfind(kn->kn_id);
	if (p == NULL)
		return (ESRCH);
	if (p->p_flag & P_WEXIT)
		exiting = true;

	if ((error = p_cansee(curthread, p))) {
		PROC_UNLOCK(p);
		return (error);
	}

	kn->kn_ptr.p_proc = p;
	kn->kn_flags |= EV_CLEAR;		/* automatically set */

	/*
	 * Internal flag indicating registration done by kernel for the
	 * purposes of getting a NOTE_CHILD notification.
	 */
	if (kn->kn_flags & EV_FLAG2) {
		kn->kn_flags &= ~EV_FLAG2;
		kn->kn_data = kn->kn_sdata;		/* ppid */
		kn->kn_fflags = NOTE_CHILD;
		kn->kn_sfflags &= ~(NOTE_EXIT | NOTE_EXEC | NOTE_FORK);
		immediate = true; /* Force immediate activation of child note. */
	}
	/*
	 * Internal flag indicating registration done by kernel (for other than
	 * NOTE_CHILD).
	 */
	if (kn->kn_flags & EV_FLAG1) {
		kn->kn_flags &= ~EV_FLAG1;
	}

	knlist_add(p->p_klist, kn, 1);

	/*
	 * Immediately activate any child notes or, in the case of a zombie
	 * target process, exit notes.  The latter is necessary to handle the
	 * case where the target process, e.g. a child, dies before the kevent
	 * is registered.
	 */
	if (immediate || (exiting && filt_proc(kn, NOTE_EXIT)))
		knote_activate(kn);

	PROC_UNLOCK(p);

	return (0);
}

/*
 * The knote may be attached to a different process, which may exit,
 * leaving nothing for the knote to be attached to.  So when the process
 * exits, the knote is marked as DETACHED and also flagged as ONESHOT so
 * it will be deleted when read out.  However, as part of the knote deletion,
 * this routine is called, so a check is needed to avoid actually performing
 * a detach, because the original process does not exist any more.
 */
/* XXX - move to kern_proc.c?  */
static void
filt_procdetach(struct knote *kn)
{

	knlist_remove(kn->kn_knlist, kn, 0);
	kn->kn_ptr.p_proc = NULL;
}

/* XXX - move to kern_proc.c?  */
static int
filt_proc(struct knote *kn, long hint)
{
	struct proc *p;
	u_int event;

	CTR1(KTR_KQ, "KQUEUE: filt_proc called for kn %p", kn);

	p = kn->kn_ptr.p_proc;
	if (p == NULL) /* already activated, from attach filter */
		return (0);

	/* Mask off extra data. */
	event = (u_int)hint & NOTE_PCTRLMASK;

	/* If the user is interested in this event, record it. */
	if (kn->kn_sfflags & event)
		kn->kn_fflags |= event;

	/* Process is gone, so flag the event as finished. */
	if (event == NOTE_EXIT) {
		kn->kn_flags |= EV_EOF | EV_ONESHOT;
		kn->kn_ptr.p_proc = NULL;
		if (kn->kn_fflags & NOTE_EXIT)
			kn->kn_data = KW_EXITCODE(p->p_xexit, p->p_xsig);
		if (kn->kn_fflags == 0)
			kn->kn_flags |= EV_DROP;
		return (1);
	}

	return (kn->kn_fflags != 0);
}

/*
 * Called when the process forked. It mostly does the same as the
 * knote(), activating all knotes registered to be activated when the
 * process forked. Additionally, for each knote attached to the
 * parent, check whether user wants to track the new process. If so
 * attach a new knote to it, and immediately report an event with the
 * child's pid.
 */
void
knote_fork(struct knlist *list, struct thread *td, int pid)
{
	struct kqueue *kq;
	struct knote *kn;
	struct kevq *kevq;
	struct kevent kev;
	int error;
	int event;

	MPASS(list != NULL);
	KNL_ASSERT_LOCKED(list);
	if (SLIST_EMPTY(&list->kl_list))
		return;

	memset(&kev, 0, sizeof(kev));
	SLIST_FOREACH(kn, &list->kl_list, kn_selnext) {
		kq = kn->kn_kq;
		kevq = kn->kn_org_kevq;

		KQ_LOCK(kq);
		KN_FLUX_LOCK(kn);
		if (kn_in_flux(kn) && (kn->kn_status & KN_SCAN) == 0) {
			KN_FLUX_UNLOCK(kn);
			KQ_UNLOCK(kq);
			continue;
		}

		knote_enter_flux(kn);
		KN_FLUX_UNLOCK(kn);

		/*
		 * The same as knote(), activate the event.
		 */
		if ((kn->kn_sfflags & NOTE_TRACK) == 0) {
			event = kn->kn_fop->f_event(kn, NOTE_FORK);
			KQ_UNLOCK(kq);

			if (event)
				knote_activate(kn);

			KN_LEAVE_FLUX_WAKEUP(kn);
			continue;
		}

		/*
		 * The NOTE_TRACK case. In addition to the activation
		 * of the event, we need to register new events to
		 * track the child. Drop the locks in preparation for
		 * the call to kqueue_register().
		 */
		KQ_UNLOCK(kq);
		list->kl_unlock(list->kl_lockarg);

		/*
		 * Activate existing knote and register tracking knotes with
		 * new process.
		 *
		 * First register a knote to get just the child notice. This
		 * must be a separate note from a potential NOTE_EXIT
		 * notification since both NOTE_CHILD and NOTE_EXIT are defined
		 * to use the data field (in conflicting ways).
		 */
		kev.ident = pid;
		kev.filter = kn->kn_filter;
		kev.flags = kn->kn_flags | EV_ADD | EV_ENABLE | EV_ONESHOT |
		    EV_FLAG2;
		kev.fflags = kn->kn_sfflags;
		kev.data = kn->kn_id;		/* parent */
		kev.udata = kn->kn_kevent.udata;/* preserve udata */
		error = kqueue_register(kq, kevq, &kev, NULL, M_NOWAIT);
		if (error)
			kn->kn_fflags |= NOTE_TRACKERR;

		/*
		 * Then register another knote to track other potential events
		 * from the new process.
		 */
		kev.ident = pid;
		kev.filter = kn->kn_filter;
		kev.flags = kn->kn_flags | EV_ADD | EV_ENABLE | EV_FLAG1;
		kev.fflags = kn->kn_sfflags;
		kev.data = kn->kn_id;		/* parent */
		kev.udata = kn->kn_kevent.udata;/* preserve udata */
		error = kqueue_register(kq, kevq, &kev, NULL, M_NOWAIT);
		if (error)
			kn->kn_fflags |= NOTE_TRACKERR;
		if (kn->kn_fop->f_event(kn, NOTE_FORK))
			knote_activate(kn);
		list->kl_lock(list->kl_lockarg);
		
		KN_LEAVE_FLUX_WAKEUP(kn);
	}
}

/*
 * XXX: EVFILT_TIMER should perhaps live in kern_time.c beside the
 * interval timer support code.
 */

#define NOTE_TIMER_PRECMASK						\
    (NOTE_SECONDS | NOTE_MSECONDS | NOTE_USECONDS | NOTE_NSECONDS)

static sbintime_t
timer2sbintime(intptr_t data, int flags)
{
	int64_t secs;

        /*
         * Macros for converting to the fractional second portion of an
         * sbintime_t using 64bit multiplication to improve precision.
         */
#define NS_TO_SBT(ns) (((ns) * (((uint64_t)1 << 63) / 500000000)) >> 32)
#define US_TO_SBT(us) (((us) * (((uint64_t)1 << 63) / 500000)) >> 32)
#define MS_TO_SBT(ms) (((ms) * (((uint64_t)1 << 63) / 500)) >> 32)
	switch (flags & NOTE_TIMER_PRECMASK) {
	case NOTE_SECONDS:
#ifdef __LP64__
		if (data > (SBT_MAX / SBT_1S))
			return (SBT_MAX);
#endif
		return ((sbintime_t)data << 32);
	case NOTE_MSECONDS: /* FALLTHROUGH */
	case 0:
		if (data >= 1000) {
			secs = data / 1000;
#ifdef __LP64__
			if (secs > (SBT_MAX / SBT_1S))
				return (SBT_MAX);
#endif
			return (secs << 32 | MS_TO_SBT(data % 1000));
		}
		return (MS_TO_SBT(data));
	case NOTE_USECONDS:
		if (data >= 1000000) {
			secs = data / 1000000;
#ifdef __LP64__
			if (secs > (SBT_MAX / SBT_1S))
				return (SBT_MAX);
#endif
			return (secs << 32 | US_TO_SBT(data % 1000000));
		}
		return (US_TO_SBT(data));
	case NOTE_NSECONDS:
		if (data >= 1000000000) {
			secs = data / 1000000000;
#ifdef __LP64__
			if (secs > (SBT_MAX / SBT_1S))
				return (SBT_MAX);
#endif
			return (secs << 32 | US_TO_SBT(data % 1000000000));
		}
		return (NS_TO_SBT(data));
	default:
		break;
	}
	return (-1);
}

struct kq_timer_cb_data {
	struct callout c;
	sbintime_t next;	/* next timer event fires at */
	sbintime_t to;		/* precalculated timer period, 0 for abs */
};

static void
filt_timerexpire(void *knx)
{
	struct knote *kn;
	struct kq_timer_cb_data *kc;

	kn = knx;
	kn->kn_data++;

	knote_enter_flux_ul(kn);
	knote_activate(kn);
	KN_LEAVE_FLUX_WAKEUP(kn);

	if ((kn->kn_flags & EV_ONESHOT) != 0)
		return;
	kc = kn->kn_ptr.p_v;
	if (kc->to == 0)
		return;
	kc->next += kc->to;
	callout_reset_sbt_on(&kc->c, kc->next, 0, filt_timerexpire, kn,
	    PCPU_GET(cpuid), C_ABSOLUTE);
}

/*
 * data contains amount of time to sleep
 */
static int
filt_timervalidate(struct knote *kn, sbintime_t *to)
{
	struct bintime bt;
	sbintime_t sbt;

	if (kn->kn_sdata < 0)
		return (EINVAL);
	if (kn->kn_sdata == 0 && (kn->kn_flags & EV_ONESHOT) == 0)
		kn->kn_sdata = 1;
	/*
	 * The only fflags values supported are the timer unit
	 * (precision) and the absolute time indicator.
	 */
	if ((kn->kn_sfflags & ~(NOTE_TIMER_PRECMASK | NOTE_ABSTIME)) != 0)
		return (EINVAL);

	*to = timer2sbintime(kn->kn_sdata, kn->kn_sfflags);
	if ((kn->kn_sfflags & NOTE_ABSTIME) != 0) {
		getboottimebin(&bt);
		sbt = bttosbt(bt);
		*to -= sbt;
	}
	if (*to < 0)
		return (EINVAL);
	return (0);
}

static int
filt_timerattach(struct knote *kn)
{
	struct kq_timer_cb_data *kc;
	sbintime_t to;
	unsigned int ncallouts;
	int error;

	error = filt_timervalidate(kn, &to);
	if (error != 0)
		return (error);

	do {
		ncallouts = kq_ncallouts;
		if (ncallouts >= kq_calloutmax)
			return (ENOMEM);
	} while (!atomic_cmpset_int(&kq_ncallouts, ncallouts, ncallouts + 1));

	if ((kn->kn_sfflags & NOTE_ABSTIME) == 0)
		kn->kn_flags |= EV_CLEAR;	/* automatically set */
	kn->kn_status &= ~KN_DETACHED;		/* knlist_add clears it */
	kn->kn_ptr.p_v = kc = malloc(sizeof(*kc), M_KQUEUE, M_WAITOK);
	callout_init(&kc->c, 1);
	filt_timerstart(kn, to);

	return (0);
}

static void
filt_timerstart(struct knote *kn, sbintime_t to)
{
	struct kq_timer_cb_data *kc;

	kc = kn->kn_ptr.p_v;
	if ((kn->kn_sfflags & NOTE_ABSTIME) != 0) {
		kc->next = to;
		kc->to = 0;
	} else {
		kc->next = to + sbinuptime();
		kc->to = to;
	}
	callout_reset_sbt_on(&kc->c, kc->next, 0, filt_timerexpire, kn,
	    PCPU_GET(cpuid), C_ABSOLUTE);
}

static void
filt_timerdetach(struct knote *kn)
{
	struct kq_timer_cb_data *kc;
	unsigned int old __unused;

	kc = kn->kn_ptr.p_v;
	callout_drain(&kc->c);
	free(kc, M_KQUEUE);
	old = atomic_fetchadd_int(&kq_ncallouts, -1);
	KASSERT(old > 0, ("Number of callouts cannot become negative"));
	kn->kn_status |= KN_DETACHED;	/* knlist_remove sets it */
}

static void
filt_timertouch(struct knote *kn, struct kevent *kev, u_long type)
{
	struct kq_timer_cb_data *kc;	
	struct kqueue *kq;
	struct kevq *kevq;
	sbintime_t to;
	int error;

	switch (type) {
	case EVENT_REGISTER:
		/* Handle re-added timers that update data/fflags */
		if (kev->flags & EV_ADD) {
			kc = kn->kn_ptr.p_v;

			/* Drain any existing callout. */
			callout_drain(&kc->c);

			/* Throw away any existing undelivered record
			 * of the timer expiration. This is done under
			 * the presumption that if a process is
			 * re-adding this timer with new parameters,
			 * it is no longer interested in what may have
			 * happened under the old parameters. If it is
			 * interested, it can wait for the expiration,
			 * delete the old timer definition, and then
			 * add the new one.
			 *
			 * This has to be done while the kq is locked:
			 *   - if enqueued, dequeue
			 *   - make it no longer active
			 *   - clear the count of expiration events
			 */
			kq = kn->kn_kq;
			kevq = kn->kn_kevq;
			KQ_LOCK(kq);
			if (kn->kn_status & KN_QUEUED) {
				KEVQ_LOCK(kevq);
				knote_dequeue(kn);
				KEVQ_UNLOCK(kevq);
			}

			kn->kn_status &= ~KN_ACTIVE;
			kn->kn_data = 0;
			KQ_UNLOCK(kq);
			
			/* Reschedule timer based on new data/fflags */
			kn->kn_sfflags = kev->fflags;
			kn->kn_sdata = kev->data;
			error = filt_timervalidate(kn, &to);
			if (error != 0) {
			  	kn->kn_flags |= EV_ERROR;
				kn->kn_data = error;
			} else
			  	filt_timerstart(kn, to);
		}
		break;

        case EVENT_PROCESS:
		*kev = kn->kn_kevent;
		if (kn->kn_flags & EV_CLEAR) {
			kn->kn_data = 0;
			kn->kn_fflags = 0;
		}
		break;

	default:
		panic("filt_timertouch() - invalid type (%ld)", type);
		break;
	}
}

static int
filt_timer(struct knote *kn, long hint)
{
	CTR1(KTR_KQ, "filt_timer called for kn %p", kn);
	return (kn->kn_data != 0);
}

static int
filt_userattach(struct knote *kn)
{
	
	/* 
	 * EVFILT_USER knotes are not attached to anything in the kernel.
	 */ 
	kn->kn_hook = NULL;
	if (kn->kn_fflags & NOTE_TRIGGER)
		kn->kn_hookid = 1;
	else
		kn->kn_hookid = 0;
	return (0);
}

static void
filt_userdetach(__unused struct knote *kn)
{

	/*
	 * EVFILT_USER knotes are not attached to anything in the kernel.
	 */
}

static int
filt_user(struct knote *kn, __unused long hint)
{
	CTR1(KTR_KQ, "KQUEUE: filt_user called for kn %p", kn);
	return (kn->kn_hookid);
}

static void
filt_usertouch(struct knote *kn, struct kevent *kev, u_long type)
{
	u_int ffctrl;

	switch (type) {
	case EVENT_REGISTER:
		if (kev->fflags & NOTE_TRIGGER)
			kn->kn_hookid = 1;

		ffctrl = kev->fflags & NOTE_FFCTRLMASK;
		kev->fflags &= NOTE_FFLAGSMASK;
		switch (ffctrl) {
		case NOTE_FFNOP:
			break;

		case NOTE_FFAND:
			kn->kn_sfflags &= kev->fflags;
			break;

		case NOTE_FFOR:
			kn->kn_sfflags |= kev->fflags;
			break;

		case NOTE_FFCOPY:
			kn->kn_sfflags = kev->fflags;
			break;

		default:
			/* XXX Return error? */
			break;
		}
		kn->kn_sdata = kev->data;
		if (kev->flags & EV_CLEAR) {
			kn->kn_hookid = 0;
			kn->kn_data = 0;
			kn->kn_fflags = 0;
		}
		break;

        case EVENT_PROCESS:
		*kev = kn->kn_kevent;
		kev->fflags = kn->kn_sfflags;
		kev->data = kn->kn_sdata;
		if (kn->kn_flags & EV_CLEAR) {
			kn->kn_hookid = 0;
			kn->kn_data = 0;
			kn->kn_fflags = 0;
		}
		break;

	default:
		panic("filt_usertouch() - invalid type (%ld)", type);
		break;
	}
}

int
sys_kqueue(struct thread *td, struct kqueue_args *uap)
{
	return (kern_kqueue(td, 0, NULL));
}

static void
kqueue_init(struct kqueue *kq)
{
	mtx_init(&kq->kq_lock, "kqueue", NULL, MTX_DEF | MTX_DUPOK);
	knlist_init_mtx(&kq->kq_sel.si_note, &kq->kq_lock);
	TASK_INIT(&kq->kq_task, 0, kqueue_task, kq);

	/* XXX: move these guys to init only when needed */
	veclist_init(&kq->kevq_vlist, 0, M_KQUEUE);
	rw_init(&kq->kevq_vlist_lk, "kevq_vlist_lk");
	kqueue_ensure_kqdom(kq);
}

int
kern_kqueue(struct thread *td, int flags, struct filecaps *fcaps)
{
	struct filedesc *fdp;
	struct kqueue *kq;
	struct file *fp;
	struct ucred *cred;
	int fd, error;

	fdp = td->td_proc->p_fd;
	cred = td->td_ucred;
	if (!chgkqcnt(cred->cr_ruidinfo, 1, lim_cur(td, RLIMIT_KQUEUES)))
		return (ENOMEM);

	error = falloc_caps(td, &fp, &fd, flags, fcaps);
	if (error != 0) {
		chgkqcnt(cred->cr_ruidinfo, -1, 0);
		return (error);
	}

	/* An extra reference on `fp' has been held for us by falloc(). */
	kq = malloc(sizeof *kq, M_KQUEUE, M_WAITOK | M_ZERO);
	kqueue_init(kq);
	kq->kq_fdp = fdp;
	kq->kq_cred = crhold(cred);

	FILEDESC_XLOCK(fdp);
	TAILQ_INSERT_HEAD(&fdp->fd_kqlist, kq, kq_list);
	FILEDESC_XUNLOCK(fdp);

	finit(fp, FREAD | FWRITE, DTYPE_KQUEUE, kq, &kqueueops);
	fdrop(fp, td);

	td->td_retval[0] = fd;
	return (0);
}

struct g_kevent_args {
	int	fd;
	void	*changelist;
	int	nchanges;
	void	*eventlist;
	int	nevents;
	const struct timespec *timeout;
};

int
sys_kevent(struct thread *td, struct kevent_args *uap)
{
	struct kevent_copyops k_ops = {
		.arg = uap,
		.k_copyout = kevent_copyout,
		.k_copyin = kevent_copyin,
		.kevent_size = sizeof(struct kevent),
	};
	struct g_kevent_args gk_args = {
		.fd = uap->fd,
		.changelist = uap->changelist,
		.nchanges = uap->nchanges,
		.eventlist = uap->eventlist,
		.nevents = uap->nevents,
		.timeout = uap->timeout,
	};

	return (kern_kevent_generic(td, &gk_args, &k_ops, "kevent"));
}

static int
kern_kevent_generic(struct thread *td, struct g_kevent_args *uap,
    struct kevent_copyops *k_ops, const char *struct_name)
{
	struct timespec ts, *tsp;
#ifdef KTRACE
	struct kevent *eventlist = uap->eventlist;
#endif
	int error;

	if (uap->timeout != NULL) {
		error = copyin(uap->timeout, &ts, sizeof(ts));
		if (error)
			return (error);
		tsp = &ts;
	} else
		tsp = NULL;

#ifdef KTRACE
	if (KTRPOINT(td, KTR_STRUCT_ARRAY))
		ktrstructarray(struct_name, UIO_USERSPACE, uap->changelist,
		    uap->nchanges, k_ops->kevent_size);
#endif

	error = kern_kevent(td, uap->fd, uap->nchanges, uap->nevents,
	    k_ops, tsp);

#ifdef KTRACE
	if (error == 0 && KTRPOINT(td, KTR_STRUCT_ARRAY))
		ktrstructarray(struct_name, UIO_USERSPACE, eventlist,
		    td->td_retval[0], k_ops->kevent_size);
#endif

	return (error);
}

/*
 * Copy 'count' items into the destination list pointed to by uap->eventlist.
 */
static int
kevent_copyout(void *arg, struct kevent *kevp, int count)
{
	struct kevent_args *uap;
	int error;

	KASSERT(count <= KQ_NEVENTS, ("count (%d) > KQ_NEVENTS", count));
	uap = (struct kevent_args *)arg;

	error = copyout(kevp, uap->eventlist, count * sizeof *kevp);
	if (error == 0)
		uap->eventlist += count;
	return (error);
}

/*
 * Copy 'count' items from the list pointed to by uap->changelist.
 */
static int
kevent_copyin(void *arg, struct kevent *kevp, int count)
{
	struct kevent_args *uap;
	int error;

	KASSERT(count <= KQ_NEVENTS, ("count (%d) > KQ_NEVENTS", count));
	uap = (struct kevent_args *)arg;

	error = copyin(uap->changelist, kevp, count * sizeof *kevp);
	if (error == 0)
		uap->changelist += count;
	return (error);
}

#ifdef COMPAT_FREEBSD11
static int
kevent11_copyout(void *arg, struct kevent *kevp, int count)
{
	struct freebsd11_kevent_args *uap;
	struct kevent_freebsd11 kev11;
	int error, i;

	KASSERT(count <= KQ_NEVENTS, ("count (%d) > KQ_NEVENTS", count));
	uap = (struct freebsd11_kevent_args *)arg;

	for (i = 0; i < count; i++) {
		kev11.ident = kevp->ident;
		kev11.filter = kevp->filter;
		kev11.flags = kevp->flags;
		kev11.fflags = kevp->fflags;
		kev11.data = kevp->data;
		kev11.udata = kevp->udata;
		error = copyout(&kev11, uap->eventlist, sizeof(kev11));
		if (error != 0)
			break;
		uap->eventlist++;
		kevp++;
	}
	return (error);
}

/*
 * Copy 'count' items from the list pointed to by uap->changelist.
 */
static int
kevent11_copyin(void *arg, struct kevent *kevp, int count)
{
	struct freebsd11_kevent_args *uap;
	struct kevent_freebsd11 kev11;
	int error, i;

	KASSERT(count <= KQ_NEVENTS, ("count (%d) > KQ_NEVENTS", count));
	uap = (struct freebsd11_kevent_args *)arg;

	for (i = 0; i < count; i++) {
		error = copyin(uap->changelist, &kev11, sizeof(kev11));
		if (error != 0)
			break;
		kevp->ident = kev11.ident;
		kevp->filter = kev11.filter;
		kevp->flags = kev11.flags;
		kevp->fflags = kev11.fflags;
		kevp->data = (uintptr_t)kev11.data;
		kevp->udata = kev11.udata;
		bzero(&kevp->ext, sizeof(kevp->ext));
		uap->changelist++;
		kevp++;
	}
	return (error);
}

int
freebsd11_kevent(struct thread *td, struct freebsd11_kevent_args *uap)
{
	struct kevent_copyops k_ops = {
		.arg = uap,
		.k_copyout = kevent11_copyout,
		.k_copyin = kevent11_copyin,
		.kevent_size = sizeof(struct kevent_freebsd11),
	};
	struct g_kevent_args gk_args = {
		.fd = uap->fd,
		.changelist = uap->changelist,
		.nchanges = uap->nchanges,
		.eventlist = uap->eventlist,
		.nevents = uap->nevents,
		.timeout = uap->timeout,
	};

	return (kern_kevent_generic(td, &gk_args, &k_ops, "kevent_freebsd11"));
}
#endif

int
kern_kevent(struct thread *td, int fd, int nchanges, int nevents,
    struct kevent_copyops *k_ops, const struct timespec *timeout)
{
	cap_rights_t rights;
	struct file *fp;
	int error;

	cap_rights_init(&rights);
	if (nchanges > 0)
		cap_rights_set(&rights, CAP_KQUEUE_CHANGE);
	if (nevents > 0)
		cap_rights_set(&rights, CAP_KQUEUE_EVENT);
	error = fget(td, fd, &rights, &fp);
	if (error != 0)
		return (error);

	error = kern_kevent_fp(td, fp, nchanges, nevents, k_ops, timeout);
	fdrop(fp, td);

	return (error);
}

static struct kevq *
kevqlist_find(struct kevqlist *kevq_list, struct kqueue *kq) 
{
	struct kevq *kevq_found, *kevq_each, *tkevq;

	kevq_found = NULL;

	LIST_FOREACH_SAFE(kevq_each, kevq_list, kevq_th_e, tkevq) {
		if (kevq_each->kq == kq) {
			kevq_found = kevq_each;
			break;
		}
	}

	return kevq_found;
}


static int
kqueue_kevent(struct kqueue *kq, struct kevq *kevq, struct thread *td, int nchanges, int nevents,
    struct kevent_copyops *k_ops, const struct timespec *timeout)
{
	struct kevent keva[KQ_NEVENTS];
	struct kevent *kevp, *changes;
	int i, n, nerrors, error;
	uint64_t avg;
	uint64_t cur_ts;

	if ((KQSCHED_GET_SCHED(kq) & KEVQ_LAT_FLAGS) && (kevq->kevq_state & KEVQ_ACTIVE))
	{
		/* prob don't need the lock here as these are only accessible by one thread */
		if (kevq->kevq_last_nkev != 0)
		{
			/* make sure we actually processed events last time */
			cur_ts = get_cyclecount();

			CTR3(KTR_KQ, "kevent: td %d nkev %d kevent (enter) %ld ns", td->td_tid, kevq->kevq_last_nkev, cur_ts);

			cur_ts = cur_ts - kevq->kevq_last_kev;

			CTR3(KTR_KQ, "kevent: td %d nkev %d kevent (delta) %ld ns", td->td_tid, kevq->kevq_last_nkev, cur_ts);
			
			/* divide by the number of events processed */
			avg = cur_ts / kevq->kevq_last_nkev;

			CTR3(KTR_KQ, "kevent: td %d nkev %d kevent (avg) %ld ns", td->td_tid, kevq->kevq_last_nkev, avg);
			
			if (kevq->kevq_avg_lat != 0) {
				kevq->kevq_avg_lat = CALC_OVERTIME_AVG(kevq->kevq_avg_lat, avg);
			} else {
				kevq->kevq_avg_lat = avg;
			}

			CTR3(KTR_KQ, "kevent: td %d nkev %d kevent (new avg) %ld ns", td->td_tid, kevq->kevq_last_nkev, kevq->kevq_avg_lat);
			/* clear parameters */
			kevq->kevq_last_kev = 0;
			kevq->kevq_last_nkev = 0;
			
			//kqdom_update_lat(kevq->kevq_kqd, avg);
		}
	}

	nerrors = 0;
	while (nchanges > 0) {
		n = nchanges > KQ_NEVENTS ? KQ_NEVENTS : nchanges;
		error = k_ops->k_copyin(k_ops->arg, keva, n);
		if (error)
			return (error);
		changes = keva;
		for (i = 0; i < n; i++) {
			kevp = &changes[i];
			if (!kevp->filter)
				continue;
			kevp->flags &= ~EV_SYSFLAGS;
			error = kqueue_register(kq, kevq, kevp, td, M_WAITOK);
			if (error || (kevp->flags & EV_RECEIPT)) {
				if (nevents == 0)
					return (error);
				kevp->flags = EV_ERROR;
				kevp->data = error;
				(void)k_ops->k_copyout(k_ops->arg, kevp, 1);
				nevents--;
				nerrors++;
			}
		}
		nchanges -= n;
	}
	if (nerrors) {
		td->td_retval[0] = nerrors;
		return (0);
	}

	return (kqueue_scan(kevq, nevents, k_ops, timeout, keva, td));
}

int
kern_kevent_fp(struct thread *td, struct file *fp, int nchanges, int nevents,
    struct kevent_copyops *k_ops, const struct timespec *timeout)
{
	struct kqueue *kq;
	struct kevq *kevq;
	int error;

	error = kqueue_acquire_kevq(fp, td, &kq, &kevq);

	if (error != 0)
		return (error);

	error = kqueue_kevent(kq, kevq, td, nchanges, nevents, k_ops, timeout);
	kevq_release(kevq, 0);
	return (error);
}

/*
 * Performs a kevent() call on a temporarily created kqueue. This can be
 * used to perform one-shot polling, similar to poll() and select().
 */
int
kern_kevent_anonymous(struct thread *td, int nevents,
    struct kevent_copyops *k_ops)
{
	struct kqueue kq = {};
	struct kevq kevq = {};
	int error;

	kqueue_init(&kq);
	kevq_init(&kevq);
	kq.kq_kevq = &kevq;
	kevq.kq = &kq;
	kevq.kevq_refcnt = 1;
	error = kqueue_kevent(&kq, &kevq, td, nevents, nevents, k_ops, NULL);
	// TODO: kevq destroy called here but memory not dynamically allocated
	kqueue_drain(&kq, &kevq, td);
	kqueue_destroy(&kq);
	return (error);
}

int
kqueue_add_filteropts(int filt, struct filterops *filtops)
{
	int error;

	error = 0;
	if (filt > 0 || filt + EVFILT_SYSCOUNT < 0) {
		CTR2(KTR_KQ, "trying to add a filterop that is out of range: %d is beyond %d", ~filt, EVFILT_SYSCOUNT);
		return EINVAL;
	}
	mtx_lock(&filterops_lock);
	if (sysfilt_ops[~filt].for_fop != &null_filtops &&
	    sysfilt_ops[~filt].for_fop != NULL)
		error = EEXIST;
	else {
		sysfilt_ops[~filt].for_fop = filtops;
		sysfilt_ops[~filt].for_refcnt = 0;
	}
	mtx_unlock(&filterops_lock);

	return (error);
}

int
kqueue_del_filteropts(int filt)
{
	int error;

	error = 0;
	if (filt > 0 || filt + EVFILT_SYSCOUNT < 0)
		return EINVAL;

	mtx_lock(&filterops_lock);
	if (sysfilt_ops[~filt].for_fop == &null_filtops ||
	    sysfilt_ops[~filt].for_fop == NULL)
		error = EINVAL;
	else if (sysfilt_ops[~filt].for_refcnt != 0)
		error = EBUSY;
	else {
		sysfilt_ops[~filt].for_fop = &null_filtops;
		sysfilt_ops[~filt].for_refcnt = 0;
	}
	mtx_unlock(&filterops_lock);

	return error;
}

static struct filterops *
kqueue_fo_find(int filt)
{

	if (filt > 0 || filt + EVFILT_SYSCOUNT < 0)
		return NULL;

	if (sysfilt_ops[~filt].for_nolock)
		return sysfilt_ops[~filt].for_fop;

	mtx_lock(&filterops_lock);
	sysfilt_ops[~filt].for_refcnt++;
	if (sysfilt_ops[~filt].for_fop == NULL)
		sysfilt_ops[~filt].for_fop = &null_filtops;
	mtx_unlock(&filterops_lock);

	return sysfilt_ops[~filt].for_fop;
}

static void
kqueue_fo_release(int filt)
{

	if (filt > 0 || filt + EVFILT_SYSCOUNT < 0)
		return;

	if (sysfilt_ops[~filt].for_nolock)
		return;

	mtx_lock(&filterops_lock);
	KASSERT(sysfilt_ops[~filt].for_refcnt > 0,
	    ("filter object refcount not valid on release"));
	sysfilt_ops[~filt].for_refcnt--;
	mtx_unlock(&filterops_lock);
}


static int
kqueue_register(struct kqueue *kq, struct kevq *kevq, struct kevent *kev, struct thread *td,
    int mflag)
{
	struct filterops *fops;
	struct file *fp;
	struct knote *kn, *tkn;
	struct knlist *knl;
	int error, filt;
	int haskqglobal, filedesc_unlock;

	CTR5(KTR_KQ, "kqueue_register: kq %p, kevq %p, ident: %d, filter: %d, flags: 0x%X", kq, kevq, (int)kev->ident, kev->filter, kev->flags);

	if ((kev->flags & (EV_ENABLE | EV_DISABLE)) == (EV_ENABLE | EV_DISABLE))
		return (EINVAL);

	fp = NULL;
	kn = NULL;
	knl = NULL;
	error = 0;
	haskqglobal = 0;
	filedesc_unlock = 0;

	filt = kev->filter;
	fops = kqueue_fo_find(filt);
	if (fops == NULL)
		return EINVAL;

	if (kev->flags & EV_ADD) {
		/*
		 * Prevent waiting with locks.  Non-sleepable
		 * allocation failures are handled in the loop, only
		 * if the spare knote appears to be actually required.
		 */
		tkn = knote_alloc(mflag);
	} else {
		tkn = NULL;
	}

findkn:
	if (fops->f_isfd) {
		KASSERT(td != NULL, ("td is NULL"));
		if (kev->ident > INT_MAX)
			error = EBADF;
		else
			error = fget(td, kev->ident, &cap_event_rights, &fp);
		if (error)
			goto done;

		if ((kev->flags & EV_ADD) == EV_ADD && kqueue_expand(kq, fops,
		    kev->ident, M_NOWAIT) != 0) {
			/* try again */
			fdrop(fp, td);
			fp = NULL;
			error = kqueue_expand(kq, fops, kev->ident, mflag);
			if (error)
				goto done;
			goto findkn;
		}

		if (fp->f_type == DTYPE_KQUEUE) {
			/*
			 * If we add some intelligence about what we are doing,
			 * we should be able to support events on ourselves.
			 * We need to know when we are doing this to prevent
			 * getting both the knlist lock and the kq lock since
			 * they are the same thing.
			 */
			if (fp->f_data == kq) {
				error = EINVAL;
				goto done;
			}

			/*
			 * Pre-lock the filedesc before the global
			 * lock mutex, see the comment in
			 * kqueue_close().
			 */
			FILEDESC_XLOCK(td->td_proc->p_fd);
			filedesc_unlock = 1;
			KQ_GLOBAL_LOCK(&kq_global, haskqglobal);
		}

		/* lock the kq lock for accessing kq_knhash table */
		KQ_LOCK(kq);
		if (kev->ident < kq->kq_knlistsize) {
			SLIST_FOREACH(kn, &kq->kq_knlist[kev->ident], kn_link)
				if (kev->filter == kn->kn_filter)
					break;
		}
	} else {
		if ((kev->flags & EV_ADD) == EV_ADD) {
			error = kqueue_expand(kq, fops, kev->ident, mflag);
			if (error != 0)
				goto done;
		}

		/* lock the kq lock for accessing kq_knhash table */
		KQ_LOCK(kq);

		/*
		 * If possible, find an existing knote to use for this kevent.
		 */
		if (kev->filter == EVFILT_PROC &&
		    (kev->flags & (EV_FLAG1 | EV_FLAG2)) != 0) {
			/* This is an internal creation of a process tracking
			 * note. Don't attempt to coalesce this with an
			 * existing note.
			 */
			;			
		} else if (kq->kq_knhashmask != 0) {
			struct klist *list;

			list = &kq->kq_knhash[
			    KN_HASH((u_long)kev->ident, kq->kq_knhashmask)];
			SLIST_FOREACH(kn, list, kn_link)
				if (kev->ident == kn->kn_id &&
				    kev->filter == kn->kn_filter)
					break;
		}
	}

	/* We need the kq lock because attaching to KQ requires KQ Lock */
	KQ_OWNED(kq);

	/* knote is in the process of changing, wait for it to stabilize. */
	if (kn != NULL) {
		KN_FLUX_LOCK(kn);
		if (kn_in_flux(kn)) {
			KQ_GLOBAL_UNLOCK(&kq_global, haskqglobal);
			if (filedesc_unlock) {
				FILEDESC_XUNLOCK(td->td_proc->p_fd);
				filedesc_unlock = 0;
			}
			kn->kn_fluxwait = 1;
			KQ_UNLOCK(kq);
			msleep(kn, &kn->kn_fluxlock, PSOCK | PDROP, "kqflxwt", 0);
	
			if (fp != NULL) {
				fdrop(fp, td);
				fp = NULL;
			}
			goto findkn;
		}
	}
	/* We now have exclusive access to the knote with flux lock and kq lock */

	/*
	 * kn now contains the matching knote, or NULL if no match
	 */
	if (kn == NULL) {
		if (kev->flags & EV_ADD) {
			kn = tkn;
			tkn = NULL;
			if (kn == NULL) {
				KQ_UNLOCK(kq);
				error = ENOMEM;
				goto done;
			}
			kn->kn_kevq = NULL;
			// this is set later depending on the scheduled CPU
			kn->kn_kqd = NULL;
			kn->kn_fp = fp;
			kn->kn_kq = kq;
			kn->kn_fop = fops;
			/*
			 * apply reference counts to knote structure, and
			 * do not release it at the end of this routine.
			 */
			fops = NULL;
			fp = NULL;

			kn->kn_sfflags = kev->fflags;
			kn->kn_sdata = kev->data;
			kev->fflags = 0;
			kev->data = 0;
			kn->kn_kevent = *kev;
			kn->kn_kevent.flags &= ~(EV_ADD | EV_DELETE |
			    EV_ENABLE | EV_DISABLE | EV_FORCEONESHOT);
			kn->kn_status = KN_DETACHED;
			if ((kev->flags & EV_DISABLE) != 0)
				kn->kn_status |= KN_DISABLED;
			knote_enter_flux_ul(kn);

			error = knote_attach(kn, kq);
			KQ_UNLOCK(kq);
			if (error != 0) {
				tkn = kn;
				goto done;
			}

			if ((error = kn->kn_fop->f_attach(kn)) != 0) {
				knote_drop_detached(kn, td);
				goto done;
			}
			knl = kn_list_lock(kn);
			goto done_ev_add;
		} else {
			/* No matching knote and the EV_ADD flag is not set. */
			KQ_UNLOCK(kq);
			error = ENOENT;
			goto done;
		}
	}
	
	if (kev->flags & EV_DELETE) {
		/* We have the exclusive flux lock here */
		knote_enter_flux(kn);

		KN_FLUX_UNLOCK(kn);
		KQ_UNLOCK(kq);

		knote_drop(kn, td);
		goto done;
	}

	/* We have the exclusive lock */
	knote_enter_flux(kn);
	KN_FLUX_UNLOCK(kn);
	KQ_UNLOCK(kq);

	// we have kq lock and knote influx
	if (kev->flags & EV_FORCEONESHOT) {
		kn->kn_flags |= EV_ONESHOT;

		knote_activate(kn);
	}

	if ((kev->flags & EV_ENABLE) != 0)
		kn->kn_status &= ~KN_DISABLED;
	else if ((kev->flags & EV_DISABLE) != 0)
		kn->kn_status |= KN_DISABLED;

	/*
	 * The user may change some filter values after the initial EV_ADD,
	 * but doing so will not reset any filter which has already been
	 * triggered.
	 */
	kn->kn_status |= KN_SCAN;
	
	knl = kn_list_lock(kn);
	kn->kn_kevent.udata = kev->udata;
	if (!fops->f_isfd && fops->f_touch != NULL) {
		fops->f_touch(kn, kev, EVENT_REGISTER);
	} else {
		kn->kn_sfflags = kev->fflags;
		kn->kn_sdata = kev->data;
	}

done_ev_add:
	/*
	 * We can get here with kn->kn_knlist == NULL.  This can happen when
	 * the initial attach event decides that the event is "completed" 
	 * already, e.g., filt_procattach() is called on a zombie process.  It
	 * will call filt_proc() which will remove it from the list, and NULL
	 * kn_knlist.
	 *
	 * KN_DISABLED will be stable while the knote is in flux, so the
	 * unlocked read will not race with an update.
	 */
	if ((kn->kn_status & KN_DISABLED) == 0 && kn->kn_fop->f_event(kn, 0))
			kn->kn_status |= KN_ACTIVE;

	if ((kn->kn_status & (KN_ACTIVE | KN_DISABLED | KN_QUEUED)) == KN_ACTIVE)
		knote_activate(kn);
	
	kn->kn_status &= ~KN_SCAN;
	KN_LEAVE_FLUX_WAKEUP(kn);

	kn_list_unlock(knl);

done:
	KQ_GLOBAL_UNLOCK(&kq_global, haskqglobal);
	if (filedesc_unlock)
		FILEDESC_XUNLOCK(td->td_proc->p_fd);
	if (fp != NULL)
		fdrop(fp, td);
	knote_free(tkn);
	if (fops != NULL)
		kqueue_fo_release(filt);
	return (error);
}

static void 
kevq_thred_init(struct kevq_thred *kevq_th) {
	mtx_init(&kevq_th->lock, "kevq_th", NULL, MTX_DEF | MTX_DUPOK);
	LIST_INIT(&kevq_th->kevq_list);
}

static void
kevq_thred_destroy(struct kevq_thred *kevq_th) {
	mtx_destroy(&kevq_th->lock);
	free(kevq_th->kevq_hash, M_KQUEUE);
	free(kevq_th, M_KQUEUE);
	CTR1(KTR_KQ, "kevq_thred_destroy: freed kevq_th %p", kevq_th);
}

void
kevq_thred_drain(struct kevq_thred *kevq_th, struct thread* td) {
	struct kevq *kevq;
	
	CTR2(KTR_KQ, "kevq_thred_drain: draining kevq_th %p on thread %d", kevq_th, td->td_tid);
	
	KEVQ_TH_LOCK(kevq_th);
	while((kevq = LIST_FIRST(&kevq_th->kevq_list)) != NULL) {
		if (kevq_acquire(kevq, 0) == 0) {
			CTR2(KTR_KQ, "kevq_thred_drain: draining kevq %p on kevq_th %p", kevq, kevq_th);
			KEVQ_TH_UNLOCK(kevq_th);
			kevq_drain(kevq, td);
			KEVQ_TH_LOCK(kevq_th);
		}
	}
	KEVQ_TH_UNLOCK(kevq_th);

	kevq_thred_destroy(kevq_th);
}

static void
kevq_init(struct kevq *kevq) {
	mtx_init(&kevq->lock, "kevq", NULL, MTX_DEF | MTX_DUPOK);
	TAILQ_INIT(&kevq->kn_head);
	kevq->kevq_last_kev = 0;
}

static void
kevq_release(struct kevq* kevq, int locked)
{
	if (locked)
		KEVQ_OWNED(kevq);
	else
		KEVQ_LOCK(kevq);
	/* CTR2(KTR_KQ, "releasing kevq %p (refcnt = %d)", kevq, kevq->kevq_refcnt); */
	kevq->kevq_refcnt--;
	if (kevq->kevq_refcnt == 1)
		wakeup(&kevq->kevq_refcnt);
	if (!locked)
		KEVQ_UNLOCK(kevq);
}

static int
kevq_acquire(struct kevq *kevq, int locked)
{
	int error;
	if (locked) {
		KEVQ_OWNED(kevq);
	} else {
		KEVQ_LOCK(kevq);
	}
	error = 0;
	/* CTR2(KTR_KQ, "referencing kevq %p (refcnt = %d)", kevq, kevq->kevq_refcnt); */
	if ((kevq->kevq_state & KEVQ_CLOSING) == KEVQ_CLOSING) {
		error = EINVAL;
	} else {
		kevq->kevq_refcnt++;
	}

	if (!locked) {
		KEVQ_UNLOCK(kevq);
	}
	return error;
}

static int
kqueue_obtain_kevq(struct kqueue *kq, struct thread *td, struct kevq **kevqp)
{
 	void *to_free;
	struct kevq_thred *kevq_th;
	struct kevq *kevq, *alloc_kevq;
	struct kevqlist *kevq_list;

	kevq = NULL;
	to_free = NULL;
	kevq_th = NULL;

	KQ_NOTOWNED(kq);

	if ((kq->kq_state & KQ_CLOSING) == KQ_CLOSING) {
		return EINVAL;
	}

	if ((kq->kq_flags & KQ_FLAG_MULTI) == KQ_FLAG_MULTI) {
		if (td->td_kevq_thred == NULL) {

			/* allocate kevq_thred for each thread */
			kevq_th = malloc(sizeof(struct kevq_thred), M_KQUEUE, M_WAITOK | M_ZERO);

			kevq_thred_init(kevq_th);
			kevq_th->kevq_hash = hashinit_flags(KEVQ_HASHSIZE, M_KQUEUE, &kevq_th->kevq_hashmask, HASH_WAITOK);

			thread_lock(td);
			if (td->td_kevq_thred == NULL) {
				td->td_kevq_thred = kevq_th;
				CTR2(KTR_KQ, "kqueue_ensure_kevq(M): allocated kevq_th %p for thread %d", kevq_th, td->td_tid);
			} else {
				to_free = kevq_th;
				kevq_th = td->td_kevq_thred;
			}
			thread_unlock(td);

			if (to_free != NULL) {
				free(((struct kevq_thred *)to_free)->kevq_hash, M_KQUEUE);
				free(to_free, M_KQUEUE);
			}
		} else {
			kevq_th = td->td_kevq_thred;
		}
		
		KASSERT(kevq_th != NULL && kevq_th->kevq_hashmask != 0, ("unallocated kevq"));

		KEVQ_TH_LOCK(kevq_th);
		kevq_list = &kevq_th->kevq_hash[KEVQ_HASH((unsigned long long)kq, kevq_th->kevq_hashmask)];
		kevq = kevqlist_find(kevq_list, kq);
		KEVQ_TH_UNLOCK(kevq_th);

		if (kevq == NULL) {
			/* allocate kevq */
			to_free = NULL;
			alloc_kevq = malloc(sizeof(struct kevq), M_KQUEUE, M_WAITOK | M_ZERO);
			kevq_init(alloc_kevq);
			alloc_kevq->kq = kq;
			alloc_kevq->kevq_th = kevq_th;

			CTR3(KTR_KQ, "kqueue_ensure_kevq(M): allocated kevq %p for thread %d (oncpu = %d)", alloc_kevq, td->td_tid, td->td_oncpu);
			
			KQ_LOCK(kq);
			KEVQ_TH_LOCK(kevq_th);
			kevq = kevqlist_find(kevq_list, kq);

			/* kevq should only be allocated by the current thread. 
			 * This might only happen inside interrupt handler
			 * which I'm not actually sure about
			 * KASSERT(kevq != NULL, ("kevq double allocated")); 
			 */

			if (kevq == NULL) {
				kevq = alloc_kevq;
				/* insert kevq to the kevq_th hash table */
				LIST_INSERT_HEAD(kevq_list, kevq, kevq_th_e);

				/* insert kevq to the kevq_th list */ 
				LIST_INSERT_HEAD(&kevq_th->kevq_list, kevq, kevq_th_tqe);

				/* insert into kqueue */
				LIST_INSERT_HEAD(&kq->kq_kevqlist, kevq, kq_e);
				
				KEVQ_TH_UNLOCK(kevq_th);
				KQ_UNLOCK(kq);

			} else {
				to_free = alloc_kevq;

				KEVQ_TH_UNLOCK(kevq_th);
				KQ_UNLOCK(kq);
			}

			if (to_free != NULL) {
				free(to_free, M_KQUEUE);
			}
		}
	} else {
		kevq = kq->kq_kevq;
		if (kevq == NULL) {
			alloc_kevq = malloc(sizeof(struct kevq), M_KQUEUE, M_WAITOK | M_ZERO);
			CTR2(KTR_KQ, "kqueue_ensure_kevq(S): allocated kevq %p for kq %p", alloc_kevq, kq);
			kevq_init(alloc_kevq);
			alloc_kevq->kq = kq;

			KQ_LOCK(kq);
			if ((kevq = kq->kq_kevq) == NULL) {
				kq->kq_kevq = alloc_kevq;
				kevq = alloc_kevq;
			} else {
				to_free = alloc_kevq;
			}
			KQ_UNLOCK(kq);

			if (to_free != NULL) {
				free(to_free, M_KQUEUE);
			}
		}
	}

	KASSERT(kevq != NULL, ("kevq isn't allocated."));
	*kevqp = kevq;

	return 0;
}

static void 
kqueue_ensure_kqdom(struct kqueue *kq)
{
	struct kqdom* kqd;
	KQ_NOTOWNED(kq);

	kqd = kqdom_build();

	KQ_LOCK(kq);
	if (kq->kq_kqd == NULL) {
		kq->kq_kqd = kqd;
		kqd = NULL;
	}
	KQ_UNLOCK(kq);

	if (kqd != NULL) {
		kqdom_destroy(kqd);
	}
}

static int
kqueue_acquire_kevq(struct file *fp, struct thread *td, struct kqueue **kqp, struct kevq **kevqp)
{
	struct kqueue *kq;
	struct kevq *kevq;
	int error;

	kq = fp->f_data;
	if (fp->f_type != DTYPE_KQUEUE || kq == NULL)
		return (EBADF);
	*kqp = kq;

	/* We already know that only one thread can be in kqueue syscall context 
	 * when kqueue_close is called due to file descriptor limitations
	 */
	KASSERT((kq->kq_state & KQ_CLOSING) == 0, ("kq still in syscall context while closing"));

	/* set the init flag, which blocks others from changing ioctls */
	if ((kq->kq_flags & KQ_FLAG_INIT) == 0) {
		KQ_LOCK(kq);
		kq->kq_flags |= KQ_FLAG_INIT;
		KQ_UNLOCK(kq);
	}

	error = kqueue_obtain_kevq(kq, td, &kevq);

	if (error == 0) {
		*kevqp = kevq;
		error = kevq_acquire(kevq, 0);
	}

	/*
	 * we can obtain ref then acquire because kevq can be destroyed either:
	 * 1. by our own thread exiting
	 * 2. by whoever closes the kq, but then nobody else should be in kqueue syscall context 
	 * All of the above imply the kevq reference cannot be invalid here
	 */
	return error;
}

static void
kqueue_schedtask(struct kqueue *kq)
{

	KQ_OWNED(kq);
	KASSERT(((kq->kq_state & KQ_TASKDRAIN) != KQ_TASKDRAIN),
	    ("scheduling kqueue task while draining"));

	if ((kq->kq_state & KQ_TASKSCHED) != KQ_TASKSCHED) {
		taskqueue_enqueue(taskqueue_kqueue_ctx, &kq->kq_task);
		kq->kq_state |= KQ_TASKSCHED;
	}
}

/*
// not guaranteed to have a children
static struct kqdom *
kqdom_next_leaf(struct kqdom *kqd)
{
	struct kqdom *parent;
	struct kqdom *cur;
	struct kqdom *next;
	struct kqdom *each_child;
	
	cur = kqd;
	parent = cur->parent;
	next = NULL;

	// move right once
	while (cur != NULL) {
		next = TAILQ_NEXT(cur, child_e);

		if (next != NULL && next->num_kevq > 0)
			break;

		cur = parent;
		parent = cur->parent;
	}

	// if the selected kqdom isn't leaf, return a leaf 
	while (next != NULL && next->num_children > 0) {
		TAILQ_FOREACH(each_child, &next->children, child_e) {
			if (each_child->num_kevq > 0) {
				if (each_child->num_children == 0) {
					// return if we have a valid leaf node
					break;
				}
				// we have a non-leaf node, set next to child and try again
				next = each_child;
				continue;
			}
		}
		// we traversed all children and nobody has >0 kevqs, return NULL
		next = NULL;
		break;
	}

	return next;
}
*/

static void
kqdom_init(struct kqdom *kqd)
{
	veclist_init(&kqd->children, 0, M_KQUEUE);
	veclist_init(&kqd->kqd_activelist, 0, M_KQUEUE);
	veclist_init(&kqd->kqd_kevqs, 0, M_KQUEUE);
	rw_init(&kqd->kqd_lock, "kqdom_lock");
}

static int 
kqdom_is_leaf(struct kqdom *kqd)
{
	return veclist_size(&kqd->children) == 0;
}

/* inserts a kevq into a leaf kqdom */
static void
kqdom_insert(struct kqdom *kqd, struct kevq *kevq)
{
	int err, sz;

	KASSERT(kqdom_is_leaf(kqd), ("inserting into a non-leaf kqdom"));
	CTR2(KTR_KQ, "kqdom_insert: kevq: %p kqdom %d", kevq, kqd->id);

	KQD_WLOCK(kqd);
	err = veclist_insert_tail(&kqd->kqd_kevqs, kevq);
	/* XXX: this is a hack, need to handle ENOMEM */
	if (err) {
		panic("kqdom veclist failed to insert tail");
	}

	sz = veclist_size(&kqd->kqd_kevqs);
	KQD_WUNLOCK(kqd);

	if (sz == 1)
		kqdom_update_parents(kqd, KQDIR_ACTIVE);
}

/* removes a kevq from a leaf kqdom */
static void
kqdom_remove(struct kqdom *kqd, struct kevq *kevq)
{
	int sz;

	KASSERT(kqdom_is_leaf(kqd), ("removing from a non-leaf kqdom"));
	CTR2(KTR_KQ, "kqdom_remove: kevq: %p kqdom %d", kevq, kqd->id);

	KQD_WLOCK(kqd);
	veclist_remove(&kqd->kqd_kevqs, kevq);
	sz = veclist_size(&kqd->kqd_kevqs);
	KQD_WUNLOCK(kqd);

	if (sz == 0)
	 	kqdom_update_parents(kqd, KQDIR_INACTIVE);
}

static void
kqdom_destroy(struct kqdom *root)
{
	for(int i = 0; i < veclist_size(&root->children); i++) {
		kqdom_destroy(veclist_at(&root->children, i));
	}

	CTR2(KTR_KQ, "kqdom_destroy: destroyed kqdom %d with %d child kqdoms", root->id, veclist_size(&root->children));

	veclist_destroy(&root->kqd_kevqs);
	veclist_destroy(&root->kqd_activelist);
	veclist_destroy(&root->children);

	rw_destroy(&root->kqd_lock);

	free(root, M_KQUEUE);
}

static void 
kevq_dump(struct kevq *kevq, int level, struct sbuf *buf)
{
	sbuf_printf(buf, "%*c<kevq: %p #knotes: %d avg_lat: %ld/>\n", level * 2, ' ', kevq, kevq->kn_count, kevq->kevq_avg_lat);
}

static void
kqdom_dump(struct kqdom *kqd, int level, struct sbuf *buf)
{	
	/* XXX: No potential race between this and kqdom_build() for now. 
	 * If we move kqdom_build() out of kqueue() syscall then there is a potential race */

	
	/* XXX2: check return code */
	sbuf_printf(buf, "%*c<kqdom id: %d level: %d cpu_mask:0x%lx #children: %d #active: %d leaf: %d #kevq: %d>\n", level * 2, ' ',
																													kqd->id, 
																													level,
																													kqd->cpu_mask.__bits[0], 
																													veclist_size(&kqd->children),
																													veclist_size(&kqd->kqd_activelist), 
																													kqdom_is_leaf(kqd), 
																													veclist_size(&kqd->kqd_kevqs));
	if (kqdom_is_leaf(kqd)) {
		KQD_RLOCK(kqd);
		/* print all kevqs */
		for (int i = 0; i < veclist_size(&kqd->kqd_kevqs); i++) {
			/* XXX: check return code */
			kevq_dump(veclist_at(&kqd->kqd_kevqs, i), level + 1, buf);
		}
		KQD_RUNLOCK(kqd);
	} else {
		for(int i = 0; i < veclist_size(&kqd->children); i++) {
			kqdom_dump(veclist_at(&kqd->children, i), level + 1, buf);
		}
	}

	/* XXX: check return code */
	sbuf_printf(buf, "%*c</kqdom>\n", level * 2, ' ');
}



/* Expensive if called *frequently* 
 * 
 * Updates a kqdom based on the currently active children
 */
static void
kqdom_update_parents(struct kqdom *kqd, int direction)
{
	int err;
	int cont;
	struct kqdom *child;

	/* We are locking parent kqdoms while the leaf lock is acquired. 
	 * Just a note, not a problem (so far) 
	 */
	cont = 1;
	while (cont) {
		child = kqd;
		kqd = child->parent;

		if(kqd == NULL)
			break;
		
		KQD_WLOCK(kqd);
		
		CTR3(KTR_KQ, "kqdom_update_parents: %d updating kqdom %d with %d active children", direction, kqd->id, veclist_size(&kqd->kqd_activelist));

		if (direction == KQDIR_INACTIVE) {
			veclist_remove(&kqd->kqd_activelist, child);

			/* didn't change from 1 to 0, stop */
			if (veclist_size(&kqd->kqd_activelist) != 0) {
				cont = 0;
			}
		} else {
			/* kqd->kqd_activelist are preallocated with maximum children for non-leaf nodes
			 * Should NEVER fail
			 */
			err = veclist_insert_tail(&kqd->kqd_activelist, child);
			/* NOT a hack! */
			if (err) {
				panic("kqdom activelist requires expansion");
			}
			/* KASSERT(!err, ("kqdom activelist requires expansion")); */

			/* didn't change from 0 to 1, stop */
			if (veclist_size(&kqd->kqd_activelist) != 1) {
				cont = 0;
			}
		}
		KQD_WUNLOCK(kqd);
	}
}

static void
kqdom_update_lat(struct kqdom *leaf, uint64_t avg)
{
	/* We don't need this function for now */
	KASSERT(0, ("kqdom_update_lat called"));

	while(leaf != NULL) {
		if (leaf->avg_lat != 0) {
			// bit rot race here?
			leaf->avg_lat = CALC_OVERTIME_AVG(leaf->avg_lat, avg);
		} else {
			leaf->avg_lat = avg;
		}

		CTR2(KTR_KQ, "kqdom_update_lat: updated avg lat %ld us for kqdom %d", leaf->avg_lat, leaf->id);

		leaf = leaf->parent;
	}
}


/* Mirror the cpu_group structure */
static void
kqdom_build_internal(struct kqdom *kqd_cur, struct cpu_group *cg_cur, int *kqd_id)
{
	int err;
	struct kqdom *child;
	int cg_numchild = cg_cur->cg_children;
	CTR4(KTR_KQ, "kqdom_build_internal: processing cpu_group with %d child groups, %d CPUs, shared cache level %d, kqd_id %d", cg_numchild, cg_cur->cg_count, cg_cur->cg_level, *kqd_id);

	/* init fields for current */
	kqd_cur->id = *kqd_id;
	(*kqd_id)++;
	CPU_COPY(&cg_cur->cg_mask, &kqd_cur->cpu_mask);

	/* allocate children and active lists */
	if (cg_numchild > 0) {
		err = veclist_expand(&kqd_cur->children, cg_numchild);
		/* XXX: These are hacks */
		if (err) {
			panic("kqdom build veclist expand");
		}

		err = veclist_expand(&kqd_cur->kqd_activelist, cg_numchild);
		if (err) {
			panic("kqdom build veclist expand");
		}
	}

	for (int i = 0; i < cg_numchild; i++) {
		child = malloc(sizeof(struct kqdom), M_KQUEUE, M_WAITOK | M_ZERO);
		kqdom_init(child);

		child->parent = kqd_cur;
		err = veclist_insert_tail(&kqd_cur->children, child);

		/* Not a hack! */
		if (err) {
			panic("kqdom build insert tail failed");
		}
		/* KASSERT(!err, ("kqdom build insert tail failed")); */

		kqdom_build_internal(child, &cg_cur->cg_child[i], kqd_id);
	}
}

static struct kqdom *
kqdom_build()
{
	int kqd_id = 0;
	CTR0(KTR_KQ, "kqueue_build_sched: mirroring cpu_group...");
	struct kqdom* kqd_root = malloc(sizeof(struct kqdom), M_KQUEUE, M_WAITOK | M_ZERO);
	kqdom_init(kqd_root);
	kqdom_build_internal(kqd_root, cpu_top, &kqd_id);
	return kqd_root;
}

static struct kqdom *
kqdom_find(struct kqdom *root, int cpuid)
{
	if (kqdom_is_leaf(root)) {
		KASSERT(CPU_ISSET(cpuid, &root->cpu_mask), ("kqdom_find: cpuid and cpumask mismatch"));
		return root;
	}

	for(int i = 0; i < veclist_size(&root->children); i++) {
		if(CPU_ISSET(cpuid, &((struct kqdom *)veclist_at(&root->children, i))->cpu_mask)) {
			return kqdom_find((struct kqdom *)veclist_at(&root->children, i), cpuid);
		}
	}

	KASSERT(0, ("kqdom_find: cpu doesn't exist "));
	return NULL;
}

/*
 * Expand the kq to make sure we have storage for fops/ident pair.
 *
 * Return 0 on success (or no work necessary), return errno on failure.
 */
static int
kqueue_expand(struct kqueue *kq, struct filterops *fops, uintptr_t ident,
    int mflag)
{
	struct klist *list, *tmp_knhash, *to_free;
	u_long tmp_knhashmask;
	int error, fd, size;

	KQ_NOTOWNED(kq);

	error = 0;
	to_free = NULL;
	if (fops->f_isfd) {
		fd = ident;
		if (kq->kq_knlistsize <= fd) {
			size = kq->kq_knlistsize;
			while (size <= fd)
				size += KQEXTENT;
			list = malloc(size * sizeof(*list), M_KQUEUE, mflag);
			if (list == NULL)
				return ENOMEM;
			KQ_LOCK(kq);
			if ((kq->kq_state & KQ_CLOSING) != 0) {
				to_free = list;
				error = EBADF;
			} else if (kq->kq_knlistsize > fd) {
				to_free = list;
			} else {
				if (kq->kq_knlist != NULL) {
					bcopy(kq->kq_knlist, list,
					    kq->kq_knlistsize * sizeof(*list));
					to_free = kq->kq_knlist;
					kq->kq_knlist = NULL;
				}
				bzero((caddr_t)list +
				    kq->kq_knlistsize * sizeof(*list),
				    (size - kq->kq_knlistsize) * sizeof(*list));
				kq->kq_knlistsize = size;
				kq->kq_knlist = list;
			}
			KQ_UNLOCK(kq);
		}
	} else {
		if (kq->kq_knhashmask == 0) {
			tmp_knhash = hashinit_flags(KN_HASHSIZE, M_KQUEUE,
			    &tmp_knhashmask, (mflag & M_WAITOK) != 0 ?
			    HASH_WAITOK : HASH_NOWAIT);
			if (tmp_knhash == NULL)
				return (ENOMEM);
			KQ_LOCK(kq);
			if ((kq->kq_state & KQ_CLOSING) != 0) {
				to_free = tmp_knhash;
				error = EBADF;
			} else if (kq->kq_knhashmask == 0) {
				kq->kq_knhash = tmp_knhash;
				kq->kq_knhashmask = tmp_knhashmask;
			} else {
				to_free = tmp_knhash;
			}
			KQ_UNLOCK(kq);
		}
	}
	free(to_free, M_KQUEUE);

	KQ_NOTOWNED(kq);
	return (error);
}

static void
kqueue_task(void *arg, int pending)
{
	struct kqueue *kq;
	int haskqglobal;

	haskqglobal = 0;
	kq = arg;

	KQ_GLOBAL_LOCK(&kq_global, haskqglobal);
	KQ_LOCK(kq);

	KNOTE_LOCKED(&kq->kq_sel.si_note, 0);

	kq->kq_state &= ~KQ_TASKSCHED;
	if ((kq->kq_state & KQ_TASKDRAIN) == KQ_TASKDRAIN) {
		wakeup(&kq->kq_state);
	}
	KQ_UNLOCK(kq);
	KQ_GLOBAL_UNLOCK(&kq_global, haskqglobal);
}

static void
kevq_worksteal(struct kevq *kevq)
{
	struct kevq *other_kevq;
	struct kqueue *kq;
	struct knote *ws_kn, *next_kn;
	struct knlist *knl;
	int ws_count, valid, tgt_count;

	KEVQ_OWNED(kevq);
	KEVQ_UNLOCK(kevq);

	kq = kevq->kq;
	ws_count = 0;
	tgt_count = KQSCHED_GET_FARGS(kq);

	TAILQ_HEAD(, knote) kn_wsq;
	TAILQ_INIT(&kn_wsq);

	KVLST_RLOCK(kq);
	other_kevq = kevq_vec_select_kevq(&kq->kevq_vlist, 1);
	other_kevq = kevq_lock_check_avail(other_kevq);
	KVLST_RUNLOCK(kq);

	CTR2(KTR_KQ, "kevq_worksteal: kevq %p selected kevq %p", kevq, other_kevq);
		
	if (other_kevq != NULL && other_kevq != kevq && other_kevq->kn_count > 0) {
		ws_kn = TAILQ_FIRST(&other_kevq->kn_head);

		while(ws_count < tgt_count && ws_kn != NULL) {
			KEVQ_OWNED(other_kevq);
			next_kn = TAILQ_NEXT(ws_kn, kn_tqe);

			/* don't care about markers */
			if ((ws_kn->kn_status & KN_MARKER) != 0) {
				goto end_loop;
			}

			KN_FLUX_LOCK(ws_kn);

			/* ignore influx, inactive and disabled */
			if (kn_in_flux(ws_kn) || (ws_kn->kn_status & (KN_ACTIVE | KN_DISABLED | KN_REQUEUE)) != (KN_ACTIVE)) {
				KN_FLUX_UNLOCK(ws_kn);
				goto end_loop;
			}
			
			knote_enter_flux(ws_kn);
			KN_FLUX_UNLOCK(ws_kn);

			/* Remove from the old kevq first, due to lock order */
			knote_dequeue(ws_kn);
			KEVQ_UNLOCK(other_kevq);

			/* validate event */
			knl = kn_list_lock(ws_kn);
			valid = ws_kn->kn_fop->f_event(ws_kn, 0);
			kn_list_unlock(knl);

			if (valid) {
				TAILQ_INSERT_TAIL(&kn_wsq, ws_kn, kn_wse);
				ws_count++;
			}

			KEVQ_LOCK(other_kevq);

			if (!valid) {
				/* if not valid, return it to the previous queue */
				knote_enqueue(ws_kn, other_kevq);
				KN_LEAVE_FLUX_WAKEUP(ws_kn);
			}
end_loop:
			ws_kn = next_kn;
		}
	}

	if (other_kevq != NULL) {
		KEVQ_UNLOCK(other_kevq);
	}

	KEVQ_LOCK(kevq);
	while(!TAILQ_EMPTY(&kn_wsq)) {
		ws_kn = TAILQ_FIRST(&kn_wsq);
		TAILQ_REMOVE(&kn_wsq, ws_kn, kn_wse);
		knote_enqueue(ws_kn, kevq);
		KN_LEAVE_FLUX_WAKEUP(ws_kn);
		CTR4(KTR_KQ, "kevq_worksteal: kevq %p stole kn %p, ident: %d from kevq %p", kevq, ws_kn, ws_kn->kn_id, other_kevq);
	}
}

static void 
kevq_activate(struct kevq *kevq, struct thread *td)
{
	struct kqueue *kq;
	struct kqdom *kqd;
	int err;

	KEVQ_NOTOWNED(kevq);
	KASSERT((kevq->kevq_state & KEVQ_ACTIVE) == 0, ("activating a ready kevq"));
	
	kq = kevq->kq;
	CTR1(KTR_KQ, "kevq_activate: kevq %p", kevq);

	KEVQ_LOCK(kevq);
	
	kevq->kevq_state |= KEVQ_ACTIVE;
	KEVQ_UNLOCK(kevq);

	if (kq->kq_flags & KQ_FLAG_MULTI) {
		/* insert into sched structures */
		KVLST_WLOCK(kq);
		err = veclist_insert_tail(&kq->kevq_vlist, kevq);
		/* XXX: this is a hack, handle ENOMEM */
		if (err) {
			panic("kevq_vlist insert tail failed");
		}
		KVLST_WUNLOCK(kq);

		if (KQSCHED_GET_SCHED(kq) & KQDOM_FLAGS) {
			/* assign to the proper kqdom */
			KASSERT(kq->kq_kqd != NULL, ("kqdom doesn't exist after referecing kq"));
			
			kqd = kqdom_find(kq->kq_kqd, td->td_oncpu);
			kevq->kevq_kqd = kqd;

			kqdom_insert(kqd, kevq);
		}
	}
}

/*
 * Scan, update kn_data (if not ONESHOT), and copyout triggered events.
 * We treat KN_MARKER knotes as if they are in flux.
 */
static int
kqueue_scan(struct kevq *kevq, int maxevents, struct kevent_copyops *k_ops,
    const struct timespec *tsp, struct kevent *keva, struct thread *td)
{
	struct kqueue *kq;
	struct kevent *kevp;
	struct knote *kn, *marker;
	struct knlist *knl;
	sbintime_t asbt, rsbt;
	int count, error, haskqglobal, influx, nkev, touch;

	kq = kevq->kq;
	count = maxevents;
	nkev = 0;
	error = 0;
	haskqglobal = 0;

	if (maxevents == 0)
		goto done_nl;

	rsbt = 0;
	if (tsp != NULL) {
		if (tsp->tv_sec < 0 || tsp->tv_nsec < 0 ||
		    tsp->tv_nsec >= 1000000000) {
			error = EINVAL;
			goto done_nl;
		}
		if (timespecisset(tsp)) {
			if (tsp->tv_sec <= INT32_MAX) {
				rsbt = tstosbt(*tsp);
				if (TIMESEL(&asbt, rsbt))
					asbt += tc_tick_sbt;
				if (asbt <= SBT_MAX - rsbt)
					asbt += rsbt;
				else
					asbt = 0;
				rsbt >>= tc_precexp;
			} else
				asbt = 0;
		} else
			asbt = -1;
	} else
		asbt = 0;

	if ((kevq->kevq_state & KEVQ_ACTIVE) == 0) {
		/* activate kq if not already activated */
		kevq_activate(kevq, td);
	}

	marker = knote_alloc(M_WAITOK);
	CTR2(KTR_KQ, "kqueue_scan: td %d allocated marker %p", td->td_tid, marker);
	marker->kn_status = KN_MARKER;

	KEVQ_LOCK(kevq);
retry:
	kevp = keva;
	CTR3(KTR_KQ, "kqueue_scan: td %d on kevq %p has %d events", td->td_tid, kevq, kevq->kn_count);

	if ((KQSCHED_GET_FLAGS(kq) == KQ_SCHED_FLAG_WS) && kevq->kn_count == 0) {
		/* try work stealing */
		kevq_worksteal(kevq);
	}

	KEVQ_OWNED(kevq);
	if (kevq->kn_count == 0) {
		if (asbt == -1) {
			error = EWOULDBLOCK;
		} else {
			kevq->kevq_state |= KEVQ_SLEEP;
			CTR2(KTR_KQ, "kqueue_scan: td %d waiting on kevq %p for events", td->td_tid, kevq); 
			error = msleep_sbt(kevq, &kevq->lock, PSOCK | PCATCH,
			    "kqread", asbt, rsbt, C_ABSOLUTE);
			CTR2(KTR_KQ, "kqueue_scan: td %d wokeup on kevq %p for events", td->td_tid, kevq);
		}
		if (error == 0)
			goto retry;
		/* don't restart after signals... */
		if (error == ERESTART)
			error = EINTR;
		else if (error == EWOULDBLOCK)
			error = 0;
		goto done;
	}

	KEVQ_OWNED(kevq);
	TAILQ_INSERT_TAIL(&kevq->kn_head, marker, kn_tqe);
	influx = 0;

	while (count) {
		KEVQ_OWNED(kevq);
		kn = TAILQ_FIRST(&kevq->kn_head);

		KN_FLUX_LOCK(kn);
		if ((kn->kn_status == KN_MARKER && kn != marker) ||
		    kn_in_flux(kn)) {
			if (influx) {
				influx = 0;
				knote_flux_wakeup(kn);
			}
			kn->kn_fluxwait = 1;
			KEVQ_UNLOCK(kevq);
			CTR3(KTR_KQ, "kqueue_scan: td %d fluxwait on kn %p marker %p", td->td_tid, kn, marker);
			error = msleep(kn, &kn->kn_fluxlock, PSOCK | PDROP,
			    "kevqflxwt3", 0);

			KEVQ_LOCK(kevq);

			CTR3(KTR_KQ, "kqueue_scan: td %d fluxwait WAKEUP kn %p marker %p", td->td_tid, kn, marker);
			continue;
		}

		/* Now we have exclusive access to kn */
		TAILQ_REMOVE(&kevq->kn_head, kn, kn_tqe);
		CTR3(KTR_KQ, "kqueue_scan: td %d on kevq %p dequeued knote %p", td->td_tid, kevq, kn);
		if ((kn->kn_status & KN_DISABLED) == KN_DISABLED) {
			kn->kn_status &= ~(KN_QUEUED | KN_REQUEUE);
			kevq->kn_count--;
			KN_FLUX_UNLOCK(kn);
			continue;
		}
		if (kn == marker) {
			/* We are dequeuing our marker, wakeup threads waiting on it */
			knote_flux_wakeup(kn);
			KN_FLUX_UNLOCK(kn);
			CTR2(KTR_KQ, "kqueue_scan: td %d MARKER WAKEUP %p", td->td_tid, kn);
			if (count == maxevents) {
				goto retry;
			}
			goto done;
		}
		KASSERT(!kn_in_flux(kn),
		    ("knote %p is unexpectedly in flux", kn));

		if ((kn->kn_flags & EV_DROP) == EV_DROP) {
			kn->kn_status &= ~(KN_QUEUED | KN_REQUEUE);
			knote_enter_flux(kn);
			kevq->kn_count--;
			KN_FLUX_UNLOCK(kn);
			KEVQ_UNLOCK(kevq);
			/*
			 * We don't need to lock the list since we've
			 * marked it as in flux.
			 */
			knote_drop(kn, td);
			KEVQ_LOCK(kevq);
			continue;
		} else if ((kn->kn_flags & EV_ONESHOT) == EV_ONESHOT) {
			kn->kn_status &= ~(KN_QUEUED | KN_REQUEUE);
			knote_enter_flux(kn);
			kevq->kn_count--;
			KN_FLUX_UNLOCK(kn);
			KEVQ_UNLOCK(kevq);
			/*
			 * We don't need to lock the list since we've
			 * marked the knote as being in flux.
			 */
			*kevp = kn->kn_kevent;
			knote_drop(kn, td);
			KEVQ_LOCK(kevq);
			kn = NULL;
		} else {
			kn->kn_status |= KN_SCAN;
			knote_enter_flux(kn);
			KN_FLUX_UNLOCK(kn);
			KEVQ_UNLOCK(kevq);
			if ((kn->kn_status & KN_KQUEUE) == KN_KQUEUE) {
				/* TODO: we are waiting for another kqueue
				*/
				KQ_GLOBAL_LOCK(&kq_global, haskqglobal);
			}
			knl = kn_list_lock(kn);
			if (kn->kn_fop->f_event(kn, 0) == 0) {
				KEVQ_LOCK(kevq);
				KQ_GLOBAL_UNLOCK(&kq_global, haskqglobal);
				kn->kn_status &= ~(KN_QUEUED | KN_ACTIVE |
				    KN_SCAN | KN_REQUEUE);
				knote_leave_flux_ul(kn);
				kevq->kn_count--;
				kn_list_unlock(knl);
				influx = 1;
				CTR4(KTR_KQ, "kqueue_scan: kn %p, ident: %d not valid anymore for kevq %p, td %d", kn, kn->kn_id, kevq, td->td_tid);
				continue;
			}
			touch = (!kn->kn_fop->f_isfd && kn->kn_fop->f_touch != NULL);
			if (touch)
				kn->kn_fop->f_touch(kn, kevp, EVENT_PROCESS);
			else
				*kevp = kn->kn_kevent;
			KEVQ_LOCK(kevq);
			KQ_GLOBAL_UNLOCK(&kq_global, haskqglobal);
			if (kn->kn_flags & (EV_CLEAR | EV_DISPATCH)) {
				/* 
				 * Manually clear knotes who weren't 
				 * 'touch'ed.
				 */
				if (touch == 0 && kn->kn_flags & EV_CLEAR) {
					kn->kn_data = 0;
					kn->kn_fflags = 0;
				}
				if (kn->kn_flags & EV_DISPATCH)
					kn->kn_status |= KN_DISABLED;
				kn->kn_status &= ~(KN_QUEUED | KN_ACTIVE | KN_REQUEUE);
				kevq->kn_count--;
			} else {
				/* this flag is here to prevent a subtle workstealing race where one thread gets an identifier
				   and returns, before it can process the event, another thread steals the knote and 
				   processes the same fd, resulting in the first thread having no data available. 
				   Work stealing will avoid stealing knotes with this flag set */
				kn->kn_status |= KN_REQUEUE;
				CTR3(KTR_KQ, "kqueue_scan: requeued kn %p, ident: %d to kevq %p", kn, kn->kn_id,kevq);
				TAILQ_INSERT_TAIL(&kevq->kn_head, kn, kn_tqe);
			}
			
			kn->kn_status &= ~KN_SCAN;
			knote_leave_flux_ul(kn);
			kn_list_unlock(knl);
			influx = 1;
		}

		/* we are returning a copy to the user */
		kevp++;
		nkev++;
		count--;

		if (nkev == KQ_NEVENTS) {
			influx = 0;
			knote_flux_wakeup_ul(kn);
			KEVQ_UNLOCK(kevq);
			
			error = k_ops->k_copyout(k_ops->arg, keva, nkev);
			nkev = 0;
			kevp = keva;
			KEVQ_LOCK(kevq);
			if (error)
				break;
		}
	}
	TAILQ_REMOVE(&kevq->kn_head, marker, kn_tqe);
done:
	KEVQ_OWNED(kevq);

	if (kn != NULL) {
		knote_flux_wakeup_ul(kn);
	}

	if (marker != NULL) {
		knote_flux_wakeup_ul(marker);
	}

	KEVQ_UNLOCK(kevq);

	if (nkev != 0 && (KQSCHED_GET_SCHED(kq) & KEVQ_LAT_FLAGS)) {
		/* book keep the statistics */
		kevq->kevq_last_kev = get_cyclecount();
		kevq->kevq_last_nkev = nkev;
		CTR3(KTR_KQ, "kevent: td %d nkev %d kevent (exit) %ld ns", td->td_tid, kevq->kevq_last_nkev, kevq->kevq_last_kev);
	}

	CTR2(KTR_KQ, "kqueue_scan: knote_free marker %p td %d", marker, td->td_tid);
	knote_free(marker);
done_nl:
	KEVQ_NOTOWNED(kevq);
	if (nkev != 0)
		error = k_ops->k_copyout(k_ops->arg, keva, nkev);
	td->td_retval[0] = maxevents - count;
	return (error);
}

/*ARGSUSED*/
static int
kqueue_ioctl(struct file *fp, u_long cmd, void *data,
	struct ucred *active_cred, struct thread *td)
{
	/*
	 * Enabling sigio causes two major problems:
	 * 1) infinite recursion:
	 * Synopsys: kevent is being used to track signals and have FIOASYNC
	 * set.  On receipt of a signal this will cause a kqueue to recurse
	 * into itself over and over.  Sending the sigio causes the kqueue
	 * to become ready, which in turn posts sigio again, forever.
	 * Solution: this can be solved by setting a flag in the kqueue that
	 * we have a SIGIO in progress.
	 * 2) locking problems:
	 * Synopsys: Kqueue is a leaf subsystem, but adding signalling puts
	 * us above the proc and pgrp locks.
	 * Solution: Post a signal using an async mechanism, being sure to
	 * record a generation count in the delivery so that we do not deliver
	 * a signal to the wrong process.
	 *
	 * Note, these two mechanisms are somewhat mutually exclusive!
	 */
#if 0
	struct kqueue *kq;

	kq = fp->f_data;
	switch (cmd) {
	case FIOASYNC:
		if (*(int *)data) {
			kq->kq_state |= KQ_ASYNC;
		} else {
			kq->kq_state &= ~KQ_ASYNC;
		}
		return (0);

	case FIOSETOWN:
		return (fsetown(*(int *)data, &kq->kq_sigio));
		
	case FIOGETOWN:
		*(int *)data = fgetown(&kq->kq_sigio);
		return (0);
	}
#endif
	struct sbuf buf;
	struct kqueue *kq;
	char *dat;
	int error = 0;
	int sched;

	kq = fp->f_data;
	CTR2(KTR_KQ, "kqueue_ioctl: received: kq %p   cmd: 0x%lx", kq, cmd);
	switch (cmd) {
		case FKQMULTI:
			KQ_LOCK(kq);
			if (kq->kq_flags & KQ_FLAG_INIT) {
				error = (EINVAL);
			} else {
				kq->kq_flags |= (KQ_FLAG_INIT | KQ_FLAG_MULTI);
				sched = *(int*)data;

				kq->kq_sflags = KQSCHED_PARSE_FLAGS(sched);
				kq->kq_ssargs = KQSCHED_PARSE_SARGS(sched);
				kq->kq_ssched = KQSCHED_PARSE_SCHED(sched);
				kq->kq_sfargs = KQSCHED_PARSE_FARGS(sched);
				CTR5(KTR_KQ, "kqueue_ioctl: multi kq %p, sched: %d sargs: %d flags: %d fargs: %d", kq, 
																									KQSCHED_GET_SCHED(kq),
																									KQSCHED_GET_SARGS(kq), 
																									KQSCHED_GET_FLAGS(kq),
																									KQSCHED_GET_FARGS(kq));
			}
			KQ_UNLOCK(kq);
			break;
		case FKQMPRNT:
			if (kq->kq_flags & KQ_FLAG_MULTI) {
				/* 4MB buffer */
				dat = malloc(sizeof(char) * 4096 * 1024, M_KQUEUE, M_WAITOK | M_ZERO);
				sbuf_new(&buf, dat, 4096 * 1024 * sizeof(char), SBUF_AUTOEXTEND | SBUF_INCLUDENUL);
				/* dump kvlst */
				/* XXX: check return status of sbuf_printf */

				sbuf_printf(&buf, "KQ 0x%p dump:\n\nActive KEVQ:\n", kq);
				KVLST_RLOCK(kq);
				for(int i = 0; i < veclist_size(&kq->kevq_vlist); i++) {
					kevq_dump(veclist_at(&kq->kevq_vlist, i), 0, &buf);
				}
				KVLST_RUNLOCK(kq);

				/* dump kqdom if used */
				if (KQSCHED_GET_SCHED(kq) & KQDOM_FLAGS) {
					sbuf_printf(&buf, "\nKQDOM:\n");
					kqdom_dump(kq->kq_kqd, 0, &buf);
				}

				sbuf_finish(&buf);
				uprintf("%s", sbuf_data(&buf));
				sbuf_delete(&buf);
				free(dat, M_KQUEUE);
			} else {
				error = (EINVAL);
			}

			break;
		default:
			error = (ENOTTY);
	}

	return error;
}

/*ARGSUSED*/
static int
kqueue_poll(struct file *fp, int events, struct ucred *active_cred,
	struct thread *td)
{
	struct kqueue *kq;
	struct kevq *kevq;
	int revents = 0;
	int error;

	if ((error = kqueue_acquire_kevq(fp, td, &kq, &kevq)))
		return POLLERR;

	KQ_LOCK(kq);
	if ((kq->kq_flags & KQ_FLAG_MULTI) != KQ_FLAG_MULTI ) {
		revents = 0;
	} else {
		if (events & (POLLIN | POLLRDNORM)) {
			if (kevq->kn_count) {
				revents |= events & (POLLIN | POLLRDNORM);
			} else {
				selrecord(td, &kq->kq_sel);
				if (SEL_WAITING(&kq->kq_sel))
					kq->kq_state |= KQ_SEL;
			}
		}
	}

	KQ_UNLOCK(kq);
	kevq_release(kevq, 0);
	return (revents);
}

/*ARGSUSED*/
static int
kqueue_stat(struct file *fp, struct stat *st, struct ucred *active_cred,
	struct thread *td)
{

	bzero((void *)st, sizeof *st);
	/*
	 * We no longer return kq_count because the unlocked value is useless.
	 * If you spent all this time getting the count, why not spend your
	 * syscall better by calling kevent?
	 *
	 * XXX - This is needed for libc_r.
	 */
	st->st_mode = S_IFIFO;
	return (0);
}

static void
kevq_destroy(struct kevq *kevq)
{
	CTR1(KTR_KQ, "kevq_destroy for %p", kevq);
	mtx_destroy(&kevq->lock);
	free(kevq, M_KQUEUE);
}

/* This is called on every kevq when kqueue exits 
   This is also called when a thread exits/crashes (currently racing, also to make it work need to reconfigure kq->ck_evq)
   * a ref cnt must be held */
void
kevq_drain(struct kevq *kevq, struct thread *td)
{
	struct kqueue *kq;
	struct knote *kn;
	struct kqdom *kqd;
	struct kevqlist *kevq_list;
	CTR3(KTR_KQ, "kevq_drain for %p (refcnt = %d) with %d knotes", kevq, kevq->kevq_refcnt, kevq->kn_count);
	kq = kevq->kq;
	kqd = kevq->kevq_kqd;

	KQ_NOTOWNED(kq);
	KEVQ_NOTOWNED(kevq);

	KEVQ_LOCK(kevq);
	if(kevq->kevq_state == KEVQ_CLOSING) {
		// already closing, dereference 
		kevq_release(kevq, 1);
		KEVQ_UNLOCK(kevq);
		return;
	} else {
		kevq->kevq_state |= KEVQ_CLOSING;
	}

	// Wait for extra references to the kevq
	if (kevq->kevq_refcnt > 1)
		msleep(&kevq->kevq_refcnt, &kevq->lock, PSOCK, "kevqclose1", 0);

	KEVQ_OWNED(kevq);
	KASSERT(kevq->kevq_refcnt == 1, ("other refs of kevq are out there!"));
	KEVQ_UNLOCK(kevq);

	/* remove from queues first */
	if ((kq->kq_flags & KQ_FLAG_MULTI) == KQ_FLAG_MULTI) {
		KQ_LOCK(kq);
		KEVQ_TH_LOCK(kevq->kevq_th);

		/*  detach from kevq_th */
		LIST_REMOVE(kevq, kevq_th_tqe);
		kevq_list = &kevq->kevq_th->kevq_hash[KEVQ_HASH((unsigned long long)kq, kevq->kevq_th->kevq_hashmask)];
		LIST_REMOVE(kevq, kevq_th_e);

	 	/*  detach from kqueue */
		LIST_REMOVE(kevq, kq_e);

		KEVQ_TH_UNLOCK(kevq->kevq_th);
		KQ_UNLOCK(kq);

		/* detach from sched structs */
		if (kevq->kevq_state & KEVQ_ACTIVE) {
			KVLST_WLOCK(kq);
			veclist_remove(&kq->kevq_vlist, kevq);
			KVLST_WUNLOCK(kq);

			if (KQSCHED_GET_SCHED(kq) & KQDOM_FLAGS) {
				kqdom_remove(kqd, kevq);
			}
		}
	} else {
		KQ_LOCK(kq);
		kq->kq_kevq = NULL;
		KQ_UNLOCK(kq);
	}

	KEVQ_LOCK(kevq);
	/* drain all knotes on the kevq */
	while ((kn = TAILQ_FIRST(&kevq->kn_head)) != NULL) {
		KEVQ_OWNED(kevq);
		KN_FLUX_LOCK(kn);
		/* Wait for kn to stablize */
		if (kn_in_flux(kn)) {
			kn->kn_fluxwait = 1;
			CTR2(KTR_KQ, "kevq_drain %p fluxwait knote %p", kevq, kn);

			KEVQ_UNLOCK(kevq);
			msleep(kn, &kn->kn_fluxlock, PSOCK | PDROP, "kevqclose2", 0);
			KEVQ_LOCK(kevq);

			continue;
		}

		CTR2(KTR_KQ, "kevq_drain %p draining knote %p", kevq, kn);

		KN_FLUX_OWNED(kn);
		KASSERT(!kn_in_flux(kn), ("knote is still influx"));
		knote_enter_flux(kn);
		KN_FLUX_UNLOCK(kn);

		/* remove knote from kevq */
		knote_dequeue(kn);
		
		if ((kn->kn_flags & EV_AFFINITY) == EV_AFFINITY) {
			knote_drop(kn, td);
		}

		/* a thread cannot crash while in kernel, and there is no extra refs
		 * Marker KNs should should not exist
		 */
		KASSERT((kn->kn_status & KN_MARKER) == 0, ("Marker KN present while closing"));

		if ((kq->kq_flags & KQ_FLAG_MULTI) == KQ_FLAG_MULTI && (kq->kq_state & KQ_CLOSING) != KQ_CLOSING) {
			KEVQ_UNLOCK(kevq);
			knote_activate(kn);
			KEVQ_LOCK(kevq);
		}

		KN_LEAVE_FLUX_WAKEUP(kn);
	}

	KASSERT(kevq->kn_count == 0, ("some knotes are left"));
	KEVQ_OWNED(kevq);
	KEVQ_UNLOCK(kevq);
	
	//
	// XXX: don't remove yet
	// Here it's guaranteed that no knotes contain a pointer to the kevq
	//
	// First, all knotes with kn->kn_kevq != kevq before queuing is not an issue
	// because if kn->kn_kevq == NULL, scheduler will grab kevq from either kqdom (QC) or kevqlist (RR) or kn->orgkevq (EV_AFFINITY)
	// KEVQs grabbed from QC or RR are locked with QC or RR locked, therefore they are either grabbed before kevq invalidation
	// or after kevq detachment. (In between doesn't matter since kevq is already invalidated) 
	// In the former case, the knote would be queued to the kevq and later drained as usual.
	// In the latter case, the kevq would not be found at all because it's already removed from QC or RR.
	//
	// Second, for all knotes with kn->kn_kevq == kevq. They would be already queued to kevq
	// and will be dequeued later (kn->kn_kevq will be set to another valid kevq)
	//

	/* delete the kevq */
	kevq_destroy(kevq);
}

/* kevq is only used when kq is in single mode
   in this case kevq has been referenced by the caller */
static void
kqueue_drain(struct kqueue *kq, struct kevq *kevq, struct thread *td)
{
	struct knote *kn;
	int i;

	CTR2(KTR_KQ, "kqueue_drain on %p. args kevq %p", kq, kevq);

	KQ_LOCK(kq);

	KASSERT((kq->kq_state & KQ_CLOSING) != KQ_CLOSING,
	    ("kqueue already closing"));
	kq->kq_state |= KQ_CLOSING;

	KASSERT(knlist_empty(&kq->kq_sel.si_note),
	    ("kqueue's knlist not empty"));

	// destroy knotes first
	for (i = 0; i < kq->kq_knlistsize; i++) {
		while ((kn = SLIST_FIRST(&kq->kq_knlist[i])) != NULL) {
			KQ_OWNED(kq);

			KN_FLUX_LOCK(kn);
			if (kn_in_flux(kn)) {
				kn->kn_fluxwait = 1;

				KQ_UNLOCK(kq);
				msleep(kn, &kn->kn_fluxlock, PSOCK | PDROP, "kqclo1", 0);
				KQ_LOCK(kq);

				continue;
			}
			knote_enter_flux(kn);
			KN_FLUX_UNLOCK(kn);

			KQ_UNLOCK(kq);
			knote_drop(kn, td);
			KQ_LOCK(kq);
		}
	}
	if (kq->kq_knhashmask != 0) {
		for (i = 0; i <= kq->kq_knhashmask; i++) {
			while ((kn = SLIST_FIRST(&kq->kq_knhash[i])) != NULL) {
				KQ_OWNED(kq);
				KN_FLUX_LOCK(kn);
				if (kn_in_flux(kn)) {
					kn->kn_fluxwait = 1;

					KQ_UNLOCK(kq);
					msleep(kn, &kn->kn_fluxlock, PSOCK | PDROP, "kqclo2", 0);
					KQ_LOCK(kq);

					continue;
				}
				knote_enter_flux(kn);
				KN_FLUX_UNLOCK(kn);
				KQ_UNLOCK(kq);
				knote_drop(kn, td);
				KQ_LOCK(kq);
			}
		}
	}

	if ((kq->kq_flags & KQ_FLAG_MULTI) == KQ_FLAG_MULTI) {
		while((kevq = LIST_FIRST(&kq->kq_kevqlist)) != NULL) {
			KQ_UNLOCK(kq);
			if (kevq_acquire(kevq, 0) == 0)
				kevq_drain(kevq, td);
			KQ_LOCK(kq);
		}

		KQ_OWNED(kq);
		
		/* destroy sched structs */
		if (KQSCHED_GET_SCHED(kq) & KQDOM_FLAGS) {
			kqdom_destroy(kq->kq_kqd); 
		}
	} else {
		KQ_UNLOCK(kq);
		// we already have a reference for single threaded mode
		kevq_drain(kq->kq_kevq, td);
		KQ_LOCK(kq);
	}

	KQ_OWNED(kq);

	if ((kq->kq_state & KQ_TASKSCHED) == KQ_TASKSCHED) {
		kq->kq_state |= KQ_TASKDRAIN;
		msleep(&kq->kq_state, &kq->kq_lock, PSOCK, "kqtqdr", 0);
	}

	if ((kq->kq_state & KQ_SEL) == KQ_SEL) {
		selwakeuppri(&kq->kq_sel, PSOCK);
		if (!SEL_WAITING(&kq->kq_sel))
			kq->kq_state &= ~KQ_SEL;
	}

	KQ_UNLOCK(kq);
}

static void
kqueue_destroy(struct kqueue *kq)
{

	KASSERT(kq->kq_fdp == NULL,
	    ("kqueue still attached to a file descriptor"));
	seldrain(&kq->kq_sel);
	knlist_destroy(&kq->kq_sel.si_note);
	mtx_destroy(&kq->kq_lock);

	/* XXX: move these guys to be destroyed earlier, like kqdom */
	rw_destroy(&kq->kevq_vlist_lk);
	veclist_destroy(&kq->kevq_vlist);

	if (kq->kq_knhash != NULL)
		free(kq->kq_knhash, M_KQUEUE);
	if (kq->kq_knlist != NULL)
		free(kq->kq_knlist, M_KQUEUE);

	funsetown(&kq->kq_sigio);
}

/*ARGSUSED*/
static int
kqueue_close(struct file *fp, struct thread *td)
{
	struct kqueue *kq = fp->f_data;
	struct kevq *kevq = NULL;
	struct filedesc *fdp;
	int error;
	int filedesc_unlock;

	if ((kq->kq_flags & KQ_FLAG_MULTI) == 0) {
		/* acquire kevq if we are not in single threaded mode */
		if ((error = kqueue_acquire_kevq(fp, td, &kq, &kevq)))
			return error;
	}
	
	kqueue_drain(kq, kevq, td);

	/*
	 * We could be called due to the knote_drop() doing fdrop(),
	 * called from kqueue_register().  In this case the global
	 * lock is owned, and filedesc sx is locked before, to not
	 * take the sleepable lock after non-sleepable.
	 */
	fdp = kq->kq_fdp;
	kq->kq_fdp = NULL;
	if (!sx_xlocked(FILEDESC_LOCK(fdp))) {
		FILEDESC_XLOCK(fdp);
		filedesc_unlock = 1;
	} else
		filedesc_unlock = 0;
	TAILQ_REMOVE(&fdp->fd_kqlist, kq, kq_list);
	if (filedesc_unlock)
		FILEDESC_XUNLOCK(fdp);

	kqueue_destroy(kq);
	chgkqcnt(kq->kq_cred->cr_ruidinfo, -1, 0);
	crfree(kq->kq_cred);
	free(kq, M_KQUEUE);
	fp->f_data = NULL;
	CTR1(KTR_KQ, "kqueue_close: %p.", kq);
	return (0);
}

static int
kqueue_fill_kinfo(struct file *fp, struct kinfo_file *kif, struct filedesc *fdp)
{

	kif->kf_type = KF_TYPE_KQUEUE;
	return (0);
}

static void
kevq_wakeup(struct kevq* kevq)
{
	KEVQ_OWNED(kevq);
	if ((kevq->kevq_state & KEVQ_SLEEP) == KEVQ_SLEEP) {
		kevq->kevq_state &= ~KEVQ_SLEEP;
	}
	wakeup(kevq);
}

static void
kqueue_wakeup(struct kqueue *kq)
{
	KQ_OWNED(kq);
	if ((kq->kq_state & KQ_SEL) == KQ_SEL) {
		selwakeuppri(&kq->kq_sel, PSOCK);
		if (!SEL_WAITING(&kq->kq_sel))
			kq->kq_state &= ~KQ_SEL;
	}
	if (!knlist_empty(&kq->kq_sel.si_note))
		kqueue_schedtask(kq);
	if ((kq->kq_state & KQ_ASYNC) == KQ_ASYNC) {
		pgsigio(&kq->kq_sigio, SIGIO, 0);
	}
}

/*
 * Walk down a list of knotes, activating them if their event has triggered.
 *
 * There is a possibility to optimize in the case of one kq watching another.
 * Instead of scheduling a task to wake it up, you could pass enough state
 * down the chain to make up the parent kqueue.  Make this code functional
 * first.
 */
void
knote(struct knlist *list, long hint, int lockflags)
{
	struct kqueue *kq;
	struct knote *kn, *tkn;
	int require_kqlock, kn_active;

	if (list == NULL)
		return;

	KNL_ASSERT_LOCK(list, lockflags & KNF_LISTLOCKED);

	if ((lockflags & KNF_LISTLOCKED) == 0)
		list->kl_lock(list->kl_lockarg); 

	/*
	 * If we unlock the list lock (and enter influx), we can
	 * eliminate the kqueue scheduling, but this will introduce
	 * four lock/unlock's for each knote to test.  Also, marker
	 * would be needed to keep iteration position, since filters
	 * or other threads could remove events.
	 */
	SLIST_FOREACH_SAFE(kn, &list->kl_list, kn_selnext, tkn) {
		CTR1(KTR_KQ, "knote() scanning kn %p", kn);
		KN_FLUX_LOCK(kn);
		if (kn_in_flux(kn) && ((kn->kn_status & KN_SCAN) == 0)) {
			/*
			 * Do not process the influx notes, except for
			 * the influx coming from the kq unlock in the
			 * kqueue_scan().  In the later case, we do
			 * not interfere with the scan, since the code
			 * fragment in kqueue_scan() locks the knlist,
			 * and cannot proceed until we finished.
			 */
			KN_FLUX_UNLOCK(kn);
		} else {
			// either not influx or being scanned
			kq = kn->kn_kq;
			knote_enter_flux(kn);
			KN_FLUX_UNLOCK(kn);

			require_kqlock = ((lockflags & KNF_NOKQLOCK) == 0);

			if (require_kqlock) 
				KQ_LOCK(kq);

			kn_active = kn->kn_fop->f_event(kn, hint);

			if (require_kqlock) 
				KQ_UNLOCK(kq);

			if (kn_active)
				knote_activate(kn);

			KN_LEAVE_FLUX_WAKEUP(kn);
		}
	}
	if ((lockflags & KNF_LISTLOCKED) == 0)
		list->kl_unlock(list->kl_lockarg); 
}

static void
knote_flux_wakeup_ul(struct knote *kn)
{
	KN_FLUX_NOTOWNED(kn);
	KN_FLUX_LOCK(kn);
	knote_flux_wakeup(kn);
	KN_FLUX_UNLOCK(kn);
}

static void
knote_flux_wakeup(struct knote *kn)
{
	KN_FLUX_OWNED(kn);
	if (kn->kn_fluxwait) {
	CTR1(KTR_KQ, "waking up kn %p", kn);
		kn->kn_fluxwait = 0;
	wakeup(kn);
	}
}

/*
 * activate a knote
 * the knote should be marked in flux and the knote flux lock should not be owned
 * none of the other locks should be held
 */
static void 
knote_activate(struct knote *kn)
{
	struct kqueue *kq;
	kq = kn->kn_kq;

	KQ_NOTOWNED(kq);

	CTR2(KTR_KQ, "knote_activate: kn %p, flags %d", kn, kn->kn_status);
	KN_FLUX_NOTOWNED(kn);
	KASSERT(kn_in_flux(kn), ("knote %p not in flux", kn));
	
	kn->kn_status |= KN_ACTIVE;
	
	if (((kn)->kn_status & (KN_QUEUED | KN_DISABLED)) == 0) {
		knote_sched(kn);
	} else {
		CTR2(KTR_KQ, "knote_activate: kn %p, flags %d not sched", kn, kn->kn_status);
	}

#ifdef ENABLE_SELECT
	KQ_LOCK(kq);
	kqueue_wakeup(kq);
	KQ_UNLOCK(kq);
#endif
}

/*
 * add a knote to a knlist
 */
void
knlist_add(struct knlist *knl, struct knote *kn, int islocked)
{
	/* CTR1(KTR_KQ, "knlist_add kn %p", kn); */
	KNL_ASSERT_LOCK(knl, islocked);
	KQ_NOTOWNED(kn->kn_kq);
	KASSERT(kn_in_flux(kn), ("knote %p not in flux", kn));
	KASSERT((kn->kn_status & KN_DETACHED) != 0,
	    ("knote %p was not detached", kn));
	if (!islocked)
		knl->kl_lock(knl->kl_lockarg);
	SLIST_INSERT_HEAD(&knl->kl_list, kn, kn_selnext);
	if (!islocked)
		knl->kl_unlock(knl->kl_lockarg);
	KQ_LOCK(kn->kn_kq);
	kn->kn_knlist = knl;
	kn->kn_status &= ~KN_DETACHED;
	KQ_UNLOCK(kn->kn_kq);
}

static void
knlist_remove_kq(struct knlist *knl, struct knote *kn, int knlislocked,
    int kqislocked)
{

	KASSERT(!kqislocked || knlislocked, ("kq locked w/o knl locked"));
	KNL_ASSERT_LOCK(knl, knlislocked);
	mtx_assert(&kn->kn_kq->kq_lock, kqislocked ? MA_OWNED : MA_NOTOWNED);
	KASSERT(kqislocked || kn_in_flux(kn), ("knote %p not in flux", kn));
	KASSERT((kn->kn_status & KN_DETACHED) == 0,
	    ("knote %p was already detached", kn));
	if (!knlislocked)
		knl->kl_lock(knl->kl_lockarg);
	SLIST_REMOVE(&knl->kl_list, kn, knote, kn_selnext);
	kn->kn_knlist = NULL;
	if (!knlislocked)
		kn_list_unlock(knl);
	if (!kqislocked)
		KQ_LOCK(kn->kn_kq);
	kn->kn_status |= KN_DETACHED;
	if (!kqislocked)
		KQ_UNLOCK(kn->kn_kq);
}

/*
 * remove knote from the specified knlist
 */
void
knlist_remove(struct knlist *knl, struct knote *kn, int islocked)
{

	knlist_remove_kq(knl, kn, islocked, 0);
}

int
knlist_empty(struct knlist *knl)
{

	KNL_ASSERT_LOCKED(knl);
	return (SLIST_EMPTY(&knl->kl_list));
}

static struct mtx knlist_lock;
MTX_SYSINIT(knlist_lock, &knlist_lock, "knlist lock for lockless objects",
    MTX_DEF);
static void knlist_mtx_lock(void *arg);
static void knlist_mtx_unlock(void *arg);

static void
knlist_mtx_lock(void *arg)
{

	mtx_lock((struct mtx *)arg);
}

static void
knlist_mtx_unlock(void *arg)
{

	mtx_unlock((struct mtx *)arg);
}

static void
knlist_mtx_assert_locked(void *arg)
{

	mtx_assert((struct mtx *)arg, MA_OWNED);
}

static void
knlist_mtx_assert_unlocked(void *arg)
{

	mtx_assert((struct mtx *)arg, MA_NOTOWNED);
}

static void
knlist_rw_rlock(void *arg)
{

	rw_rlock((struct rwlock *)arg);
}

static void
knlist_rw_runlock(void *arg)
{

	rw_runlock((struct rwlock *)arg);
}

static void
knlist_rw_assert_locked(void *arg)
{

	rw_assert((struct rwlock *)arg, RA_LOCKED);
}

static void
knlist_rw_assert_unlocked(void *arg)
{

	rw_assert((struct rwlock *)arg, RA_UNLOCKED);
}

void
knlist_init(struct knlist *knl, void *lock, void (*kl_lock)(void *),
    void (*kl_unlock)(void *),
    void (*kl_assert_locked)(void *), void (*kl_assert_unlocked)(void *))
{

	if (lock == NULL)
		knl->kl_lockarg = &knlist_lock;
	else
		knl->kl_lockarg = lock;

	if (kl_lock == NULL)
		knl->kl_lock = knlist_mtx_lock;
	else
		knl->kl_lock = kl_lock;
	if (kl_unlock == NULL)
		knl->kl_unlock = knlist_mtx_unlock;
	else
		knl->kl_unlock = kl_unlock;
	if (kl_assert_locked == NULL)
		knl->kl_assert_locked = knlist_mtx_assert_locked;
	else
		knl->kl_assert_locked = kl_assert_locked;
	if (kl_assert_unlocked == NULL)
		knl->kl_assert_unlocked = knlist_mtx_assert_unlocked;
	else
		knl->kl_assert_unlocked = kl_assert_unlocked;

	knl->kl_autodestroy = 0;
	SLIST_INIT(&knl->kl_list);
}

void
knlist_init_mtx(struct knlist *knl, struct mtx *lock)
{

	knlist_init(knl, lock, NULL, NULL, NULL, NULL);
}

struct knlist *
knlist_alloc(struct mtx *lock)
{
	struct knlist *knl;

	knl = malloc(sizeof(struct knlist), M_KQUEUE, M_WAITOK);
	knlist_init_mtx(knl, lock);
	return (knl);
}

void
knlist_init_rw_reader(struct knlist *knl, struct rwlock *lock)
{

	knlist_init(knl, lock, knlist_rw_rlock, knlist_rw_runlock,
	    knlist_rw_assert_locked, knlist_rw_assert_unlocked);
}

void
knlist_destroy(struct knlist *knl)
{

	KASSERT(KNLIST_EMPTY(knl),
	    ("destroying knlist %p with knotes on it", knl));
}

void
knlist_detach(struct knlist *knl)
{

	KNL_ASSERT_LOCKED(knl);
	knl->kl_autodestroy = 1;
	if (knlist_empty(knl)) {
		knlist_destroy(knl);
		free(knl, M_KQUEUE);
	}
}

/*
 * Even if we are locked, we may need to drop the lock to allow any influx
 * knotes time to "settle".
 */
void
knlist_cleardel(struct knlist *knl, struct thread *td, int islocked, int killkn)
{
	struct knote *kn, *kn2;
	struct kqueue *kq;

	KASSERT(!knl->kl_autodestroy, ("cleardel for autodestroy %p", knl));
	if (islocked)
		KNL_ASSERT_LOCKED(knl);
	else {
		KNL_ASSERT_UNLOCKED(knl);
again:		/* need to reacquire lock since we have dropped it */
		knl->kl_lock(knl->kl_lockarg);
	}

	SLIST_FOREACH_SAFE(kn, &knl->kl_list, kn_selnext, kn2) {
		kq = kn->kn_kq;
		KQ_LOCK(kq);
		KN_FLUX_LOCK(kn);
		if (kn_in_flux(kn)) {
			KN_FLUX_UNLOCK(kn);
			KQ_UNLOCK(kq);
			continue;
		}
		knlist_remove_kq(knl, kn, 1, 1);
		if (killkn) {
			knote_enter_flux(kn);
			KN_FLUX_UNLOCK(kn);
			KQ_UNLOCK(kq);
			knote_drop_detached(kn, td);
		} else {
			/* Make sure cleared knotes disappear soon */
			kn->kn_flags |= EV_EOF | EV_ONESHOT;
			KN_FLUX_UNLOCK(kn);
			KQ_UNLOCK(kq);
		}
		kq = NULL;
	}

	if (!SLIST_EMPTY(&knl->kl_list)) {
		/* there are still in flux knotes remaining */
		kn = SLIST_FIRST(&knl->kl_list);
		kq = kn->kn_kq;
		KQ_LOCK(kq);
		KN_FLUX_LOCK(kn);
		KASSERT(kn_in_flux(kn), ("knote removed w/o list lock"));
		knl->kl_unlock(knl->kl_lockarg);
		kn->kn_fluxwait = 1;
		KQ_UNLOCK(kq);
		msleep(kn, &kn->kn_fluxlock, PSOCK | PDROP, "kqkclr", 0);

		kq = NULL;
		goto again;
	}

	if (islocked)
		KNL_ASSERT_LOCKED(knl);
	else {
		knl->kl_unlock(knl->kl_lockarg);
		KNL_ASSERT_UNLOCKED(knl);
	}
}

/*
 * Remove all knotes referencing a specified fd must be called with FILEDESC
 * lock.  This prevents a race where a new fd comes along and occupies the
 * entry and we attach a knote to the fd.
 */
void
knote_fdclose(struct thread *td, int fd)
{
	struct filedesc *fdp = td->td_proc->p_fd;
	struct kqueue *kq;
	struct knote *kn;
	int influx;

	FILEDESC_XLOCK_ASSERT(fdp);

	/*
	 * We shouldn't have to worry about new kevents appearing on fd
	 * since filedesc is locked.
	 */
	TAILQ_FOREACH(kq, &fdp->fd_kqlist, kq_list) {
		KQ_LOCK(kq);

again:
		influx = 0;
		while (kq->kq_knlistsize > fd &&
		    (kn = SLIST_FIRST(&kq->kq_knlist[fd])) != NULL) {
			KQ_OWNED(kq);
			KN_FLUX_LOCK(kn);
			if (kn_in_flux(kn)) {
				/* someone else might be waiting on our knote */
				if (influx)
					knote_flux_wakeup(kn);
				kn->kn_fluxwait = 1;

				KQ_UNLOCK(kq);
				msleep(kn, &kn->kn_fluxlock, PSOCK | PDROP, "kqflxwt4", 0);
				KQ_LOCK(kq);
				
				goto again;
			}
			knote_enter_flux(kn);
			KN_FLUX_UNLOCK(kn);
			KQ_UNLOCK(kq);
			influx = 1;
			knote_drop(kn, td);
			KQ_LOCK(kq);
		}
		KQ_UNLOCK(kq);
	}
}

static int
knote_attach(struct knote *kn, struct kqueue *kq)
{
	struct klist *list;

	KASSERT(kn_in_flux(kn), ("knote %p not marked influx", kn));
	KQ_OWNED(kq);

	if ((kq->kq_state & KQ_CLOSING) != 0)
		return (EBADF);
	if (kn->kn_fop->f_isfd) {
		if (kn->kn_id >= kq->kq_knlistsize)
			return (ENOMEM);
		list = &kq->kq_knlist[kn->kn_id];
	} else {
		if (kq->kq_knhash == NULL)
			return (ENOMEM);
		list = &kq->kq_knhash[KN_HASH(kn->kn_id, kq->kq_knhashmask)];
	}
	SLIST_INSERT_HEAD(list, kn, kn_link);
	return (0);
}

static void
knote_drop(struct knote *kn, struct thread *td)
{

	if ((kn->kn_status & KN_DETACHED) == 0)
		kn->kn_fop->f_detach(kn);
	knote_drop_detached(kn, td);
}

static void
knote_drop_detached(struct knote *kn, struct thread *td)
{
	struct kqueue *kq;
	struct kevq *kevq;
	struct klist *list;

	kq = kn->kn_kq;
	kevq = kn->kn_kevq;

	KASSERT((kn->kn_status & KN_DETACHED) != 0,
	    ("knote %p still attached", kn));
	KQ_NOTOWNED(kq);

	KQ_LOCK(kq);

	KASSERT(kn->kn_influx == 1,
	    ("knote_drop called on %p with influx %d", kn, kn->kn_influx));

	if (kn->kn_fop->f_isfd)
		list = &kq->kq_knlist[kn->kn_id];
	else
		list = &kq->kq_knhash[KN_HASH(kn->kn_id, kq->kq_knhashmask)];

	if (!SLIST_EMPTY(list))
		SLIST_REMOVE(list, kn, knote, kn_link);

	if (kn->kn_status & KN_QUEUED) {
		KEVQ_LOCK(kevq);
		knote_dequeue(kn);
		KEVQ_UNLOCK(kevq);
	}

	KN_LEAVE_FLUX_WAKEUP(kn);
	KQ_UNLOCK(kq);

	if (kn->kn_fop->f_isfd) {
		fdrop(kn->kn_fp, td);
		kn->kn_fp = NULL;
	}
	kqueue_fo_release(kn->kn_kevent.filter);
	kn->kn_fop = NULL;
	knote_free(kn);
}

static struct kevq *
kevq_vec_select_kevq(struct veclist *lst, int num_rand)
{
	int sz;
	struct kevq *cur_kevq = NULL, *next_kevq;
	u_long rand;

	/* XXX: hack */
	KASSERT(num_rand < 8, ("too much num_rand"));

	CTR1(KTR_KQ, "kevq_vec_select_kevq: num - %d", num_rand);
	
	sz = veclist_size(lst);

	if (sz > 0) {
		rand = random();

		for (int i = 0; i < num_rand; i++) {
			next_kevq = veclist_at(lst, rand % sz);

			CTR5(KTR_KQ, "kevq_vec_select_kevq: candidate kevq %p [%d] avg lat %ld #kn %d Tot: %ld", next_kevq, i, next_kevq->kevq_avg_lat, next_kevq->kn_count, next_kevq->kevq_avg_lat * next_kevq->kn_count);

			if (cur_kevq == NULL || (next_kevq != NULL && kevq_lat_cmp(cur_kevq, next_kevq) > 0)) {
				cur_kevq = next_kevq;
			}

			/* XXX: hack */
			rand = rand >> 8;
		}
	}

	CTR2(KTR_KQ, "kevq_vec_select_kevq: selected kevq %p Tot: %ld", cur_kevq, cur_kevq == NULL ? 0 : cur_kevq->kevq_avg_lat * cur_kevq->kn_count);

	return cur_kevq;
}

static struct kevq *
kqdom_random_kevq_locked(struct kqdom *kqd)
{
	struct kevq *kevq;
	struct kqdom *tkqd;
	int num_active;
	u_long rand;

	rand = random();
	kevq = NULL;

	while (!kqdom_is_leaf(kqd)) {
		KQD_RLOCK(kqd);
		/* we only select active stuff inside this, need to be EXTREMELY fast */
		num_active = veclist_size(&kqd->kqd_activelist);
		CTR1(KTR_KQ, "kqdom_random_kevq_locked: randomly selected leaf kqdom %d", kqd->id);
		if (num_active > 0) {
			tkqd = veclist_at(&kqd->kqd_activelist, rand % num_active);
		} else {
			tkqd = NULL;
		}
		KQD_RUNLOCK(kqd);
		kqd = tkqd;
		/* XXX: hack */
		rand = rand >> 8;
	}

	if (kqd != NULL) {
		CTR1(KTR_KQ, "kqdom_random_kevq_locked: randomly selected leaf kqdom %d", kqd->id);
		KQD_RLOCK(kqd);

		kevq = kevq_vec_select_kevq(&kqd->kqd_kevqs, 1);
		kevq = kevq_lock_check_avail(kevq);

		KQD_RUNLOCK(kqd);
	}

	if (kevq != NULL) {
		KEVQ_OWNED(kevq);
	}

	CTR1(KTR_KQ, "kqdom_random_kevq_locked: randomly selected kevq %p", kevq);

	return kevq;
}


/* select the next kevq based on knote and scheduler flags and locks the returned kevq */
static struct kevq *
knote_next_kevq(struct knote *kn)
{
	struct kqdom *kqd;
	struct kqueue *kq;
	struct kevq *next_kevq, *other_kevq;
	int sargs;
	int sched;
	int rand, sz;

	next_kevq = NULL;
	kq = kn->kn_kq;
	sargs = KQSCHED_GET_SARGS(kq);
	sched = KQSCHED_GET_SCHED(kq);

	CTR1(KTR_KQ, "knote_next_kevq: processing kn %p", kn);

	if ((kq->kq_flags & KQ_FLAG_MULTI) == 0) {
		// single threaded mode, just return the current kevq
		KQ_LOCK(kn->kn_kq);
		if ((kq->kq_state & KQ_CLOSING) == 0) 
			next_kevq = kn->kn_kq->kq_kevq;
		KQ_UNLOCK(kn->kn_kq);

		if (next_kevq != NULL)
			KEVQ_LOCK(next_kevq);
		
		CTR2(KTR_KQ, "knote_next_kevq: [LEGACY] next kevq %p for kn %p", next_kevq, kn);
		return next_kevq;
	}

	if ((kn->kn_flags & EV_AFFINITY) == EV_AFFINITY) {
		next_kevq = kevq_lock_check_avail(kn->kn_org_kevq);

		CTR2(KTR_KQ, "knote_next_kevq: [AFFIN] next kevq %p for kn %p", kn, next_kevq);
		return next_kevq;
	}

	switch (sched) {
		case KQ_SCHED_CPU:
			kqd = kqdom_find(kq->kq_kqd, PCPU_GET(cpuid));
			goto done_cq;
		case KQ_SCHED_QUEUE:
			if (kn->kn_kqd == NULL) {
				/* the first time a knote is queued, record the kqdom */
				kn->kn_kqd = kqdom_find(kq->kq_kqd, PCPU_GET(cpuid));

				KASSERT(kn->kn_kqd != NULL, ("knote scheduled on an unidentified CPU"));
				CTR3(KTR_KQ, "knote_next_kevq: [QUEUE%d] knote %p attached to kqdom id %d", sargs, kn, kn->kn_kqd->id);
			}
			kqd = kn->kn_kqd;

done_cq:
			KASSERT(kqdom_is_leaf(kqd), ("found kqdom not leaf"));

			KQD_RLOCK(kqd);
			next_kevq = kevq_vec_select_kevq(&kqd->kqd_kevqs, 1);

			if (sargs > 0) {
				KVLST_RLOCK(kq);
				other_kevq = kevq_vec_select_kevq(&kq->kevq_vlist, sargs);
				KVLST_RUNLOCK(kq);

				if (next_kevq == NULL || (other_kevq != NULL && kevq_lat_wcmp(next_kevq, other_kevq, 80) > 0)) {
					next_kevq = other_kevq;
				}
			}

			next_kevq = kevq_lock_check_avail(next_kevq);
			KQD_RUNLOCK(kqd);

			CTR3(KTR_KQ, "knote_next_kevq: [QUEUE/CPU%d] next kevq %p for kn %p", sargs, next_kevq, kn);
			break;

		case KQ_SCHED_BEST:
			KVLST_RLOCK(kq);
			
			next_kevq = kevq_vec_select_kevq(&kq->kevq_vlist, sargs);
			next_kevq = kevq_lock_check_avail(next_kevq);

			KVLST_RUNLOCK(kq);
			CTR3(KTR_KQ, "knote_next_kevq: [BOT%d] next kevq %p for kn %p", sargs, next_kevq, kn);
			break;
			
		default:
			break;
	}

	/* fall-back rand robbin*/
	if (next_kevq == NULL) {
		rand = random();

		KVLST_RLOCK(kq);
		sz = veclist_size(&kq->kevq_vlist);
		for (int i = 0; i < sz; i++) {

			next_kevq = veclist_at(&kq->kevq_vlist, rand % sz);
			next_kevq = kevq_lock_check_avail(next_kevq);

			if (next_kevq != NULL) {
				break;
			}

			rand++;
		}
		KVLST_RUNLOCK(kq);
		CTR2(KTR_KQ, "knote_next_kevq: [RAND] next kevq %p for kn %p", next_kevq, kn);
	}

	if (next_kevq != NULL)
		KEVQ_OWNED(next_kevq);

	return next_kevq;
}

// if no kevqs are available for queueing, returns NULL
static void
knote_sched(struct knote *kn)
{
	struct kevq *next_kevq;
	
	KASSERT(kn_in_flux(kn), ("kn not in flux"));

	// note that kevq will be locked after this
	next_kevq = knote_next_kevq(kn);

	CTR2(KTR_KQ, "knote_sched: next kevq %p for kn %p", next_kevq, kn);

	if (next_kevq != NULL) {
		KEVQ_OWNED(next_kevq);
		knote_enqueue(kn, next_kevq);
		KEVQ_UNLOCK(next_kevq);
	}
}

static void
knote_enqueue(struct knote *kn, struct kevq *kevq)
{
	struct kqueue *kq;
	kq = kn->kn_kq;

	/* CTR2(KTR_KQ, "knote_enqueue: kn %p to kevq %p", kn, kevq); */

	KEVQ_OWNED(kevq);

	KASSERT((kn->kn_status & KN_QUEUED) == 0, ("knote already queued"));

	/* Queuing to a clsoing kevq is fine.
     * The refcnt wait in kevq drain is before knote requeuing 
	 * so no knote will be forgotten
	 * KASSERT((kevq->kevq_state & KEVQ_CLOSING) == 0 && (kevq->kevq_state & KEVQ_ACTIVE) != 0, ("kevq already closing or not ready")); */

	kn->kn_kevq = kevq;
	kn->kn_status |= KN_QUEUED;

	TAILQ_INSERT_TAIL(&kevq->kn_head, kn, kn_tqe);
	kevq->kn_count++;
	
	kevq_wakeup(kevq);
}

static void
knote_dequeue(struct knote *kn)
{
	struct kevq *kevq = kn->kn_kevq;

	KEVQ_OWNED(kevq);

	CTR2(KTR_KQ, "knote_dequeue: kn %p from kevq %p", kn, kevq);
	KASSERT(kn->kn_status & KN_QUEUED, ("knote not queued"));

	TAILQ_REMOVE(&kevq->kn_head, kn, kn_tqe);
	kn->kn_status &= ~KN_QUEUED;
	kn->kn_kevq = NULL;
	kevq->kn_count--;
}

static void
knote_init(void)
{

	knote_zone = uma_zcreate("KNOTE", sizeof(struct knote), NULL, NULL,
	    NULL, NULL, UMA_ALIGN_PTR, 0);
}
SYSINIT(knote, SI_SUB_PSEUDO, SI_ORDER_ANY, knote_init, NULL);

static struct knote *
knote_alloc(int mflag)
{
	struct knote *ret = uma_zalloc(knote_zone, mflag | M_ZERO);
	/* CTR1(KTR_KQ, "knote_alloc: allocating knote %p", ret); */
	mtx_init(&ret->kn_fluxlock, "kn_fluxlock", NULL, MTX_DEF | MTX_DUPOK);
	return ret;
}

static void
knote_free(struct knote *kn)
{
	/* CTR1(KTR_KQ, "knote_free: kn %p", kn); */
	if (kn != NULL) {
		mtx_destroy(&kn->kn_fluxlock);
	}
	uma_zfree(knote_zone, kn);
}

/*
 * Register the kev w/ the kq specified by fd.
 */
int 
kqfd_register(int fd, struct kevent *kev, struct thread *td, int mflag)
{
	struct kqueue *kq;
	struct kevq *kevq;
	struct file *fp;
	cap_rights_t rights;
	int error;

	error = fget(td, fd, cap_rights_init(&rights, CAP_KQUEUE_CHANGE), &fp);
	if (error != 0)
		return (error);
	if ((error = kqueue_acquire_kevq(fp, td, &kq, &kevq)) != 0)
		goto noacquire;

	error = kqueue_register(kq, kevq, kev, td, mflag);
	kevq_release(kevq, 0);

noacquire:
	fdrop(fp, td);
	return (error);
}
