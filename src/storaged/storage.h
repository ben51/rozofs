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

#ifndef _STORAGE_H
#define _STORAGE_H

#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <uuid/uuid.h>
#include <sys/param.h>

#include <rozofs/rozofs.h>
#include <rozofs/common/list.h>
#include <rozofs/common/htable.h>

/** Maximum size in bytes for the header of file bins */
#define ROZOFS_ST_BINS_FILE_HDR_SIZE 8192

/** Default open flags to use for open bins files */
#ifdef __FreeBSD__
#define ROZOFS_ST_BINS_FILE_FLAG O_RDWR | O_CREAT
#define ROZOFS_ST_BINS_FILE_FLAG_NO_CREATE O_RDWR
#else
#define ROZOFS_ST_BINS_FILE_FLAG O_RDWR | O_CREAT | O_NOATIME
#define ROZOFS_ST_BINS_FILE_FLAG_NO_CREATE O_RDWR | O_NOATIME
#endif

/** Default mode to use for open bins files */
#define ROZOFS_ST_BINS_FILE_MODE S_IFREG | S_IRUSR | S_IWUSR

/** Default mode to use for create subdirectories */
#define ROZOFS_ST_DIR_MODE S_IRUSR | S_IWUSR | S_IXUSR

#define MAX_REBUILD_ENTRIES 60

/** Directory used to store bins files for a specific storage ID*/
typedef struct storage {
    sid_t sid; ///< unique id of this storage for one cluster
    cid_t cid; //< unique id of cluster that owns this storage
    char root[FILENAME_MAX]; ///< absolute path.
} storage_t;

/**
 *  Header structure for one file bins
 */
typedef struct rozofs_stor_bins_file_hdr {
    uint8_t layout; ///< layout used for this file.
    sid_t dist_set_current[ROZOFS_SAFE_MAX]; ///< currents sids of storage nodes target for this file.
    sid_t dist_set_next[ROZOFS_SAFE_MAX]; ///< next sids of storage nodes target for this. file (not used yet)
    uint8_t version; ///<  version of rozofs. (not used yet)
} rozofs_stor_bins_file_hdr_t;

typedef struct bins_file_rebuild {
    fid_t fid;
    uint8_t layout; ///< layout used for this file.
    sid_t dist_set_current[ROZOFS_SAFE_MAX]; ///< currents sids of storage nodes target for this file.
    struct bins_file_rebuild *next;
} bins_file_rebuild_t;

/** Get the directory path for a given [storage, layout, dist_set, spare]
 *
 * @param st: the storage to be initialized.
 * @param layout: layout used for store this file.
 * @param dist_set: storages nodes used for store this file.
 * @param spare: indicator on the status of the projection.
 * @param path: the directory path.
 *
 * @return: the directory path
 */
char *storage_map_distribution(storage_t * st, uint8_t layout,
        sid_t dist_set[ROZOFS_SAFE_MAX], uint8_t spare, char *path);

/** Add the fid bins file to a given path
 *
 * @param fid: unique file id.
 * @param path: the directory path.
 *
 * @return: the directory path
 */
char *storage_map_projection(fid_t fid, char *path);

/** Initialize a storage
 *
 * @param st: the storage to be initialized.
 * @param cid: unique id of cluster that owns this storage.
 * @param sid: the unique id for this storage.
 * @param root: the absolute path.
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int storage_initialize(storage_t *st, cid_t cid, sid_t sid, const char *root);

/** Release a storage
 *
 * @param st: the storage to be released.
 */
void storage_release(storage_t * st);

/** Write nb_proj projections
 *
 * @param st: the storage to use.
 * @param layout: layout used for store this file.
 * @param dist_set: storages nodes used for store this file.
 * @param spare: indicator on the status of the projection.
 * @param fid: unique file id.
 * @param bid: first block idx (offset).
 * @param nb_proj: nb of projections to write.
 * @param version: version of rozofs used by the client. (not used yet)
 * @param *file_size: size of file after the write operation.
 * @param *bins: bins to store.
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int storage_write(storage_t * st, uint8_t layout, sid_t * dist_set,
        uint8_t spare, fid_t fid, bid_t bid, uint32_t nb_proj, uint8_t version,
        uint64_t *file_size, const bin_t * bins);

/** Read nb_proj projections
 *
 * @param st: the storage to use.
 * @param layout: layout used by this file.
 * @param dist_set: storages nodes used for store this file.
 * @param spare: indicator on the status of the projection.
 * @param fid: unique file id.
 * @param bid: first block idx (offset).
 * @param nb_proj: nb of projections to read.
 * @param *bins: bins to store.
 * @param *len_read: the length read.
 * @param *file_size: size of file after the read operation.
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int storage_read(storage_t * st, uint8_t layout, sid_t * dist_set,
        uint8_t spare, fid_t fid, bid_t bid, uint32_t nb_proj,
        bin_t * bins, size_t * len_read, uint64_t *file_size);

/** Truncate a bins file (not used yet)
 *
 * @param st: the storage to use.
 * @param layout: layout used by this file.
 * @param dist_set: storages nodes used for store this file.
 * @param spare: indicator on the status of the projection.
 * @param fid: unique file id.
 * @param proj_id: the projection id.
 * @param bid: first block idx (offset).
 * @param last_seg: length of the last segment if not modulo prj. size
 * @param last_timestamp: timestamp to associate with the last_seg
 
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int storage_truncate(storage_t * st, uint8_t layout, sid_t * dist_set,
        uint8_t spare, fid_t fid, tid_t proj_id,bid_t bid,uint8_t version,
         uint16_t last_seg,uint64_t last_timestamp,u_int len, char * data);

/** Remove a bins file
 *
 * @param st: the storage to use.
 * @param layout: layout used by this file.
 * @param dist_set: storages nodes used for store this file.
 * @param fid: unique file id.
 * @param version: version of rozofs used by the client. (not used yet)
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int storage_rm_file(storage_t * st, uint8_t layout, sid_t * dist_set,
        fid_t fid);

/** Stat a storage
 *
 * @param st: the storage to use.
 * @param sstat: structure to use for store stats about this storage.
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int storage_stat(storage_t * st, sstat_t * sstat);


int storage_list_bins_files_to_rebuild(storage_t * st, sid_t sid,
        uint8_t * layout, sid_t *dist_set, uint8_t * spare, uint64_t * cookie,
        bins_file_rebuild_t ** children, uint8_t * eof);

#endif

