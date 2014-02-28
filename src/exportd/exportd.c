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
#include <inttypes.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>

#include <rozofs/rozofs_debug_ports.h>
#include <rozofs/rozofs.h>
#include <rozofs/common/log.h>
#include <rozofs/rozofs_srv.h>
#include <rozofs/common/daemon.h>
#include <rozofs/common/xmalloc.h>
#include <rozofs/rpc/eproto.h>
#include <rozofs/rpc/epproto.h>
#include <rozofs/rozofs_timer_conf.h>
#include <rozofs/rpc/gwproto.h>
#include <rozofs/core/rozofs_ip_utilities.h>
#include <rozofs/rpc/export_profiler.h>
#include <rozofs/common/profile.h>

#include "config.h"
#include "exportd.h"
#include "export.h"
#include "monitor.h"
#include "econfig.h"
#include "volume.h"
#include "export_expgw_conf.h"
#include "export_internal_channel.h"

export_one_profiler_t  * export_profiler[EXPGW_EID_MAX_IDX+1] = { 0 };
uint32_t		 export_profiler_eid = 0;

DEFINE_PROFILING(epp_profiler_t) = {0};

#define EXPORTD_PID_FILE "exportd.pid"
/* Maximum open file descriptor number for exportd daemon */
#define EXPORTD_MAX_OPEN_FILES 5000

econfig_t exportd_config;
pthread_rwlock_t config_lock;

lv2_cache_t cache;

typedef struct export_entry {
    export_t export;
    list_t list;
} export_entry_t;

static list_t exports;
static pthread_rwlock_t exports_lock;

typedef struct volume_entry {
    volume_t volume;
    list_t list;
} volume_entry_t;

static list_t volumes;
static pthread_rwlock_t volumes_lock;

static pthread_t bal_vol_thread;
static pthread_t rm_bins_thread;
static pthread_t monitor_thread;

static char exportd_config_file[PATH_MAX] = EXPORTD_DEFAULT_CONFIG;

static SVCXPRT *exportd_svc = NULL;

extern void export_program_1(struct svc_req *rqstp, SVCXPRT * ctl_svc);

static SVCXPRT *exportd_profile_svc = 0;

extern void exportd_profile_program_1(struct svc_req *rqstp, SVCXPRT *ctl_svc);


exportd_start_conf_param_t  expgwc_non_blocking_conf;  /**< configuration of the non blocking side */

gw_configuration_t  expgw_conf;

uint32_t expgw_eid_table[EXPGW_EID_MAX_IDX];
gw_host_conf_t expgw_host_table[EXPGW_EXPGW_MAX_IDX];

uint32_t export_configuration_file_hash = 0;  /**< hash value of the configuration file */
/*
 *_______________________________________________________________________
 */
/**
*  compute the hash of a configuration file

   @param : full pathname of the file
   @param[out]: pointer to the array where hash value is returned

   @retval 0 on success
   @retval -1 on error
*/
int hash_file_compute(char *path,uint32_t *hash_p)
{
  uint32_t hash=0;
  uint8_t c;

  FILE *fp = fopen( path,"r");
  if (fp < 0)
  {
    return -1;
  }
  while (!feof(fp) && !ferror(fp))
  {
    c = fgetc(fp);
    hash = c + (hash << 6) + (hash << 16) - hash;
  }
  *hash_p = hash;
  fclose(fp);
  return 0;
}


/*
 *_______________________________________________________________________
 */

void expgw_init_configuration_message(char *exportd_hostname)
{
  gw_configuration_t *expgw_conf_p = &expgw_conf;
  expgw_conf_p->eid.eid_val = expgw_eid_table;
  expgw_conf_p->eid.eid_len = 0;
  expgw_conf_p->exportd_host = malloc(ROZOFS_HOSTNAME_MAX + 1);
  strcpy(expgw_conf_p->exportd_host, exportd_hostname);
  memset(expgw_conf_p->eid.eid_val, 0, sizeof(expgw_eid_table));
  expgw_conf_p->gateway_host.gateway_host_val = expgw_host_table;
  memset(expgw_conf_p->gateway_host.gateway_host_val, 0, sizeof(expgw_host_table));
  int i;
  for (i = 0; i < EXPGW_EXPGW_MAX_IDX; i++)
  {
    expgw_host_table[i].host = malloc( ROZOFS_HOSTNAME_MAX+1);
  }
  expgw_conf_p->gateway_host.gateway_host_len = 0;
}


/*
 *_______________________________________________________________________
 */
void expgw_reinit_configuration_message()
{
  gw_configuration_t  *expgw_conf_p = &expgw_conf;
  expgw_conf_p->eid.eid_val = expgw_eid_table;
  expgw_conf_p->eid.eid_len = 0;
  expgw_conf_p->gateway_host.gateway_host_val = expgw_host_table;
  expgw_conf_p->gateway_host.gateway_host_len = 0;
}

/*
 *_______________________________________________________________________
 */
int expgw_build_configuration_message(char * pchar, uint32_t size)
{
    list_t *iterator;
    list_t *iterator_expgw;
    DEBUG_FUNCTION;

    gw_configuration_t *expgw_conf_p = &expgw_conf;

    expgw_reinit_configuration_message();

    expgw_conf_p->eid.eid_len = 0;
    expgw_conf_p->exportd_port = 0;
    expgw_conf_p->gateway_port = 0;

    // lock the exportd configuration while searching for the eid handled by the export daemon
    if ((errno = pthread_rwlock_rdlock(&exports_lock)) != 0)
    {
        severe("can't lock exports.");
        return -1;
    }

    list_for_each_forward(iterator, &exports)
    {
       export_entry_t *entry = list_entry(iterator, export_entry_t, list);
       expgw_eid_table[expgw_conf_p->eid.eid_len] = entry->export.eid;
       expgw_conf_p->eid.eid_len++;
    }

    // unlock exportd config
    if ((errno = pthread_rwlock_unlock(&exports_lock)) != 0)
    {
        severe("can't unlock exports, potential dead lock.");
        return -1;
    }

/*
    if (expgw_conf_p->eid.eid_len == 0)
    {
      severe("no eid in the exportd configuration !!");
      return -1;
    }
*/
    /*
    ** now go through the exportd gateway configuration
    */
    if ((errno = pthread_rwlock_rdlock(&expgws_lock)) != 0)
    {
        severe("can't lock expgws.");
        return -1;
    }
    expgw_conf_p->hdr.export_id = 0;
    expgw_conf_p->hdr.nb_gateways = 0;
    expgw_conf_p->hdr.gateway_rank = 0;
    expgw_conf_p->hdr.configuration_indice = export_configuration_file_hash;

    list_for_each_forward(iterator, &expgws)
    {
        expgw_entry_t *entry = list_entry(iterator, expgw_entry_t, list);
        expgw_conf_p->hdr.export_id = entry->expgw.daemon_id;
        expgw_t *expgw = &entry->expgw;

        // loop on the storage
        list_for_each_forward(iterator_expgw, &expgw->expgw_storages) {
          expgw_storage_t *entry = list_entry(iterator_expgw, expgw_storage_t, list);
          // copy the hostname
          strcpy((char*)expgw_host_table[expgw_conf_p->gateway_host.gateway_host_len].host, entry->host);
          expgw_conf_p->gateway_host.gateway_host_len++;
          expgw_conf_p->hdr.nb_gateways++;
        }
    }

    if ((errno = pthread_rwlock_unlock(&expgws_lock)) != 0)
    {
        severe("can't unlock expgws, potential dead lock.");
        return -1;
    }

    XDR xdrs;
    int total_len = -1;

    xdrmem_create(&xdrs,(char*)pchar,size,XDR_ENCODE);

    if (xdr_gw_configuration_t(&xdrs,expgw_conf_p) == FALSE){
        severe("encoding error");
    }else{
        total_len = xdr_getpos(&xdrs) ;
    }
    return total_len;
}

static void *balance_volume_thread(void *v) {
    struct timespec ts = {8, 0};

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    for (;;) {
        list_t *p;

        if ((errno = pthread_rwlock_tryrdlock(&volumes_lock)) != 0) {
            warning("can lock volumes, balance_volume_thread deferred.");
            nanosleep(&ts, NULL);
            continue;
        }

        list_for_each_forward(p, &volumes) {
            volume_balance(&list_entry(p, volume_entry_t, list)->volume);
        }

        if ((errno = pthread_rwlock_unlock(&volumes_lock)) != 0) {
            severe("can unlock volumes, potential dead lock.");
        }

        nanosleep(&ts, NULL);
    }
    return 0;
}

/** Thread for remove bins files on storages for each exports
 */
static void *remove_bins_thread(void *v) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    list_t *iterator = NULL;
    int export_idx = 0;
    // Set the frequency of calls
    struct timespec ts = {RM_BINS_PTHREAD_FREQUENCY_SEC, 0};

    // Pointer for store start bucket index for each exports
    uint16_t * idx_p = xmalloc(list_size(&exports) * sizeof (uint16_t));
    // Init. each index to 0
    memset(idx_p, 0, sizeof (uint16_t) * list_size(&exports));

    for (;;) {
        export_idx = 0;

        list_for_each_forward(iterator, &exports) {
            export_entry_t *entry = list_entry(iterator, export_entry_t, list);

            // Remove bins file starting with specific bucket idx
            if (export_rm_bins(&entry->export, &idx_p[export_idx]) != 0) {
                severe("export_rm_bins failed (eid: %"PRIu32"): %s",
                        entry->export.eid, strerror(errno));
            }
            export_idx++;
        }
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void *monitoring_thread(void *v) {
    struct timespec ts = {2, 0};
    list_t *p;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    for (;;) {
        if ((errno = pthread_rwlock_tryrdlock(&volumes_lock)) != 0) {
            warning("can't lock volumes, monitoring_thread deferred.");
            nanosleep(&ts, NULL);
            continue;
        }


        gprofiler.nb_volumes = 0;

        list_for_each_forward(p, &volumes) {
            if (monitor_volume(&list_entry(p, volume_entry_t, list)->volume) != 0) {
                severe("monitor thread failed: %s", strerror(errno));
            }
            gprofiler.nb_volumes++;
        }

        if ((errno = pthread_rwlock_unlock(&volumes_lock)) != 0) {
            severe("can't unlock volumes, potential dead lock.");
            continue;
        }

        if ((errno = pthread_rwlock_tryrdlock(&exports_lock)) != 0) {
            warning("can't lock exports, monitoring_thread deferred.");
            nanosleep(&ts, NULL);
            continue;
        }

        gprofiler.nb_exports = 0;

        list_for_each_forward(p, &exports) {
            if (monitor_export(&list_entry(p, export_entry_t, list)->export) != 0) {
                severe("monitor thread failed: %s", strerror(errno));
            }
            gprofiler.nb_exports++;
        }

        if ((errno = pthread_rwlock_unlock(&exports_lock)) != 0) {
            severe("can't unlock exports, potential dead lock.");
            continue;
        }

        nanosleep(&ts, NULL);
    }
    return 0;
}

eid_t *exports_lookup_id(ep_path_t path) {
    list_t *iterator;
    char export_path[PATH_MAX];
    DEBUG_FUNCTION;

    if (!realpath(path, export_path)) {
        return NULL;
    }

    if ((errno = pthread_rwlock_rdlock(&exports_lock)) != 0) {
        severe("can lock exports.");
        return NULL;
    }

    list_for_each_forward(iterator, &exports) {
        export_entry_t *entry = list_entry(iterator, export_entry_t, list);
        if (strcmp(entry->export.root, export_path) == 0) {
            if ((errno = pthread_rwlock_unlock(&exports_lock)) != 0) {
                severe("can unlock exports, potential dead lock.");
                return NULL;
            }
            return &entry->export.eid;
        }
    }

    if ((errno = pthread_rwlock_unlock(&exports_lock)) != 0) {
        severe("can unlock exports, potential dead lock.");
        return NULL;
    }
    errno = EINVAL;
    return NULL;
}

export_t *exports_lookup_export(eid_t eid) {
    list_t *iterator;
    DEBUG_FUNCTION;

    if ((errno = pthread_rwlock_rdlock(&exports_lock)) != 0) {
        severe("can lock exports.");
        return NULL;
    }

    list_for_each_forward(iterator, &exports) {
        export_entry_t *entry = list_entry(iterator, export_entry_t, list);
        if (eid == entry->export.eid) {
            if ((errno = pthread_rwlock_unlock(&exports_lock)) != 0) {
                severe("can unlock exports, potential dead lock.");
                return NULL;
            }
            return &entry->export;
        }
    }

    if ((errno = pthread_rwlock_unlock(&exports_lock)) != 0) {
        severe("can unlock exports, potential dead lock.");
        return NULL;
    }
    errno = EINVAL;
    return NULL;
}

volume_t *volumes_lookup_volume(vid_t vid) {
    list_t *iterator;
    DEBUG_FUNCTION;

    if ((errno = pthread_rwlock_rdlock(&volumes_lock)) != 0) {
        severe("can't lock volumes.");
        return NULL;
    }

    list_for_each_forward(iterator, &volumes) {
        volume_entry_t *entry = list_entry(iterator, volume_entry_t, list);
        if (vid == entry->volume.vid) {
            if ((errno = pthread_rwlock_unlock(&volumes_lock)) != 0) {
                severe("can't unlock volumes, potential dead lock.");
                return NULL;
            }
            return &entry->volume;
        }
    }

    if ((errno = pthread_rwlock_unlock(&volumes_lock)) != 0) {
        severe("can't unlock volumes, potential dead lock.");
        return NULL;
    }

    errno = EINVAL;
    return NULL;
}

int exports_initialize() {
    list_init(&exports);
    if (pthread_rwlock_init(&exports_lock, NULL) != 0) {
        return -1;
    }
    return 0;
}

int volumes_initialize() {
    list_init(&volumes);
    if (pthread_rwlock_init(&volumes_lock, NULL) != 0) {
        return -1;
    }
    return 0;
}

void exports_release() {
    list_t *p, *q;

    list_for_each_forward_safe(p, q, &exports) {
        export_entry_t *entry = list_entry(p, export_entry_t, list);
	export_profiler_free(entry->export.eid);
        export_release(&entry->export);
        list_remove(p);
        free(entry);
    }

    if ((errno = pthread_rwlock_destroy(&exports_lock)) != 0) {
        severe("can't release exports lock: %s", strerror(errno));
    }
}

void volumes_release() {
    list_t *p, *q;

    list_for_each_forward_safe(p, q, &volumes) {
        volume_entry_t *entry = list_entry(p, volume_entry_t, list);
        volume_release(&entry->volume);
        list_remove(p);
        free(entry);
    }
    if ((errno = pthread_rwlock_destroy(&volumes_lock)) != 0) {
        severe("can't release volumes lock: %s", strerror(errno));
    }
}

static int load_volumes_conf() {
    list_t *p, *q, *r;
    DEBUG_FUNCTION;

    // For each volume

    list_for_each_forward(p, &exportd_config.volumes) {
        volume_config_t *vconfig = list_entry(p, volume_config_t, list);
        volume_entry_t *ventry = 0;

        // Memory allocation for this volume
        ventry = (volume_entry_t *) xmalloc(sizeof (volume_entry_t));

        // Initialize the volume
        volume_initialize(&ventry->volume, vconfig->vid, vconfig->layout);

        // For each cluster of this volume

        list_for_each_forward(q, &vconfig->clusters) {
            cluster_config_t *cconfig = list_entry(q, cluster_config_t, list);

            // Memory allocation for this cluster
            cluster_t *cluster = (cluster_t *) xmalloc(sizeof (cluster_t));
            cluster_initialize(cluster, cconfig->cid, 0, 0);

            list_for_each_forward(r, &cconfig->storages) {
                storage_node_config_t *sconfig = list_entry(r, storage_node_config_t, list);
                volume_storage_t *vs = (volume_storage_t *) xmalloc(sizeof (volume_storage_t));
                volume_storage_initialize(vs, sconfig->sid, sconfig->host);
                list_push_back(&cluster->storages, &vs->list);
            }
            // Add this cluster to the list of this volume
            list_push_back(&ventry->volume.clusters, &cluster->list);
        }
        // Add this volume to the list of volume
        list_push_back(&volumes, &ventry->list);
    }

    return 0;
}

static int load_exports_conf() {
    int status = -1;
    list_t *p;
    DEBUG_FUNCTION;

    // For each export

    list_for_each_forward(p, &exportd_config.exports) {
        export_config_t *econfig = list_entry(p, export_config_t, list);
        export_entry_t *entry = xmalloc(sizeof (export_entry_t));
        volume_t *volume;

        list_init(&entry->list);

        if (!(volume = volumes_lookup_volume(econfig->vid))) {
            severe("can't lookup volume for vid %d: %s\n",
                    econfig->vid, strerror(errno));
        }

        if (export_is_valid(econfig->root) != 0) {
            // try to create it
            if (export_create(econfig->root) != 0) {
                severe("can't create export with path %s: %s\n",
                        econfig->root, strerror(errno));
                goto out;
            }
        }

        // Initialize export
        if (export_initialize(&entry->export, volume,
                &cache, econfig->eid, econfig->root, econfig->md5,
                econfig->squota, econfig->hquota) != 0) {
            severe("can't initialize export with path %s: %s\n",
                    econfig->root, strerror(errno));
            goto out;
        }
	
	// Allocate default profiler structure
        export_profiler_allocate(econfig->eid);


        // Add this export to the list of exports
        list_push_back(&exports, &entry->list);
    }

    status = 0;
out:
    return status;
}

static int exportd_initialize() {
    DEBUG_FUNCTION;

    if ((errno = pthread_rwlock_init(&config_lock, NULL)) != 0)
        fatal("can't initialize lock for config: %s", strerror(errno));

    // Initialize lv2 cache
    lv2_cache_initialize(&cache);

    // Initialize list of volume(s)
    if (volumes_initialize() != 0)
        fatal("can't initialize volume: %s", strerror(errno));

    // Initialize list of export gateway(s)
    if (expgw_root_initialize() != 0)
        fatal("can't initialize export gateways: %s", strerror(errno));

    // Initialize list of exports
    if (exports_initialize() != 0)
        fatal("can't initialize exports: %s", strerror(errno));

    // Initialize monitoring
    if (monitor_initialize() != 0)
        fatal("can't initialize monitoring: %s", strerror(errno));

    // Load rozofs parameters
    rozofs_layout_initialize();

    if (load_volumes_conf() != 0)
        fatal("can't load volume");

    if (load_export_expgws_conf() != 0)
        fatal("can't load export gateways");


    if (load_exports_conf() != 0)
        fatal("can't load exports");


    if (pthread_create(&bal_vol_thread, NULL, balance_volume_thread, NULL) !=
            0)
        fatal("can't create balancing thread %s", strerror(errno));

    if (pthread_create(&rm_bins_thread, NULL, remove_bins_thread, NULL) != 0)
        fatal("can't create remove files thread %s", strerror(errno));

    if (pthread_create(&monitor_thread, NULL, monitoring_thread, NULL) != 0)
        fatal("can't create monitoring thread %s", strerror(errno));

    return 0;
}

static void exportd_release() {

    pthread_cancel(bal_vol_thread);
    pthread_cancel(rm_bins_thread);
    pthread_cancel(monitor_thread);

    if ((errno = pthread_rwlock_destroy(&config_lock)) != 0) {
        severe("can't release config lock: %s", strerror(errno));
    }

    monitor_release();
    exports_release();
    volumes_release();
    export_expgws_release();
    econfig_release(&exportd_config);
    lv2_cache_release(&cache);
}

static void on_start() {
    int sock;
    int one = 1;
    struct rlimit rls;
    pthread_t thread;
    DEBUG_FUNCTION;
    int loop_count = 0;

    // Allocate default profiler structure
    export_profiler_allocate(0);
    
    /**
    * start the non blocking thread
    */
    expgwc_non_blocking_thread_started = 0;
    if ((errno = pthread_create(&thread, NULL, (void*) expgwc_start_nb_blocking_th, &expgwc_non_blocking_conf)) != 0) {
        severe("can't create non blocking thread: %s", strerror(errno));
    }

    if (exportd_initialize() != 0) {
        fatal("can't initialize exportd.");
    }
    /*
    ** wait for end of init of the non blocking thread
    */
    while (expgwc_non_blocking_thread_started == 0)
    {
       sleep(1);
       loop_count++;
       if (loop_count > 5) fatal("Non Blocking thread does not answer");
    }

     /*
     ** build the structure for sending out an exportd gateway configuration message
     */
#define MSG_SIZE  (32*1024)
     char * pChar = malloc(MSG_SIZE);
     int msg_sz;
     if (pChar == NULL)  {
       fatal("malloc %d", MSG_SIZE);
     }
    expgw_init_configuration_message(exportd_config.exportd_vip);
    if ( (msg_sz = expgw_build_configuration_message(pChar, MSG_SIZE) ) < 0)
    {
        fatal("can't build export gateway configuration message");
    }
    /*
    ** create the array for building export gateway configuration
    ** message for rozofsmount
    */
    ep_expgw_init_configuration_message(exportd_config.exportd_vip);

    /*
    ** create the array for storing storage configuration
    */
    if (exportd_init_storage_configuration_message()!=0)
    {
        fatal("can't allocate storage configuration array");

    }

    /*
    ** Send the exportd gateway configuration towards the non blocking thread
    */
    {
      int ret;
      ret = expgwc_internal_channel_send(EXPGWC_LOAD_CONF,msg_sz,pChar);
      if (ret < 0)
      {
         severe("EXPGWC_LOAD_CONF: %s",strerror(errno));
      }
    }
    free(pChar);

    // Metadata service
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    // SET NONBLOCK
    int value = 1;
    int oldflags = fcntl(sock, F_GETFL, 0);
    // If reading the flags failed, return error indication now
    if (oldflags < 0) {
        fatal("can't initialize exportd.");
        return;
    }
    // Set just the flag we want to set
    if (value != 0) {
        oldflags |= O_NONBLOCK;
    } else {
        oldflags &= ~O_NONBLOCK;
    }
    // Store modified flag word in the descriptor
    fcntl(sock, F_SETFL, oldflags);

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof (int));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof (int));
//    setsockopt(sock, SOL_TCP, TCP_DEFER_ACCEPT, (char *) &one, sizeof (int));


    // Change the value of the maximum file descriptor number
    // that can be opened by this process.
    rls.rlim_cur = EXPORTD_MAX_OPEN_FILES;
    rls.rlim_max = EXPORTD_MAX_OPEN_FILES;
    if (setrlimit(RLIMIT_NOFILE, &rls) < 0) {
        warning("Failed to change open files limit to %u", EXPORTD_MAX_OPEN_FILES);
    }
    
    {
      pmap_unset(EXPORT_PROGRAM, EXPORT_VERSION); // in case !

      struct sockaddr_in sin;
      uint16_t tcp_port = ROZOFS_GET_EXPNB_PORT;

            
      sin.sin_addr.s_addr = INADDR_ANY;
//      sin.sin_addr.s_addr = htonl(0x7F000001);
      sin.sin_family      = AF_INET;
      sin.sin_port        = htons(tcp_port); 
      if (bind(sock, (struct sockaddr *) &sin, sizeof(struct sockaddr)) < 0) {
        fatal("bind %s",strerror(errno));
      }
      
      pmap_set(EXPORT_PROGRAM, EXPORT_VERSION, IPPROTO_TCP, tcp_port);
      
    }

    // XXX Buffers sizes hard coded
    exportd_svc = svctcp_create(sock, ROZOFS_RPC_BUFFER_SIZE,
            ROZOFS_RPC_BUFFER_SIZE);
    if (exportd_svc == NULL) {
        fatal("can't create service %s", strerror(errno));
    }

    if (!svc_register
            (exportd_svc, EXPORT_PROGRAM, EXPORT_VERSION, export_program_1,
            IPPROTO_TCP)) {
        fatal("can't register service %s", strerror(errno));
    }

    // Profiling service
    if ((exportd_profile_svc = svctcp_create(RPC_ANYSOCK, 0, 0)) == NULL) {
        severe("can't create profiling service.");
    }
    pmap_unset(EXPORTD_PROFILE_PROGRAM, EXPORTD_PROFILE_VERSION); // in case !

    if (!svc_register(exportd_profile_svc, EXPORTD_PROFILE_PROGRAM,
            EXPORTD_PROFILE_VERSION, exportd_profile_program_1, 0)) {
        severe("can't register service : %s", strerror(errno));
    }

//    SET_PROBE_VALUE(uptime, time(0));
//    strncpy((char *) gprofiler.vers, VERSION, 20);

    info("running.");
    svc_run();
}

static void on_stop() {
    DEBUG_FUNCTION;

    svc_exit();

    svc_unregister(EXPORT_PROGRAM, EXPORT_VERSION);
    pmap_unset(EXPORT_PROGRAM, EXPORT_VERSION);
    if (exportd_svc) {
        svc_destroy(exportd_svc);
        exportd_svc = NULL;
    }

    svc_unregister(EXPORTD_PROFILE_PROGRAM, EXPORTD_PROFILE_VERSION);
    pmap_unset(EXPORTD_PROFILE_PROGRAM, EXPORTD_PROFILE_VERSION);
    if (exportd_profile_svc) {
        svc_destroy(exportd_profile_svc);
        exportd_profile_svc = NULL;
    }

    exportd_release();

    info("stopped.");
    closelog();
}

static void on_hup() {
    econfig_t new;
    list_t *p, *q;

    info("hup signal received.");

    // Check if the new exportd configuration file is valid

    if (econfig_initialize(&new) != 0) {
        severe("can't initialize exportd config: %s.", strerror(errno));
        goto invalid_conf;
    }

    if (econfig_read(&new, exportd_config_file) != 0) {
        severe("failed to parse configuration file: %s.", strerror(errno));
        goto invalid_conf;
    }

    if (econfig_validate(&new) != 0) {
        severe("invalid configuration file: %s.", strerror(errno));
        goto invalid_conf;
    }
    /*
    ** compute the hash of the exportd configuration
    */
    if (hash_file_compute(exportd_config_file,&export_configuration_file_hash) != 0) {
        severe("error while computing hash value of the configuration file: %s.\n",
                strerror(errno));
        goto error;
    }

    econfig_release(&new);

    // Reload the exportd_config structure

    if ((errno = pthread_rwlock_wrlock(&config_lock)) != 0) {
        severe("can't lock config: %s", strerror(errno));
        goto error;
    }

    econfig_release(&exportd_config);

    if (econfig_read(&exportd_config, exportd_config_file) != 0) {
        severe("failed to parse configuration file: %s.", strerror(errno));
        goto error;
    }

    // Reload the list of volumes

    if ((errno = pthread_rwlock_wrlock(&volumes_lock)) != 0) {
        severe("can't lock volumes: %s", strerror(errno));
        goto error;
    }

    list_for_each_forward_safe(p, q, &volumes) {
        volume_entry_t *entry = list_entry(p, volume_entry_t, list);
        volume_release(&entry->volume);
        list_remove(p);
        free(entry);
    }

    load_volumes_conf();

    // volumes lock should be released before loading exports config
    // since load_exports_conf calls volume_lookup_volume which
    // needs to acquire volumes lock
    if ((errno = pthread_rwlock_unlock(&volumes_lock)) != 0) {
        severe("can't unlock volumes: %s", strerror(errno));
        goto error;
    }

    list_for_each_forward(p, &volumes) {
        volume_balance(&list_entry(p, volume_entry_t, list)->volume);
    }

    // Canceled the remove bins pthread before reload list of exports
    if ((errno = pthread_cancel(rm_bins_thread)) != 0)
        severe("can't canceled remove bins pthread: %s", strerror(errno));

    // Acquire lock on export list
    if ((errno = pthread_rwlock_wrlock(&exports_lock)) != 0) {
        severe("can't lock exports: %s", strerror(errno));
        goto error;
    }

    // Release the list of exports

    list_for_each_forward_safe(p, q, &exports) {
        export_entry_t *entry = list_entry(p, export_entry_t, list);

        // Canceled the load trash pthread if neccesary before
        // reload list of exports

        pthread_cancel(entry->export.load_trash_thread);

        export_release(&entry->export);
        list_remove(p);
        free(entry);
    }

    // Load new list of exports
    load_exports_conf();

    if ((errno = pthread_rwlock_unlock(&exports_lock)) != 0) {
        severe("can't unlock exports: %s", strerror(errno));
        goto error;
    }

    if ((errno = pthread_rwlock_unlock(&config_lock)) != 0) {
        severe("can't unlock config: %s", strerror(errno));
        goto error;
    }

    // XXX: An export may have been deleted while the rest of the files deleted.
    // These files will never be deleted.

    // Start pthread for remove bins file
    if ((errno = pthread_create(&rm_bins_thread, NULL,
            remove_bins_thread, NULL)) != 0) {
        severe("can't create remove files pthread %s", strerror(errno));
        goto error;
    }
    /*
    ** reload the export gateways configuration
    */
    // Acquire lock on export gateways list
    if ((errno = pthread_rwlock_wrlock(&expgws_lock)) != 0) {
        severe("can't lock export gateways: %s", strerror(errno));
        goto error;
    }


    list_for_each_forward_safe(p, q, &expgws) {
        expgw_entry_t *entry = list_entry(p, expgw_entry_t, list);
        expgw_release(&entry->expgw);
        list_remove(p);
        free(entry);
    }
    load_export_expgws_conf();

    //Release lock on export gateways list
    if ((errno = pthread_rwlock_unlock(&expgws_lock)) != 0) {
        severe("can't lock export gateways: %s", strerror(errno));
        goto error;
    }
     /*
     ** build the structure for sending out an exportd gateway configuration message
     */
#define MSG_SIZE  (32*1024)
     char * pChar = malloc(MSG_SIZE);
     int msg_sz;
     if (pChar == NULL)  {
       fatal("malloc %d", MSG_SIZE);
     }
//    expgw_init_configuration_message(exportd_config.exportd_vip);
    if ( (msg_sz = expgw_build_configuration_message(pChar,MSG_SIZE))< 0)
    {
        severe("can't build export gateway configuration message");
        free(pChar);
        goto error;
    }
    /*
    ** Send the exportd gateway configuration towards the non blocking thread
    */
    {
      int ret;
      ret = expgwc_internal_channel_send(EXPGWC_LOAD_CONF,msg_sz,pChar);
      if (ret < 0)
      {
         severe("EXPGWC_LOAD_CONF: %s",strerror(errno));
         free(pChar);
        goto error;
      }

    }
    free(pChar);


    info("reloaded.");
    goto out;
error:
    pthread_rwlock_unlock(&exports_lock);
    pthread_rwlock_unlock(&volumes_lock);
    pthread_rwlock_unlock(&config_lock);
    pthread_rwlock_unlock(&expgws_lock);
invalid_conf:
    severe("reload failed.");
out:
    econfig_release(&new);
    return;
}

static void usage() {
    printf("Rozofs export daemon - %s\n", VERSION);
    printf("Usage: exportd [OPTIONS]\n\n");
    printf("\t-h, --help\tprint this message.\n");
    printf("\t-d,--debug <port>\t\texportd non blocking debug port(default: none) \n");
//    printf("\t-n,--hostname <name>\t\texportd host name(default: none) \n");
    //printf("\t-i,--instance <value>\t\texportd instance id(default: 1) \n");
    printf("\t-c, --config\tconfiguration file to use (default: %s).\n",
            EXPORTD_DEFAULT_CONFIG);
};

int main(int argc, char *argv[]) {
    int c;
    int val;

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"debug", required_argument, 0, 'd'},
        //{"instance", required_argument, 0, 'i'},
        {"config", required_argument, 0, 'c'},
        {0, 0, 0, 0}
    };

    /* Try to get debug port from /etc/services */
    expgwc_non_blocking_conf.debug_port = get_service_port("rozo_exportd_dbg",NULL,rzdbg_default_base_port);
    expgwc_non_blocking_conf.instance = 1;
    expgwc_non_blocking_conf.exportd_hostname = NULL;

    while (1) {

        int option_index = 0;
        c = getopt_long(argc, argv, "hc:i:d:", long_options, &option_index);

        if (c == -1)
            break;

        switch (c) {

            case 'h':
                usage();
                exit(EXIT_SUCCESS);
                break;
            case 'c':
                if (!realpath(optarg, exportd_config_file)) {
                    fprintf(stderr,
                            "exportd failed: configuration file: %s: %s\n",
                            optarg, strerror(errno));
                    exit(EXIT_FAILURE);
                }
                break;
             case 'd':
                errno = 0;
                val = (int) strtol(optarg, (char **) NULL, 10);
                if (errno != 0) {
                    strerror(errno);
                    usage();
                    exit(EXIT_FAILURE);
                }
                expgwc_non_blocking_conf.debug_port = val;
                break;
             case 'i':
                errno = 0;
                val = (int) strtol(optarg, (char **) NULL, 10);
                if (errno != 0) {
                    strerror(errno);
                    usage();
                    exit(EXIT_FAILURE);
                }
                expgwc_non_blocking_conf.instance = val;
                break;
            case '?':
                usage();
                exit(EXIT_SUCCESS);
                break;
            default:
                usage();
                exit(EXIT_FAILURE);
                break;
        }
    }

    if (econfig_initialize(&exportd_config) != 0) {
        fprintf(stderr, "can't initialize exportd config: %s.\n",
                strerror(errno));
        goto error;
    }
    if (econfig_read(&exportd_config, exportd_config_file) != 0) {
        fprintf(stderr, "failed to parse configuration file: %s.\n",
                strerror(errno));
        goto error;
    }
    if (econfig_validate(&exportd_config) != 0) {
        fprintf(stderr, "inconsistent configuration file: %s.\n",
                strerror(errno));
        goto error;
    }
    /*
    ** compute the hash of the exportd configuration
    */
    if (hash_file_compute(exportd_config_file,&export_configuration_file_hash) != 0) {
        fprintf(stderr, "error while computing hash value of the configuration file: %s.\n",
                strerror(errno));
        goto error;
    }

    openlog("exportd", LOG_PID, LOG_DAEMON);
    daemon_start("exportd",exportd_config.nb_cores,EXPORTD_PID_FILE, on_start, on_stop, on_hup);


    exit(0);
error:
    fprintf(stderr, "see log for more details.\n");
    exit(EXIT_FAILURE);
}
