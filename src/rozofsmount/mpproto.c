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

#include <rozofs/common/log.h>
#include <rozofs/common/profile.h>
#include <string.h>

DECLARE_PROFILING(mpp_profiler_t);

void *mpp_null_1_svc(void *args, struct svc_req *req) {
    DEBUG_FUNCTION;
    return 0;
}

mpp_profiler_ret_t *mpp_get_profiler_1_svc(void * args,struct svc_req * req) {
    static mpp_profiler_ret_t ret;
    DEBUG_FUNCTION;

    SET_PROBE_VALUE(now, time(0))
    memcpy(&ret.mpp_profiler_ret_t_u.profiler, &gprofiler, sizeof(gprofiler));
    ret.status = MPP_SUCCESS;

    return &ret;
}

mpp_status_ret_t *mpp_clear_1_svc(void * args,struct svc_req * req) {
    static mpp_status_ret_t ret;
    DEBUG_FUNCTION;

    CLEAR_PROBE(rozofs_ll_lookup);
    CLEAR_PROBE(rozofs_ll_lookup);
    CLEAR_PROBE(rozofs_ll_forget);
    CLEAR_PROBE(rozofs_ll_getattr);
    CLEAR_PROBE(rozofs_ll_setattr);
    CLEAR_PROBE(rozofs_ll_readlink);
    CLEAR_PROBE(rozofs_ll_mknod);
    CLEAR_PROBE(rozofs_ll_mkdir);
    CLEAR_PROBE(rozofs_ll_unlink);
    CLEAR_PROBE(rozofs_ll_rmdir);
    CLEAR_PROBE(rozofs_ll_symlink);
    CLEAR_PROBE(rozofs_ll_rename);
    CLEAR_PROBE(rozofs_ll_open);
    CLEAR_PROBE(rozofs_ll_link);
    CLEAR_PROBE(rozofs_ll_read);
    CLEAR_PROBE(rozofs_ll_write);
    CLEAR_PROBE(rozofs_ll_flush);
    CLEAR_PROBE(rozofs_ll_release);
    CLEAR_PROBE(rozofs_ll_opendir);
    CLEAR_PROBE(rozofs_ll_readdir);
    CLEAR_PROBE(rozofs_ll_releasedir);
    CLEAR_PROBE(rozofs_ll_fsyncdir);
    CLEAR_PROBE(rozofs_ll_statfs);
    CLEAR_PROBE(rozofs_ll_setxattr);
    CLEAR_PROBE(rozofs_ll_getxattr);
    CLEAR_PROBE(rozofs_ll_listxattr);
    CLEAR_PROBE(rozofs_ll_removexattr);
    CLEAR_PROBE(rozofs_ll_access);
    CLEAR_PROBE(rozofs_ll_create);
    CLEAR_PROBE(rozofs_ll_getlk);
    CLEAR_PROBE(rozofs_ll_setlk);
    CLEAR_PROBE(rozofs_ll_clearlkowner);
    CLEAR_PROBE(rozofs_ll_ioctl);

    ret.status = MPP_SUCCESS;

    return &ret;
}
