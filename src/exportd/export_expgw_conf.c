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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <netinet/tcp.h>
#include <getopt.h>
#include <libconfig.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>
#include <netdb.h>

#include <rozofs/rozofs.h>
#include <rozofs/common/log.h>
#include <rozofs/common/xmalloc.h>

#include "config.h"
#include "exportd.h"
#include "export.h"
#include "econfig.h"
#include "export_expgw_conf.h"
#include <rozofs/rpc/gwproto.h>


list_t expgws;
uint32_t expgws_nb_gateways;
uint32_t expgws_timestamp = 0;
pthread_rwlock_t expgws_lock;




int expgw_root_initialize() {
    list_init(&expgws);
    expgws_nb_gateways = 0;
    expgws_timestamp += 1;
    if (pthread_rwlock_init(&expgws_lock, NULL) != 0) {
        return -1;
    }
    return 0;
}






/*
**_________________________________________________________
*/
/** initialize a expgw
 *
 * @param expgw: the expgw to initialize
 * @param daemon_id: the id to set
 * @param size: the size to set
 * @param free: free space to set
 */

int expgw_initialize(expgw_t *expgw, int daemon_id) {
    int status = -1;
    DEBUG_FUNCTION;
    expgw->daemon_id = daemon_id;
    list_init(&expgw->expgw_storages);
    if (pthread_rwlock_init(&expgw->lock, NULL) != 0) {
        goto out;
    }
    status = 0;
out:
    return status;
}
/*
**_________________________________________________________
*/

/** release a expgw
 *
 * empty the list of expgw storages and free each entry
 * (assuming entry were well allocated)
 *
 * @param expgw: the expgw to release
 */
void expgw_release(expgw_t *expgw)
{
    list_t *p, *q;
    DEBUG_FUNCTION;

    list_for_each_forward_safe(p, q, &expgw->expgw_storages) {
        expgw_storage_t *entry = list_entry(p, expgw_storage_t, list);
        list_remove(p);
        expgw_storage_release(entry);
        free(entry);
    }
    if ((errno = pthread_rwlock_destroy(&expgw->lock)) != 0) {
        severe("can't release expgw lock: %s", strerror(errno));
    }
}
/*
**_________________________________________________________
*/
/** initialize a expgw storage
 *
 * @param vs: the expgw storage to initialize
 * @param gwid: identifier to set
 * @param hostname : hostname to set (memory is copied)
 */
void expgw_storage_initialize(expgw_storage_t * vs, int gwid,
        const char *hostname) {
    DEBUG_FUNCTION;
    vs->gwid = gwid;
    strcpy(vs->host, hostname);
    vs->status = 0;
    list_init(&vs->list);
}

/*
**_________________________________________________________
*/
/** release a expgw storage
 *
 * has no effect for now
 *
 * @param vs: the expgw storage to release
 */
void expgw_storage_release(expgw_storage_t *vs) {
    DEBUG_FUNCTION;
    return;
}

/*
**_________________________________________________________
*/
int load_export_expgws_conf() {
    list_t  *q, *r;
    DEBUG_FUNCTION;

   /* For each expgw */

   list_for_each_forward(q, &exportd_config.expgw) 
   {
       expgw_config_t *cconfig = list_entry(q, expgw_config_t, list);
       expgw_entry_t *ventry = 0;

       // Memory allocation for this expgw
       ventry = (expgw_entry_t *) xmalloc(sizeof (expgw_entry_t));            
       expgw_initialize(&ventry->expgw, cconfig->daemon_id);

       /* For each expgw of the export_expgw set */
       list_for_each_forward(r, &cconfig->expgw_node) 
       {
           expgw_node_config_t *sconfig = list_entry(r, expgw_node_config_t, list);
           expgw_storage_t *vs = (expgw_storage_t *) xmalloc(sizeof (expgw_storage_t));
           expgw_storage_initialize(vs, sconfig->gwid, sconfig->host);
           list_push_back(&ventry->expgw.expgw_storages, &vs->list);
       }

   // Add this expgw to the list of the expgw
   list_push_back(&expgws, &ventry->list);
   }

    return 0;
}


/*
**_________________________________________________________
*/
/**
*  that procedure is intended to return the pointer to  the
   expgw context whose identifier is daemon_id
   
   @param daemon_id : expgw identifier
   
   @retval <> NULL: expgw context
   @retval NULL not found 
*/
expgw_t *expgws_lookup_expgw(int daemon_id) {
    list_t *iterator;
    DEBUG_FUNCTION;

    if ((errno = pthread_rwlock_rdlock(&expgws_lock)) != 0) {
        severe("can't lock expgws.");
        return NULL;
    }

    list_for_each_forward(iterator, &expgws) {
        expgw_entry_t *entry = list_entry(iterator, expgw_entry_t, list);
        if (daemon_id == entry->expgw.daemon_id) {
            if ((errno = pthread_rwlock_unlock(&expgws_lock)) != 0) {
                severe("can't unlock expgws, potential dead lock.");
                return NULL;
            }
            return &entry->expgw;
        }
    }

    if ((errno = pthread_rwlock_unlock(&expgws_lock)) != 0) {
        severe("can't unlock expgws, potential dead lock.");
        return NULL;
    }

    errno = EINVAL;
    return NULL;
}


/** release all the export expgws
 *
 * empty the list of volume storages and free each entry
 * (assuming entry were well allocated)
 *
 * @param expgw: the expgw to release
 */
void export_expgws_release()
{
    list_t *p, *q;

    list_for_each_forward_safe(p, q, &expgws) {
        expgw_entry_t *entry = list_entry(p, expgw_entry_t, list);
        expgw_release(&entry->expgw);
        list_remove(p);
        free(entry);
    }
    if ((errno = pthread_rwlock_destroy(&expgws_lock)) != 0) {
        severe("can't release expgws lock: %s", strerror(errno));
    }
}

