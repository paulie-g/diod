/*************************************************************\
 * Copyright (C) 2005 by Latchesar Ionkov <lucho@ionkov.net>
 * Copyright (C) 2010 by Lawrence Livermore National Security, LLC.
 *
 * This file is part of npfs, a framework for 9P synthetic file systems.
 * For details see https://sourceforge.net/projects/npfs.
 *
 * SPDX-License-Identifier: MIT
 *************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <libgen.h>
#include <sys/param.h>

#include "src/libnpfs/npfs.h"
#include "npclient.h"
#include "npcimpl.h"

int
npc_readdir (Npcfid *fid, u64 offset, char *data, u32 count)
{
	Npfcall *tc = NULL, *rc = NULL;
	int ret = -1;

	if (!(tc = np_create_treaddir(fid->fid, offset, count))) {
		np_uerror (ENOMEM);
		goto done;
	}
	if (fid->fsys->rpc(fid->fsys, tc, &rc) < 0)
		goto done;
	NP_ASSERT(rc->u.rreaddir.count <= count);
	memcpy (data, rc->u.rreaddir.data, rc->u.rreaddir.count);
	ret = rc->u.rreaddir.count;
done:
	if (tc)
		free(tc);
	if (rc)
		free(rc);
	return ret;
}

Npcfid *
npc_opendir (Npcfid *root, char *path)
{
	Npcfid *fid = NULL;
	struct stat sb;

	if ((fid = npc_open_bypath (root, path, O_RDONLY))) {
		if (npc_fstat (fid, &sb) < 0)
			goto error;
		if (!S_ISDIR (sb.st_mode)) {
			np_uerror (ENOTDIR);
			goto error;
		}
		fid->buf_size = root->fsys->msize - IOHDRSZ;
		if (!(fid->buf = malloc (fid->buf_size))) {
			(void)npc_clunk (fid);
			np_uerror (ENOMEM);
			fid = NULL;
		}
		fid->offset = 0;
		fid->buf_len = 0;
		fid->buf_used = 0;
	}
	return fid;
error:
	if (fid)
		npc_clunk(fid);
	return NULL;
}

/* Returns error code > 0 on error.
 * Returns 0 on success: result set to NULL at EOF, o/w set to entry
 */
int
npc_readdir_r (Npcfid *fid, struct dirent *entry, struct dirent **result)
{
	Npqid qid;
	int dname_size = PATH_MAX + 1;
	u64 offset;
	u8 type;
	int res;

	if (!fid->buf) /* not opened with npc_opendir */
		return EINVAL;

	if (fid->buf_used >= fid->buf_len) {
		fid->buf_len = npc_readdir (fid, fid->offset, fid->buf,
					      fid->buf_size);
		if (fid->buf_len < 0)
			return np_rerror ();
		if (fid->buf_len == 0) {	/* EOF */
			*result = NULL;
			return 0;
		}
		fid->buf_used = 0;
	}
	res = np_deserialize_p9dirent (&qid, &offset, &type,
				       entry->d_name, dname_size,
				       (u8 *)fid->buf + fid->buf_used,
				       fid->buf_len   - fid->buf_used);
	if (res == 0)
		return EIO;
#ifdef _DIRENT_HAVE_D_OFF
	entry->d_off = offset;
#endif
	entry->d_type = type;
	entry->d_ino = qid.path;
	//entry->d_reclen
	fid->offset = offset;
	fid->buf_used += res;
	*result = entry;
	return 0;
}

void
npc_seekdir (Npcfid *fid, long offset)
{
	fid->offset = offset;
	fid->buf_used = fid->buf_len; /* force a 9p readdir call */
}

long
npc_telldir (Npcfid *fid)
{
	return fid->offset;
}
