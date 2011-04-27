/*
 * Copyright (c) 2011 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "../common/private.h"

struct event_buf {
    DWORD       bytes;
    ULONG_PTR   key;
    OVERLAPPED *overlap;
};

/*
 * Per-thread evt event buffer used to ferry data between
 * kevent_wait() and kevent_copyout().
 */
static __thread struct event_buf iocp_buf;

/* FIXME: remove these as filters are implemented */
const struct filter evfilt_proc = EVFILT_NOTIMPL;
const struct filter evfilt_vnode = EVFILT_NOTIMPL;
const struct filter evfilt_signal = EVFILT_NOTIMPL;
const struct filter evfilt_write = EVFILT_NOTIMPL;

const struct kqueue_vtable kqops = {
    windows_kqueue_init,
    windows_kqueue_free,
    windows_kevent_wait,
    windows_kevent_copyout,
    windows_filter_init,
    windows_filter_free,
};

#ifndef MAKE_STATIC

BOOL WINAPI DllMain(
        HINSTANCE self,
        DWORD reason,
        LPVOID unused)
{
    switch (reason) { 
        case DLL_PROCESS_ATTACH:

#if XXX
			//move to EVFILT_READ?
            if (WSAStartup(MAKEWORD(2,2), NULL) != 0)
                return (FALSE);
#endif
            if (libkqueue_init() < 0)
				return (FALSE);
            break;

        case DLL_PROCESS_DETACH:
#if XXX
            WSACleanup();
#endif
            break;
    }

    return (TRUE);
}

#endif

int
windows_kqueue_init(struct kqueue *kq)
{
    kq->kq_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 
                                         (ULONG_PTR) 0, 0);
    if (kq->kq_iocp == NULL) {
        dbg_lasterror("CreateIoCompletionPort");
        return (-1);
    }

    /* Create a handle whose sole purpose is to indicate a synthetic
     * IO event. */
    kq->kq_synthetic_event = CreateSemaphore(NULL, 0, 1, NULL);
    if (kq->kq_synthetic_event == NULL) {
        /* FIXME: close kq_iocp */
        dbg_lasterror("CreateSemaphore");
        return (-1);
    }

#if DEADWOOD
    kq->kq_loop = evt_create();
    if (kq->kq_loop == NULL) {
        dbg_perror("evt_create()");
        return (-1);
    }
#endif

	if(filter_register_all(kq) < 0) {
		evt_destroy(kq->kq_loop);
		return (-1);
	}

    return (0);
}

void
windows_kqueue_free(struct kqueue *kq)
{
    evt_destroy(kq->kq_loop);
    free(kq);
}

int
windows_kevent_wait(struct kqueue *kq, int no, const struct timespec *timeout)
{
	int retval;
    DWORD       timeout_ms;
    BOOL        success;
    
	/* Convert timeout to milliseconds */
	/* NOTE: loss of precision for timeout values less than 1ms */
	if (timeout == NULL) {
		timeout_ms = INFINITE;
	} else {
		timeout_ms = 0;
		if (timeout->tv_sec > 0)
			timeout_ms += ((DWORD)timeout->tv_sec) / 1000;
		if (timeout->tv_sec > 0)
			timeout_ms += timeout->tv_nsec / 1000000;
	}

    dbg_printf("waiting for events (timeout=%u ms)", (unsigned int) timeout_ms);
    memset(&iocp_buf, 0, sizeof(iocp_buf));
    success = GetQueuedCompletionStatus(kq->kq_iocp, 
            &iocp_buf.bytes, &iocp_buf.key, &iocp_buf.overlap, 
            timeout_ms);
    if (success) {
        return (1);
    } else {
        if (GetLastError() == WAIT_TIMEOUT) {
            dbg_puts("no events within the given timeout");
            return (0);
        }
        dbg_lasterror("GetQueuedCompletionStatus");
        return (-1);
    }
    
#if DEADWOOD
    DWORD rv;

	/* Wait for an event */
    dbg_printf("waiting for %u events (timeout=%u ms)", kq->kq_filt_count, (unsigned int)timeout_ms);
	rv = evt_run(kq->kq_loop, pending_events, MAX_KEVENT, timeout); 
	switch (rv) {
	case EVT_TIMEDOUT:
		dbg_puts("no events within the given timeout");
		retval = 0;
		break;

	case EVT_ERR:
		dbg_lasterror("WaitForSingleEvent()");
		retval = -1;

	default:
		retval = rv;
	}
#endif

    return (retval);
}

int
windows_kevent_copyout(struct kqueue *kq, int nready,
        struct kevent *eventlist, int nevents)
{
    struct filter *filt;
	struct knote* kn;
    int rv, nret;

    //FIXME: not true for EVFILT_IOCP
    kn = (struct knote *) iocp_buf.overlap;
    knote_lock(kn);
    filt = &kq->kq_filt[~(kn->kev.filter)];
    rv = filt->kf_copyout(eventlist, kn, &iocp_buf);
    knote_unlock(kn);
    if (slowpath(rv < 0)) {
        dbg_puts("knote_copyout failed");
        /* XXX-FIXME: hard to handle this without losing events */
        abort();
    } else {
        nret = 1;
    }

    /*
     * Certain flags cause the associated knote to be deleted
     * or disabled.
     */
    if (eventlist->flags & EV_DISPATCH) 
        knote_disable(filt, kn); //TODO: Error checking
    if (eventlist->flags & EV_ONESHOT) 
        knote_delete(filt, kn); //TODO: Error checking

    /* If an empty kevent structure is returned, the event is discarded. */
    if (fastpath(eventlist->filter != 0)) {
        eventlist++;
    } else {
        dbg_puts("spurious wakeup, discarding event");
        nret--;
    }

	return nret;
}

int
windows_filter_init(struct kqueue *kq, struct filter *kf)
{

	kq->kq_filt_ref[kq->kq_filt_count] = (struct filter *) kf;
    kq->kq_filt_count++;

	return (0);
}

void
windows_filter_free(struct kqueue *kq, struct filter *kf)
{

}
