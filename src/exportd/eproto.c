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

#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <sys/types.h>
#include <pthread.h>
#include <string.h>

#include <rozofs/common/log.h>
#include <rozofs/common/xmalloc.h>
#include <rozofs/rpc/epproto.h>
#include <rozofs/rpc/eproto.h>
#include <rozofs/rpc/sproto.h>
#include <rozofs/rpc/export_profiler.h>

#include "export.h"
#include "volume.h"
#include "exportd.h"

void *ep_null_1_svc(void *noargs, struct svc_req *req) {
    static void *ret = NULL;
    return &ret;
}


uint32_t exportd_storage_host_count = 0; /**< number of host storage in the configuration of an eid */
ep_cnf_storage_node_t exportd_storage_host_table[STORAGE_NODES_MAX]; /**< configuration for each storage */
epgw_conf_ret_t export_storage_conf; /**< preallocated area to build storage configuration message */
/*
 *_______________________________________________________________________
 */
/**
*  Get export id free block count in case a quota has been set
  
  @param none
  
  @retval 0 on success
  @retval -1 on error
 */
static inline uint64_t exportd_get_free_quota(export_t *exp) {
   
  uint64_t quota;
  
  if (exp->hquota == 0) return -1;
  quota = exp->hquota - exp->fstat.blocks;
  return quota; 
}
/*
 *_______________________________________________________________________
 */
/**
*  Init of the array that is used for building an exportd configuration message
  That array is allocated during the initialization of the exportd and might be
  released upon the termination of the exportd process
  
  @param none
  
  @retval 0 on success
  @retval -1 on error
 */
int exportd_init_storage_configuration_message()
{
  ep_cnf_storage_node_t *storage_cnf_p;
  int i;
  /*
  ** clear the memory that contains the area for building a storage configuration message
  */
  memset(&export_storage_conf,0,sizeof(epgw_conf_ret_t));
  export_storage_conf.status_gw.ep_conf_ret_t_u.export.storage_nodes.storage_nodes_len = 0;
  export_storage_conf.status_gw.ep_conf_ret_t_u.export.storage_nodes.storage_nodes_val = exportd_storage_host_table;
  /*
  ** init of the storage node array
  */
  storage_cnf_p = exportd_storage_host_table;
  export_storage_conf.status_gw.ep_conf_ret_t_u.export.storage_nodes.storage_nodes_val = storage_cnf_p ;
  char* host;
  
  host = malloc( ROZOFS_HOSTNAME_MAX+1);
  storage_cnf_p->host = host;

  

  for (i = 0; i < STORAGE_NODES_MAX; i++,storage_cnf_p++)
  {
    storage_cnf_p->host = (char*)malloc( ROZOFS_HOSTNAME_MAX+1);
    if (storage_cnf_p->host == NULL)
    {
      severe("exportd_init_storage_configuration_message: out of memory");
      return -1;
    
    }
    storage_cnf_p->host[0] = 0;  
    storage_cnf_p->sids_nb = 0;
  }
  return 0;
}


/*
 *_______________________________________________________________________
 */
 /**
 *  That API is intended to be called by ep_conf_storage_1_svc() 
    prior to build the configuration message
    
    The goal is to clear the number of storages and to clear the
    number of sid per storage entry
    
    @param none
    retval none
*/
void exportd_reinit_storage_configuration_message()
{
  int i;
  ep_cnf_storage_node_t *storage_cnf_p = exportd_storage_host_table;
  export_storage_conf.status_gw.ep_conf_ret_t_u.export.storage_nodes.storage_nodes_len = 0;

  for (i = 0; i < STORAGE_NODES_MAX; i++,storage_cnf_p++)
  {
    storage_cnf_p->sids_nb = 0;

  }
}



/*
**______________________________________________________________________________
*/
/**
*   exportd configuration polling : check if the configuration of
    the remote is inline with the current one of the exportd.
*/

epgw_status_ret_t * ep_poll_conf_1_svc(ep_gateway_t *args, struct svc_req *req)
{
    static epgw_status_ret_t ret;
    
    // Default profiler export index
    export_profiler_eid = args->eid;    
    
    START_PROFILING(ep_poll);

    if (args->hash_config == export_configuration_file_hash) 
    {   
      ret.status_gw.status = EP_SUCCESS;
    }
    else
    {
      ret.status_gw.status = EP_NOT_SYNCED;
    }
    STOP_PROFILING(ep_poll);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd: Get the configuration of the storaged for a given eid

  : returns to the rozofsmount the list of the
*   volume,clusters and storages
*/
epgw_conf_ret_t *ep_conf_storage_1_svc(ep_path_t * arg, struct svc_req * req) {
    static epgw_conf_ret_t ret;
    
    epgw_conf_ret_t *ret_cnf_p = &export_storage_conf;
    epgw_conf_ret_t *ret_out = NULL;
    list_t *p, *q, *r;
    eid_t *eid = NULL;
    export_t *exp;
    int i = 0;
    int stor_idx = 0;
    int exist = 0;
    ep_cnf_storage_node_t *storage_cnf_p;

    DEBUG_FUNCTION;
    
    // XXX exportd_lookup_id could return export_t *
    eid = exports_lookup_id(*arg);	

    // XXX exportd_lookup_id could return export_t *
    if (eid) export_profiler_eid = *eid;
    else     export_profiler_eid = 0;
	
    START_PROFILING(ep_configuration);
    if (!eid) goto error;
        
    exportd_reinit_storage_configuration_message();
#if 0
#warning fake xdrmem_create
    XDR               xdrs; 
    int total_len = -1 ;
    int size = 1024*64;
    char *pchar = malloc(size);
    xdrmem_create(&xdrs,(char*)pchar,size,XDR_ENCODE);    
#endif

    if (!(exp = exports_lookup_export(*eid)))
        goto error;

    /* Get lock on config */
    if ((errno = pthread_rwlock_rdlock(&config_lock)) != 0) {
        goto error;
    }

    /* For each volume */
    list_for_each_forward(p, &exportd_config.volumes) {

        volume_config_t *vc = list_entry(p, volume_config_t, list);

        /* Get volume with this vid */
        if (vc->vid == exp->volume->vid) {

            stor_idx = 0;
            ret_cnf_p->status_gw.ep_conf_ret_t_u.export.storage_nodes.storage_nodes_len = 0;
            storage_cnf_p = &exportd_storage_host_table[stor_idx];
//            memset(ret_cnf_p->status_gw.ep_conf_ret_t_u.export.storage_nodes, 0, sizeof (ep_cnf_storage_node_t) * STORAGE_NODES_MAX);

            /* For each cluster */
            list_for_each_forward(q, &vc->clusters) {

                cluster_config_t *cc = list_entry(q, cluster_config_t, list);

                /* For each sid */
                list_for_each_forward(r, &cc->storages) {

                    storage_node_config_t *s = list_entry(r, storage_node_config_t, list);

                    /* Verify that this hostname does not already exist
                     * in the list of physical storage nodes. */
                    ep_cnf_storage_node_t *storage_cmp_p = &exportd_storage_host_table[0];
                    for (i = 0; i < stor_idx; i++,storage_cmp_p++) {

                        if (strcmp(s->host, storage_cmp_p->host) == 0) {

                            /* This physical storage node exist
                             *  but we add this SID*/
                            uint8_t sids_nb = storage_cmp_p->sids_nb;
                            storage_cmp_p->sids[sids_nb] = s->sid;
                            storage_cmp_p->cids[sids_nb] = cc->cid;
                            storage_cmp_p->sids_nb++;
                            exist = 1;
                            break;
                        }
                    }

                    /* This physical storage node doesn't exist*/
                    if (exist == 0) {

                        /* Add this storage node to the list */
                        strncpy(storage_cnf_p->host, s->host, ROZOFS_HOSTNAME_MAX);
                        /* Add this sid */
                        storage_cnf_p->sids[0] = s->sid;
                        storage_cnf_p->cids[0] = cc->cid;
                        storage_cnf_p->sids_nb++;

                        /* Increments the nb. of physical storage nodes */
                        stor_idx++;
                        storage_cnf_p++;
                    }
                    exist = 0;
                }
            }
        }
    }
    ret_cnf_p->status_gw.ep_conf_ret_t_u.export.storage_nodes.storage_nodes_len = stor_idx;
    ret_cnf_p->status_gw.ep_conf_ret_t_u.export.eid = *eid;
    ret_cnf_p->status_gw.ep_conf_ret_t_u.export.hash_conf = export_configuration_file_hash;
    memcpy(ret_cnf_p->status_gw.ep_conf_ret_t_u.export.md5, exp->md5, ROZOFS_MD5_SIZE);
    ret_cnf_p->status_gw.ep_conf_ret_t_u.export.rl = exp->layout;
    memcpy(ret_cnf_p->status_gw.ep_conf_ret_t_u.export.rfid, exp->rfid, sizeof (fid_t));

    if ((errno = pthread_rwlock_unlock(&config_lock)) != 0) {
        goto error;
    }

    ret_cnf_p->status_gw.status = EP_SUCCESS;
#if 0
    if (xdr_epgw_conf_ret_t(&xdrs,ret_cnf_p) == FALSE)
    {
      severe("encoding error");    
    } 
    else
    {   
     total_len = xdr_getpos(&xdrs) ;

    }
#endif
    ret_out = ret_cnf_p;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_conf_ret_t_u.error = errno;
    ret_out = &ret;

out:
#if 0
    if (pchar != NULL) free(pchar);
#endif
    STOP_PROFILING(ep_mount);
    return ret_out;
}

static uint32_t local_expgw_eid_table[EXPGW_EID_MAX_IDX];
static ep_gw_host_conf_t local_expgw_host_table[EXPGW_EXPGW_MAX_IDX];
/*
**______________________________________________________________________________
*/
/**
*  Init of the data structure used for sending out export gateway configuration
   to rozofsmount.
   That API must be called during the inif of exportd
  
  @param exportd_hostname: VIP address of the exportd (extracted from the exportd configuration file)
  @retval none
*/
void ep_expgw_init_configuration_message(char *exportd_hostname)
{
//  expgw_conf_p->exportd_host = malloc( ROZOFS_HOSTNAME_MAX+1);
//  strcpy(expgw_conf_p->exportd_host,exportd_hostname);
  memset(local_expgw_eid_table,0,sizeof(local_expgw_eid_table));
  memset(local_expgw_host_table,0,sizeof(local_expgw_host_table));
  int i;
  for (i = 0; i < EXPGW_EXPGW_MAX_IDX; i++)
  {
    local_expgw_host_table[i].host = malloc( ROZOFS_HOSTNAME_MAX+1);
  }
}

/*
**______________________________________________________________________________
*/
/**
 *  Message to provide a ROZOFS with the export gateway configuration: EP_CONF_EXPGW
*/
ep_gw_gateway_configuration_ret_t *ep_conf_expgw_1_svc(ep_path_t * arg, struct svc_req * req) {
    static ep_gw_gateway_configuration_ret_t ret;
    static char exportd_hostname[ROZOFS_HOSTNAME_MAX];
    eid_t *eid = NULL;
    export_t *exp;

    //int err = 0;
    list_t *iterator;
    list_t *iterator_expgw;    
    ep_gateway_configuration_t *expgw_conf_p= &ret.status_gw.ep_gateway_configuration_ret_t_u.config;

    // XXX exportd_lookup_id could return export_t *
    eid = exports_lookup_id(*arg);	

    // XXX exportd_lookup_id could return export_t *
    if (eid) export_profiler_eid = *eid;
    else     export_profiler_eid = 0;
	
    START_PROFILING(ep_conf_gateway);
    ret.hdr.eid          = 0;      /* NS */
    ret.hdr.nb_gateways  = 0; /* NS */
    ret.hdr.gateway_rank = 0; /* NS */
    ret.hdr.hash_config  = 0; /* NS */
      
    if (!eid) goto error; 

    if (!(exp = exports_lookup_export(*eid)))
        goto error;

    /* Get lock on config */
    if ((errno = pthread_rwlock_rdlock(&config_lock)) != 0) {
        goto error;
    }            
    expgw_conf_p->eid.eid_len = 0;
    expgw_conf_p->eid.eid_val = local_expgw_eid_table;
    expgw_conf_p->exportd_host = exportd_hostname;
    strcpy(expgw_conf_p->exportd_host,exportd_config.exportd_vip);
    expgw_conf_p->exportd_port = 0;
    expgw_conf_p->gateway_port = 0;
    expgw_conf_p->gateway_host.gateway_host_len = 0;  
    expgw_conf_p->gateway_host.gateway_host_val = local_expgw_host_table;  
  
    list_for_each_forward(iterator, &exportd_config.exports) 
    {
       export_config_t *entry = list_entry(iterator, export_config_t, list);
       local_expgw_eid_table[expgw_conf_p->eid.eid_len] = entry->eid;
       expgw_conf_p->eid.eid_len++;
    }
    /*
    ** unlock exportd config
    */
    if ((errno = pthread_rwlock_unlock(&config_lock)) != 0) 
    {
        severe("can unlock config_lock, potential dead lock.");
        goto error;
    }
    if (expgw_conf_p->eid.eid_len == 0)
    {
      severe("no eid in the exportd configuration !!");
      //err = EPROTO;
      goto error;
    }

    expgw_conf_p->hdr.export_id            = 0;
    expgw_conf_p->hdr.nb_gateways          = 0;
    expgw_conf_p->hdr.gateway_rank         = 0;
    expgw_conf_p->hdr.configuration_indice = export_configuration_file_hash;
    /*
    ** Lock Config.
    */
    if ((errno = pthread_rwlock_rdlock(&config_lock)) != 0) 
    {
        goto error;
    }       
    list_for_each_forward(iterator, &exportd_config.expgw) 
    {
        expgw_config_t *entry = list_entry(iterator, expgw_config_t, list);
        expgw_conf_p->hdr.export_id = entry->daemon_id;
        /*
        ** loop on the storage
        */
        
        list_for_each_forward(iterator_expgw, &entry->expgw_node) 
        {
          expgw_node_config_t *entry = list_entry(iterator_expgw, expgw_node_config_t, list);
          /*
          ** copy the hostname
          */
 
          strcpy((char*)local_expgw_host_table[expgw_conf_p->gateway_host.gateway_host_len].host, entry->host);
           expgw_conf_p->gateway_host.gateway_host_len++;
          expgw_conf_p->hdr.nb_gateways++;
        }
    }
    /*
    ** Unlock Config.
    */
    if ((errno = pthread_rwlock_unlock(&config_lock)) != 0) 
    {
        severe("can't unlock expgws, potential dead lock.");
        goto error;
    } 
    ret.status_gw.status = EP_SUCCESS;

    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_gateway_configuration_ret_t_u.error = errno;
out:

    STOP_PROFILING(ep_conf_gateway);
    return &ret;
}


/*
**______________________________________________________________________________
*/
/**
*   exportd mount file systems: returns to the rozofsmount the list of the
*   volume,clusters and storages
*/
epgw_mount_ret_t *ep_mount_1_svc(ep_path_t * arg, struct svc_req * req) {
    static epgw_mount_ret_t ret;
    list_t *p, *q, *r;
    eid_t *eid = NULL;
    export_t *exp;
    int i = 0;
    int stor_idx = 0;
    int exist = 0;
    

    DEBUG_FUNCTION;

    // XXX exportd_lookup_id could return export_t *
    eid = exports_lookup_id(*arg);    
    if (eid) export_profiler_eid = *eid;	
    else     export_profiler_eid = 0; 
        
    START_PROFILING(ep_mount);
    if (!eid) goto error;

    if (!(exp = exports_lookup_export(*eid)))
        goto error;

    /* Get lock on config */
    if ((errno = pthread_rwlock_rdlock(&config_lock)) != 0) {
        goto error;
    }

    /* For each volume */
    list_for_each_forward(p, &exportd_config.volumes) {

        volume_config_t *vc = list_entry(p, volume_config_t, list);

        /* Get volume with this vid */
        if (vc->vid == exp->volume->vid) {

            stor_idx = 0;
            ret.status_gw.ep_mount_ret_t_u.export.storage_nodes_nb = 0;
            memset(ret.status_gw.ep_mount_ret_t_u.export.storage_nodes, 0, sizeof (ep_cnf_storage_node_t) * STORAGE_NODES_MAX);

            /* For each cluster */
            list_for_each_forward(q, &vc->clusters) {

                cluster_config_t *cc = list_entry(q, cluster_config_t, list);

                /* For each sid */
                list_for_each_forward(r, &cc->storages) {

                    storage_node_config_t *s = list_entry(r, storage_node_config_t, list);

                    /* Verify that this hostname does not already exist
                     * in the list of physical storage nodes. */
                    for (i = 0; i < stor_idx; i++) {

                        if (strcmp(s->host, ret.status_gw.ep_mount_ret_t_u.export.storage_nodes[i].host) == 0) {

                            /* This physical storage node exist
                             *  but we add this SID*/
                            uint8_t sids_nb = ret.status_gw.ep_mount_ret_t_u.export.storage_nodes[i].sids_nb;
                            ret.status_gw.ep_mount_ret_t_u.export.storage_nodes[i].sids[sids_nb] = s->sid;
                            ret.status_gw.ep_mount_ret_t_u.export.storage_nodes[i].cids[sids_nb] = cc->cid;
                            ret.status_gw.ep_mount_ret_t_u.export.storage_nodes[i].sids_nb++;
                            exist = 1;
                            break;
                        }
                    }

                    /* This physical storage node doesn't exist*/
                    if (exist == 0) {

                        /* Add this storage node to the list */
                        strncpy(ret.status_gw.ep_mount_ret_t_u.export.storage_nodes[stor_idx].host, s->host, ROZOFS_HOSTNAME_MAX);
                        /* Add this sid */
                        ret.status_gw.ep_mount_ret_t_u.export.storage_nodes[stor_idx].sids[0] = s->sid;
                        ret.status_gw.ep_mount_ret_t_u.export.storage_nodes[stor_idx].cids[0] = cc->cid;
                        ret.status_gw.ep_mount_ret_t_u.export.storage_nodes[stor_idx].sids_nb++;

                        /* Increments the nb. of physical storage nodes */
                        stor_idx++;
                    }
                    exist = 0;
                }
            }
        }
    }

    ret.status_gw.ep_mount_ret_t_u.export.storage_nodes_nb = stor_idx;
    ret.status_gw.ep_mount_ret_t_u.export.eid = *eid;
    ret.status_gw.ep_mount_ret_t_u.export.hash_conf = export_configuration_file_hash;

    memcpy(ret.status_gw.ep_mount_ret_t_u.export.md5, exp->md5, ROZOFS_MD5_SIZE);
    ret.status_gw.ep_mount_ret_t_u.export.rl = exp->layout;
    memcpy(ret.status_gw.ep_mount_ret_t_u.export.rfid, exp->rfid, sizeof (fid_t));

    if ((errno = pthread_rwlock_unlock(&config_lock)) != 0) {
        goto error;
    }

    ret.status_gw.status = EP_SUCCESS;

    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_mount_ret_t_u.error = errno;
out:

    STOP_PROFILING(ep_mount);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd list of clusters of exportd
*/
epgw_cluster_ret_t *ep_list_cluster_1_svc(uint16_t * cid, struct svc_req * req) {
    static epgw_cluster_ret_t ret;
    list_t *p, *q, *r;
    uint8_t stor_idx = 0;

    DEBUG_FUNCTION;

    ret.status_gw.status = EP_FAILURE;

    // Get lock on config
    if ((errno = pthread_rwlock_rdlock(&config_lock)) != 0) {
        ret.status_gw.ep_cluster_ret_t_u.error = errno;
        goto out;
    }

    // For each volume

    list_for_each_forward(p, &exportd_config.volumes) {

        volume_config_t *vc = list_entry(p, volume_config_t, list);

        ret.status_gw.ep_cluster_ret_t_u.cluster.storages_nb = 0;
        memset(ret.status_gw.ep_cluster_ret_t_u.cluster.storages, 0, sizeof (ep_storage_t) * SID_MAX);

        // For each cluster

        list_for_each_forward(q, &vc->clusters) {

            cluster_config_t *cc = list_entry(q, cluster_config_t, list);

            // Check if it's a the good cluster
            if (cc->cid == *cid) {

                // Copy cid
                ret.status_gw.ep_cluster_ret_t_u.cluster.cid = cc->cid;

                // For each storage 

                list_for_each_forward(r, &cc->storages) {

                    storage_node_config_t *s = list_entry(r, storage_node_config_t, list);

                    // Add the storage to response
                    strncpy(ret.status_gw.ep_cluster_ret_t_u.cluster.storages[stor_idx].host, s->host, ROZOFS_HOSTNAME_MAX);
                    ret.status_gw.ep_cluster_ret_t_u.cluster.storages[stor_idx].sid = s->sid;
                    stor_idx++;
                }
                // OK -> answered
                ret.status_gw.ep_cluster_ret_t_u.cluster.storages_nb = stor_idx;
                ret.status_gw.status = EP_SUCCESS;
                goto unlock;
            }
        }
    }
    // cid not found
    ret.status_gw.ep_cluster_ret_t_u.error = EINVAL;

unlock:
    if ((errno = pthread_rwlock_unlock(&config_lock)) != 0) {
        ret.status_gw.ep_cluster_ret_t_u.error = errno;
        goto out;
    }
out:
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd unmount
*/
/* Will do something one day !! */
epgw_status_ret_t * ep_umount_1_svc(uint32_t * arg, struct svc_req * req) {
    static epgw_status_ret_t ret;
    DEBUG_FUNCTION;
    START_PROFILING(ep_umount);

    ret.status_gw.status = EP_SUCCESS;

    STOP_PROFILING(ep_umount);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd file system statfs
*/
epgw_statfs_ret_t * ep_statfs_1_svc(uint32_t * arg, struct svc_req * req) {
    static epgw_statfs_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;
    
    // Set profiler export index
    export_profiler_eid = * arg;	
        
    START_PROFILING(ep_statfs);

    if (!(exp = exports_lookup_export((eid_t) * arg)))
        goto error;
    if (export_stat(exp, (estat_t *) & ret.status_gw.ep_statfs_ret_t_u.stat) != 0)
        goto error;
    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_statfs_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_statfs);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd lookup fid,name
    @param args : fid parent and name of the object
    
    @retval: EP_SUCCESS :attributes of the parent and child (fid,name)
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_mattr_ret_t * ep_lookup_1_svc(epgw_lookup_arg_t * arg, struct svc_req * req) {
    static epgw_mattr_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_lookup);

    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if (export_lookup
            (exp, (unsigned char *) arg->arg_gw.parent, arg->arg_gw.name,
            (mattr_t *) & ret.status_gw.ep_mattr_ret_t_u.attrs,
            (mattr_t *) & ret.parent_attr.ep_mattr_ret_t_u.attrs) != 0)
        goto error;
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status   = EP_SUCCESS;
    ret.parent_attr.status = EP_SUCCESS;
    ret.free_quota = exportd_get_free_quota(exp);
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_mattr_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_lookup);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd get attributes

    @param args : fid of the object 
    
    @retval: EP_SUCCESS :attributes of the object
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_mattr_ret_t * ep_getattr_1_svc(epgw_mfile_arg_t * arg, struct svc_req * req) {
    static epgw_mattr_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_getattr);

    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if (export_getattr
            (exp, (unsigned char *) arg->arg_gw.fid,
            (mattr_t *) & ret.status_gw.ep_mattr_ret_t_u.attrs) != 0)
        goto error;
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_SUCCESS;
    ret.free_quota = exportd_get_free_quota(exp);
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_mattr_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_getattr);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd set attributes

    @param args : fid of the object and attributes to set
    
    @retval: EP_SUCCESS :attributes of the object
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_mattr_ret_t * ep_setattr_1_svc(epgw_setattr_arg_t * arg, struct svc_req * req) {
    static epgw_mattr_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_setattr);

    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if (export_setattr(exp, (unsigned char *) arg->arg_gw.attrs.fid,
            (mattr_t *) & arg->arg_gw.attrs, arg->arg_gw.to_set) != 0)
        goto error;
    if (export_getattr(exp, (unsigned char *) arg->arg_gw.attrs.fid,
            (mattr_t *) & ret.status_gw.ep_mattr_ret_t_u.attrs) != 0)
        goto error;
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_SUCCESS;
    ret.free_quota = exportd_get_free_quota(exp);
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_mattr_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_setattr);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd read link

    @param args : fid of the object 
    
    @retval: EP_SUCCESS :content of the link (max is ROZOFS_PATH_MAX)
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_readlink_ret_t * ep_readlink_1_svc(epgw_mfile_arg_t * arg,
        struct svc_req * req) {
    static epgw_readlink_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_readlink);

    xdr_free((xdrproc_t) xdr_epgw_readlink_ret_t, (char *) &ret);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    ret.status_gw.ep_readlink_ret_t_u.link = xmalloc(ROZOFS_PATH_MAX);

    if (export_readlink(exp, (unsigned char *) arg->arg_gw.fid,
            ret.status_gw.ep_readlink_ret_t_u.link) != 0)
        goto error;

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    if (ret.status_gw.ep_readlink_ret_t_u.link != NULL)
        free(ret.status_gw.ep_readlink_ret_t_u.link);
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_readlink_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_readlink);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd link

    @param args : fid of the parent and object name and the target inode (fid)  
    
    @retval: EP_SUCCESS :attributes of the parent and of the object
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/

epgw_mattr_ret_t * ep_link_1_svc(epgw_link_arg_t * arg, struct svc_req * req) {
    static epgw_mattr_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_link);
    
    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if (export_link(exp, (unsigned char *) arg->arg_gw.inode,
            (unsigned char *) arg->arg_gw.newparent, arg->arg_gw.newname,
            (mattr_t *) & ret.status_gw.ep_mattr_ret_t_u.attrs,
            (mattr_t *) & ret.parent_attr.ep_mattr_ret_t_u.attrs) != 0)
        goto error;
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status   = EP_SUCCESS;
    ret.parent_attr.status = EP_SUCCESS;
    ret.free_quota = exportd_get_free_quota(exp);
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_mattr_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_link);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd mknod (create a new regular file)

    @param args : fid of the parent and object name 
    
    @retval: EP_SUCCESS :attributes of the parent and of the object
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_mattr_ret_t * ep_mknod_1_svc(epgw_mknod_arg_t * arg, struct svc_req * req) {
    static epgw_mattr_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_mknod);

    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if (export_mknod
            (exp, (unsigned char *) arg->arg_gw.parent, arg->arg_gw.name, arg->arg_gw.uid, arg->arg_gw.gid,
            arg->arg_gw.mode, (mattr_t *) & ret.status_gw.ep_mattr_ret_t_u.attrs,
            (mattr_t *) & ret.parent_attr.ep_mattr_ret_t_u.attrs) != 0)
        goto error;
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.parent_attr.status = EP_SUCCESS;
    ret.status_gw.status = EP_SUCCESS;
    ret.free_quota = exportd_get_free_quota(exp);    
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_mattr_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_mknod);
    return &ret;
}

/*
**______________________________________________________________________________
*/
/**
*   exportd mkdir (create a new directory)

    @param args : fid of the parent and object name 
    
    @retval: EP_SUCCESS :attributes of the parent and of the object
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_mattr_ret_t * ep_mkdir_1_svc(epgw_mkdir_arg_t * arg, struct svc_req * req) {
    static epgw_mattr_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_mkdir);

    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if (export_mkdir
            (exp, (unsigned char *) arg->arg_gw.parent, arg->arg_gw.name, arg->arg_gw.uid, arg->arg_gw.gid,
            arg->arg_gw.mode, (mattr_t *) & ret.status_gw.ep_mattr_ret_t_u.attrs,
            (mattr_t *) & ret.parent_attr.ep_mattr_ret_t_u.attrs) != 0)
        goto error;
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.parent_attr.status = EP_SUCCESS;
    ret.status_gw.status = EP_SUCCESS;
    ret.free_quota = exportd_get_free_quota(exp);
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_mattr_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_mkdir);
    return &ret;
}

/*
**______________________________________________________________________________
*/
/**
*   exportd unlink: delete a regular,symlink or hardlink object

    @param args : fid of the parent and object name 
    
    @retval: EP_SUCCESS :attributes of the parent and fid of the deleted object (contains 0 if not deleted)
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_fid_ret_t * ep_unlink_1_svc(epgw_unlink_arg_t * arg, struct svc_req * req) {
    static epgw_fid_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_unlink);

    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if (export_unlink(exp, (unsigned char *) arg->arg_gw.pfid, arg->arg_gw.name,
            (unsigned char *) ret.status_gw.ep_fid_ret_t_u.fid,
            (mattr_t *) &ret.parent_attr.ep_mattr_ret_t_u.attrs) != 0)
        goto error;

    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_SUCCESS;
    ret.parent_attr.status = EP_SUCCESS;
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_fid_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_unlink);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd rmdir: delete a directory

    @param args : fid of the parent and directory  name 
    
    @retval: EP_SUCCESS :attributes of the parent and fid of the deleted directory 
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_fid_ret_t * ep_rmdir_1_svc(epgw_rmdir_arg_t * arg, struct svc_req * req) {
    static epgw_fid_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_rmdir);

    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if (export_rmdir(exp, (unsigned char *) arg->arg_gw.pfid, arg->arg_gw.name,
            (unsigned char *) ret.status_gw.ep_fid_ret_t_u.fid,
            (mattr_t *) &ret.parent_attr.ep_mattr_ret_t_u.attrs) != 0)
        goto error;

    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_SUCCESS;
    ret.parent_attr.status = EP_SUCCESS;
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_fid_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_rmdir);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd symlink: create a symbolic link

    @param args : fid of the parent and symlink name  and target link (name)
    
    @retval: EP_SUCCESS :attributes of the parent and  of the created symlink
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_mattr_ret_t * ep_symlink_1_svc(epgw_symlink_arg_t * arg, struct svc_req * req) {
    static epgw_mattr_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_symlink);

    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    if (export_symlink(exp, arg->arg_gw.link, (unsigned char *) arg->arg_gw.parent, arg->arg_gw.name,
            (mattr_t *) & ret.status_gw.ep_mattr_ret_t_u.attrs,
            (mattr_t *) & ret.parent_attr.ep_mattr_ret_t_u.attrs) != 0)
        goto error;

    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.parent_attr.status = EP_SUCCESS;
    ret.status_gw.status = EP_SUCCESS;
    ret.free_quota = exportd_get_free_quota(exp);    
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_mattr_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_symlink);
    return &ret;
}

/*
**______________________________________________________________________________
*/
/**
*   exportd rename: rename a file or a directory

    @param args : old fid parent/name  and new fid parent/name
    
    @retval: EP_SUCCESS :to be completed
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/

epgw_rename_ret_t * ep_rename_1_svc(epgw_rename_arg_t * arg, struct svc_req * req) {
    static epgw_rename_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_rename);
    
    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if (export_rename(exp, (unsigned char *) arg->arg_gw.pfid, arg->arg_gw.name,
            (unsigned char *) arg->arg_gw.npfid, arg->arg_gw.newname,
            (unsigned char *) ret.status_gw.ep_fid_ret_t_u.fid,
	    (mattr_t *) &ret.child_attr.ep_mattr_ret_t_u.attrs) != 0)
        goto error;

    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_fid_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_rename);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd readdir: list the content of a directory

    @param args : fid of the directory
    
    @retval: EP_SUCCESS :set of name and fid associated with the directory and cookie for next readdir
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_readdir_ret_t * ep_readdir_1_svc(epgw_readdir_arg_t * arg,
        struct svc_req * req) {
    static epgw_readdir_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_readdir);

    xdr_free((xdrproc_t) xdr_epgw_readdir_ret_t, (char *) &ret);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    if (export_readdir(exp, (unsigned char *) arg->arg_gw.fid, &arg->arg_gw.cookie,
            (child_t **) & ret.status_gw.ep_readdir_ret_t_u.reply.children,
            (uint8_t *) & ret.status_gw.ep_readdir_ret_t_u.reply.eof) != 0)
        goto error;

    ret.status_gw.ep_readdir_ret_t_u.reply.cookie = arg->arg_gw.cookie;

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_readdir_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_readdir);
    return &ret;
}

/* not used anymore
ep_io_ret_t *ep_read_1_svc(ep_io_arg_t * arg, struct svc_req * req) {
    static ep_io_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if ((ret.status_gw.ep_io_ret_t_u.length =
            export_read(exp, arg->arg_gw.fid, arg->arg_gw.offset, arg->arg_gw.length)) < 0)
        goto error;
    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_io_ret_t_u.error = errno;
out:
    return &ret;
}
 */
/*
**______________________________________________________________________________
*/
/**
*   exportd read_block : OBSOLETE


*/
epgw_read_block_ret_t * ep_read_block_1_svc(epgw_io_arg_t * arg, struct svc_req * req) {
    static epgw_read_block_ret_t ret;
    export_t *exp = NULL;
    int64_t length = -1;
    uint64_t first_blk = 0;
    uint32_t nb_blks = 0;

    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING_IO(ep_read_block, arg->arg_gw.length);

    // Free memory buffers for xdr
    xdr_free((xdrproc_t) xdr_epgw_read_block_ret_t, (char *) &ret);

    // Get export
    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    // Check if EOF, get nb. of blocks to read and update atime
    if ((length = export_read(exp, (unsigned char *) arg->arg_gw.fid, arg->arg_gw.offset,
            arg->arg_gw.length, &first_blk, &nb_blks)) == -1)
        goto error;

    ret.status_gw.ep_read_block_ret_t_u.ret.length = length;
    ret.status_gw.ep_read_block_ret_t_u.ret.dist.dist_len = nb_blks;
    ret.status_gw.ep_read_block_ret_t_u.ret.dist.dist_val =
            xmalloc(nb_blks * sizeof (dist_t));

    // Get distributions
    if (export_read_block(exp, (unsigned char *) arg->arg_gw.fid, first_blk, nb_blks,
            ret.status_gw.ep_read_block_ret_t_u.ret.dist.dist_val) != 0)
        goto error;

    ret.status_gw.status = EP_SUCCESS;
    goto out;

error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_read_block_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_read_block);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd write_block: update the size and date of a file

    @param args : fid of the file, offset and length written
    
    @retval: EP_SUCCESS :attributes of the updated file
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_mattr_ret_t * ep_write_block_1_svc(epgw_write_block_arg_t * arg,
        struct svc_req * req) {
    static epgw_mattr_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING_IO(ep_write_block, arg->arg_gw.length);

    ret.parent_attr.status = EP_EMPTY;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;
    if (export_write_block(exp,(unsigned char *) arg->arg_gw.fid, arg->arg_gw.bid, arg->arg_gw.nrb, arg->arg_gw.dist,
            arg->arg_gw.offset, arg->arg_gw.length,
            (mattr_t *) & ret.status_gw.ep_mattr_ret_t_u.attrs) < 0)
        goto error;
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status   = EP_SUCCESS;
    ret.free_quota = exportd_get_free_quota(exp);    
    goto out;
error:
    ret.hdr.eid = arg->arg_gw.eid ;  
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_mattr_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_write_block);
    return &ret;
}

/* not used anymore
ep_status_ret_t *ep_open_1_svc(epgw_mfile_arg_t * arg, struct svc_req * req) {
    static ep_status_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    if (export_open(exp, arg->arg_gw.fid) != 0)
        goto error;

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_status_ret_t_u.error = errno;
out:

    ret.status_gw.status = EP_SUCCESS;
    return &ret;
}
 */

/*
ep_status_ret_t *ep_close_1_svc(epgw_mfile_arg_t * arg, struct svc_req * req) {
    static ep_status_ret_t ret;

    export_t *exp;
    DEBUG_FUNCTION;

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    if (export_close(exp, arg->arg_gw.fid) != 0)
        goto error;

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_status_ret_t_u.error = errno;
out:
    ret.status_gw.status = EP_SUCCESS;
    return &ret;
}
 */
/*
**______________________________________________________________________________
*/
/**
*   exportd setxattr: set extended attribute

    @param args : fid of the object, extended attribute name and value
    
    @retval: EP_SUCCESS : no specific returned parameter
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_status_ret_t * ep_setxattr_1_svc(epgw_setxattr_arg_t * arg, struct svc_req * req) {
    static epgw_status_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_setxattr);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    if (export_setxattr(exp, (unsigned char *) arg->arg_gw.fid, arg->arg_gw.name,
            arg->arg_gw.value.value_val, arg->arg_gw.value.value_len, arg->arg_gw.flags) != 0) {
        goto error;
    }

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_status_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_setxattr);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd getxattr: get extended attributes

    @param args : fid of the object, extended attribute name 
    
    @retval: EP_SUCCESS : extended attribute value
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_getxattr_ret_t * ep_getxattr_1_svc(epgw_getxattr_arg_t * arg, struct svc_req * req) {
    static epgw_getxattr_ret_t ret;
    export_t *exp;
    ssize_t size = -1;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_getxattr);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    xdr_free((xdrproc_t) xdr_epgw_getxattr_ret_t, (char *) &ret);

    ret.status_gw.ep_getxattr_ret_t_u.value.value_val = xmalloc(ROZOFS_XATTR_VALUE_MAX);

    if ((size = export_getxattr(exp, (unsigned char *) arg->arg_gw.fid, arg->arg_gw.name,
            ret.status_gw.ep_getxattr_ret_t_u.value.value_val, arg->arg_gw.size)) == -1) {
        goto error;
    }

    ret.status_gw.ep_getxattr_ret_t_u.value.value_len = size;

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    if (ret.status_gw.ep_getxattr_ret_t_u.value.value_val != NULL)
        free(ret.status_gw.ep_getxattr_ret_t_u.value.value_val);
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_getxattr_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_getxattr);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd removexattr: remove extended attribute

    @param args : fid of the object, extended attribute name 
    
    @retval: EP_SUCCESS : no returned value
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_status_ret_t * ep_removexattr_1_svc(epgw_removexattr_arg_t * arg, struct svc_req * req) {
    static epgw_status_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_removexattr);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    if (export_removexattr(exp, (unsigned char *) arg->arg_gw.fid, arg->arg_gw.name) != 0) {
        goto error;
    }

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_status_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_removexattr);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd listxattr: list the extended attributes associated with an object (file,directory)

    @param args : fid of the object 
    
    @retval: EP_SUCCESS : list of the object extended attributes (name and value)
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_listxattr_ret_t * ep_listxattr_1_svc(epgw_listxattr_arg_t * arg, struct svc_req * req) {
    static epgw_listxattr_ret_t ret;
    export_t *exp;
    ssize_t size = -1;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_listxattr);

    xdr_free((xdrproc_t) xdr_epgw_listxattr_ret_t, (char *) &ret);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    // Allocate memory
    ret.status_gw.ep_listxattr_ret_t_u.list.list_val =
            (char *) xmalloc(arg->arg_gw.size * sizeof (char));

    if ((size = export_listxattr(exp, (unsigned char *) arg->arg_gw.fid,
            ret.status_gw.ep_listxattr_ret_t_u.list.list_val, arg->arg_gw.size)) == -1) {
        goto error;
    }

    ret.status_gw.ep_listxattr_ret_t_u.list.list_len = size;

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    if (ret.status_gw.ep_listxattr_ret_t_u.list.list_val != NULL)
        free(ret.status_gw.ep_listxattr_ret_t_u.list.list_val);
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_listxattr_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_listxattr);
    return &ret;
}

/*
**______________________________________________________________________________
*/
/**
*   exportd set file lock 

    @param args 
    
    @retval: EP_SUCCESS : no returned value
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
/*
**______________________________________________________________________________
*/
/**
*   exportd set file lock 

    @param args 
    
    @retval: EP_SUCCESS : no returned value
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_lock_ret_t * ep_set_file_lock_1_svc(epgw_lock_arg_t * arg, struct svc_req * req) {
    static epgw_lock_ret_t ret;
    int    res;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_set_file_lock);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    res = export_set_file_lock(exp, (unsigned char *) arg->arg_gw.fid, &arg->arg_gw.lock, &ret.gw_status.ep_lock_ret_t_u.lock);
    if(res == 0) {
        ret.gw_status.status = EP_SUCCESS;
	memcpy(&ret.gw_status.ep_lock_ret_t_u.lock,&arg->arg_gw.lock, sizeof(ep_lock_t));
        goto out;
    }
    if (errno == EWOULDBLOCK) {
       ret.gw_status.status = EP_EAGAIN;
       goto out;	
    } 
error:    
    ret.gw_status.status = EP_FAILURE;
    ret.gw_status.ep_lock_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_set_file_lock);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd reset file lock 

    @param args 
    
    @retval: EP_SUCCESS : no returned value
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_status_ret_t * ep_clear_client_file_lock_1_svc(epgw_lock_arg_t * arg, struct svc_req * req) {
    static epgw_status_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_clearclient_flock);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    if (export_clear_client_file_lock(exp, &arg->arg_gw.lock) != 0) {
        goto error;
    }

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_status_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_clearclient_flock);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd reset all file locks of an owner 

    @param args 
    
    @retval: EP_SUCCESS : no returned value
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_status_ret_t * ep_clear_owner_file_lock_1_svc(epgw_lock_arg_t * arg, struct svc_req * req) {
    static epgw_status_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_clearowner_flock);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    if (export_clear_owner_file_lock(exp, (unsigned char *) arg->arg_gw.fid, &arg->arg_gw.lock) != 0) {
        goto error;
    }

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_status_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_clearowner_flock);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd set file lock 

    @param args 
    
    @retval: EP_SUCCESS : no returned value
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_lock_ret_t * ep_get_file_lock_1_svc(epgw_lock_arg_t * arg, struct svc_req * req) {
    static epgw_lock_ret_t ret;
    int    res;
    export_t *exp;
    DEBUG_FUNCTION;
    
    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_get_file_lock);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    res = export_get_file_lock(exp, (unsigned char *) arg->arg_gw.fid, &arg->arg_gw.lock, &ret.gw_status.ep_lock_ret_t_u.lock);
    if(res == 0) {
        ret.gw_status.status = EP_SUCCESS;
	memcpy(&ret.gw_status.ep_lock_ret_t_u.lock,&arg->arg_gw.lock, sizeof(ep_lock_t));
        goto out;
    }
    if (errno == EWOULDBLOCK) {
       ret.gw_status.status = EP_EAGAIN;
       goto out;	
    } 
error:
    ret.gw_status.status = EP_FAILURE;
    ret.gw_status.ep_lock_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_get_file_lock);
    return &ret;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd poll file lock 

    @param args 
    
    @retval: EP_SUCCESS : no returned value
    @retval: EP_FAILURE :error code associated with the operation (errno)
*/
epgw_status_ret_t * ep_poll_file_lock_1_svc(epgw_lock_arg_t * arg, struct svc_req * req) {
    static epgw_status_ret_t ret;
    export_t *exp;
    DEBUG_FUNCTION;

    // Set profiler export index
    export_profiler_eid = arg->arg_gw.eid;

    START_PROFILING(ep_poll_file_lock);

    if (!(exp = exports_lookup_export(arg->arg_gw.eid)))
        goto error;

    if (export_poll_file_lock(exp, &arg->arg_gw.lock) != 0) {
        goto error;
    }

    ret.status_gw.status = EP_SUCCESS;
    goto out;
error:
    ret.status_gw.status = EP_FAILURE;
    ret.status_gw.ep_status_ret_t_u.error = errno;
out:
    STOP_PROFILING(ep_poll_file_lock);
    return &ret;
}
