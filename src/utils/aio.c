/*
    Copyright (c) 2012 250bpm s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "aio.h"
#include "err.h"
#include "fast.h"

#if !defined SP_HAVE_WINDOWS
#if defined SP_HAVE_ACCEPT4
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <fcntl.h>
#endif

/*  Private functions. */
void sp_usock_tune (struct sp_usock *self);

int sp_usock_init (struct sp_usock *self, int domain, int type, int protocol)
{
#if !defined SOCK_CLOEXEC && defined FD_CLOEXEC
    int rc;
#endif

    /*  If the operating system allows to directly open the socket with CLOEXEC
        flag, do so. That way there are no race conditions. */
#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif

    /*  Open the underlying socket. */
    self->s = socket (domain, type, protocol);
#if defined SP_HAVE_WINDOWS
    if (self->s == INVALID_SOCKET)
       return -sp_err_wsa_to_posix (WSAGetLastError ());
#else
    if (self->s < 0)
       return -errno;
#endif
    self->domain = domain;
    self->type = type;
    self->protocol = protocol;
#if !defined SP_HAVE_WINDOWS
    self->aio = NULL;
#endif

    /*  Setting FD_CLOEXEC option immediately after socket creation is the
        second best option. There is a race condition (if process is forked
        between socket creation and setting the option) but the problem is
        pretty unlikely to happen. */
#if !defined SOCK_CLOEXEC && defined FD_CLOEXEC
    rc = fcntl (self->s, F_SETFD, FD_CLOEXEC);
    errno_assert (rc != -1);
#endif

    sp_usock_tune (self);

    return 0;
}

void sp_usock_tune (struct sp_usock *self)
{
    int rc;
    int opt;
#if defined SP_HAVE_WINDOWS
    u_long flags;
    BOOL brc;
    DWORD only;
#else
    int flags;
    int only;
#endif

    /*  If applicable, prevent SIGPIPE signal when writing to the connection
        already closed by the peer. */
#ifdef SO_NOSIGPIPE
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof (opt));
    errno_assert (rc == 0);
#endif

    /*  Switch the socket to the non-blocking mode. All underlying sockets
        are always used in the asynchronous mode. */
#if defined SP_HAVE_WINDOWS
    flags = 1;
    rc = ioctlsocket (self->s, FIONBIO, &flags);
    wsa_assert (rc != SOCKET_ERROR);
#else
	flags = fcntl (self->s, F_GETFL, 0);
	if (flags == -1)
        flags = 0;
	rc = fcntl (self->s, F_SETFL, flags | O_NONBLOCK);
    errno_assert (rc != -1);
#endif

    /*  On TCP sockets switch off the Nagle's algorithm to get
        the best possible latency. */
    if ((self->domain == AF_INET || self->domain == AF_INET6) &&
          self->type == SOCK_STREAM) {
        opt = 1;
        rc = setsockopt (self->s, IPPROTO_TCP, TCP_NODELAY,
            (const char*) &opt, sizeof (opt));
#if defined SP_HAVE_WINDOWS
        wsa_assert (rc != SOCKET_ERROR);
#else
        errno_assert (rc == 0);
#endif
    }

    /*  If applicable, disable delayed acknowledgements to improve latency. */
#if defined TCP_NODELACK
    opt = 1;
    rc = setsockopt (self->s, IPPROTO_TCP, TCP_NODELACK, &opt, sizeof (opt));
    errno_assert (rc == 0);
#endif

    /*  On some operating systems IPv4 mapping for IPv6 sockets is disabled
        by default. In such case, switch it on. */
#if defined IPV6_V6ONLY
    if (self->domain == AF_INET6) {
        only = 0;
        rc = setsockopt (self->s, IPPROTO_IPV6, IPV6_V6ONLY,
            (const char*) &only, sizeof (only));
#ifdef SP_HAVE_WINDOWS
        wsa_assert (rc != SOCKET_ERROR);
#else
        errno_assert (rc == 0);
#endif
    }
#endif

/*  On Windows, disable inheriting the socket to the child processes. */
#if defined SP_HAVE_WINDOWS && defined HANDLE_FLAG_INHERIT
    brc = SetHandleInformation ((HANDLE) self->s, HANDLE_FLAG_INHERIT, 0);
    win_assert (brc);
#endif
}

#if defined SP_HAVE_WINDOWS

#include <string.h>

void sp_cp_init (struct sp_cp *self)
{
    self->hndl = CreateIoCompletionPort (INVALID_HANDLE_VALUE, NULL, 0, 0);
    win_assert (self->hndl);
}

void sp_cp_term (struct sp_cp *self)
{
    BOOL brc;

    brc = CloseHandle (self->hndl);
    win_assert (brc);
}

void sp_cp_post (struct sp_cp *self, int op, void *arg)
{
    BOOL brc;

    brc = PostQueuedCompletionStatus (self->hndl, (DWORD) op,
        (ULONG_PTR) arg, NULL);
    win_assert (brc);
}

int sp_cp_wait (struct sp_cp *self, int timeout, int *op, void **arg)
{
    BOOL brc;
    DWORD nbytes;
    ULONG_PTR key;
    LPOVERLAPPED olpd;

    brc = GetQueuedCompletionStatus (self->hndl, &nbytes, &key,
        &olpd, timeout < 0 ? INFINITE : timeout);
    if (sp_slow (!brc && !olpd))
        return -ETIMEDOUT;
    win_assert (brc);
    *op = (int) nbytes;
    *arg = (void*) key;

    return 0;
}

void sp_cp_register_usock (struct sp_cp *self, struct sp_usock *usock)
{
    HANDLE cp;

    cp = CreateIoCompletionPort ((HANDLE) usock->s, self->hndl,
        (ULONG_PTR) NULL, 0);
    sp_assert (cp);
}

void sp_usock_term (struct sp_usock *self)
{
    int rc;

    rc = closesocket (self->s);
    wsa_assert (rc != SOCKET_ERROR);
}

int sp_usock_bind (struct sp_usock *self, const struct sockaddr *addr,
    sp_socklen addrlen)
{
    int rc;

    rc = bind (self->s, addr, addrlen);
    if (sp_slow (rc == SOCKET_ERROR))
       return -sp_err_wsa_to_posix (WSAGetLastError ());

    return 0;
}

int sp_usock_connect (struct sp_usock *self, const struct sockaddr *addr,
    sp_socklen addrlen, struct sp_cp_hndl *hndl)
{
    int rc;
    BOOL brc;
    const GUID fid = WSAID_CONNECTEX;
    LPFN_CONNECTEX pconnectex;
    DWORD nbytes;

    rc = WSAIoctl (self->s, SIO_GET_EXTENSION_FUNCTION_POINTER,
        (void*) &fid, sizeof (fid), (void*) &pconnectex, sizeof (pconnectex),
        &nbytes, NULL, NULL);
    wsa_assert (rc == 0);
    sp_assert (nbytes == sizeof (pconnectex));
    memset (&hndl->olpd, 0, sizeof (hndl->olpd));
    brc = pconnectex (self->s, (struct sockaddr*) &addr, addrlen,
        NULL, 0, NULL, (OVERLAPPED*) &hndl->olpd);
    if (sp_fast (brc == TRUE))
        return 0;
    wsa_assert (WSAGetLastError () == WSA_IO_PENDING);
    return -EINPROGRESS;
}

int sp_usock_listen (struct sp_usock *self, int backlog)
{
    int rc;
    int opt;

    /*  On Windows, the bound port can be hijacked if SO_EXCLUSIVEADDRUSE
        is not set. */
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        (const char*) &opt, sizeof (opt));
    wsa_assert (rc != SOCKET_ERROR);

    rc = listen (self->s, backlog);
    if (sp_slow (rc == SOCKET_ERROR))
       return -sp_err_wsa_to_posix (WSAGetLastError ());

    return 0;
}

int sp_usock_accept (struct sp_usock *self, struct sp_usock *usock,
    struct sp_cp_hndl *hndl)
{
    BOOL brc;
    char info [64];
    DWORD nbytes;

    usock->s = socket (self->domain, self->type, self->protocol);
    wsa_assert (usock->s != INVALID_SOCKET);
    usock->domain = self->domain;
    usock->type = self->type;
    usock->protocol = self->protocol;

    memset (&hndl->olpd, 0, sizeof (hndl->olpd));
    brc = AcceptEx (self->s, usock->s, info, 0, 256, 256, &nbytes,
        &hndl->olpd);
    if (sp_fast (brc == TRUE))
        return 0;
    wsa_assert (WSAGetLastError () == WSA_IO_PENDING);
    return -EINPROGRESS;
}

int sp_usock_send (struct sp_usock *self, const void *buf, size_t *len,
    int flags, struct sp_cp_hndl *hndl)
{
    int rc;
    WSABUF wbuf;
    DWORD nbytes;

    /*  TODO: Support partial send. */

    wbuf.len = (u_long) *len;
    wbuf.buf = (char FAR*) buf;
    memset (&hndl->olpd, 0, sizeof (hndl->olpd));
    rc = WSASend (self->s, &wbuf, 1, &nbytes, 0, &hndl->olpd, NULL);
    if (sp_fast (rc == 0)) {
        *len = nbytes;
        return 0;
    }
    wsa_assert (WSAGetLastError () == WSA_IO_PENDING);
    return -EINPROGRESS;
}

int sp_usock_recv (struct sp_usock *self, void *buf, size_t *len,
    int flags, struct sp_cp_hndl *hndl)
{
    int rc;
    WSABUF wbuf;
    DWORD wflags;
    DWORD nbytes;

    /*  TODO: Support partial recv. */

    wbuf.len = (u_long) *len;
    wbuf.buf = (char FAR*) buf;
    wflags = MSG_WAITALL;
    memset (&hndl->olpd, 0, sizeof (hndl->olpd));
    rc = WSARecv (self->s, &wbuf, 1, &nbytes, &wflags, &hndl->olpd, NULL);
    if (sp_fast (rc == 0)) {
        *len = nbytes;
        return 0;
    }
    wsa_assert (WSAGetLastError () == WSA_IO_PENDING);
    return -EINPROGRESS;
}

#else

#include "alloc.h"

#define SP_CP_INITIAL_CAPACITY 64

void sp_cp_init (struct sp_cp *self)
{
    sp_mutex_init (&self->sync, 0);
    sp_poller_init (&self->poller);
    sp_eventfd_init (&self->eventfd);
    sp_poller_add_fd (&self->poller, sp_eventfd_getfd (&self->eventfd),
        &self->evhndl);
    sp_poller_set_in (&self->poller, &self->evhndl);
    self->capacity = SP_CP_INITIAL_CAPACITY;
    self->head = 0;
    self->tail = 0;
    self->items = sp_alloc (self->capacity * sizeof (struct sp_cp_item));
    alloc_assert (self->items);
}

void sp_cp_term (struct sp_cp *self)
{
    sp_free (self->items);
    sp_poller_rm_fd (&self->poller, &self->evhndl);
    sp_eventfd_term (&self->eventfd);
    sp_poller_term (&self->poller);
    sp_mutex_term (&self->sync);
}

void sp_cp_post (struct sp_cp *self, int op, void *arg)
{
    int empty;

    sp_mutex_lock (&self->sync);

    /*  Fill in new item in the circular buffer. */
    self->items [self->tail].op = op;
    self->items [self->tail].arg = arg;

    /*  Move tail by 1 position. */
    empty = self->tail == self->head ? 1 : 0;
    self->tail = (self->tail + 1) % self->capacity;

    /*  If the capacity of the circular buffer is exhausted, allocate some
        more memory. */
    if (sp_slow (self->head == self->tail)) {
        self->items = sp_realloc (self->items,
            self->capacity * 2 * sizeof (struct sp_cp_item));
        alloc_assert (self->items);
        memcpy (self->items + self->capacity, self->items,
            self->tail * sizeof (struct sp_cp_item));
        self->tail += self->capacity;
        self->capacity *= 2;
    }
    
    if (empty)
        sp_eventfd_signal (&self->eventfd);

    sp_mutex_unlock (&self->sync);
}

int sp_cp_wait (struct sp_cp *self, int timeout, int *op, void **arg)
{
    int rc;
    int event;
    struct sp_poller_hndl *hndl;

    /*  If there's an item available, return it. */
    sp_mutex_lock (&self->sync);
    if (sp_fast (self->head != self->tail)) {
        *op = self->items [self->head].op;
        *arg = self->items [self->head].arg;
        self->head = (self->head + 1) % self->capacity;
        if (self->head == self->tail)
           sp_eventfd_unsignal (&self->eventfd);
        sp_mutex_unlock (&self->sync);
        return 0;
    }
    sp_mutex_unlock (&self->sync);

    /*  Wait for new item. */
    rc = sp_poller_wait (&self->poller, timeout, &event, &hndl);
    if (sp_slow (rc == -ETIMEDOUT || rc == -EINTR))
        return rc;
    errnum_assert (rc == 0, -rc);

    /*  TODO */
    sp_assert (hndl == &self->evhndl);

    /*  If there's an item available now, return it. */
    sp_mutex_lock (&self->sync);
    if (sp_fast (self->head != self->tail)) {
        *op = self->items [self->head].op;
        *arg = self->items [self->head].arg;
        self->head = (self->head + 1) % self->capacity;
        if (self->head == self->tail)
           sp_eventfd_unsignal (&self->eventfd);
        sp_mutex_unlock (&self->sync);
        return 0;
    }
    sp_mutex_unlock (&self->sync);

    /*  Spurious wake-up. */
    return -ETIMEDOUT;
}

void sp_cp_register_usock (struct sp_cp *self, struct sp_usock *usock)
{
    sp_assert (!usock->aio);
    usock->aio = self;
}

void sp_usock_term (struct sp_usock *self)
{
    int rc;

    rc = close (self->s);
    errno_assert (rc == 0);
}

int sp_usock_bind (struct sp_usock *self, const struct sockaddr *addr,
    sp_socklen addrlen)
{
    int rc;

    rc = bind (self->s, addr, addrlen);
    if (sp_slow (rc < 0))
       return -errno;

    return 0;
}

int sp_usock_connect (struct sp_usock *self, const struct sockaddr *addr,
    sp_socklen addrlen, struct sp_cp_hndl *hndl)
{
    sp_assert (0);
}

int sp_usock_listen (struct sp_usock *self, int backlog)
{
    int rc;
    int opt;

    /*  To allow for rapid restart of SP services, allow new bind to succeed
        immediately after previous instance of the process failed, skipping the
        grace period. */
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));
    errno_assert (rc == 0);

    rc = listen (self->s, backlog);
    if (sp_slow (rc < 0))
       return -errno;

    return 0;
}

int sp_usock_accept (struct sp_usock *self, struct sp_usock *usock,
    struct sp_cp_hndl *hndl)
{
    sp_assert (0);
}

int sp_usock_send (struct sp_usock *self, const void *buf, size_t *len,
    int flags, struct sp_cp_hndl *hndl)
{
    ssize_t nbytes;
#if defined MSG_NOSIGNAL
    const int sflags = MSG_NOSIGNAL;
#else
    const int sflags = 0;
#endif

    /*  If there's nothing to send, return straight away. */
    if (*len == 0)
        return 0;

    /*  Try to send as much data as possible in synchronous manner. */
    nbytes = send (self->s, buf, *len, sflags);
    if (sp_fast (nbytes == *len))
        return 0;

    /*  Handle errors. */
    if (nbytes < 0) {

        /*  If no bytes were transferred. */
        if (sp_fast (errno != EAGAIN && errno != EWOULDBLOCK)) {
            nbytes = 0;
            goto async;
        }

        /*  In theory, this should never happen as all the sockets are
            non-blocking. However, test the condition just in case. */
        if (errno == EINTR)
            return -EINTR;

        /*  In the case of connection failure. */
        if (errno == ECONNRESET || errno == EPIPE)
            return -ECONNRESET;

        /*  Other errors are not expected to happen. */
        errno_assert (0);
    }

async:

    sp_assert (0);
    return -EINPROGRESS;
}

int sp_usock_recv (struct sp_usock *self, void *buf, size_t *len,
    int flags, struct sp_cp_hndl *hndl)
{
    ssize_t nbytes;

    /*  If there's nothing to receive, return straight away. */
    if (*len == 0)
        return 0;

    /*  Try to receive as much data as possible in synchronous manner. */
    nbytes = recv (self->s, buf, *len, 0);

    /*  Success. */
    if (sp_fast (nbytes == *len))
        return 0;
    if (sp_fast (nbytes > 0 && flags & SP_USOCK_PARTIAL)) {
        *len = nbytes;
        return 0;
    }

    /*  Connection terminated. */
    if (sp_slow (nbytes == 0))
        return -ECONNRESET;

    /*  Handle errors. */
    if (nbytes < 0) {

        /*  If no bytes were received. */
        if (sp_fast (errno == EAGAIN || errno == EWOULDBLOCK)) {
            nbytes = 0;
            goto async;
        }
     
        /*  In theory, this should never happen as all the sockets are
            non-blocking. However, test the condition just in case. */
        if (errno == EINTR)
            return -EINTR;

        /*  In the case of connection failure. */
        if (errno == ECONNRESET || errno == ECONNREFUSED ||
              errno == ETIMEDOUT || errno == EHOSTUNREACH || errno == ENOTCONN)
            return -ECONNRESET;

        /*  Other errors are not expected to happen. */
        errno_assert (0);
    }

async:

    sp_assert (0);
    return -EINPROGRESS;
}

#endif

