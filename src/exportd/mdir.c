/*
 Copyright (c) 2010 Fizians SAS. <http://www.fizians.com>
 This file is part of Rozofs.

 Rozofs is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published
 by the Free Software Foundation, version 2.

 Rozofs is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see
 <http://www.gnu.org/licenses/>.
 */

#ifdef __linux__
#define _XOPEN_SOURCE 700
#endif

#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <attr/xattr.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

#include <rozofs/rpc/export_profiler.h>
#include <rozofs/rpc/epproto.h>

#include "mdir.h"

int mdir_open(mdir_t *mdir, const char *path) {
    int status = -1;

    START_PROFILING(mdir_open);

    if ((mdir->fdp = open(path, O_RDONLY
#ifdef __linux__
				| O_NOATIME
#endif
				, S_IRWXU)) < 0) {
        goto out;
    }

    if ((mdir->fdattrs = openat(mdir->fdp, MDIR_ATTRS_FNAME, O_RDWR | O_CREAT,
            S_IRWXU)) < 0) {
        int xerrno = errno;
        close(mdir->fdp);
        errno = xerrno;
        goto out;
    }
    status = 0;
out:
    STOP_PROFILING(mdir_open);
    return status;
}

void mdir_close(mdir_t *mdir) {
    START_PROFILING(mdir_close);
    close(mdir->fdattrs);
    close(mdir->fdp);
    STOP_PROFILING(mdir_close);
}

int mdir_read_attributes(mdir_t *mdir, mattr_t *attrs) {
    int status = -1;

    START_PROFILING(mdir_read_attributes);

    // read attributes
    if (pread(mdir->fdattrs, attrs, sizeof (mattr_t), 0) != sizeof (mattr_t)) {
        goto out;
    }

    status = 0;
out:
    STOP_PROFILING(mdir_read_attributes);
    return status;
}

int mdir_write_attributes(mdir_t *mdir, mattr_t *attrs) {
    int status = -1;

    START_PROFILING(mdir_write_attributes);
    // write attributes
    if (pwrite(mdir->fdattrs, attrs, sizeof (mattr_t), 0) != sizeof (mattr_t)) {
        return -1;
    }

    status = 0;

    STOP_PROFILING(mdir_write_attributes);
    return status;
}

/*
 *  Extended attributes of a directory are placed in the directory itself
 *  and not in the attributes file because when the filesystem use ACL,
 *  ACL defaults attributes can not be applied on regular files.
*/

int mdir_set_xattr(mdir_t *mdir, const char *name, const void *value, 
        size_t size, int flags) {
    int status;

    status = fsetxattr(mdir->fdp, name, value, size, flags);

    return status;
}

ssize_t mdir_get_xattr(mdir_t *mdir, const char *name, void *value,
        size_t size) {
    ssize_t status;

    status = fgetxattr(mdir->fdp, name, value, size);

    return status;
}

int mdir_remove_xattr(mdir_t *mdir, const char *name) {
    int status;

    status = fremovexattr(mdir->fdp, name);

    return status;
}

ssize_t mdir_list_xattr(mdir_t *mdir, char *list, size_t size) {
    ssize_t status;

    status = flistxattr(mdir->fdp, list, size);

    return status;
}
