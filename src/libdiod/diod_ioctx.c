/************************************************************\
 * Copyright 2010 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the diod 9P server project.
 * For details, see https://github.com/chaos/diod.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
\************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>
#include <fcntl.h>
#include <utime.h>
#include <stdarg.h>

#include "src/libnpfs/npfs.h"
#include "src/liblsd/list.h"
#include "src/liblsd/hash.h"
#include "src/liblsd/hostlist.h"
#include "src/libnpfs/xpthread.h"

#include "diod_conf.h"
#include "diod_log.h"

#include "diod_ioctx.h"
#include "diod_xattr.h"
#include "diod_fid.h"
#include "diod_ops.h"

typedef struct pathpool_struct *PathPool;

struct ioctx_struct {
    pthread_mutex_t lock;
    int             refcount;
    int             fd;
    DIR             *dir;
    int             lock_type;
    Npqid           qid;
    u32             iounit;
    u32             open_flags;
    Npuser          *user;
    IOCtx           next;
    IOCtx           prev;
};

struct path_struct {
    pthread_mutex_t lock;
    int             refcount;
    char            *s;
    int             len;
    IOCtx           ioctx;  /* double-linked list of IOCtx opening this path */
};

struct pathpool_struct {
    pthread_mutex_t lock;
    hash_t          hash;
};

static void
_unlink_ioctx (IOCtx *head, IOCtx i)
{
    if (i->prev)
        i->prev->next = i->next;
    else
        *head = i->next;
    if (i->next)
        i->next->prev = i->prev;
    i->prev = i->next = NULL;
}

static void
_link_ioctx (IOCtx *head, IOCtx i)
{
    i->next = *head;
    i->prev = NULL;
    if (*head)
        (*head)->prev = i;
    *head = i;
}

static void
_count_ioctx (IOCtx i, int *shared, int *unique)
{
    for (*unique = *shared = 0; i != NULL; i = i->next) {
        (*unique)++;
        xpthread_mutex_lock (&i->lock);
        (*shared) += i->refcount;
        xpthread_mutex_unlock (&i->lock);
    }
}

static IOCtx
_ioctx_incref (IOCtx ioctx)
{
    xpthread_mutex_lock (&ioctx->lock);
    ioctx->refcount++;
    xpthread_mutex_unlock (&ioctx->lock);

    return ioctx;
}

static int
_ioctx_decref (IOCtx ioctx)
{
    int n;

    xpthread_mutex_lock (&ioctx->lock);
    n = --ioctx->refcount;
    xpthread_mutex_unlock (&ioctx->lock);

    return n;
}

static int
_ioctx_close_destroy (IOCtx ioctx, int seterrno)
{
    int rc = 0;

    if (ioctx->dir) {
        rc = closedir(ioctx->dir);
        if (rc < 0 && seterrno)
            np_uerror (errno);
    } else if (ioctx->fd != -1) {
        rc = close (ioctx->fd);
        if (rc < 0 && seterrno)
            np_uerror (errno);
    }
    if (ioctx->user)
        np_user_decref (ioctx->user);
    pthread_mutex_destroy (&ioctx->lock);
    free (ioctx);

    return rc;
}

static IOCtx
_ioctx_create_open (Npuser *user, Path path, int flags, u32 mode)
{
    IOCtx ioctx;
    struct stat sb;

    ioctx = malloc (sizeof (*ioctx));
    if (!ioctx) {
        np_uerror (ENOMEM);
        goto error;
    }
    pthread_mutex_init (&ioctx->lock, NULL);
    ioctx->refcount = 1;
    ioctx->lock_type = LOCK_UN;
    ioctx->dir = NULL;
    ioctx->open_flags = flags;
    ioctx->user = user;
    np_user_incref (user);
    ioctx->prev = ioctx->next = NULL;
    ioctx->fd = open (path->s, flags, mode);
    if (ioctx->fd < 0) {
        np_uerror (errno);
        goto error;
    }
    if (fstat (ioctx->fd, &sb) < 0) {
        np_uerror (errno);
        goto error;
    }
    ioctx->iounit = 0; /* if iounit=0, v9fs will use msize-P9_IOHDRSZ */
    if (S_ISDIR(sb.st_mode) && !(ioctx->dir = fdopendir (ioctx->fd))) {
        np_uerror (errno);
        goto error;
    }
    diod_ustat2qid (&sb, &ioctx->qid);
    return ioctx;
error:
    if (ioctx)
        _ioctx_close_destroy (ioctx, 0);
    return NULL;
}

int
ioctx_close (Npfid *fid, int seterrno)
{
    Fid *f = fid->aux;
    int n;
    int rc = 0;

    NP_ASSERT (f->ioctx != NULL);

    xpthread_mutex_lock (&f->path->lock);
    n = _ioctx_decref (f->ioctx);
    if (n == 0)
        _unlink_ioctx (&f->path->ioctx, f->ioctx);
    xpthread_mutex_unlock (&f->path->lock);
    if (n == 0)
        rc = _ioctx_close_destroy (f->ioctx, seterrno);
    f->ioctx = NULL;

    return rc;
}

int
ioctx_open (Npfid *fid, u32 flags, u32 mode)
{
    Fid *f = fid->aux;
    IOCtx ip = NULL;

    NP_ASSERT (f->ioctx == NULL);

    xpthread_mutex_lock (&f->path->lock);
    if ((f->flags & DIOD_FID_FLAGS_SHAREFD) && (flags & 3) == O_RDONLY) {
        for (ip = f->path->ioctx; ip != NULL; ip = ip->next) {
            if (ip->qid.type != Qtfile)
                continue;
            if (ip->open_flags != flags)
                continue;
            if (ip->user->uid != fid->user->uid)
                continue;
            /* NOTE: we could do a stat and check qid? */
            _ioctx_incref (ip);
            break;
        }
    }
    if (!ip) {
        if ((ip = _ioctx_create_open (fid->user, f->path, flags, mode)))
            _link_ioctx (&f->path->ioctx, ip);
    }
    xpthread_mutex_unlock (&f->path->lock);
    if (!ip)
        goto error;
    f->ioctx = ip;
    return 0;
error:
    return -1;
}

int
ioctx_pread (IOCtx ioctx, void *buf, size_t count, off_t offset)
{
    return pread (ioctx->fd, buf, count, offset);
}

int
ioctx_pwrite (IOCtx ioctx, const void *buf, size_t count, off_t offset)
{
    return pwrite (ioctx->fd, buf, count, offset);
}

int
ioctx_stat (IOCtx ioctx, struct stat *sb)
{
    return fstat (ioctx->fd, sb);
}

int
ioctx_chmod (IOCtx ioctx, u32 mode)
{
    return fchmod (ioctx->fd, mode);
}

int
ioctx_chown (IOCtx ioctx, u32 uid, u32 gid)
{
    return fchown (ioctx->fd, uid, gid);
}

int
ioctx_truncate (IOCtx ioctx, u64 size)
{
    return ftruncate (ioctx->fd, size);
}

#if HAVE_UTIMENSAT
int
ioctx_utimensat (IOCtx ioctx, const struct timespec ts[2], int flags)
{
    return futimens (ioctx->fd, ts);
}

#else /* HAVE_UTIMENSAT */
int
ioctx_utimes (IOCtx ioctx, const utimbuf *times)
{
    return futimes (ioctx->fd, times);
}
#endif

void
ioctx_rewinddir (IOCtx ioctx)
{
    if (ioctx->dir)
        rewinddir (ioctx->dir);
}

void
ioctx_seekdir (IOCtx ioctx, long offset)
{
    if (ioctx->dir)
        seekdir (ioctx->dir, offset);
}

/* Modern readdir() is thread-safe, however, if d->off_t is not available,
 * take a lock over readdir() + telldir() so that if there are two threads
 * walking the directory, telldir() returns the offset after this readdir()
 * and not that of a racing thread.  If d->off_t is available, we can avoid
 * taking the lock.
 */
struct dirent *
ioctx_readdir(IOCtx ioctx, long *offset)
{
    struct dirent *d;

    if (!ioctx->dir) {
        errno = EINVAL;
        return NULL;
    }
#ifndef _DIRENT_HAVE_D_OFF
    xpthread_mutex_lock (&ioctx->lock);
#endif
    if (!(d = readdir (ioctx->dir)))
        goto done;
#ifndef _DIRENT_HAVE_D_OFF
    *offset = telldir (ioctx->dir);
#else
    *offset = d->d_off;
#endif
    xpthread_mutex_unlock (&ioctx->lock);
done:
#ifndef _DIRENT_HAVE_D_OFF
    xpthread_mutex_unlock (&ioctx->lock);
#endif
    return d;
}

int
ioctx_fsync(IOCtx ioctx, int datasync)
{
    if (datasync)
        return fdatasync (ioctx->fd);
    return fsync (ioctx->fd);
}

int
ioctx_flock (IOCtx ioctx, int operation)
{
    if (flock (ioctx->fd, operation) < 0)
        return -1;
    if ((operation & LOCK_UN))
        ioctx->lock_type = LOCK_UN;
    else if ((operation & LOCK_SH))
        ioctx->lock_type = LOCK_SH;
    else if ((operation & LOCK_EX))
        ioctx->lock_type = LOCK_EX;
    return 0;
}

/* If lock of 'type' could be obtained, return LOCK_UN, otherwise LOCK_EX.
 */
int
ioctx_testlock (IOCtx ioctx, int type)
{
    int ret = LOCK_UN;

    if (type == LOCK_SH) {
        switch (ioctx->lock_type) {
            case LOCK_EX:
            case LOCK_SH:
                break;
            case LOCK_UN:
                if (flock (ioctx->fd, LOCK_SH | LOCK_NB) == 0)
                    (void)flock (ioctx->fd, LOCK_UN);
                else
                    ret = LOCK_EX;
                break;
        }
    } else if (type == LOCK_EX) {
        switch (ioctx->lock_type) {
            case LOCK_EX:
                break;
            case LOCK_SH:
                /* Rather than upgrade the lock to LOCK_EX and risk
            `    * not reacquiring the LOCK_SH afterwards, lie about
                 * the lock being available.  Getlock is racy anyway.
                 */
                break;
            case LOCK_UN:
                if (flock (ioctx->fd, LOCK_EX | LOCK_NB) == 0)
                    (void)flock (ioctx->fd, LOCK_UN);
                else
                    ret = LOCK_EX; /* could also be LOCK_SH actually */
                break;
        }
    }
    return ret;
}

u32
ioctx_iounit (IOCtx ioctx)
{
    return ioctx->iounit;
}

Npqid *
ioctx_qid (IOCtx ioctx)
{
    return &ioctx->qid;
}

/* N.B. When diod_fidclone() calls path_incref(), the path will not be
 * removed from the pool even though the ppool lock is not held,
 * because the fid being cloned holds a reference on the path.
 */

static void
_path_free (Path path)
{
    NP_ASSERT (path->ioctx == NULL);
    if (path->s)
        free (path->s);
    pthread_mutex_destroy (&path->lock);
    free (path);
}

Path
path_incref (Path path)
{
    xpthread_mutex_lock (&path->lock);
    path->refcount++;
    xpthread_mutex_unlock (&path->lock);

    return path;
}

void
path_decref (Npsrv *srv, Path path)
{
    PathPool pp = srv->srvaux;
    int n;

    xpthread_mutex_lock (&pp->lock);
    xpthread_mutex_lock (&path->lock);
    n = --path->refcount;
    xpthread_mutex_unlock (&path->lock);
    if (n == 0)
        hash_remove (pp->hash, path->s);
    xpthread_mutex_unlock (&pp->lock);
    if (n == 0)
        _path_free (path);
}

static Path
_path_alloc (Npsrv *srv, char *s, int len)
{
    PathPool pp = srv->srvaux;
    Path path;

    xpthread_mutex_lock (&pp->lock);
    path = hash_find (pp->hash, s);
    if (path) {
        path_incref (path);
        free (s);
    } else {
        NP_ASSERT (errno == 0);
        if (!(path = malloc (sizeof (*path)))) {
            free (s);
            goto error;
        }
        path->refcount = 1;
        pthread_mutex_init (&path->lock, NULL);
        path->s = s;
        path->len = len;
        path->ioctx = NULL;
        if (!hash_insert (pp->hash, path->s, path)) {
            NP_ASSERT (errno == ENOMEM);
            goto error;
        }
    }
    xpthread_mutex_unlock (&pp->lock);
    return path;
error:
    xpthread_mutex_unlock (&pp->lock);
    if (path)
        _path_free (path);
    return NULL;
}

Path
path_create (Npsrv *srv, Npstr *ns)
{
    char *s;

    if (!(s = np_strdup (ns)))
        return NULL;
    return _path_alloc (srv, s, ns->len);
}

Path
path_append (Npsrv *srv, Path opath, Npstr *ns)
{
    char *s;
    int len = opath->len + 1 + ns->len;

    if (!(s = malloc (len + 1)))
        return NULL;
    memcpy (s, opath->s, opath->len);
    s[opath->len] = '/';
    memcpy (s + opath->len + 1, ns->str, ns->len);
    s[len] = '\0';
    return _path_alloc (srv, s, len);
}

char *
path_s (Path path)
{
    return path->s;
}

typedef struct {
    int len;
    char *s;
} DynStr;

static int
_get_one_file (Path path, char *s, DynStr *ds)
{
    int unique, shared;

    xpthread_mutex_lock (&path->lock);
    _count_ioctx (path->ioctx, &shared, &unique);
    aspf (&ds->s, &ds->len, "%d %d %d %s\n", path->refcount, shared, unique, s);
    xpthread_mutex_unlock (&path->lock);
    return 0;
}

static char *
_ppool_dump (char *name, void *a)
{
    Npsrv *srv = a;
    PathPool pp = srv->srvaux;
    DynStr ds = { .s = NULL, .len = 0 };

    xpthread_mutex_lock (&pp->lock);
    hash_for_each (pp->hash, (hash_arg_f)_get_one_file, &ds);
    xpthread_mutex_unlock (&pp->lock);

    return ds.s;
}

void
ppool_fini (Npsrv *srv)
{
    PathPool pp = srv->srvaux;

    if (pp) {
        if (pp->hash) {
            /* issue 99: this triggers when shutting down with active clients */
            /*NP_ASSERT (hash_is_empty (pp->hash));*/
            hash_destroy (pp->hash);
        }
        pthread_mutex_destroy (&pp->lock);
        free (pp);
    }
    srv->srvaux = NULL;
}

int
ppool_init (Npsrv *srv)
{
    PathPool pp;

    if (!(pp = malloc (sizeof (*pp))))
        goto error;

    pthread_mutex_init (&pp->lock, NULL);
    pp->hash = hash_create (1000,
                            (hash_key_f)hash_key_string,
                            (hash_cmp_f)strcmp, NULL);
    if (!pp->hash) {
        free (pp);
        goto error;
    }
    srv->srvaux = pp;
    if (!np_ctl_addfile (srv->ctlroot, "files", _ppool_dump, srv, 0))
        goto error;
    return 0;
error:
    ppool_fini (srv);
    return -1;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
