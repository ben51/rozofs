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

#ifndef _ROZOFS_H
#define _ROZOFS_H

#include <stdint.h>
#include <uuid/uuid.h>

#include <config.h>

/**
* Ports definition of RozoFS
*/
#define EXPNB_SLAVE_PORT       53000  /**< supports up to 8 exportd slave */
#define ROZOFS_GET_EXPNB_PORT  get_service_port("rozo_export_nb",NULL,EXPNB_SLAVE_PORT)
 
/*
** Port on which storaged services MPROTO and SPPROTO
*/
#define ROZOFS_MPROTO_PORT 51000

#define ROZOFS_UUID_SIZE 16
/* Instead of using an array of unsigned char for store the UUID, we use an
 * array of uint32_t in the RPC protocol to use less space (see XDR). */
#define ROZOFS_UUID_SIZE_RPC (ROZOFS_UUID_SIZE/sizeof(uint32_t))
#define ROZOFS_UUID_SIZE_NET ROZOFS_UUID_SIZE_RPC
#define ROZOFS_HOSTNAME_MAX 128
#define ROZOFS_BSIZE 8192 // could it be export specific?
#define ROZOFS_SAFE_MAX 36
#define ROZOFS_SAFE_MAX_RPC  (ROZOFS_SAFE_MAX/sizeof(uint32_t))
/* Instead of using an array of sid_t for store the dist_set, we use an
 * array of uint32_t in the RPC protocol to use less space (see XDR). */
#define ROZOFS_SAFE_MAX_NET ROZOFS_SAFE_MAX_RPC
#define ROZOFS_DIR_SIZE 4096
#define ROZOFS_PATH_MAX 1024
#define ROZOFS_XATTR_NAME_MAX 255
#define ROZOFS_XATTR_VALUE_MAX 65536
#define ROZOFS_XATTR_LIST_MAX 65536
#define ROZOFS_FILENAME_MAX 255
/* Maximum file size (check for truncate) */
#define ROZOFS_FILESIZE_MAX 0x20000000000LL

/* Value for rpc buffer size used for sproto */
#define ROZOFS_RPC_STORAGE_BUFFER_SIZE (1024*300) 

#define ROZOFS_CLUSTERS_MAX 255 /**< FDL : limit for cluster */
/* Value max for a SID */
#define SID_MAX 255
/* Value min for a SID */
#define SID_MIN 1
/* Nb. max of storage node for one volume */
#define STORAGE_NODES_MAX 64
/* Nb. max of storaged ports on the same storage node */
#define STORAGE_NODE_PORTS_MAX 32
/* Nb. max of storages (couple cid:sid) on the same storage node */
#define STORAGES_MAX_BY_STORAGE_NODE 32
/* First TCP port used */
#define STORAGE_PORT_NUM_BEGIN 40000

#define MAX_DIR_ENTRIES 100
#define ROZOFS_MD5_SIZE 22
#define ROZOFS_MD5_NONE "0000000000000000000000"


#define EXPGW_EID_MAX_IDX 1024 /**< max number of eid  */
#define EXPGW_EXPGW_MAX_IDX 32 /**< max number of export gateway per exportd */
#define EXPGW_EXPORTD_MAX_IDX 64 /**< max number of exportd */


#define P_COUNT     0
#define P_ELAPSE    1
#define P_BYTES     2

#define MICROLONG(time) ((unsigned long long)time.tv_sec * 1000000 + time.tv_usec)

/* Value max for an Exportd Gateway */
#define GWID_MAX 32
/* Value min for a Exportd Gateway */
#define GWID_MIN 1


/* Timeout in seconds for storaged requests by mproto */
#define ROZOFS_MPROTO_TIMEOUT_SEC 4

typedef enum {
    LAYOUT_2_3_4, LAYOUT_4_6_8, LAYOUT_8_12_16
} rozofs_layout_t;

typedef uint8_t tid_t; /**< projection id */
typedef uint64_t bid_t; /**< block id */
typedef uuid_t fid_t; /**< file id */
typedef uint8_t sid_t; /**< storage id */
typedef uint16_t cid_t; /**< cluster id */
typedef uint16_t vid_t; /**< volume id */
typedef uint32_t eid_t; /**< export id */

// storage stat

typedef struct sstat {
    uint64_t size;
    uint64_t free;
} sstat_t;

typedef struct estat {
    uint16_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t files;
    uint64_t ffree;
    uint16_t namemax;
} estat_t;

/**
 *  Header structure for one projection
 */
typedef union {
    uint64_t u64[2];

    struct {
        uint64_t timestamp : 64; ///<  time stamp.
        uint64_t effective_length : 16; ///<  effective length of the rebuilt block size: MAX is 64K.
        uint64_t projection_id : 8; ///<  index of the projection -> needed to find out angles/sizes: MAX is 255.
        uint64_t version : 8; ///<  version of rozofs. (not used yet)
        uint64_t filler : 32; ///<  for future usage.
    } s;
} rozofs_stor_bins_hdr_t;

typedef struct child {
    char *name;
    fid_t fid;
    struct child *next;
} child_t;

#include "common/transform.h"

/**
 *  By default the system uses 256 slices with 4096 subslices per slice
 */
#define MAX_SLICE_BIT 8
#define MAX_SLICE_NB (1<<MAX_SLICE_BIT)
#define MAX_SUBSLICE_BIT 12
#define MAX_SUBSLICE_NB (1<<MAX_SUBSLICE_BIT)
/*
 **__________________________________________________________________
 */
static inline void mstor_get_slice_and_subslice(fid_t fid, uint32_t *slice, uint32_t *subslice) {
    uint32_t hash = 0;
    uint8_t *c = 0;

    for (c = fid; c != fid + 16; c++)
        hash = *c + (hash << 6) + (hash << 16) - hash;

    *slice = hash & ((1 << MAX_SLICE_BIT) - 1);
    hash = hash >> MAX_SLICE_BIT;
    *subslice = hash & ((1 << MAX_SUBSLICE_BIT) - 1);
}

#ifdef __FreeBSD__
#include <rpc/rpc.h>
extern bool_t	xdr_int8_t(XDR *, int8_t *);
extern bool_t	xdr_u_int8_t(XDR *, u_int8_t *);
extern bool_t	xdr_uint8_t(XDR *, u_int8_t *);
#endif

#endif
