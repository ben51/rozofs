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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <attr/xattr.h>

#ifdef __FreeBSD__
#include <sys/param.h>
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif

#include <uuid/uuid.h>
#include <sys/types.h>
#include <inttypes.h>
#include <dirent.h>
#include <time.h>

#include <rozofs/common/log.h>
#include <rozofs/common/xmalloc.h>
#include <rozofs/common/list.h>
#include <rozofs/rozofs_srv.h>
#include <rozofs/rpc/export_profiler.h>
#include <rozofs/rpc/epproto.h>
#include <rozofs/rpc/mclient.h>

#include "config.h"
#include "export.h"
#include "cache.h"
#include "mdirent.h"

/** Max entries of lv1 directory structure (nb. of buckets) */
#define MAX_LV1_BUCKETS 1024
#define LV1_NOCREATE 0
#define LV1_CREATE 1

/** Default mode for export root directory */
#define EXPORT_DEFAULT_ROOT_MODE S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

/**
 *  Structure used for store information about a file to remove
 */
typedef struct rmfentry {
    fid_t fid; ///<  unique file id.
    cid_t cid; /// unique cluster id where the file is stored.
    sid_t initial_dist_set[ROZOFS_SAFE_MAX];
    ///< initial sids of storage nodes target for this file.
    sid_t current_dist_set[ROZOFS_SAFE_MAX];
    ///< current sids of storage nodes target for this file.
    list_t list; ///<  pointer for extern list.
} rmfentry_t;

typedef struct cnxentry {
    mclient_t *cnx;
    list_t list;
} cnxentry_t;


extern epp_profiler_t  gprofiler;

/*
 **__________________________________________________________________
 */

/** get the lv1 directory.
 *
 * lv1 entries are first level directories of an export root named by uint32_t
 * string value and used has entry of a hash table storing the export
 * meta data files.
 *
 * @param root_path: root path of the exportd
 * @param slice: value of the slice
 *
 * @return 0 on success otherwise -1
 */
static inline int mstor_slice_resolve_entry(char *root_path, uint32_t slice) {
    char path[PATH_MAX];
    sprintf(path, "%s/%"PRId32"", root_path, slice);
    if (access(path, F_OK) == -1) {
        if (errno == ENOENT) {
            /*
             ** it is the fisrt time we access to the slice
             **  we need to create the level 1 directory and the 
             ** timestamp file
             */
            if (mkdir(path, S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
                severe("mkdir (%s): %s", path, strerror(errno));
                return -1;
            }
            //          mstor_ts_srv_create_slice_timestamp_file(export,slice); 
            return 0;
        }
        /*
         ** any other error
         */
        severe("access (%s): %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 **__________________________________________________________________
 */

/** get the subslice directory index.
 *
 * lv1 entries are first level directories of an export root named by uint32_t
 * string value and used has entry of a hash table storing the export
 * meta data files.
 *
 * @param root_path: root path of the exportd
 * @param fid: the search fid
 *
 * @return 0 on success otherwise -1
 */
static inline int mstor_subslice_resolve_entry(char *root_path, fid_t fid, uint32_t slice, uint32_t subslice) {
    char path[PATH_MAX];


    sprintf(path, "%s/%d/%d", root_path, slice, subslice);
    if (access(path, F_OK) == -1) {
        if (errno == ENOENT) {
            /*
             ** it is the fisrt time we access to the subslice
             **  we need to create the associated directory 
             */
            if (mkdir(path, S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
                severe("mkdir (%s): %s", path, strerror(errno));
                return -1;
            }
            return 0;
        }
        severe("access (%s): %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

/** build a full path based on export root and fid of the lv2 file
 *
 * lv2 is the second level of files or directories in storage of metadata
 * they are acceded thru mreg or mdir API according to their type.
 *
 * @param root_path: root path of the exportd
 * @param fid: the fid we are looking for
 * @param path: the path to fill in
 */

static inline int export_lv2_resolve_path_internal(char *root_path, fid_t fid, char *path) {
    uint32_t slice;
    uint32_t subslice;
    char str[37];

    /*
     ** extract the slice and subsclie from the fid
     */
    mstor_get_slice_and_subslice(fid, &slice, &subslice);
    /*
     ** check the existence of the slice directory: create it if it does not exist
     */
    if (mstor_slice_resolve_entry(root_path, slice) < 0) {
        goto error;
    }
    /*
     ** check the existence of the subslice directory: create it if it does not exist
     */
    if (mstor_subslice_resolve_entry(root_path, fid, slice, subslice) < 0) {
        goto error;
    }
    /*
     ** convert the fid in ascii
     */
    uuid_unparse(fid, str);
    sprintf(path, "%s/%d/%d/%s", root_path, slice, subslice, str);
    return 0;

error:
    return -1;
}

/** build a full path based on export root and fid of the lv2 file
 *
 * lv2 is the second level of files or directories in storage of metadata
 * they are acceded thru mreg or mdir API according to their type.
 *
 * @param export: the export we are searching on
 * @param fid: the fid we are looking for
 * @param path: the path to fill in
 */

int export_lv2_resolve_path(export_t *export, fid_t fid, char *path) {
    int ret;

    START_PROFILING(export_lv2_resolve_path);

    ret = export_lv2_resolve_path_internal(export->root, fid, path);

    STOP_PROFILING(export_lv2_resolve_path);
    return ret;
}

/** search a fid in the cache
 *
 * if fid is not cached, try to find it on the underlying file system
 * and cache it.
 *
 * @param e: the underlying export
 * @param fid: the searched fid
 *
 * @return a pointer to lv2 entry or null on error (errno is set)
 */
static lv2_entry_t *export_lookup_fid(export_t *e, fid_t fid) {
    lv2_entry_t *lv2 = 0;
    START_PROFILING(export_lookup_fid);

    if (!(lv2 = lv2_cache_get(e->lv2_cache, fid))) {
        // not cached, find it an cache it
        char lv2_path[PATH_MAX];
        if (export_lv2_resolve_path(e, fid, lv2_path) != 0) {
            return lv2;
        }
        if (!(lv2 = lv2_cache_put(e->lv2_cache, fid, lv2_path))) {
            return lv2;
        }
    }

    STOP_PROFILING(export_lookup_fid);
    return lv2;
}

/** store the attributes part of a lv2_entry_t to the export's file system
 *
 * @param entry: the entry used
 *
 * @return: 0 on success otherwise -1
 */
static int export_lv2_write_attributes(lv2_entry_t *entry) {

    if (S_ISDIR(entry->attributes.mode)) {
        return mdir_write_attributes(&entry->container.mdir, &entry->attributes);
    } else if (S_ISREG(entry->attributes.mode)) {
        return mreg_write_attributes(&entry->container.mreg, &entry->attributes);
    } else if (S_ISLNK(entry->attributes.mode)) {
        return mslnk_write_attributes(&entry->container.mslnk, &entry->attributes);
    } else {
        errno = ENOTSUP;
        return -1;
    }
}

/** set an extended attribute value for a lv2_entry_t.
 *
 * @param entry: the entry used
 * @param name: the extended attribute name.
 * @param value: the value of this extended attribute.
 * @param size: the size of a buffer to hold the value associated
 *  with this extended attribute.
 * @param flags: parameter can be used to refine the semantics of the operation.
 *
 * @return: On success, zero is returned.  On failure, -1 is returned.
 */
static int export_lv2_set_xattr(lv2_entry_t *entry, const char *name,
        const void *value, size_t size, int flags) {

    if (S_ISDIR(entry->attributes.mode)) {
        return mdir_set_xattr(&entry->container.mdir, name, value, size, flags);
    } else if (S_ISREG(entry->attributes.mode)) {
        return mreg_set_xattr(&entry->container.mreg, name, value, size, flags);
    } else if (S_ISLNK(entry->attributes.mode)) {
        return mslnk_set_xattr(&entry->container.mslnk, name, value, size, flags);
    } else {
        errno = ENOTSUP;
        return -1;
    }
}

/** retrieve an extended attribute value from the lv2_entry_t.
 *
 * @param entry: the entry used
 * @param name: the extended attribute name.
 * @param value: the value of this extended attribute.
 * @param size: the size of a buffer to hold the value associated
 *  with this extended attribute.
 *
 * @return: On success, the size of the extended attribute value.
 * On failure, -1 is returned and errno is set appropriately.
 */
static ssize_t export_lv2_get_xattr(lv2_entry_t *entry, const char *name,
        void *value, size_t size) {

    if (S_ISDIR(entry->attributes.mode)) {
        return mdir_get_xattr(&entry->container.mdir, name, value, size);
    } else if (S_ISREG(entry->attributes.mode)) {
        return mreg_get_xattr(&entry->container.mreg, name, value, size);
    } else if (S_ISLNK(entry->attributes.mode)) {
        return mslnk_get_xattr(&entry->container.mslnk, name, value, size);
    } else {
        errno = ENOTSUP;
        return -1;
    }
}

/** remove an extended attribute from the lv2_entry_t.
 *
 * @param entry: the entry used
 * @param name: the extended attribute name.
 *
 * @return: On success, zero is returned.  On failure, -1 is returned.
 */
static int export_lv2_remove_xattr(lv2_entry_t *entry, const char *name) {

    if (S_ISDIR(entry->attributes.mode)) {
        return mdir_remove_xattr(&entry->container.mdir, name);
    } else if (S_ISREG(entry->attributes.mode)) {
        return mreg_remove_xattr(&entry->container.mreg, name);
    } else if (S_ISLNK(entry->attributes.mode)) {
        return mslnk_remove_xattr(&entry->container.mslnk, name);
    } else {
        errno = ENOTSUP;
        return -1;
    }
}

/** list extended attribute names from the lv2_entry_t.
 *
 * @param entry: the entry used
 * @param list: list of extended attribute names associated with this directory.
 * @param size: the size of a buffer to hold the list of extended attributes.
 *
 * @return: On success, the size of the extended attribute name list.
 * On failure, -1 is returned and errno is set appropriately.
 */
static ssize_t export_lv2_list_xattr(lv2_entry_t *entry, void *list,
        size_t size) {

    if (S_ISDIR(entry->attributes.mode)) {
        return mdir_list_xattr(&entry->container.mdir, list, size);
    } else if (S_ISREG(entry->attributes.mode)) {
        return mreg_list_xattr(&entry->container.mreg, list, size);
    } else if (S_ISLNK(entry->attributes.mode)) {
        return mslnk_list_xattr(&entry->container.mslnk, list, size);
    } else {
        errno = ENOTSUP;
        return -1;
    }
}

/** update the number of files in file system
 *
 * @param e: the export to update
 * @param n: number of files
 *
 * @return 0 on success -1 otherwise
 */
static int export_update_files(export_t *e, int32_t n) {
    int status = -1;
    START_PROFILING(export_update_files);

    e->fstat.files += n;
    if (pwrite(e->fdstat, &e->fstat, sizeof (export_fstat_t), 0)
            != sizeof (export_fstat_t))
        goto out;

    status = 0;
out:
    STOP_PROFILING(export_update_files);
    return status;
}

/** update the number of blocks in file system
 *
 * @param e: the export to update
 * @param n: number of blocks
 *
 * @return 0 on success -1 otherwise
 */
static int export_update_blocks(export_t * e, int32_t n) {
    int status = -1;
    START_PROFILING(export_update_blocks);

    if (e->hquota > 0 && e->fstat.blocks + n > e->hquota) {
        warning("quota exceed: %"PRIu64" over %"PRIu64"", e->fstat.blocks + n,
                e->hquota);
        errno = EDQUOT;
        goto out;
    }

    e->fstat.blocks += n;
    if (pwrite(e->fdstat, &e->fstat, sizeof (export_fstat_t), 0)
            != sizeof (export_fstat_t))
        goto out;

    status = 0;
out:
    STOP_PROFILING(export_update_blocks);
    return status;
}

/** constants of the export */
typedef struct export_const {
    char version[20]; ///< rozofs version
    fid_t rfid; ///< root id
} export_const_t;

int export_is_valid(const char *root) {
    char path[PATH_MAX];
    char trash_path[PATH_MAX];
    char fstat_path[PATH_MAX];
    char const_path[PATH_MAX];

    if (!realpath(root, path))
        return -1;

    if (access(path, R_OK | W_OK | X_OK) != 0)
        return -1;

    // check trash directory
    sprintf(trash_path, "%s/%s", path, TRASH_DNAME);
    if (access(trash_path, F_OK) != 0)
        return -1;

    // check fstat file
    sprintf(fstat_path, "%s/%s", path, FSTAT_FNAME);
    if (access(fstat_path, F_OK) != 0)
        return -1;

    // check const file
    sprintf(const_path, "%s/%s", path, CONST_FNAME);
    if (access(const_path, F_OK) != 0)
        return -1;

    return 0;
}

static int export_load_rmfentry(export_t * e) {
    int status = -1;
    DIR *dd = NULL;
    struct dirent *dirent_sub_trash = NULL;
    rmfentry_t *rmfe = NULL;
    char trash_path[PATH_MAX];
    int i = 0;

    // Build thrash path
    sprintf(trash_path, "%s/%s", e->root, TRASH_DNAME);

    // Create trash directory if necessary
    if (access(trash_path, F_OK) != 0) {
        if (mkdir(trash_path, S_IRUSR | S_IWUSR | S_IXUSR) < 0) {
            severe("mkdir failed (%s): %s", trash_path, strerror(errno));
            goto out;
        }
    }

    // Open trash directory
    if ((dd = opendir(trash_path)) == NULL) {
        severe("opendir failed (%s): %s", trash_path, strerror(errno));
        goto out;
    }

    // For each subdirectory under trash
    while ((dirent_sub_trash = readdir(dd)) != NULL) {

        // Check . and ..
        if ((strcmp(dirent_sub_trash->d_name, ".") == 0) ||
                (strcmp(dirent_sub_trash->d_name, "..") == 0)) {
            continue;
        }

        char bucket_path[PATH_MAX];
        DIR *bucket_dir = NULL;
        struct dirent *dirent_sub_bucket = NULL;
        struct stat s;

        // Build bucket directory path
        sprintf(bucket_path, "%s/%s", trash_path, dirent_sub_trash->d_name);

        // Check if is a directory
        if (stat(bucket_path, &s) != 0)
            continue;
        if (S_ISDIR(s.st_mode) == 0)
            continue;

        // Check if is a valid number
        i = atoi(dirent_sub_trash->d_name);
        if (i > RM_MAX_BUCKETS)
            continue;

        // Open bucket directory
        if ((bucket_dir = opendir(bucket_path)) == NULL) {
            severe("opendir (%s) failed: %s", bucket_path, strerror(errno));
            continue;
        }

        // Scan bucket directory
        while ((dirent_sub_bucket = readdir(bucket_dir)) != NULL) {

            char rm_file_path[PATH_MAX];
            int fd = -1;
            mattr_t attrs;

            // Check . and ..
            if ((strcmp(dirent_sub_bucket->d_name, ".") == 0) ||
                    (strcmp(dirent_sub_bucket->d_name, "..") == 0)) {
                continue;
            }

            // Build file to delete path
            sprintf(rm_file_path, "%s/%s/%s/%s", e->root, TRASH_DNAME,
                    dirent_sub_trash->d_name, dirent_sub_bucket->d_name);

            // Open file to delete
            if ((fd = open(rm_file_path, O_RDWR, S_IRWXU)) == -1) {
                severe("open (%s) failed: %s", rm_file_path, strerror(errno));
                continue;
            }

            // Read file to delete
            if ((pread(fd, &attrs, sizeof (mattr_t), 0))
                    != (sizeof (mattr_t))) {
                severe("pread (%s) failed: %s", rm_file_path, strerror(errno));
                continue;
            }

            // Build the rmfentry_t
            rmfe = xmalloc(sizeof (rmfentry_t));
            memcpy(rmfe->fid, attrs.fid, sizeof (fid_t));
            rmfe->cid = attrs.cid;
            memcpy(rmfe->initial_dist_set, attrs.sids,
                    sizeof (sid_t) * ROZOFS_SAFE_MAX);
            memcpy(rmfe->current_dist_set, attrs.sids,
                    sizeof (sid_t) * ROZOFS_SAFE_MAX);
            list_init(&rmfe->list);

            // Acquire lock on bucket trash list
            if ((errno = pthread_rwlock_wrlock(&e->trash_buckets[i].rm_lock)) != 0) {
                severe("pthread_rwlock_wrlock failed: %s", strerror(errno));
                goto out;
            }

            // Check size of file 
            if (attrs.size >= RM_FILE_SIZE_TRESHOLD) {
                // Add to front of list
                list_push_front(&e->trash_buckets[i].rmfiles, &rmfe->list);
            } else {
                // Add to back of list
                list_push_back(&e->trash_buckets[i].rmfiles, &rmfe->list);
            }

            if ((errno = pthread_rwlock_unlock(&e->trash_buckets[i].rm_lock)) != 0) {
                severe("pthread_rwlock_unlock failed: %s", strerror(errno));
                goto out;
            }

            // Close file
            if (fd != -1)
                close(fd);
        }

        // Close bucket directory
        if (bucket_dir != NULL)
            closedir(bucket_dir);
    }
    status = 0;
out:
    // Close trash directory
    if (dd != NULL)
        closedir(dd);

    return status;
}

int export_create(const char *root) {
    const char *version = VERSION;
    char path[PATH_MAX];
    char trash_path[PATH_MAX];
    char fstat_path[PATH_MAX];
    char const_path[PATH_MAX];
    char root_path[PATH_MAX];
    export_fstat_t est;
    export_const_t ect;
    int fd = -1;
    mattr_t root_attrs;
    mdir_t root_mdir;

    if (!realpath(root, path))
        return -1;

    // create trash directory
    sprintf(trash_path, "%s/%s", path, TRASH_DNAME);
    if (mkdir(trash_path, S_IRUSR | S_IWUSR | S_IXUSR) < 0) {
        return -1;
    }

    // create fstat file
    sprintf(fstat_path, "%s/%s", path, FSTAT_FNAME);
    if ((fd = open(fstat_path, O_RDWR | O_CREAT, S_IRWXU)) < 1) {
        return -1;
    }

    est.blocks = est.files = 0;
    if (write(fd, &est, sizeof (export_fstat_t)) != sizeof (export_fstat_t)) {
        close(fd);
        return -1;
    }
    close(fd);

    //create root
    memset(&root_attrs, 0, sizeof (mattr_t));
    uuid_generate(root_attrs.fid);
    root_attrs.cid = 0;
    memset(root_attrs.sids, 0, ROZOFS_SAFE_MAX * sizeof (sid_t));

    // Put the default mode for the root directory
    root_attrs.mode = EXPORT_DEFAULT_ROOT_MODE;

    root_attrs.nlink = 2;
    root_attrs.uid = 0; // root
    root_attrs.gid = 0; // root
    if ((root_attrs.ctime = root_attrs.atime = root_attrs.mtime = time(NULL)) == -1)
        return -1;
    root_attrs.size = ROZOFS_DIR_SIZE;
    /*
     ** create the slice and subslice directory for root if they don't exist
     ** and then create the "fid" directory or the root
     */
    if (export_lv2_resolve_path_internal(path, root_attrs.fid, root_path) != 0)
        return -1;

    if (mkdir(root_path, S_IRUSR | S_IWUSR | S_IXUSR) < 0) {
        return -1;
    }

    // open the root mdir
    if (mdir_open(&root_mdir, root_path) != 0) {
        return -1;
    }

    // Set children count to 0
    root_attrs.children = 0;

    if (mdir_write_attributes(&root_mdir, &root_attrs) != 0) {
        mdir_close(&root_mdir);
        return -1;
    }

    // Initialize the dirent level 0 cache
    dirent_cache_level0_initialize();

    // create "." ".." lv3 entries
    if (put_mdirentry(root_mdir.fdp, root_attrs.fid, ".", root_attrs.fid, S_IFDIR | S_IRWXU) != 0) {
        mdir_close(&root_mdir);
        return -1;
    }
    if (put_mdirentry(root_mdir.fdp, root_attrs.fid, "..", root_attrs.fid, S_IFDIR | S_IRWXU) != 0) {
        mdir_close(&root_mdir);
        return -1;
    }
    mdir_close(&root_mdir);

    // create const file.
    memset(&ect, 0, sizeof (export_const_t));
    uuid_copy(ect.rfid, root_attrs.fid);
    strncpy(ect.version, version, 20);
    sprintf(const_path, "%s/%s", path, CONST_FNAME);
    if ((fd = open(const_path, O_RDWR | O_CREAT, S_IRWXU)) < 1) {
        return -1;
    }

    if (write(fd, &ect, sizeof (export_const_t)) != sizeof (export_const_t)) {
        close(fd);
        return -1;
    }
    close(fd);

    return 0;
}

static void *load_trash_dir_thread(void *v) {

    export_t *export = (export_t*) v;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    // Load files to delete in trash list
    if (export_load_rmfentry(export) != 0) {
        severe("export_load_rmfentry failed: %s", strerror(errno));
        return 0;
    }

    info("Load trash directory pthread completed successfully (eid=%d)",
            export->eid);

    return 0;
}

int export_initialize(export_t * e, volume_t *volume,
        lv2_cache_t *lv2_cache, uint32_t eid, const char *root, const char *md5,
        uint64_t squota, uint64_t hquota) {

    char fstat_path[PATH_MAX];
    char const_path[PATH_MAX];
    char root_path[PATH_MAX];
    export_const_t ect;
    int fd = -1;
    int i = 0;

    if (!realpath(root, e->root))
        return -1;

    e->eid = eid;
    e->volume = volume;
    e->lv2_cache = lv2_cache;
    e->layout = volume->layout; // Layout used for this volume

    // Initialize the dirent level 0 cache
    dirent_cache_level0_initialize();

    if (strlen(md5) == 0) {
        memcpy(e->md5, ROZOFS_MD5_NONE, ROZOFS_MD5_SIZE);
    } else {
        memcpy(e->md5, md5, ROZOFS_MD5_SIZE);
    }
    e->squota = squota;
    e->hquota = hquota;

    // open the export_stat file an load it
    sprintf(fstat_path, "%s/%s", e->root, FSTAT_FNAME);
    if ((e->fdstat = open(fstat_path, O_RDWR)) < 0)
        return -1;
    if (pread(e->fdstat, &e->fstat, sizeof (export_fstat_t), 0)
            != sizeof (export_fstat_t))
        return -1;

    // Register the root
    sprintf(const_path, "%s/%s", e->root, CONST_FNAME);
    if ((fd = open(const_path, O_RDWR, S_IRWXU)) < 1) {
        return -1;
    }

    if (read(fd, &ect, sizeof (export_const_t)) != sizeof (export_const_t)) {
        close(fd);
        return -1;
    }
    close(fd);
    uuid_copy(e->rfid, ect.rfid);

    if (export_lv2_resolve_path(e, e->rfid, root_path) != 0) {
        close(e->fdstat);
        return -1;
    }

    if (!lv2_cache_put(e->lv2_cache, e->rfid, root_path)) {
        close(e->fdstat);
        return -1;
    }

    // For each trash bucket 
    for (i = 0; i < RM_MAX_BUCKETS; i++) {
        // Initialize list of files to delete
        list_init(&e->trash_buckets[i].rmfiles);
        // Initialize lock for the list of files to delete
        if ((errno = pthread_rwlock_init(&e->trash_buckets[i].rm_lock, NULL)) != 0) {
            severe("pthread_rwlock_init failed: %s", strerror(errno));
            return -1;
        }
    }

    // Initialize pthread for load files to remove
    if ((errno = pthread_create(&e->load_trash_thread, NULL,
            load_trash_dir_thread, e)) != 0) {
        severe("can't create load trash pthread: %s", strerror(errno));
        return -1;
    }

    return 0;
}

void export_release(export_t * e) {
    close(e->fdstat);
    // TODO set members to clean values
}

int export_stat(export_t * e, estat_t * st) {
    int status = -1;
    struct statfs stfs;
    volume_stat_t vstat;
    START_PROFILING(export_stat);

    st->bsize = ROZOFS_BSIZE;
    if (statfs(e->root, &stfs) != 0)
        goto out;

    // may be ROZOFS_FILENAME_MAX should be stfs.f_namelen
    //st->namemax = stfs.f_namelen;
    st->namemax = ROZOFS_FILENAME_MAX;
    st->ffree = stfs.f_ffree;
    st->blocks = e->fstat.blocks;
    volume_stat(e->volume, &vstat);

    if (e->hquota > 0) {
        if (e->hquota < vstat.bfree) {
            st->bfree = e->hquota - st->blocks;
        } else {
            st->bfree = vstat.bfree - st->blocks;
        }
    } else {
        st->bfree = vstat.bfree;
    }
    //st->bfree = e->hquota > 0 && e->hquota < vstat.bfree ? e->hquota : vstat.bfree;
    // blocks store in export stat file is the number of currently stored blocks
    // blocks in estat_t is the total number of blocks (see struct statvfs)
    // rozofs does not have a constant total number of blocks
    // it depends on usage made of storage (through other services)
    st->blocks += st->bfree;
    st->files = e->fstat.files;

    status = 0;
out:
    STOP_PROFILING(export_stat);
    return status;
}

int export_lookup(export_t *e, fid_t pfid, char *name, mattr_t *attrs,mattr_t *pattrs) {
    int status = -1;
    lv2_entry_t *plv2 = 0;
    lv2_entry_t *lv2 = 0;
    fid_t child_fid;
    uint32_t child_type;
    START_PROFILING(export_lookup);

    // get the lv2 parent
    if (!(plv2 = export_lookup_fid(e, pfid))) {
        goto out;
    }
    /*
    ** copy the parent attributes
    */
    memcpy(pattrs, &plv2->attributes, sizeof (mattr_t));

    if (get_mdirentry(plv2->container.mdir.fdp, pfid, name, child_fid, &child_type) != 0) {
        goto out;
    }

    // get the lv2
    if (!(lv2 = export_lookup_fid(e, child_fid))) {
        /*
         ** It might be possible that the file is still referenced in the dirent file but 
         ** not present on disk: its FID has been released (and the associated file deleted)
         ** In that case when attempt to read that fid file, we get a ENOENT error.
         ** So for that particular case, we remove the entry from the dirent file
         **
         **  open point : that issue is not related to regular file but also applied to directory
         ** 
         */
        int xerrno;
        uint32_t type;
        fid_t fid;
        if (errno == ENOENT) {
            /*
             ** save the initial errno and remove that entry
             */
            xerrno = errno;
            del_mdirentry(plv2->container.mdir.fdp, pfid, name, fid, &type);
            errno = xerrno;
        }

        goto out;
    }

    memcpy(attrs, &lv2->attributes, sizeof (mattr_t));

    status = 0;
out:
    STOP_PROFILING(export_lookup);
    return status;
}
/** get attributes of a managed file
 *
 * @param e: the export managing the file
 * @param fid: the id of the file
 * @param attrs: attributes to fill.
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int export_getattr(export_t *e, fid_t fid, mattr_t *attrs) {
    int status = -1;
    lv2_entry_t *lv2 = 0;
    START_PROFILING(export_getattr);

    if (!(lv2 = export_lookup_fid(e, fid))) {
        severe("export_getattr failed: %s", strerror(errno));
        goto out;
    }
    memcpy(attrs, &lv2->attributes, sizeof (mattr_t));

    status = 0;
out:
    STOP_PROFILING(export_getattr);
    return status;
}
/** set attributes of a managed file
 *
 * @param e: the export managing the file
 * @param fid: the id of the file
 * @param attrs: attributes to set.
 * @param to_set: fields to set in attributes
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int export_setattr(export_t *e, fid_t fid, mattr_t *attrs, int to_set) {
    int status = -1;
    lv2_entry_t *lv2 = 0;

    START_PROFILING(export_setattr);

    if (!(lv2 = export_lookup_fid(e, fid))) {
        goto out;
    }

    if ((to_set & EXPORT_SET_ATTR_SIZE) && S_ISREG(lv2->attributes.mode)) {
        
        // Check new file size
        if (attrs->size >= ROZOFS_FILESIZE_MAX) {
            errno = EFBIG;
            goto out;
        }

        uint64_t nrb_new = ((attrs->size + ROZOFS_BSIZE - 1) / ROZOFS_BSIZE);
        uint64_t nrb_old = ((lv2->attributes.size + ROZOFS_BSIZE - 1) /
                ROZOFS_BSIZE);

        if (lv2->attributes.size > attrs->size) {
            if (ftruncate(lv2->container.mreg.fdattrs, sizeof (mattr_t) + nrb_new * sizeof (dist_t)) != 0)
                goto out;
        } 
        if (export_update_blocks(e, ((int32_t) nrb_new - (int32_t) nrb_old))
                != 0)
            goto out;

        lv2->attributes.size = attrs->size;
    }

    if (to_set & EXPORT_SET_ATTR_MODE)
        lv2->attributes.mode = attrs->mode;
    if (to_set & EXPORT_SET_ATTR_UID)
        lv2->attributes.uid = attrs->uid;
    if (to_set & EXPORT_SET_ATTR_GID)
        lv2->attributes.gid = attrs->gid;    
    if (to_set & EXPORT_SET_ATTR_ATIME)
        lv2->attributes.atime = attrs->atime;
    if (to_set & EXPORT_SET_ATTR_MTIME)
        lv2->attributes.mtime = attrs->mtime;
    
    lv2->attributes.ctime = time(NULL);

    status = export_lv2_write_attributes(lv2);
out:
    STOP_PROFILING(export_setattr);
    return status;
}
/** create a hard link
 *
 * @param e: the export managing the file
 * @param inode: the id of the file we want to be link on
 * @param newparent: parent od the new file (the link)
 * @param newname: the name of the new file
 * @param[out] attrs: mattr_t to fill (child attributes used by upper level functions)
 * @param[out] pattrs: mattr_t to fill (parent attributes)
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int export_link(export_t *e, fid_t inode, fid_t newparent, char *newname, mattr_t *attrs,mattr_t *pattrs) {
    int status = -1;
    lv2_entry_t *target = NULL;
    lv2_entry_t *plv2 = NULL;
    fid_t child_fid;
    uint32_t child_type;

    START_PROFILING(export_link);

    // Get the lv2 inode
    if (!(target = export_lookup_fid(e, inode)))
        goto out;

    // Verify that the target is not a directory
    if (S_ISDIR(target->attributes.mode)) {
        errno = EPERM;
        goto out;
    }

    // Get the lv2 parent
    if (!(plv2 = export_lookup_fid(e, newparent)))
        goto out;

    // Verify that the mdirentry does not already exist
    if (get_mdirentry(plv2->container.mdir.fdp, newparent, newname, child_fid, &child_type) != -1) {
        errno = EEXIST;
        goto out;
    }
    /*
     ** nothing has been found, need to check the read only flag:
     ** that flag is asserted if some parts of dirent files are unreadable 
     */
    if (DIRENT_ROOT_IS_READ_ONLY()) {
        errno = EIO;
        goto out;
    }
    // Put the new mdirentry
    if (put_mdirentry(plv2->container.mdir.fdp, newparent, newname, target->attributes.fid, target->attributes.mode) != 0)
        goto out;

    // Update nlink and ctime for inode
    target->attributes.nlink++;
    target->attributes.ctime = time(NULL);

    // Write attributes of target
    if (export_lv2_write_attributes(target) != 0)
        goto out;

    // Update parent
    plv2->attributes.children++;
    plv2->attributes.mtime = plv2->attributes.ctime = time(NULL);

    // Write attributes of parents
    if (export_lv2_write_attributes(plv2) != 0)
        goto out;

    // Return attributes
    memcpy(attrs, &target->attributes, sizeof (mattr_t));
    /*
    ** return the parent attributes
    */
    memcpy(pattrs, &plv2->attributes, sizeof (mattr_t));
    status = 0;

out:
    STOP_PROFILING(export_link);
    return status;
}
/** create a new file
 *
 * @param e: the export managing the file
 * @param pfid: the id of the parent
 * @param name: the name of this file.
 * @param uid: the user id
 * @param gid: the group id
 * @param mode: mode of this file
 * @param[out] attrs: mattr_t to fill (child attributes used by upper level functions)
 * @param[out] pattrs: mattr_t to fill (parent attributes)
  
 * @return: 0 on success -1 otherwise (errno is set)
 */
int export_mknod(export_t *e, fid_t pfid, char *name, uint32_t uid,
        uint32_t gid, mode_t mode, mattr_t *attrs,mattr_t *pattrs) {
    int status = -1;
    lv2_entry_t *plv2;
    fid_t node_fid;
    char node_path[PATH_MAX];
    mreg_t node_mreg;
    int xerrno = errno;
    uint32_t type;

    START_PROFILING(export_mknod);

    // get the lv2 parent
    if (!(plv2 = export_lookup_fid(e, pfid)))
        goto error;

    // check if exists
    if (get_mdirentry(plv2->container.mdir.fdp, pfid, name, node_fid, &type) == 0) {
        errno = EEXIST;
        goto error;
    }
    /*
     ** nothing has been found, need to check the read only flag:
     ** that flag is asserted if some parts of dirent files are unreadable 
     */
    if (DIRENT_ROOT_IS_READ_ONLY()) {
        xerrno = EIO;
        goto error_read_only;
    }

    if (!S_ISREG(mode)) {
        errno = ENOTSUP;
        goto error;
    }

    
    /*
    ** Check that some space os left for the new file in case a hard quota is set
    */
    if (e->hquota) {
      if (e->fstat.blocks >= e->hquota) {
        errno = ENOSPC;
       goto error;
      }
    }


    // create the lv2 new file
    uuid_generate(node_fid);
    if (export_lv2_resolve_path(e, node_fid, node_path) != 0)
        goto error;

    if (mknod(node_path, S_IRWXU, 0) != 0)
        goto error;

    // generate attributes
    uuid_copy(attrs->fid, node_fid);
    if (volume_distribute(e->volume, &attrs->cid, attrs->sids) != 0)
        goto error;
    attrs->mode = mode;
    attrs->uid = uid;
    attrs->gid = gid;
    attrs->nlink = 1;
    if ((attrs->ctime = attrs->atime = attrs->mtime = time(NULL)) == -1)
        goto error;
    attrs->size = 0;

    // write attributes to mreg file
    if (mreg_open(&node_mreg, node_path) < 0)
        goto error;
    if (mreg_write_attributes(&node_mreg, attrs) != 0) {
        mreg_close(&node_mreg);
        goto error;
    }
    mreg_close(&node_mreg);

    // update the parent
    // add the new child to the parent
    if (put_mdirentry(plv2->container.mdir.fdp, pfid, name, node_fid, attrs->mode) != 0) {
        goto error;
    }

    // Update children nb. and times of parent
    plv2->attributes.children++;
    plv2->attributes.mtime = plv2->attributes.ctime = time(NULL);
    if (export_lv2_write_attributes(plv2) != 0) {
        goto error;
    }

    // update export files
    if (export_update_files(e, 1) != 0)
        goto error;

    status = 0;
    /*
    ** return the parent attributes
    */
    memcpy(pattrs, &plv2->attributes, sizeof (mattr_t));
    goto out;

error:
    xerrno = errno;
    if (xerrno != EEXIST) {
        unlink(node_path);
    }
error_read_only:
    errno = xerrno;

out:
    STOP_PROFILING(export_mknod);
    return status;
}
/** create a new directory
 *
 * @param e: the export managing the file
 * @param pfid: the id of the parent
 * @param name: the name of this file.
 * @param uid: the user id
 * @param gid: the group id
 * @param mode: mode of this file
 * @param[out] attrs: mattr_t to fill (child attributes used by upper level functions)
 * @param[out] pattrs: mattr_t to fill (parent attributes)
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int export_mkdir(export_t *e, fid_t pfid, char *name, uint32_t uid,
        uint32_t gid, mode_t mode, mattr_t * attrs,mattr_t * pattrs) {
    int status = -1;
    lv2_entry_t *plv2;
    fid_t node_fid;
    char node_path[PATH_MAX];
    mdir_t node_mdir;
    int xerrno = errno;

    START_PROFILING(export_mkdir);

    // get the lv2 parent
    if (!(plv2 = export_lookup_fid(e, pfid)))
        goto error;

    // check if exists
    if (get_mdirentry(plv2->container.mdir.fdp, pfid, name, node_fid, &attrs->mode) == 0) {
        errno = EEXIST;
        goto error;
    }

    /*
     ** nothing has been found, need to check the read only flag:
     ** that flag is asserted if some parts of dirent files are unreadable 
     */
    if (DIRENT_ROOT_IS_READ_ONLY()) {
        xerrno = EIO;
        goto error_read_only;
    }
    
    
    /*
    ** Check that some space is left for the new file in case a hard quota is set
    */
    if (e->hquota) {
      if (e->fstat.blocks >= e->hquota) {
        errno = ENOSPC;
       goto error;
      }
    }    
    
    // create the lv2 new file
    uuid_generate(node_fid);
    if (export_lv2_resolve_path(e, node_fid, node_path) != 0)
        goto error;

    if (mkdir(node_path, S_IRWXU) != 0)
        goto error;

    // generate attributes
    uuid_copy(attrs->fid, node_fid);
    attrs->cid = 0;
    memset(attrs->sids, 0, ROZOFS_SAFE_MAX * sizeof (sid_t));
    attrs->mode = mode;
    attrs->uid = uid;
    attrs->gid = gid;
    attrs->nlink = 2;
    if ((attrs->ctime = attrs->atime = attrs->mtime = time(NULL)) == -1)
        goto error;
    attrs->size = ROZOFS_DIR_SIZE;
    attrs->children = 0;

    // write attributes to mdir file
    if (mdir_open(&node_mdir, node_path) < 0)
        goto error;
    if (mdir_write_attributes(&node_mdir, attrs) != 0) {
        mdir_close(&node_mdir);
        goto error;
    }

    // create "." ".." lv3 entries
    if (put_mdirentry(node_mdir.fdp, node_fid, ".", node_fid, S_IFDIR | S_IRWXU) != 0) {
        mdir_close(&node_mdir);
        return -1;
    }
    if (put_mdirentry(node_mdir.fdp, node_fid, "..", plv2->attributes.fid, S_IFDIR | S_IRWXU) != 0) {
        mdir_close(&node_mdir);
        return -1;
    }

    // update the parent
    // add the new child to the parent
    if (put_mdirentry(plv2->container.mdir.fdp, pfid, name, node_fid, attrs->mode) != 0) {
        goto error;
    }

    plv2->attributes.children++;
    plv2->attributes.nlink++;
    plv2->attributes.mtime = plv2->attributes.ctime = time(NULL);
    if (export_lv2_write_attributes(plv2) != 0)
        goto error;

    // update export files
    if (export_update_files(e, 1) != 0)
        goto error;

    mdir_close(&node_mdir);
    status = 0;
    /*
    ** return the parent attributes
    */
    memcpy(pattrs, &plv2->attributes, sizeof (mattr_t));
    goto out;

error:
    xerrno = errno;
    if (xerrno != EEXIST) {
        char fname[PATH_MAX];
        mdir_t node_mdir_del;
        // XXX: put version
        fid_t fid;
        uint32_t type;
        sprintf(fname, "%s/%s", node_path, MDIR_ATTRS_FNAME);
        unlink(fname);
        node_mdir_del.fdp = open(node_path, O_RDONLY, S_IRWXU);
        del_mdirentry(node_mdir_del.fdp, node_fid, ".", fid, &type);
        del_mdirentry(node_mdir_del.fdp, node_fid, "..", fid, &type);
        rmdir(node_path);
        mdir_close(&node_mdir_del);
    }
error_read_only:
    errno = xerrno;

out:
    STOP_PROFILING(export_mkdir);
    return status;
}
/** remove a file
 *
 * @param e: the export managing the file
 * @param pfid: the id of the parent
 * @param name: the name of this file.
 * @param[out] fid: the fid of the removed file
 * @param[out] pattrs: mattr_t to fill (parent attributes)
 * 
 * @return: 0 on success -1 otherwise (errno is set)
 */
int export_unlink(export_t * e, fid_t parent, char *name, fid_t fid,mattr_t * pattrs) {
    int status = -1;
    lv2_entry_t *plv2, *lv2;
    fid_t child_fid;
    uint32_t child_type;
    uint16_t nlink = 0;
    char child_path[PATH_MAX];

    START_PROFILING(export_unlink);

    // Get the lv2 parent
    if (!(plv2 = export_lookup_fid(e, parent)))
        goto out;

    // Check if name exist
    if (get_mdirentry(plv2->container.mdir.fdp, parent, name, child_fid, &child_type) != 0)
        goto out;

    if (S_ISDIR(child_type)) {
        errno = EISDIR;
        goto out;
    }

    // Delete the mdirentry if exist
    if (del_mdirentry(plv2->container.mdir.fdp, parent, name, child_fid, &child_type) != 0)
        goto out;

    // Resolve path for the file to delete
    if (export_lv2_resolve_path(e, child_fid, child_path) != 0)
        goto out;

    // Get mattrs of child to delete
    if (!(lv2 = export_lookup_fid(e, child_fid)))
        goto out;

    // Return the fid of deleted file
    memcpy(fid, child_fid, sizeof (fid_t));


    // Get nlink
    nlink = lv2->attributes.nlink;

    // 2 cases:
    // nlink > 1, it's a hardlink -> not delete the lv2 file
    // nlink=1, it's not a harlink -> put the lv2 file on trash directory

    // Not a hardlink
    if (nlink == 1) {

        if (S_ISREG(lv2->attributes.mode)) {

            char trash_file_path[PATH_MAX];
            char trash_bucket_path[PATH_MAX];

            // Compute hash value for this fid
            uint32_t hash = 0;
            uint8_t *c = 0;
            for (c = lv2->attributes.fid; c != lv2->attributes.fid + 16; c++)
                hash = *c + (hash << 6) + (hash << 16) - hash;
            hash %= RM_MAX_BUCKETS;

            // Build trash_bucket_path
            sprintf(trash_bucket_path, "%s/%s/%"PRIu32"",
                    e->root, TRASH_DNAME, hash);

            // Check existence of trash bucket directory
            if (access(trash_bucket_path, F_OK) == -1) {
                if (errno == ENOENT) {
                    if (mkdir(trash_bucket_path,
                            S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
                        severe("mkdir for trash bucket (%s) failed: %s",
                                trash_bucket_path, strerror(errno));
                        goto out;
                    }
                } else {
                    severe("access for trash bucket (%s) failed: %s",
                            trash_bucket_path, strerror(errno));
                    goto out;
                }
            }

            // Unparse fid
            char fid_str[37];
            uuid_unparse(lv2->attributes.fid, fid_str);

            // Build trash path for the file
            sprintf(trash_file_path, "%s/%s", trash_bucket_path, fid_str);

            // Move the file in trash directoory
            if (rename(child_path, trash_file_path) == -1) {
                severe("rename for trash (%s to %s) failed: %s",
                        child_path, trash_file_path, strerror(errno));
                // Best effort
            }

            // Preparation of the rmfentry
            rmfentry_t *rmfe = xmalloc(sizeof (rmfentry_t));
            memcpy(rmfe->fid, lv2->attributes.fid, sizeof (fid_t));
            rmfe->cid = lv2->attributes.cid;
            memcpy(rmfe->initial_dist_set, lv2->attributes.sids,
                    sizeof (sid_t) * ROZOFS_SAFE_MAX);
            memcpy(rmfe->current_dist_set, lv2->attributes.sids,
                    sizeof (sid_t) * ROZOFS_SAFE_MAX);
            list_init(&rmfe->list);

            // Acquire lock on bucket trash list
            if ((errno = pthread_rwlock_wrlock
                    (&e->trash_buckets[hash].rm_lock)) != 0) {
                severe("pthread_rwlock_wrlock failed: %s", strerror(errno));
                // Best effort
            }

            // Check size of file 
            if (lv2->attributes.size >= RM_FILE_SIZE_TRESHOLD) {
                // Add to front of list
                list_push_front(&e->trash_buckets[hash].rmfiles, &rmfe->list);
            } else {
                // Add to back of list
                list_push_back(&e->trash_buckets[hash].rmfiles, &rmfe->list);
            }

            if ((errno = pthread_rwlock_unlock
                    (&e->trash_buckets[hash].rm_lock)) != 0) {
                severe("pthread_rwlock_unlock failed: %s", strerror(errno));
                // Best effort
            }

            // Update the nb. of blocks
            if (export_update_blocks(e,
                    -(((int64_t) lv2->attributes.size + ROZOFS_BSIZE - 1)
                    / ROZOFS_BSIZE)) != 0) {
                severe("export_update_blocks failed: %s", strerror(errno));
                // Best effort
            }
        } else {
            // file empty
            if (unlink(child_path) != 0) {
                severe("unlink failed (%s): %s", child_path, strerror(errno));
                // Best effort
            }
        }
       // Update export files
        if (export_update_files(e, -1) != 0)
            goto out;

        // Remove from the cache (will be closed and freed)
        lv2_cache_del(e->lv2_cache, child_fid);
    }
    // It's a hardlink
    if (nlink > 1) {
        lv2->attributes.nlink--;
        lv2->attributes.ctime = time(NULL);
        export_lv2_write_attributes(lv2);
        // Return a empty fid because no inode has been deleted
        //memset(fid, 0, sizeof (fid_t));
    }

    // Update parent
    plv2->attributes.mtime = plv2->attributes.ctime = time(NULL);
    plv2->attributes.children--;

    // Write attributes of parents
    if (export_lv2_write_attributes(plv2) != 0)
        goto out;
    /*
    ** return the parent attributes
    */
    memcpy(pattrs, &plv2->attributes, sizeof (mattr_t));
    status = 0;

out:
    STOP_PROFILING(export_unlink);
    return status;
}
/*
**______________________________________________________________________________
*/
static int init_storages_cnx(volume_t *volume, list_t *list) {
    list_t *p, *q;
    int status = -1;
    DEBUG_FUNCTION;

    if ((errno = pthread_rwlock_rdlock(&volume->lock)) != 0) {
        severe("pthread_rwlock_rdlock failed (vid: %d): %s", volume->vid,
                strerror(errno));
        goto out;
    }

    list_for_each_forward(p, &volume->clusters) {

        cluster_t *cluster = list_entry(p, cluster_t, list);

        list_for_each_forward(q, &cluster->storages) {

            volume_storage_t *vs = list_entry(q, volume_storage_t, list);

            mclient_t * mclt = (mclient_t *) xmalloc(sizeof (mclient_t));

            strncpy(mclt->host, vs->host, ROZOFS_HOSTNAME_MAX);
            mclt->cid = cluster->cid;
            mclt->sid = vs->sid;
            struct timeval timeo;
            timeo.tv_sec = ROZOFS_MPROTO_TIMEOUT_SEC;
            timeo.tv_usec = 0;
	    
	    init_rpcctl_ctx(&mclt->rpcclt);

	    init_rpcctl_ctx(&mclt->rpcclt);

            if (mclient_initialize(mclt, timeo) != 0) {
                warning("failed to join: %s,  %s", vs->host, strerror(errno));
            }

            cnxentry_t *cnx_entry = (cnxentry_t *) xmalloc(sizeof (cnxentry_t));
            cnx_entry->cnx = mclt;

            // Add to the list
            list_push_back(list, &cnx_entry->list);

        }
    }

    if ((errno = pthread_rwlock_unlock(&volume->lock)) != 0) {
        severe("pthread_rwlock_unlock failed (vid: %d): %s", volume->vid,
                strerror(errno));
        goto out;
    }

    status = 0;
out:

    return status;
}
/*
**______________________________________________________________________________
*/
static mclient_t * lookup_cnx(list_t *list, cid_t cid, sid_t sid) {

    list_t *p;
    DEBUG_FUNCTION;

    list_for_each_forward(p, list) {
        cnxentry_t *cnx_entry = list_entry(p, cnxentry_t, list);

        if ((sid == cnx_entry->cnx->sid) && (cid == cnx_entry->cnx->cid)) {
            return cnx_entry->cnx;
            break;
        }
    }

    severe("lookup_cnx failed: storage connexion (cid: %u; sid: %u) not found",
            cid, sid);

    errno = EINVAL;

    return NULL;
}
/*
**______________________________________________________________________________
*/
static void release_storages_cnx(list_t *list) {

    list_t *p, *q;
    DEBUG_FUNCTION;

    list_for_each_forward_safe(p, q, list) {

        cnxentry_t *cnx_entry = list_entry(p, cnxentry_t, list);
        mclient_release(cnx_entry->cnx);
        if (cnx_entry->cnx != NULL)
            free(cnx_entry->cnx);
        list_remove(p);
        if (cnx_entry != NULL)
            free(cnx_entry);
    }
}
/*
**______________________________________________________________________________
*/
int export_rm_bins(export_t * e, uint16_t * first_bucket_idx) {
    int status = -1;
    int rm_bins_file_nb = 0;
    int i = 0;
    uint16_t idx = 0;
    uint16_t bucket_idx = 0;
    uint8_t cnx_init = 0;
    int limit_rm_files = RM_FILES_MAX;
    int curr_rm_files = 0;
    uint8_t rozofs_safe = 0;
    list_t connexions;

    DEBUG_FUNCTION;

    // Get the nb. of safe storages for this layout
    rozofs_safe = rozofs_get_rozofs_safe(e->layout);

    // For each trash bucket (MAX_RM_BUCKETS buckets)
    // Begin with trash bucket idx = *first_bucket_idx
    for (idx = *first_bucket_idx; idx < (*first_bucket_idx + RM_MAX_BUCKETS);
            idx++) {

        // Compute valid trash bucket idx
        bucket_idx = idx % RM_MAX_BUCKETS;

        // Check if the bucket is empty
        if (list_empty(&e->trash_buckets[bucket_idx].rmfiles))
            continue; // Try with the next bucket

        // If the connexions are not initialized
        if (cnx_init == 0) {
            // Init list of connexions
            list_init(&connexions);
            cnx_init = 1;
            if (init_storages_cnx(e->volume, &connexions) != 0) {
                // Problem with lock
                severe("init_storages_cnx failed: %s", strerror(errno));
                goto out;
            }
        }

        // Acquire lock on this list
        if ((errno = pthread_rwlock_wrlock
                (&e->trash_buckets[bucket_idx].rm_lock)) != 0) {
            severe("pthread_rwlock_wrlock failed: %s", strerror(errno));
            continue; // Best effort
        }

        // Remove rmfentry_t from the list of files to remove for this bucket
        rmfentry_t *entry = list_first_entry(
                &e->trash_buckets[bucket_idx].rmfiles, rmfentry_t, list);
        list_remove(&entry->list);

        if ((errno = pthread_rwlock_unlock(&e->trash_buckets[bucket_idx].rm_lock)) != 0) {
            severe("pthread_rwlock_unlock failed: %s", strerror(errno));
            continue;
        }

        // Nb. of bins files removed for this file
        rm_bins_file_nb = 0;

        // For each storage associated with this file
        for (i = 0; i < rozofs_safe; i++) {

            mclient_t* stor = NULL;

            if (0 == entry->current_dist_set[i]) {
                // The bins file has already been deleted for this server
                rm_bins_file_nb++;
                continue; // Go to the next storage
            }

            if ((stor = lookup_cnx(&connexions, entry->cid,
                    entry->current_dist_set[i])) == NULL) {
                // lookup_cnx failed !!! 
                continue; // Go to the next storage
            }

            if (0 == stor->status) {
                // This storage is down
                // it's not necessary to send a request
                continue; // Go to the next storage
            }

            // Send remove request
            if (mclient_remove(stor, e->layout, entry->initial_dist_set,
                    entry->fid) != 0) {
                // Problem with request
                warning("mclient_remove failed (cid: %u; sid: %u): %s",
                        stor->cid, stor->sid, strerror(errno));
                continue; // Go to the next storage
            }

            // The bins file has been deleted successfully
            // Update distribution and nb. of bins file deleted
            entry->current_dist_set[i] = 0;
            rm_bins_file_nb++;
        }

        // If all bins files are deleted
        // Remove the file from trash
        if (rm_bins_file_nb == rozofs_safe) {
            char path[PATH_MAX];

            char fid_str[37];
            uuid_unparse(entry->fid, fid_str);

            // Build path file
            sprintf(path, "%s/%s/%"PRIu32"/%s",
                    e->root, TRASH_DNAME, bucket_idx, fid_str);

            // Free entry
            if (entry != NULL)
                free(entry);

            // Unlink file
            if (unlink(path) == -1) {
                // Under certain circumstances it is possible that the
                // file is already deleted
                if (errno != ENOENT) {
                    severe("unlink failed (%s): %s", path, strerror(errno));
                    // Best effort
                }
            }

        } else { // If NO all bins are deleted

            if ((errno = pthread_rwlock_wrlock
                    (&e->trash_buckets[bucket_idx].rm_lock)) != 0) {
                severe("pthread_rwlock_wrlock failed: %s", strerror(errno));
                continue; // Best effort
            }

            // Repush back entry in the list of files to delete
            list_push_back(&e->trash_buckets[bucket_idx].rmfiles, &entry->list);

            if ((errno = pthread_rwlock_unlock
                    (&e->trash_buckets[bucket_idx].rm_lock)) != 0) {
                severe("pthread_rwlock_unlock failed: %s", strerror(errno));
            }
        }
        // Update the nb. of files that have been tested to be deleted.
        curr_rm_files++;

        // Check if enough files are removed
        if (curr_rm_files >= limit_rm_files)
            break; // Exit from the loop
    }

    // Update the first bucket index to use for the next call
    if (0 == curr_rm_files) {
        // If no files removed 
        // The next first bucket index will be 0
        // not necessary but better for debug
        *first_bucket_idx = 0;
    } else {
        *first_bucket_idx = (bucket_idx + 1) % RM_MAX_BUCKETS;
    }

    status = 0;
out:
    if (cnx_init == 1) {
        // Release storage connexions
        release_storages_cnx(&connexions);
    }
    return status;
}
/*
**______________________________________________________________________________
*/
/**
*   exportd rmdir: delete a directory

    @param pfid : fid of the parent and directory  name 
    @param name : fid of the parent and directory  name 
    
    @param[out] fid:  fid of the deleted directory 
    @param[out] pattrs:  attributes of the parent 
    
    @retval: 0 : success
    @retval: <0 error see errno
*/
int export_rmdir(export_t *e, fid_t pfid, char *name, fid_t fid,mattr_t * pattrs) {
    int status = -1;
    lv2_entry_t *plv2;
    lv2_entry_t *lv2;
    fid_t fake_fid;
    fid_t dot_fid;
    fid_t dot_dot_fid;
    uint32_t fake_type;
    uint32_t dot_type;
    uint32_t dot_dot_type;
    char lv2_path[PATH_MAX];
    char lv3_path[PATH_MAX];

    START_PROFILING(export_rmdir);

    // get the lv2 parent
    if (!(plv2 = export_lookup_fid(e, pfid)))
        goto out;

    // get the fid according to name
    if (get_mdirentry(plv2->container.mdir.fdp, pfid, name, fid, &fake_type) != 0)
        goto out;

    // get the lv2
    if (!(lv2 = export_lookup_fid(e, fid)))
        goto out;

    // sanity checks (is a directory and lv3 is empty)
    if (!S_ISDIR(lv2->attributes.mode)) {
        errno = ENOTDIR;
        goto out;
    }

    if (lv2->attributes.children != 0) {
        errno = ENOTEMPTY;
        goto out;
    }

    // remove lv2
    if (export_lv2_resolve_path(e, fid, lv2_path) != 0)
        goto out;
    /*
     ** once the attributes file has been removed 
     ** consider that the directory is deleted, all the remaining is best effort
     */
    sprintf(lv3_path, "%s/%s", lv2_path, MDIR_ATTRS_FNAME);

    if (unlink(lv3_path) != 0) {
        if (errno != ENOENT) goto out;
    }


    // XXX starting from here, any failure will leads to inconsistent state: best effort mode
    del_mdirentry(lv2->container.mdir.fdp, fid, ".", dot_fid, &dot_type);
    del_mdirentry(lv2->container.mdir.fdp, fid, "..", dot_dot_fid, &dot_dot_type);

    // remove from the cache (will be closed and freed)
    lv2_cache_del(e->lv2_cache, fid);
    /*
     ** rmdir is best effort since it might possible that some dirent file with empty entries remain
     */
    rmdir(lv2_path);


    // update parent:
    /*
     ** attributes of the parent must be updated first otherwise we can afce the situation where
     ** parent directory cannot be removed because the number of children is not 0
     */
    if (plv2->attributes.children > 0) plv2->attributes.children--;
    plv2->attributes.nlink--;
    plv2->attributes.mtime = plv2->attributes.ctime = time(NULL);
    if (export_lv2_write_attributes(plv2) != 0)
        goto out;

    // update export nb files: best effort mode
    export_update_files(e, -1);

    /*
     ** remove the entry from the parent directory: best effort
     */
    del_mdirentry(plv2->container.mdir.fdp, pfid, name, fake_fid, &fake_type);
    /*
    ** return the parent attributes
    */
    memcpy(pattrs, &plv2->attributes, sizeof (mattr_t));
    status = 0;
out:
    STOP_PROFILING(export_rmdir);

    return status;
}
/*
**______________________________________________________________________________
*/
/** create a symlink
 *
 * @param e: the export managing the file
 * @param link: target name
 * @param pfid: the id of the parent
 * @param name: the name of the file to link.
 * @param[out] attrs: mattr_t to fill (child attributes used by upper level functions)
 * @param[out] pattrs: mattr_t to fill (parent attributes)
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int export_symlink(export_t * e, char *link, fid_t pfid, char *name,
        mattr_t * attrs,mattr_t *pattrs) {

    int status = -1;
    lv2_entry_t *plv2;
    fid_t node_fid;
    char node_path[PATH_MAX];
    mslnk_t node_mslnk;
    int xerrno = errno;

    START_PROFILING(export_symlink);

    // get the lv2 parent
    if (!(plv2 = export_lookup_fid(e, pfid)))
        goto error;

    // check if exists
    if (get_mdirentry(plv2->container.mdir.fdp, pfid, name, node_fid, &attrs->mode) == 0) {
        errno = EEXIST;
        goto error;
    }
    /*
     ** nothing has been found, need to check the read only flag:
     ** that flag is asserted if some parts of dirent files are unreadable 
     */
    if (DIRENT_ROOT_IS_READ_ONLY()) {
        xerrno = EIO;
        goto error_read_only;
    }

    // create the lv2 new file
    uuid_generate(node_fid);
    if (export_lv2_resolve_path(e, node_fid, node_path) != 0)
        goto error;

    if (mknod(node_path, S_IRWXU, 0) != 0)
        goto error;

    // generate attributes
    uuid_copy(attrs->fid, node_fid);
    attrs->cid = 0;
    memset(attrs->sids, 0, ROZOFS_SAFE_MAX * sizeof (sid_t));
    attrs->mode = S_IFLNK | S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP |
            S_IXGRP | S_IROTH | S_IWOTH | S_IXOTH;
    attrs->uid = getuid();
    attrs->gid = getgid();
    attrs->nlink = 1;
    if ((attrs->ctime = attrs->atime = attrs->mtime = time(NULL)) == -1)
        goto error;
    attrs->size = strlen(link);

    // write attributes and link
    if (mslnk_open(&node_mslnk, node_path) < 0)
        goto error;
    if (mslnk_write_attributes(&node_mslnk, attrs) != 0) {
        mslnk_close(&node_mslnk);
        goto error;
    }
    if (mslnk_write_link(&node_mslnk, link) != 0) {
        mslnk_close(&node_mslnk);
        goto error;
    }

    mslnk_close(&node_mslnk);

    // update the parent
    // add the new child to the parent
    if (put_mdirentry(plv2->container.mdir.fdp, pfid, name, node_fid, attrs->mode) != 0)
        goto error;
    plv2->attributes.children++;
    // update times of parent
    plv2->attributes.mtime = plv2->attributes.ctime = time(NULL);
    if (export_lv2_write_attributes(plv2) != 0)
        goto error;

    // update export files
    if (export_update_files(e, 1) != 0)
        goto error;

    status = 0;
    /*
    ** return the parent attributes
    */
    memcpy(pattrs, &plv2->attributes, sizeof (mattr_t));
    goto out;

error:
    xerrno = errno;
    if (xerrno != EEXIST) {
        unlink(node_path);
    }
error_read_only:
    errno = xerrno;

out:
    STOP_PROFILING(export_symlink);

    return status;
}
/*
**______________________________________________________________________________
*/
/** read a symbolic link
 *
 * @param e: the export managing the file
 * @param fid: file id
 * @param link: link to fill
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int export_readlink(export_t *e, fid_t fid, char *link) {
    int status = -1;
    lv2_entry_t *lv2 = 0;
    START_PROFILING(export_readlink);

    if (!(lv2 = export_lookup_fid(e, fid))) {
        goto out;
    }
    status = mslnk_read_link(&lv2->container.mslnk, link);
out:
    STOP_PROFILING(export_readlink);

    return status;
}
/*
**______________________________________________________________________________
*/
/** rename (move) a file
 *
 * @param e: the export managing the file
 * @param pfid: parent file id
 * @param name: file name
 * @param npfid: target parent file id
 * @param newname: target file name
 * @param fid: file id
 * @param attrs: attributes of the renamed file
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int export_rename(export_t *e, fid_t pfid, char *name, fid_t npfid,
        char *newname, fid_t fid,
	mattr_t * attrs) {
    int status = -1;
    lv2_entry_t *lv2_old_parent = 0;
    lv2_entry_t *lv2_new_parent = 0;
    lv2_entry_t *lv2_to_rename = 0;
    lv2_entry_t *lv2_to_replace = 0;
    fid_t fid_to_rename;
    uint32_t type_to_rename;
    fid_t fid_to_replace;
    uint32_t type_to_replace;

    START_PROFILING(export_rename);

    // Get the lv2 entry of old parent
    if (!(lv2_old_parent = export_lookup_fid(e, pfid))) {
        goto out;
    }

    // Verify that the old parent is a directory
    if (!S_ISDIR(lv2_old_parent->attributes.mode)) {
        errno = ENOTDIR;
        goto out;
    }

    // Check if the file/dir to rename exist
    if (get_mdirentry(lv2_old_parent->container.mdir.fdp, pfid, name, fid_to_rename, &type_to_rename) != 0)
        goto out;

    // Get the lv2 entry of file/dir to rename
    if (!(lv2_to_rename = export_lookup_fid(e, fid_to_rename)))
        goto out;

    // Get the lv2 entry of newparent
    if (!(lv2_new_parent = export_lookup_fid(e, npfid)))
        goto out;

    // Verify that the new parent is a directory
    if (!S_ISDIR(lv2_new_parent->attributes.mode)) {
        errno = ENOTDIR;
        goto out;
    }

    memset(fid, 0, sizeof (fid_t));

    // Get the old mdirentry if exist
    if (get_mdirentry(lv2_new_parent->container.mdir.fdp, npfid, newname, fid_to_replace, &type_to_replace) == 0) {

        // We must delete the old entry

        // Get mattrs of entry to delete
        if (!(lv2_to_replace = export_lookup_fid(e, fid_to_replace)))
            goto out;

        // The entry (to replace) is an existing directory
        if (S_ISDIR(lv2_to_replace->attributes.mode)) {

            // The entry to rename must be a directory
            if (!S_ISDIR(lv2_to_rename->attributes.mode)) {
                errno = EISDIR;
                goto out;
            }

            // The entry to replace must be a empty directory
            if (lv2_to_replace->attributes.children != 0) {
                errno = ENOTEMPTY;
                goto out;
            }

            // Delete mdirentry . and .. of dir to replace
            fid_t dot_fid, dot_dot_fid;
            uint32_t dot_type, dot_dot_type;

            if (del_mdirentry(lv2_to_replace->container.mdir.fdp, fid_to_replace, ".", dot_fid, &dot_type) != 0)
                goto out;
            if (del_mdirentry(lv2_to_replace->container.mdir.fdp, fid_to_replace, "..", dot_dot_fid, &dot_dot_type) != 0)
                goto out;

            // Update parent directory
            lv2_new_parent->attributes.nlink--;
            lv2_new_parent->attributes.children--;

            // We'll write attributes of parents after

            // Update export files
            if (export_update_files(e, -1) != 0)
                goto out;

            char lv2_path[PATH_MAX];
            char lv3_path[PATH_MAX];

            if (export_lv2_resolve_path(e, lv2_to_replace->attributes.fid, lv2_path) != 0)
                goto out;

            sprintf(lv3_path, "%s/%s", lv2_path, MDIR_ATTRS_FNAME);

            if (unlink(lv3_path) != 0)
                goto out;

            if (rmdir(lv2_path) != 0)
                goto out;

            // Remove the dir to replace from the cache (will be closed and freed)
            lv2_cache_del(e->lv2_cache, fid_to_replace);

            // Return the fid of deleted directory
            memcpy(fid, fid_to_replace, sizeof (fid_t));

        } else {
            // The entry (to replace) is an existing file
            if (S_ISREG(lv2_to_replace->attributes.mode) || S_ISLNK(lv2_to_replace->attributes.mode)) {

                // Get nlink
                uint16_t nlink = lv2_to_replace->attributes.nlink;

                // 2 cases:
                // nlink > 1, it's a hardlink -> not delete the lv2 file
                // nlink=1, it's not a harlink -> put the lv2 file on trash
                // directory

                // Not a hardlink
                if (nlink == 1) {

                    char old_path[PATH_MAX];

                    // Resolve path for the node to delete
                    if (export_lv2_resolve_path(e, fid_to_replace,
                            old_path) != 0)
                        goto out;

                    // Check if it's a regular file not empty 
                    if (lv2_to_replace->attributes.size > 0 &&
                            S_ISREG(lv2_to_replace->attributes.mode)) {

                        char trash_file_path[PATH_MAX];
                        char trash_bucket_path[PATH_MAX];

                        // Compute hash value for this fid
                        uint32_t hash = 0;
                        uint8_t *c = 0;
                        for (c = lv2_to_replace->attributes.fid;
                                c != lv2_to_replace->attributes.fid + 16; c++)
                            hash = *c + (hash << 6) + (hash << 16) - hash;
                        hash %= RM_MAX_BUCKETS;

                        // Build trash_bucket_path
                        sprintf(trash_bucket_path, "%s/%s/%"PRIu32"",
                                e->root, TRASH_DNAME, hash);

                        // Check existence of trash bucket directory
                        if (access(trash_bucket_path, F_OK) == -1) {
                            if (errno == ENOENT) {
                                if (mkdir(trash_bucket_path,
                                        S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
                                    severe("mkdir for trash bucket (%s) failed:"
                                            " %s",
                                            trash_bucket_path, strerror(errno));
                                    goto out;
                                }
                            } else {
                                severe("access for trash bucket (%s) failed:"
                                        " %s",
                                        trash_bucket_path, strerror(errno));
                                goto out;
                            }
                        }

                        // Unparse fid
                        char fid_str[37];
                        uuid_unparse(lv2_to_replace->attributes.fid, fid_str);

                        // Build path for the trash file
                        sprintf(trash_file_path, "%s/%s", trash_bucket_path,
                                fid_str);

                        // Move the file in trash directoory
                        if (rename(old_path, trash_file_path) == -1) {
                            severe("rename for trash (%s to %s) failed: %s",
                                    old_path, trash_file_path, strerror(errno));
                            // Best effort
                        }

                        // Preparation of the rmfentry
                        rmfentry_t *rmfe = xmalloc(sizeof (rmfentry_t));
                        memcpy(rmfe->fid, lv2_to_replace->attributes.fid,
                                sizeof (fid_t));
                        rmfe->cid = lv2_to_replace->attributes.cid;
                        memcpy(rmfe->initial_dist_set,
                                lv2_to_replace->attributes.sids,
                                sizeof (sid_t) * ROZOFS_SAFE_MAX);
                        memcpy(rmfe->current_dist_set,
                                lv2_to_replace->attributes.sids,
                                sizeof (sid_t) * ROZOFS_SAFE_MAX);
                        list_init(&rmfe->list);

                        // Acquire lock on bucket trash list
                        if ((errno = pthread_rwlock_wrlock
                                (&e->trash_buckets[hash].rm_lock)) != 0) {
                            severe("pthread_rwlock_wrlock failed: %s",
                                    strerror(errno));
                            // Best effort
                        }

                        // Check size of file 
                        if (lv2_to_replace->attributes.size
                                >= RM_FILE_SIZE_TRESHOLD) {
                            // Add to front of list
                            list_push_front(&e->trash_buckets[hash].rmfiles,
                                    &rmfe->list);
                        } else {
                            // Add to back of list
                            list_push_back(&e->trash_buckets[hash].rmfiles,
                                    &rmfe->list);
                        }

                        if ((errno = pthread_rwlock_unlock
                                (&e->trash_buckets[hash].rm_lock)) != 0) {
                            severe("pthread_rwlock_unlock failed: %s",
                                    strerror(errno));
                            // Best effort
                        }

                        // Update the nb. of blocks
                        if (export_update_blocks(e,
                                -(((int64_t) lv2_to_replace->attributes.size
                                + ROZOFS_BSIZE - 1) / ROZOFS_BSIZE)) != 0) {
                            severe("export_update_blocks failed: %s",
                                    strerror(errno));
                            // Best effort
                        }
                    } else {
                        // file empty
                        if (unlink(old_path) != 0) {
                            severe("unlink failed (%s): %s", old_path,
                                    strerror(errno));
                            // Best effort
                        }
                    }

                    // Update export files
                    if (export_update_files(e, -1) != 0)
                        goto out;

                    // Remove from the cache (will be closed and freed)
                    lv2_cache_del(e->lv2_cache, fid_to_replace);

                    // Return the fid of deleted directory
                    memcpy(fid, fid_to_replace, sizeof (fid_t));
                }

                // It's a hardlink
                if (nlink > 1) {
                    lv2_to_replace->attributes.nlink--;
                    export_lv2_write_attributes(lv2_to_replace);
                    // Return a empty fid because no inode has been deleted
                    memset(fid, 0, sizeof (fid_t));
                }
                lv2_new_parent->attributes.children--;
            }
        }
    } else {
        /*
         ** nothing has been found, need to check the read only flag:
         ** that flag is asserted if some parts of dirent files are unreadable 
         */
        if (DIRENT_ROOT_IS_READ_ONLY()) {
            errno = EIO;
            goto out;
        }
    }

    // Put the mdirentry
    if (put_mdirentry(lv2_new_parent->container.mdir.fdp, npfid, newname, lv2_to_rename->attributes.fid, lv2_to_rename->attributes.mode) != 0) {
        goto out;
    }

    // Delete the mdirentry
    if (del_mdirentry(lv2_old_parent->container.mdir.fdp, pfid, name, fid_to_rename, &type_to_rename) != 0)
        goto out;

    if (memcmp(pfid, npfid, sizeof (fid_t)) != 0) {

        lv2_new_parent->attributes.children++;
        lv2_old_parent->attributes.children--;

        if (S_ISDIR(lv2_to_rename->attributes.mode)) {
            lv2_new_parent->attributes.nlink++;
            lv2_old_parent->attributes.nlink--;

            // If the node to rename is a directory
            // We must change the subdirectory '..'
            if (put_mdirentry(lv2_to_rename->container.mdir.fdp, fid_to_rename, "..", lv2_new_parent->attributes.fid, lv2_new_parent->attributes.mode) != 0) {
                goto out;
            }

        }

        lv2_new_parent->attributes.mtime = lv2_new_parent->attributes.ctime = time(NULL);
        lv2_old_parent->attributes.mtime = lv2_old_parent->attributes.ctime = time(NULL);

        if (export_lv2_write_attributes(lv2_new_parent) != 0)
            goto out;

        if (export_lv2_write_attributes(lv2_old_parent) != 0)
            goto out;
    } else {

        lv2_new_parent->attributes.mtime = lv2_new_parent->attributes.ctime = time(NULL);

        if (export_lv2_write_attributes(lv2_new_parent) != 0)
            goto out;
    }

    // Update ctime of renamed file/directory
    lv2_to_rename->attributes.ctime = time(NULL);

    // Write attributes of renamed file
    if (export_lv2_write_attributes(lv2_to_rename) != 0)
        goto out;

    memcpy(attrs,&lv2_to_rename->attributes,sizeof(mattr_t));
    status = 0;

out:
    STOP_PROFILING(export_rename);

    return status;
}
/*
**______________________________________________________________________________
*/
int64_t export_read(export_t * e, fid_t fid, uint64_t offset, uint32_t len,
        uint64_t * first_blk, uint32_t * nb_blks) {
    lv2_entry_t *lv2 = NULL;
    int64_t length = -1;
    uint64_t i_first_blk = 0;
    uint64_t i_last_blk = 0;
    uint32_t i_nb_blks = 0;

    START_PROFILING(export_read);

    // Get the lv2 entry
    if (!(lv2 = export_lookup_fid(e, fid))) {
        goto error;
    }

    // EOF ?
    if (offset > lv2->attributes.size) {
        errno = 0;
        goto error;
    }

    // Length to read
    length = (offset + len < lv2->attributes.size ? len : lv2->attributes.size - offset);
    // Nb. of the first block to read
    i_first_blk = offset / ROZOFS_BSIZE;
    // Nb. of the last block to read
    i_last_blk = (offset + length) / ROZOFS_BSIZE + ((offset + length) % ROZOFS_BSIZE == 0 ? -1 : 0);
    // Nb. of blocks to read
    i_nb_blks = (i_last_blk - i_first_blk) + 1;

    *first_blk = i_first_blk;
    *nb_blks = i_nb_blks;

    // Managed access time
    if ((lv2->attributes.atime = time(NULL)) == -1)
        goto error;

    // Write attributes of file
    if (export_lv2_write_attributes(lv2) != 0)
        goto error;

    // Return the length that can be read
    goto out;

error:
    length = -1;
out:
    STOP_PROFILING(export_read);

    return length;
}
/*
**______________________________________________________________________________
*/
int export_read_block(export_t *e, fid_t fid, bid_t bid, uint32_t n, dist_t * d) {
    int status = 0;
    lv2_entry_t *lv2 = NULL;

    START_PROFILING(export_read_block);

    // Get the lv2 entry
    if (!(lv2 = export_lookup_fid(e, fid)))
        goto out;

    status = mreg_read_dist(&lv2->container.mreg, bid, n, d);
out:
    STOP_PROFILING(export_read_block);

    return status;
}

/* not used anymore
int64_t export_write(export_t *e, fid_t fid, uint64_t off, uint32_t len) {
    lv2_entry_t *lv2;

    if (!(lv2 = export_lookup_fid(e, fid))) {
        return -1;
    }

    if (off + len > lv2->attributes.size) {
        // Don't skip intermediate computation to keep ceil rounded
        uint64_t nbold = (lv2->attributes.size + ROZOFS_BSIZE - 1) / ROZOFS_BSIZE;
        uint64_t nbnew = (off + len + ROZOFS_BSIZE - 1) / ROZOFS_BSIZE;

        if (export_update_blocks(e, nbnew - nbold) != 0)
            return -1;

        lv2->attributes.size = off + len;
    }

    lv2->attributes.mtime = lv2->attributes.ctime = time(NULL);

    if (export_lv2_write_attributes(lv2) != 0)
        return -1;

    return len;
}*/
/*
**______________________________________________________________________________
*/
/**  update the file size, mtime and ctime
 *
 * dist is the same for all blocks
 *
 * @param e: the export managing the file
 * @param fid: id of the file to read
 * @param bid: first block address (from the start of the file)
 * @param n: number of blocks
 * @param d: distribution to set
 * @param off: offset to write from
 * @param len: length written
 * @param[out] attrs: updated attributes of the file
 *
 * @return: the written length on success or -1 otherwise (errno is set)
 */
int64_t export_write_block(export_t *e, fid_t fid, uint64_t bid, uint32_t n,
        dist_t d, uint64_t off, uint32_t len,mattr_t *attrs) {
    int64_t length = -1;
    lv2_entry_t *lv2 = NULL;

    START_PROFILING(export_write_block);

    // Get the lv2 entry
    if (!(lv2 = export_lookup_fid(e, fid)))
        goto out;


    // Update size of file
    if (off + len > lv2->attributes.size) {
        // Don't skip intermediate computation to keep ceil rounded
        uint64_t nbold = (lv2->attributes.size + ROZOFS_BSIZE - 1) / ROZOFS_BSIZE;
        uint64_t nbnew = (off + len + ROZOFS_BSIZE - 1) / ROZOFS_BSIZE;

        if (export_update_blocks(e, nbnew - nbold) != 0)
            goto out;

        lv2->attributes.size = off + len;
    }

    // Update mtime and ctime
    lv2->attributes.mtime = lv2->attributes.ctime = time(NULL);
    if (export_lv2_write_attributes(lv2) != 0)
        goto out;
    /*
    ** return the parent attributes
    */
    memcpy(attrs, &lv2->attributes, sizeof (mattr_t));
    length = len;

out:
    STOP_PROFILING(export_write_block);

    return length;
}
/*
**______________________________________________________________________________
*/
/** read a directory
 *
 * @param e: the export managing the file
 * @param fid: the id of the directory
 * @param children: pointer to pointer where the first children we will stored
 * @param cookie: index mdirentries where we must begin to list the mdirentries
 * @param eof: pointer that indicates if we list all the entries or not
 *
 * @return: 0 on success -1 otherwise (errno is set)
 */
int export_readdir(export_t * e, fid_t fid, uint64_t * cookie,
        child_t ** children, uint8_t * eof) {
    int status = -1;
    lv2_entry_t *parent = NULL;

    START_PROFILING(export_readdir);

    // Get the lv2 inode
    if (!(parent = export_lookup_fid(e, fid))) {
        severe("export_readdir failed: %s", strerror(errno));
        goto out;
    }

    // Verify that the target is a directory
    if (!S_ISDIR(parent->attributes.mode)) {
        severe("export_readdir failed: %s", strerror(errno));
        errno = ENOTDIR;
        goto out;
    }

    // List directory
    if (list_mdirentries(parent->container.mdir.fdp, fid, children, cookie, eof) != 0) {
        goto out;
    }

    // Access time of the directory is not changed any more on readdir

    
    // Update atime of parent
    //parent->attributes.atime = time(NULL);
    //if (export_lv2_write_attributes(parent) != 0)
    //    goto out;

    status = 0;
out:
    STOP_PROFILING(export_readdir);

    return status;
}
/*
**______________________________________________________________________________
*/
/** Display RozoFS special xattribute 
 *
 * 
 * @return: On success, the size of the extended attribute value.
 * On failure, -1 is returned and errno is set appropriately.
 */
#define ROZOFS_XATTR "rozofs"
#define ROZOFS_USER_XATTR "user.rozofs"
#define ROZOFS_ROOT_XATTR "trusted.rozofs"

#define ROZOFS_XATTR_ID "rozofs_id"
#define ROZOFS_USER_XATTR_ID "user.rozofs_id"
#define ROZOFS_ROOT_XATTR_ID "trusted.rozofs_id"

#define ROZOFS_XATTR_MAX_SIZE "rozofs_maxsize"
#define ROZOFS_USER_XATTR_MAX_SIZE "user.rozofs_maxsize"
#define ROZOFS_ROOT_XATTR_MAX_SIZE "trusted.rozofs_maxsize"

#define DISPLAY_ATTR_TITLE(name) p += sprintf(p,"%-7s : ",name);
#define DISPLAY_ATTR_INT(name,val) p += sprintf(p,"%-7s : %d\n",name,val);
#define DISPLAY_ATTR_LONG(name,val) p += sprintf(p,"%-7s : %llu\n",name,(unsigned long long int)val);
#define DISPLAY_ATTR_2INT(name,val1,val2) p += sprintf(p,"%-7s : %d/%d\n",name,val1,val2);
#define DISPLAY_ATTR_TXT(name,val) p += sprintf(p,"%-7s : %s\n",name,val);
static inline int get_rozofs_xattr(export_t *e, lv2_entry_t *lv2, char * value, int size) {
  char    * p=value;
  uint8_t * pFid;
  int       idx;
  int       left;
  uint8_t   rozofs_safe = rozofs_get_rozofs_safe(e->layout);
  
  pFid = (uint8_t *) lv2->attributes.fid;  
  DISPLAY_ATTR_INT("EID", e->eid);
  DISPLAY_ATTR_INT("LAYOUT", e->layout);  
  
  DISPLAY_ATTR_TITLE( "FID"); 
  p += sprintf(p,"%2.2x%2.2x%2.2x%2.2x-%2.2x%2.2x-%2.2x%2.2x-%2.2x%2.2x-%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x\n", 
               pFid[0],pFid[1],pFid[2],pFid[3],pFid[4],pFid[5],pFid[6],pFid[7],
	       pFid[8],pFid[9],pFid[10],pFid[11],pFid[12],pFid[13],pFid[14],pFid[15]);

  DISPLAY_ATTR_2INT("UID/GID",lv2->attributes.uid,lv2->attributes.gid);


  if (S_ISDIR(lv2->attributes.mode)) {
    DISPLAY_ATTR_TXT("MODE", "DIRECTORY");
    DISPLAY_ATTR_INT("CHILDREN",lv2->attributes.children);
    return (p-value);  
  }

  if (S_ISLNK(lv2->attributes.mode)) {
    DISPLAY_ATTR_TXT("MODE", "SYMBOLIC LINK");
  }  
  else {
    DISPLAY_ATTR_TXT("MODE", "REGULAR FILE");
  }
  
  /*
  ** File only
  */
  DISPLAY_ATTR_INT("CLUSTER",lv2->attributes.cid);
  DISPLAY_ATTR_TITLE("STORAGE");
  p += sprintf(p, "%3.3d", lv2->attributes.sids[0]);  
  for (idx = 1; idx < rozofs_safe; idx++) {
    p += sprintf(p,"-%3.3d", lv2->attributes.sids[idx]);
  } 
  p += sprintf(p,"\n");

  DISPLAY_ATTR_INT("NLINK",lv2->attributes.nlink);
  DISPLAY_ATTR_LONG("SIZE",lv2->attributes.size);


  DISPLAY_ATTR_INT("LOCK",lv2->nb_locks);  
  if (lv2->nb_locks != 0) {
    rozofs_file_lock_t *lock_elt;
    list_t             * pl;
    char               * sizeType;


    /* Check for left space */
    left = size;
    left -= ((int)(p-value));
    if (left < 110) {
      if (left > 4) p += sprintf(p,"...");
      return (p-value);
    }
    

    /* List the locks */
    list_for_each_forward(pl, &lv2->file_lock) {

      lock_elt = list_entry(pl, rozofs_file_lock_t, next_fid_lock);	
      switch(lock_elt->lock.user_range.size) {
        case EP_LOCK_TOTAL:      sizeType = "TOTAL"; break;
	case EP_LOCK_FROM_START: sizeType = "START"; break;
	case EP_LOCK_TO_END:     sizeType = "END"; break;
	case EP_LOCK_PARTIAL:    sizeType = "PARTIAL"; break;
	default:                 sizeType = "??";
      }  
      p += sprintf(p,"   %-5s %-7s client %16.16llx owner %16.16llx [%"PRIu64":%"PRIu64"[ [%"PRIu64":%"PRIu64"[\n",
	       (lock_elt->lock.mode==EP_LOCK_WRITE)?"WRITE":"READ",sizeType, 
	       (long long unsigned int)lock_elt->lock.client_ref, 
	       (long long unsigned int)lock_elt->lock.owner_ref,
	       (uint64_t) lock_elt->lock.user_range.offset_start,
	       (uint64_t) lock_elt->lock.user_range.offset_stop,
	       (uint64_t) lock_elt->lock.effective_range.offset_start,
	       (uint64_t) lock_elt->lock.effective_range.offset_stop);

    }       
  } 

  return (p-value);  
} 
static inline int set_rozofs_xattr(export_t *e, lv2_entry_t *lv2, char * value,int length) {
  char       * p=value;
  int          idx,jdx;
  int          new_cid;
  int          new_sids[ROZOFS_SAFE_MAX]; 
  uint8_t      rozofs_safe;

  if (S_ISDIR(lv2->attributes.mode)) {
    errno = EISDIR;
    return -1;
  }
     
  if (S_ISLNK(lv2->attributes.mode)) {
    errno = EMLINK;
    return -1;
  }

  /*
  ** File must not yet be written 
  */
  if (lv2->attributes.size != 0) {
    errno = EFBIG;
    return -1;
  } 
  
  /*
  ** Scan value
  */
  rozofs_safe = rozofs_get_rozofs_safe(e->layout);
  memset (new_sids,0,sizeof(new_sids));
  new_cid = 0;

  errno = 0;
  new_cid = strtol(p,&p,10);
  if (errno != 0) return -1; 
  
  for (idx=0; idx < rozofs_safe; idx++) {
  
    if ((p-value)>=length) {
      errno = EINVAL;
      return -1;
    }

    new_sids[idx] = strtol(p,&p,10);
    if (errno != 0) return -1;
    if (new_sids[idx]<0) new_sids[idx] *= -1;
  }
  /*
  ** Check the same sid is not set 2 times
  */
  for (idx=0; idx < rozofs_safe; idx++) {
    for (jdx=idx+1; jdx < rozofs_safe; jdx++) {
      if (new_sids[idx] == new_sids[jdx]) {
        errno = EINVAL;
	return -1;
      }
    }
  }  

  /*
  ** Check cluster and sid exist
  */
  if (volume_distribution_check(e->volume, rozofs_safe, new_cid, new_sids) != 0) return -1;
  
  /*
  ** OK for the new distribution
  */
  lv2->attributes.cid = new_cid;
  for (idx=0; idx < rozofs_safe; idx++) {
    lv2->attributes.sids[idx] = new_sids[idx];
  }
  
  /*
  ** Save new distribution on disk
  */
  return export_lv2_write_attributes(lv2);  
} 
/*
**______________________________________________________________________________
*/
static inline int get_rozofs_xattr_internal_id(export_t *e, lv2_entry_t *lv2, char * value, int size) {
  char    * p=value;
  int       idx;
  uint8_t   rozofs_safe = rozofs_get_rozofs_safe(e->layout);
  
  
  p+=sprintf(p,"@rozofs@%d-%d-%d-",e->eid,lv2->attributes.cid,e->layout);
  for (idx = 0; idx < rozofs_safe; idx++) {
    p += sprintf(p,"%d-", lv2->attributes.sids[idx]);
  }
  uuid_unparse(lv2->attributes.fid,p); 
  p+=36;
  /*
  ** put the length
  */
  p+=sprintf(p,"-%llu",(long long unsigned int)lv2->attributes.size);
  
  return (p-value);  
} 
/*
**______________________________________________________________________________
*/
static inline int get_rozofs_xattr_max_size(export_t *e, lv2_entry_t *lv2, char * value, int size) {
  char    * p=value;
  int       idx;
  uint64_t  free=-1;
  uint8_t   rozofs_forward = rozofs_get_rozofs_forward(e->layout);
  //uint8_t   rozofs_inverse = rozofs_get_rozofs_inverse(e->layout);
  int       rozofs_psize;
  volume_t * volume; 
  epp_sstat_t * storage_stat = NULL; 
  int        idx_stor;
  int        found = 0;
  int        quota=0;
  

  if (S_ISDIR(lv2->attributes.mode)) {
    p += sprintf(p,"rozof_max_size is not applicable to a directory."); 
    return (p-value);  
  }

  if (S_ISLNK(lv2->attributes.mode)) {
    p += sprintf(p,"rozof_max_size is not applicable to a symbolic link."); 
    return (p-value);
  }  
  

  volume = e->volume;

  /*
  ** Find out the volume of this file
  */
  int nb_volumes = gprofiler.nb_volumes;
  for (idx=0; idx < nb_volumes; idx++) { 
    if (gprofiler.vstats[idx].vid == volume->vid) break;
  }
  if (idx == nb_volumes) {
    p += sprintf(p,"Temporary unavailable. Please retry."); 
    return (p-value);
  }  


  /*
  ** Loop on the storages of this volume to check those
  ** of the file distribution and get the one that has the
  ** less free size.
  */
  int nb_storages = gprofiler.vstats[idx].nb_storages;  
  storage_stat = &gprofiler.vstats[idx].sstats[0];;   
  
  for (idx=0; idx <  nb_storages; idx++,storage_stat++) {
  
    if (storage_stat->cid != lv2->attributes.cid) continue;

    for(idx_stor=0; idx_stor < rozofs_forward; idx_stor++) {
    
      if (storage_stat->sid != lv2->attributes.sids[idx_stor]) continue;
      
      /*
      ** Get the storage that has the less free space
      */
      if (free > storage_stat->free) free = storage_stat->free;
      found++;
      
      if (found == rozofs_forward) break;
    }
    if (found == rozofs_forward) break;        
  } 
  
  
  if (idx == nb_storages) { 
    p += sprintf(p,"Temporary unavailable. Please retry."); 
    return (p-value);
  }

  /*
  ** This is the max number of blocks that we can still write
  ** for this distribution
  */
  rozofs_psize = rozofs_get_max_psize(e->layout)*sizeof (bin_t)
               + sizeof (rozofs_stor_bins_hdr_t)
	       // + sizeof (rozofs_stor_bins_footer_t)
	       ;
  
  free /= rozofs_psize;
        
  /*
  ** Respect the hard quota
  */	
  if (e->hquota > 0) {
    uint64_t quota_left = (e->hquota - e->fstat.blocks);
    if (free > quota_left) {
      free = quota_left;
      quota =1;
    }
  }
  
  /*
  ** Find out the rigth unit to display the result
  */
  free *= ROZOFS_BSIZE;
  
  if (free < 1024) {
    p += sprintf(p,"Remaining available space %llu B%s", (unsigned long long int) free, quota?" (hard quota).":"."); 
    return (p-value);   
  } 
  
  free /= 1024;
  if (free < 1024) {
    p += sprintf(p,"Remaining available space %llu KiB%s", (unsigned long long int) free, quota?" (hard quota).":"."); 
    return (p-value);   
  } 

  free /= 1024;    
  if (free < 1024) {
    p += sprintf(p,"Remaining available space %llu MiB%s", (unsigned long long int) free, quota?" (hard quota).":"."); 
    return (p-value);   
  }  
    
  free /= 1024;
  if (free < 1024) {
    p += sprintf(p,"Remaining available space %llu GiB%s", (unsigned long long int) free, quota?" (hard quota).":"."); 
    return (p-value);   
  }
  
  free /= 1024;
  p += sprintf(p,"Remaining available space %llu TiB%s", (unsigned long long int) free, quota?" (hard quota).":"."); 
  return (p-value);    
}  

/*
**______________________________________________________________________________
*/
/** retrieve an extended attribute value.
 *
 * @param e: the export managing the file or directory.
 * @param fid: the id of the file or directory.
 * @param name: the extended attribute name.
 * @param value: the value of this extended attribute.
 * @param size: the size of a buffer to hold the value associated
 *  with this extended attribute.
 * 
 * @return: On success, the size of the extended attribute value.
 * On failure, -1 is returned and errno is set appropriately.
 */
ssize_t export_getxattr(export_t *e, fid_t fid, const char *name, void *value, size_t size) {
    ssize_t status = -1;
    lv2_entry_t *lv2 = 0;

    START_PROFILING(export_getxattr);

    if (!(lv2 = export_lookup_fid(e, fid))) {
        severe("export_getattr failed: %s", strerror(errno));
        goto out;
    }

    if ((strcmp(name,ROZOFS_XATTR)==0)||(strcmp(name,ROZOFS_USER_XATTR)==0)||(strcmp(name,ROZOFS_ROOT_XATTR)==0)) {
      status = get_rozofs_xattr(e,lv2,value,size);
      goto out;
    }  
    if ((strcmp(name,ROZOFS_XATTR_ID)==0)||(strcmp(name,ROZOFS_USER_XATTR_ID)==0)||(strcmp(name,ROZOFS_ROOT_XATTR_ID)==0)) {
      status = get_rozofs_xattr_internal_id(e,lv2,value,size);
      goto out;
    } 
    if ((strcmp(name,ROZOFS_XATTR_MAX_SIZE)==0)||(strcmp(name,ROZOFS_USER_XATTR_MAX_SIZE)==0)||(strcmp(name,ROZOFS_ROOT_XATTR_MAX_SIZE)==0)) {
      status = get_rozofs_xattr_max_size(e,lv2,value,size);
      goto out;
    }      
    if ((status = export_lv2_get_xattr(lv2, name, value, size)) < 0) {
        goto out;
    }

out:
    STOP_PROFILING(export_getxattr);

    return status;
}
/*
**______________________________________________________________________________
*/
/** Set a lock on a file
 *
 * @param e: the export managing the file or directory.
 * @param fid: the id of the file or directory.
 * @param lock: the lock to set/remove
 * 
 * @return: On success, the size of the extended attribute value.
 * On failure, -1 is returned and errno is set appropriately.
 */
int export_set_file_lock(export_t *e, fid_t fid, ep_lock_t * lock_requested, ep_lock_t * blocking_lock) {
    ssize_t status = -1;
    lv2_entry_t *lv2 = 0;
    list_t      *p;
    rozofs_file_lock_t * lock_elt;
    rozofs_file_lock_t * new_lock;
    int                  overlap=0;

    START_PROFILING(export_set_file_lock);

    if (!(lv2 = export_lookup_fid(e, fid))) {
        severe("export_set_lock failed: %s", strerror(errno));
        goto out;
    }

    /*
    ** Freeing a lock 
    */
    if (lock_requested->mode == EP_LOCK_FREE) {
    
      /* Always succcess */
      status = 0;

      /* Already free */
      if (lv2->nb_locks == 0) {
	goto out;
      }
      if (list_empty(&lv2->file_lock)) {
	lv2->nb_locks = 0;
	goto out;
      }  
      
reloop:       
      /* Search the given lock */
      list_for_each_forward(p, &lv2->file_lock) {
      
        lock_elt = list_entry(p, rozofs_file_lock_t, next_fid_lock);	
	
	if (must_file_lock_be_removed(lock_requested, &lock_elt->lock, &new_lock)) {
	  lv2_cache_free_file_lock(lock_elt);
	  lv2->nb_locks--;
	  if (list_empty(&lv2->file_lock)) {
	    lv2->nb_locks = 0;
	    goto out;
	  }
	  goto reloop;
	}

	if (new_lock) {
	  list_push_front(&lv2->file_lock,&new_lock->next_fid_lock);
	  lv2->nb_locks++;
	}
      }
      goto out; 
    }

    /*
    ** Setting a new lock. Check its compatibility against every already set lock
    */
    list_for_each_forward(p, &lv2->file_lock) {
    
      lock_elt = list_entry(p, rozofs_file_lock_t, next_fid_lock);
      
      /*
      ** Check compatibility between 2 different applications
      */
      if ((lock_elt->lock.client_ref != lock_requested->client_ref) 
      ||  (lock_elt->lock.owner_ref != lock_requested->owner_ref)) { 
      
	if (!are_file_locks_compatible(&lock_elt->lock,lock_requested)) {
	  memcpy(blocking_lock,&lock_elt->lock,sizeof(ep_lock_t));     
          errno = EWOULDBLOCK;
	  goto out;      
	} 
    	continue;    
      }
      
      /*
      ** Check compatibility of 2 locks of a same application
      */

      /*
      ** Two read or two write locks. Check whether they overlap
      */
      if (lock_elt->lock.mode == lock_requested->mode) {
        if (are_file_locks_overlapping(lock_requested,&lock_elt->lock)) {
	  overlap++;
	}  
        continue;
      }
      
      /*
      ** One read and one write
      */
      if (!are_file_locks_compatible(&lock_elt->lock,lock_requested)) {
	memcpy(blocking_lock,&lock_elt->lock,sizeof(ep_lock_t));     
        errno = EWOULDBLOCK;
	goto out;      
      }     
      continue; 			  
    }

    /*
    ** This lock overlaps with a least one existing lock of the same application.
    ** Let's concatenate all those locks
    */  
concatenate:  
    if (overlap != 0) {
      list_for_each_forward(p, &lv2->file_lock) {

	lock_elt = list_entry(p, rozofs_file_lock_t, next_fid_lock);

	if ((lock_elt->lock.client_ref != lock_requested->client_ref) 
	||  (lock_elt->lock.owner_ref != lock_requested->owner_ref)) continue;

	if (lock_elt->lock.mode != lock_requested->mode) continue;

	if (try_file_locks_concatenate(lock_requested,&lock_elt->lock)) {
          overlap--;
	  lv2_cache_free_file_lock(lock_elt);
	  lv2->nb_locks--;
	  if (list_empty(&lv2->file_lock)) {
	    lv2->nb_locks = 0;
	  }
	  goto concatenate;	  
	}
      } 
    }   
        
    /*
    ** Since we have reached this point all the locks are compatibles with the new one.
    ** and it does not overlap any more with an other lock. Let's insert this new lock
    */
    lock_elt = lv2_cache_allocate_file_lock(lock_requested);
    list_push_front(&lv2->file_lock,&lock_elt->next_fid_lock);
    lv2->nb_locks++;
   status = 0; 
    
out:
#if 0
    {
      char BuF[4096];
      char * pChar = BuF;
      debug_file_lock_list(pChar);
      info("%s",BuF);
    }
#endif      
    if (lv2) lv2_cache_update_lru(e->lv2_cache, lv2);	
    STOP_PROFILING(export_set_file_lock);
    return status;
}
/*
**______________________________________________________________________________
*/
/** Get a lock on a file
 *
 * @param e: the export managing the file or directory.
 * @param fid: the id of the file or directory.
 * @param lock: the lock to set/remove
 * 
 * @return: On success, the size of the extended attribute value.
 * On failure, -1 is returned and errno is set appropriately.
 */
int export_get_file_lock(export_t *e, fid_t fid, ep_lock_t * lock_requested, ep_lock_t * blocking_lock) {
    ssize_t status = -1;
    lv2_entry_t *lv2 = 0;
    rozofs_file_lock_t *lock_elt;
    list_t * p;

    START_PROFILING(export_get_file_lock);

    if (!(lv2 = export_lookup_fid(e, fid))) {
        severe("export_get_lock failed: %s", strerror(errno));
        goto out;
    }

    /*
    ** Freeing a lock 
    */
    if (lock_requested->mode == EP_LOCK_FREE) {    
      /* Always succcess */
      status = 0;
      goto out; 
    }

    /*
    ** Setting a new lock. Check its compatibility against every already set lock
    */
    list_for_each_forward(p, &lv2->file_lock) {
    
      lock_elt = list_entry(p, rozofs_file_lock_t, next_fid_lock);

      if (!are_file_locks_compatible(&lock_elt->lock,lock_requested)) {
	memcpy(blocking_lock,&lock_elt->lock,sizeof(ep_lock_t));     
        errno = EWOULDBLOCK;
	goto out;      
      }     
    }
    status = 0;
    
out:
    STOP_PROFILING(export_get_file_lock);
    return status;
}
/*
**______________________________________________________________________________
*/
/** reset a lock from a client
 *
 * @param e: the export managing the file or directory.
 * @param lock: the identifier of the client whose locks are to remove
 * 
 * @return: On success, the size of the extended attribute value.
 * On failure, -1 is returned and errno is set appropriately.
 */
int export_clear_client_file_lock(export_t *e, ep_lock_t * lock_requested) {

    START_PROFILING(export_clearclient_flock);
    file_lock_remove_client(lock_requested->client_ref);
    STOP_PROFILING(export_clearclient_flock);
    return 0;
}
/*
**______________________________________________________________________________
*/
/** reset all the locks from an owner
 *
 * @param e: the export managing the file or directory.
 * @param lock: the identifier of the client whose locks are to remove
 * 
 * @return: On success, the size of the extended attribute value.
 * On failure, -1 is returned and errno is set appropriately.
 */
int export_clear_owner_file_lock(export_t *e, fid_t fid, ep_lock_t * lock_requested) {
    int status = -1;
    lv2_entry_t *lv2 = 0;
    list_t * p;
    rozofs_file_lock_t *lock_elt;
    
    START_PROFILING(export_clearowner_flock);

    if (!(lv2 = export_lookup_fid(e, fid))) {
        severe("export_lookup_fid failed: %s", strerror(errno));
        goto out;
    }
    
    status = 0;

reloop:    
    /* Search the given lock */
    list_for_each_forward(p, &lv2->file_lock) {
      lock_elt = list_entry(p, rozofs_file_lock_t, next_fid_lock);
      if ((lock_elt->lock.client_ref == lock_requested->client_ref) &&
          (lock_elt->lock.owner_ref == lock_requested->owner_ref)) {
	  /* Found a lock to free */
	  lv2_cache_free_file_lock(lock_elt);
	  lv2->nb_locks--;
	  if (list_empty(&lv2->file_lock)) {
	    lv2->nb_locks = 0;
	    // Remove it from the lru
	    lv2_cache_update_lru(e->lv2_cache, lv2);	    
	    break;
	  }
	  goto reloop;
      }       
    }    

out:
    STOP_PROFILING(export_clearowner_flock);
    return status;
}
/*
**______________________________________________________________________________
*/
/** Get a poll event from a client
 *
 * @param e: the export managing the file or directory.
 * @param lock: the lock to set/remove
 * 
 * @return: On success, the size of the extended attribute value.
 * On failure, -1 is returned and errno is set appropriately.
 */
int export_poll_file_lock(export_t *e, ep_lock_t * lock_requested) {

    START_PROFILING(export_poll_file_lock);
    file_lock_poll_client(lock_requested->client_ref);
    STOP_PROFILING(export_poll_file_lock);
    return 0;
}
/*
**______________________________________________________________________________
*/
/** set an extended attribute value for a file or directory.
 *
 * @param e: the export managing the file or directory.
 * @param fid: the id of the file or directory.
 * @param name: the extended attribute name.
 * @param value: the value of this extended attribute.
 * @param size: the size of a buffer to hold the value associated
 *  with this extended attribute.
 * @param flags: parameter can be used to refine the semantics of the operation.
 * 
 * @return: On success, zero is returned.  On failure, -1 is returned.
 */
int export_setxattr(export_t *e, fid_t fid, char *name, const void *value, size_t size, int flags) {
    int status = -1;
    lv2_entry_t *lv2 = 0;

    START_PROFILING(export_setxattr);

    if (!(lv2 = export_lookup_fid(e, fid))) {
        severe("export_getattr failed: %s", strerror(errno));
        goto out;
    }


    if ((strcmp(name,ROZOFS_XATTR)==0)||(strcmp(name,ROZOFS_USER_XATTR)==0)||(strcmp(name,ROZOFS_ROOT_XATTR)==0)) {
      status = set_rozofs_xattr(e,lv2,(char *)value,size);
      goto out;
    }  

    if ((status = export_lv2_set_xattr(lv2, name, value, size, flags)) != 0) {
        goto out;
    }

    status = 0;
out:
    STOP_PROFILING(export_setxattr);

    return status;
}
/*
**______________________________________________________________________________
*/
/** remove an extended attribute from a file or directory.
 *
 * @param e: the export managing the file or directory.
 * @param fid: the id of the file or directory.
 * @param name: the extended attribute name.
 * 
 * @return: On success, zero is returned.  On failure, -1 is returned.
 */
int export_removexattr(export_t *e, fid_t fid, char *name) {
    int status = -1;
    lv2_entry_t *lv2 = 0;

    START_PROFILING(export_removexattr);

    if (!(lv2 = export_lookup_fid(e, fid))) {
        severe("export_getattr failed: %s", strerror(errno));
        goto out;
    }

    if ((status = export_lv2_remove_xattr(lv2, name)) != 0) {
        goto out;
    }

    status = 0;
out:
    STOP_PROFILING(export_removexattr);

    return status;
}
/*
**______________________________________________________________________________
*/
/** list extended attribute names from the lv2 regular file.
 *
 * @param e: the export managing the file or directory.
 * @param fid: the id of the file or directory.
 * @param list: list of extended attribute names associated with this file/dir.
 * @param size: the size of a buffer to hold the list of extended attributes.
 * 
 * @return: On success, the size of the extended attribute name list.
 * On failure, -1 is returned and errno is set appropriately.
 */
ssize_t export_listxattr(export_t *e, fid_t fid, void *list, size_t size) {
    ssize_t status = -1;
    lv2_entry_t *lv2 = 0;

    START_PROFILING(export_listxattr);

    if (!(lv2 = export_lookup_fid(e, fid))) {
        severe("export_getattr failed: %s", strerror(errno));
        goto out;
    }

    if ((status = export_lv2_list_xattr(lv2, list, size)) < 0) {
        goto out;
    }

out:
    STOP_PROFILING(export_listxattr);
    return status;
}

/*
int export_open(export_t * e, fid_t fid) {
    int flag;

    flag = O_RDWR;

    if (!(mfe = export_get_mfentry_by_id(e, fid))) {
        severe("export_open failed: export_get_mfentry_by_id failed");
        goto out;
    }

    if (mfe->fd == -1) {
        if ((mfe->fd = open(mfe->path, flag)) < 0) {
            severe("export_open failed for file %s: %s", mfe->path,
                    strerror(errno));
            goto out;
        }
    }

    mfe->cnt++;

    status = 0;
out:

    return status;
}

int export_close(export_t * e, fid_t fid) {
    int status = -1;
    mfentry_t *mfe = 0;
    DEBUG_FUNCTION;

    if (!(mfe = export_get_mfentry_by_id(e, fid))) {
        severe("export_close failed: export_get_mfentry_by_id failed");
        goto out;
    }

    if (mfe->cnt == 1) {
        if (close(mfe->fd) != 0) {
            goto out;
        }
        mfe->fd = -1;
    }

    mfe->cnt--;

    status = 0;
out:
    return status;
}
 */
