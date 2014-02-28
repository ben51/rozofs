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

#include <limits.h>
#include <errno.h>
#include <string.h>

#include <rozofs/common/log.h>
#include <rozofs/common/xmalloc.h>
#include <rozofs/common/profile.h>
#include <rozofs/rpc/epproto.h>

#include "export.h"
#include "volume.h"
#include "exportd.h"

DECLARE_PROFILING(epp_profiler_t);

void *epp_null_1_svc(void *args, struct svc_req *req) {
    DEBUG_FUNCTION;
    return 0;
}

epp_profiler_ret_t *epp_get_profiler_1_svc(void * args,struct svc_req * req) {
    static epp_profiler_ret_t ret;
    DEBUG_FUNCTION;

    /*XXX should acquire lock on monitor thread ! */
    SET_PROBE_VALUE(now, time(0))
    memcpy(&ret.epp_profiler_ret_t_u.profiler, &gprofiler, sizeof(gprofiler));
    ret.status = EPP_SUCCESS;

    return &ret;
}

epp_status_ret_t *epp_clear_1_svc(void * args,struct svc_req * req) {
    static epp_status_ret_t ret;
    DEBUG_FUNCTION;

    ret.status = EPP_SUCCESS;

    return &ret;
}
