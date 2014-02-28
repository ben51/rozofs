#define _XOPEN_SOURCE 500

#include <unistd.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#ifdef __linux__
#include <uuid/uuid.h>
#else
#include <uuid.h>
#endif
#include <stdlib.h>

typedef uuid_t fid_t; /**< file id */
unsigned int bucket = -1;
unsigned int scoll_idx = -1;
unsigned int rcoll_idx = -1;

static void usage() {
    printf("dirent [ -b <bucket number> ] <file1> [ <file2> ] \n");
    printf("[ -b <bucket number> ]    the bucket number to display (default all)\n");
    printf(" [ -scoll <coll_idx> ]    assert presence of collision file <coll_idx>\n");
    printf(" [ -rcoll <coll_idx> ]    reset presence of collision file <coll_idx>\n");
    exit(0);

}

int read_parameters(argc, argv)
int argc;
char *argv[];
{
    unsigned int idx;
    int ret;

    if (argc < 1) usage();

    idx = 1;
    while (idx < argc) {

        /* -b <bucket number> */
        if (strcmp(argv[idx], "-b") == 0) {
            idx++;
            if (idx == argc) {
                printf("%s option set but missing value !!!\n", argv[idx - 1]);
                usage();
            }
            ret = sscanf(argv[idx], "%d", &bucket);
            if (ret != 1) {
                printf("%s option but bad value \"%s\"!!!\n", argv[idx - 1], argv[idx]);
                usage();
            }
            idx++;
            continue;
        }

        /* -scoll <coll_idx> */
        if (strcmp(argv[idx], "-scoll") == 0) {
            idx++;
            if (idx == argc) {
                printf("%s option set but missing value !!!\n", argv[idx - 1]);
                usage();
            }
            ret = sscanf(argv[idx], "%d", &scoll_idx);
            if (ret != 1) {
                printf("%s option but bad value \"%s\"!!!\n", argv[idx - 1], argv[idx]);
                usage();
            }
            idx++;
            continue;
        }

        /* -rcoll <coll_idx> */
        if (strcmp(argv[idx], "-rcoll") == 0) {
            idx++;
            if (idx == argc) {
                printf("%s option set but missing value !!!\n", argv[idx - 1]);
                usage();
            }
            ret = sscanf(argv[idx], "%d", &rcoll_idx);
            if (ret != 1) {
                printf("%s option but bad value \"%s\"!!!\n", argv[idx - 1], argv[idx]);
                usage();
            }
            idx++;
            continue;
        }

        return idx;

        usage();
    }

    return 0;
}

/**
 @ingroup DIRENT_FILE_STR
 *  dirent type: file or memory only
 *  that type is used when the system is facing a virtula pointer that is NULL , for the case
 *  of a file it should not be considered as an error since while load root dirent file, if there is
 *  any collision files there are not loaded in memory for performance reason. Remember that it could
 *  have up to 2048 collision files, so loading all the collision files in the cache might have a
 *  big impact on the system. The system rather loads them on deman when needed: either during a search
 *  or when it needs to allocate a free entry.
 */


/**
 * mdirent header new
 */
#define MDIRENTS_MAX_LEVEL  3          /**< 3 level only: rott, coll1 and coll2 */
#define MDIRENTS_MAX_COLL_LEVEL 1      /**< one collision level only        */

#define MDIRENT_CACHE_MEM_TYPE   0  ///< the type of the dirent is memory : generic cache
#define MDIRENT_CACHE_FILE_TYPE  1  ///< the type of the dirent is a file,
#define MDIRENT_FILE_VERSION_0   0  ///< current version of the dirent file

/**
 @ingroup DIRENT_FILE_STR
 *  Header used for dirent file
 */
typedef struct _mdirents_header_new_t {
    uint16_t version : 3; /// Version of the dirent file
    uint16_t type : 1; ///0: file; 1: memory
    uint16_t level_index : 12; /// 0: root, 1: coll1, 2: coll 2
    uint16_t dirent_idx[MDIRENTS_MAX_LEVEL]; ///< index of each level
    uint64_t max_number_of_hash_entries : 16; ///< number of hash entries supported by the file
    uint64_t sector_offset_of_name_entry : 16; ///< index of the first name entry sector
    uint64_t filler : 32;
} mdirents_header_new_t;

#define MDIRENTS_ENTRIES_COUNT  384  ///< @ingroup DIRENT_FILE_STR number of entries in a dirent file


#define MDIRENTS_BITMAP_FREE_HASH_SZ (((MDIRENTS_ENTRIES_COUNT-1)/8)+1)
#define MDIRENTS_BITMAP_FREE_HASH_LAST_BIT_IDX MDIRENTS_ENTRIES_COUNT  //< index of the last valid bit

/**
 *  bitmap of the free hash entries of a dirent file
 */
typedef struct _mdirents_btmap_free_hash_t {
    uint8_t bitmap[MDIRENTS_BITMAP_FREE_HASH_SZ];
} mdirents_btmap_free_hash_t;





#define MDIRENTS_MAX_COLLS_IDX   64  /**< max number of dirent collision file per dirent file */

#define MDIRENTS_BITMAP_COLL_DIRENT_LAST_BYTE  (((MDIRENTS_MAX_COLLS_IDX-1)/8)+1) //< index of the last byte of the bitmap

/**
 *  bitmap of the mdirent collision files (one bit asserted indicates the presence of a file)
 */
typedef struct _mdirents_btmap_coll_dirent_t {
    uint8_t bitmap[MDIRENTS_BITMAP_COLL_DIRENT_LAST_BYTE];
} mdirents_btmap_coll_dirent_t;

#define MDIRENTS_HASH_PTR_EOF    0   ///<  end of list or empty
#define MDIRENTS_HASH_PTR_LOCAL  1   ///< index is local to the dirent file
#define MDIRENTS_HASH_PTR_COLL   2   ///< the next entry is found in a dirent collision file (index is not significant
#define MDIRENTS_HASH_PTR_FREE   3   ///<  free entry

/**
 *  hash table logical pointer
 */
typedef struct _mdirents_hash_ptr_t {
    uint16_t bucket_idx_low : 4; ///< for hash entry usage only not significant on hash bucket entry
    uint16_t type : 2; ///< type of the hash pointer : local, collision or eof (see above)
    uint16_t idx : 10; ///< index of the hash entry within the local dirent file or index of the collision dirent entry
} mdirents_hash_ptr_t;

#define MDIRENTS_HASH_TB_INT_SZ 256

/**
 *  dirent hash table pointer structure
 */
typedef struct _mdirents_hash_tab_t {
    mdirents_hash_ptr_t first[MDIRENTS_HASH_TB_INT_SZ]; ///< pointer to the first entry for the corresponding hash value
} mdirents_hash_tab_t;

/**
 @ingroup DIRENT_FILE_STR
 * structure of a mdirent hash entry
 */


typedef struct _mdirents_hash_entry_t {
    uint32_t bucket_idx_high : 4; ///< highest bit of the bucket_idx
    uint32_t hash : 28; ///< value of the hash applied to the filename or directory
    uint16_t chunk_idx : 12; ///< index of the first block where the entry with name/type/fid is found
    uint16_t nb_chunk : 4; ///< number of "name_blocks" allocated for the entry
    mdirents_hash_ptr_t next; ///< next entry with the same level1/2 hash value
} mdirents_hash_entry_t;

/**
 @ingroup DIRENT_FILE_STR
 * structure of a mdirent entry with name, fid, type
 */
#define ROZOFS_FILENAME_MAX 255

typedef struct _mdirents_name_entry_t {
    uint32_t type : 24; ///< type of the entry: directory, regular file, symlink, etc...
    uint32_t len : 8; ///< length of name without null termination
    fid_t fid; ///< unique ID allocated to the file or directory
    char name[ROZOFS_FILENAME_MAX]; ///< name of the directory or file
} mdirents_name_entry_t;

#define MDIRENTS_NAME_CHUNK_SZ  32  ///< chunk size of a block used for storing name and fid
#define MDIRENTS_NAME_CHUNK_MAX  (((sizeof(mdirents_name_entry_t)-1)/MDIRENTS_NAME_CHUNK_SZ)+1) ///< max number of chunk for the max name length
#ifndef DIRENT_LARGE
#define MDIRENTS_NAME_CHUNK_MAX_CNT  (MDIRENTS_NAME_CHUNK_MAX*MDIRENTS_ENTRIES_COUNT) ///< max number of chunks for MDIRENTS_ENTRIES_COUNT
#define MDIRENTS_BITMAP_FREE_NAME_SZ ((((MDIRENTS_ENTRIES_COUNT*MDIRENTS_NAME_CHUNK_MAX)-1)/8)+1) ///< bitmap size in bytes
#define MDIRENTS_BITMAP_FREE_NAME_LAST_BIT_IDX ((MDIRENTS_ENTRIES_COUNT*MDIRENTS_NAME_CHUNK_MAX)-1)  //< index of the last valid bit
#else
#define MDIRENTS_NAME_CHUNK_MAX_CNT 4096 ///< max number of chunks
#define MDIRENTS_BITMAP_FREE_NAME_SZ (MDIRENTS_NAME_CHUNK_MAX_CNT/8)
#define MDIRENTS_BITMAP_FREE_NAME_LAST_BIT_IDX (MDIRENTS_NAME_CHUNK_MAX_CNT-1)
#endif

/**
 *  bitmap of the free name and fid entries of a dirent file
 */
typedef struct _mdirents_btmap_free_chunk_t {
    uint8_t bitmap[MDIRENTS_BITMAP_FREE_NAME_SZ];
} mdirents_btmap_free_chunk_t;

#define MDIRENT_SECTOR_SIZE  512

/**
 *  Sector 0 contains the header of the dirent file and the hash entry bitmap
 */
typedef struct _mdirent_sector0__not_aligned_t {
    mdirents_header_new_t header; ///< header of the dirent file: mainly management information
    mdirents_btmap_coll_dirent_t coll_bitmap; ///< bitmap of the dirent collision file
    mdirents_btmap_free_hash_t hash_bitmap; ///< bitmap of the free hash entries
} mdirent_sector0_not_aligned_t;

typedef union _mdirent_sector0_t {
    uint8_t u8[((sizeof (mdirents_header_new_t)
            + sizeof (mdirents_btmap_free_hash_t) - 1) / MDIRENT_SECTOR_SIZE + 1)
            * MDIRENT_SECTOR_SIZE];
    mdirent_sector0_not_aligned_t s;

} mdirent_sector0_t;

/**
 *  Sector 1 contains name entry bitmap
 */
typedef union _mdirent_sector1_t {
    uint8_t u8[((sizeof (mdirents_btmap_free_chunk_t) - 1) / MDIRENT_SECTOR_SIZE
            + 1) * MDIRENT_SECTOR_SIZE];

    struct {
        mdirents_btmap_free_chunk_t name_bitmap; ///< bitmap of the free name/fid/type entries
    } s;
} mdirent_sector1_t;

/**
 *  Sector 2 contains the hash entry buckets
 */
typedef union _mdirent_sector2_t {
    uint8_t u8[((sizeof (mdirents_hash_tab_t) - 1) / MDIRENT_SECTOR_SIZE + 1)
            * MDIRENT_SECTOR_SIZE];

    struct {
        mdirents_hash_tab_t hash_tbl; ///< hash table: set of of 256 logical pointers
    } s;
} mdirent_sector2_t;

/**
 *  Sector 3 and 4 contains the hash entries
 */
typedef union _mdirent_sector3_t {
    uint8_t u8[((sizeof (mdirents_hash_entry_t) * MDIRENTS_ENTRIES_COUNT - 1)
            / MDIRENT_SECTOR_SIZE + 1) * MDIRENT_SECTOR_SIZE];

    struct {
        mdirents_hash_entry_t hash_entry[MDIRENTS_ENTRIES_COUNT]; ///< table of hash entries
    } s;
} mdirent_sector3_t;

/**
 @ingroup DIRENT_FILE_STR
 * Main structure of the dirent file
 */
typedef struct _mdirents_file_t { ///< Main structure of the dirent file
    mdirent_sector0_t sect0; ///< dirent header + coll_entry bitmap+hash entry bitmap
    mdirent_sector1_t sect1; ///< bitmap of the free name/fid/type entries
    mdirent_sector2_t sect2; ///< hash table: set of of 256 logical pointers
    mdirent_sector3_t sect3; ///< table of hash entries

} mdirents_file_t;

/*
 ** base sectors
 */
#define DIRENT_HEADER_BASE_SECTOR 0   /**< index of the header+coll entry+hash entry bitmap */
#define DIRENT_NAME_BITMAP_BASE_SECTOR (sizeof(mdirent_sector0_t)/MDIRENT_SECTOR_SIZE+DIRENT_HEADER_BASE_SECTOR)   /**< bitmap of the free name:fid/type */
#define DIRENT_HASH_BUCKET_BASE_SECTOR (sizeof(mdirent_sector1_t)/MDIRENT_SECTOR_SIZE+DIRENT_NAME_BITMAP_BASE_SECTOR)   /**< 256 hash buckets*/
#define DIRENT_HASH_ENTRIES_BASE_SECTOR (sizeof(mdirent_sector2_t)/MDIRENT_SECTOR_SIZE+DIRENT_HASH_BUCKET_BASE_SECTOR)   /**< 384 hash entries*/
#define DIRENT_HASH_NAME_BASE_SECTOR (sizeof(mdirent_sector3_t)/MDIRENT_SECTOR_SIZE+DIRENT_HASH_ENTRIES_BASE_SECTOR)   /**< 384 name/fid entries*/

/**
 * number of sectors for each type of entry
 */
#define DIRENT_HEADER_SECTOR_CNT  (sizeof(mdirent_sector0_t)/MDIRENT_SECTOR_SIZE)
#define DIRENT_NAME_BITMAP_SECTOR_CNT  (sizeof(mdirent_sector1_t)/MDIRENT_SECTOR_SIZE)   /**< bitmap of the free name:fid/type */
#define DIRENT_HASH_BUCKET_SECTOR_CNT (sizeof(mdirent_sector2_t)/MDIRENT_SECTOR_SIZE)   /**< 256 hash buckets*/
#define DIRENT_HASH_ENTRIES_SECTOR_CNT (sizeof(mdirent_sector3_t)/MDIRENT_SECTOR_SIZE)   /**< 384 hash entries*/
#define DIRENT_HASH_NAME_SECTOR_CNT ((((MDIRENTS_NAME_CHUNK_MAX_CNT*MDIRENTS_NAME_CHUNK_SZ)-1)/MDIRENT_SECTOR_SIZE)+1)  /**< 384 name/fid entries*/

#define DIRENT_FILE_MAX_SECTORS (DIRENT_HASH_NAME_BASE_SECTOR +DIRENT_HASH_NAME_SECTOR_CNT)


#define MDIRENTS_HASH_PTR_EOF    0   ///<  end of list or empty
#define MDIRENTS_HASH_PTR_LOCAL  1   ///< index is local to the dirent file
#define MDIRENTS_HASH_PTR_COLL   2   ///< the next entry is found in a dirent collision file (index is not significant
#define MDIRENTS_HASH_PTR_FREE   3   ///<  free 


#define DIRENT_HASH_ENTRY_GET_BUCKET_IDX(hash_entry_p) ((hash_entry_p->bucket_idx_high << 4) | hash_entry_p->next.bucket_idx_low)

char *print_type(int type) {
    if (type == MDIRENTS_HASH_PTR_EOF) return "E";
    if (type == MDIRENTS_HASH_PTR_LOCAL) return "L";
    if (type == MDIRENTS_HASH_PTR_COLL) return "C";
    if (type == MDIRENTS_HASH_PTR_FREE) return "F";
    return "UNK";


}
/*
 **______________________________________________________________________________
 */

/**
 * Read the mdirents file on disk
 *
 * @param dirfd: file descriptor of the parent directory
 * @param *pathname: pointer to the pathname to read
 *
 * @retval NULL if this mdirents file doesn't exist
 * @retval pointer to the mdirents file
 */
void set_coll_idx(int coll_idx, char *pathname, int set) {
    int fd = -1;
    int flag = O_RDWR;
    char *path_p;
    off_t offset;
    mdirents_file_t *dirent_file_p = NULL;
    mdirent_sector0_not_aligned_t *sect0_p = NULL;
    mdirents_btmap_coll_dirent_t *coll_bitmap_p = NULL;
    int ret;
    uint32_t u8, idx;

    /*
     ** clear errno
     */
    errno = 0;

    /*
     ** build the filename of the dirent file to read
     */
    path_p = pathname;

    if ((fd = open(path_p, flag, S_IRWXU)) == -1) {
        //printf("Cannot open the file %s, error %s at line %d\n",path_p,strerror(errno),__LINE__);
        /*
         ** check if the file exists. If might be possible that the file does not exist, it can be considered
         ** as a normal error since the exportd might have crashed just after the deletion of the collision dirent
         ** file but before  the update of the dirent root file.
         */
        if (errno == ENOENT) {
            goto out;

        }
        /*
         ** fatal error on file opening
         */

        printf("Cannot open the file %s, error %s at line %d\n", path_p, strerror(errno), __LINE__);
        errno = EIO;
        goto out;
    }
    /*
     ** allocate a working array for storing the content of the file except the part
     ** that contains the name/fid and mode
     */
    dirent_file_p = malloc(sizeof (mdirents_file_t))
            ;
    if (dirent_file_p == NULL) {
        /*
         ** the system runs out of memory
         */
        errno = ENOMEM;
        printf("Out of Memory at line %d", __LINE__)
                ;
        goto error;
    }
    /*
     ** read the fixed part of the dirent file
     */
    offset = DIRENT_HEADER_BASE_SECTOR * MDIRENT_SECTOR_SIZE;
    ret = pread(fd, dirent_file_p, sizeof (mdirents_file_t), offset);
    if (ret < 0) {
        /*
         ** need to figure out what need to be done since this might block a chunk of file
         */
        printf("pread failed in file %s: %s", pathname, strerror(errno));
        errno = EIO;
        goto error;

    }
    if (ret != sizeof (mdirents_file_t)) {

        /*
         ** we consider that error as the case of the file that does not exist. By ignoring that file
         ** we just lose potentially one file at amx
         */
        printf("incomplete pread in file %s %d (expected: %d)", pathname, ret, (int) sizeof (mdirents_file_t));
        errno = ENOENT;
        goto error;
    }


    sect0_p = (mdirent_sector0_not_aligned_t *) & dirent_file_p->sect0;
    coll_bitmap_p = &sect0_p->coll_bitmap;

    u8 = coll_idx / 8;
    idx = coll_idx % 8;

    if (set) {
        coll_bitmap_p->bitmap[u8] &= ~(1 << idx);
    } else {
        coll_bitmap_p->bitmap[u8] |= (1 << idx);
    }

    ret = pwrite(fd, dirent_file_p, sizeof (mdirents_file_t), offset);
    if (ret < 0) {
        /*
         ** need to figure out what need to be done since this might block a chunk of file
         */
        printf("pwrite failed in file %s: %s", pathname, strerror(errno));
        errno = EIO;
        goto error;
    }


out:
    if (fd != -1)
        close(fd);
    if (dirent_file_p != NULL)
        free(dirent_file_p);
    return;

error:
    if (dirent_file_p != NULL)
        free(dirent_file_p);
    if (fd != -1)
        close(fd);
    return;
}
/*
 **______________________________________________________________________________
 */

/**
 * Read the mdirents file on disk
 *
 * @param dirfd: file descriptor of the parent directory
 * @param *pathname: pointer to the pathname to read
 *
 * @retval NULL if this mdirents file doesn't exist
 * @retval pointer to the mdirents file
 */
void read_mdirents_file(char *pathname) {
    int fd = -1;
    int flag = O_RDONLY;
    char *path_p;
    off_t offset;
    mdirents_file_t *dirent_file_p = NULL;
    //mdirent_sector0_not_aligned_t *sect0_p = NULL;
    int ret;

    /*
     ** clear errno
     */
    errno = 0;

    /*
     ** build the filename of the dirent file to read
     */
    path_p = pathname;

    if ((fd = open(path_p, flag, S_IRWXU)) == -1) {
        //printf("Cannot open the file %s, error %s at line %d\n",path_p,strerror(errno),__LINE__);
        /*
         ** check if the file exists. If might be possible that the file does not exist, it can be considered
         ** as a normal error since the exportd might have crashed just after the deletion of the collision dirent
         ** file but before  the update of the dirent root file.
         */
        if (errno == ENOENT) {
            goto out;

        }
        /*
         ** fatal error on file opening
         */

        printf("Cannot open the file %s, error %s at line %d\n", path_p, strerror(errno), __LINE__);
        errno = EIO;
        goto out;
    }
    /*
     ** allocate a working array for storing the content of the file except the part
     ** that contains the name/fid and mode
     */
    dirent_file_p = malloc(sizeof (mdirents_file_t))
            ;
    if (dirent_file_p == NULL) {
        /*
         ** the system runs out of memory
         */
        errno = ENOMEM;
        printf("Out of Memory at line %d", __LINE__)
                ;
        goto error;
    }
    /*
     ** read the fixed part of the dirent file
     */
    offset = DIRENT_HEADER_BASE_SECTOR * MDIRENT_SECTOR_SIZE;
    ret = pread(fd, dirent_file_p, sizeof (mdirents_file_t), offset);
    if (ret < 0) {
        /*
         ** need to figure out what need to be done since this might block a chunk of file
         */
        printf("pread failed in file %s: %s", pathname, strerror(errno));
        errno = EIO;
        goto error;

    }
    if (ret != sizeof (mdirents_file_t)) {

        /*
         ** we consider that error as the case of the file that does not exist. By ignoring that file
         ** we just lose potentially one file at amx
         */
        printf("incomplete pread in file %s %d (expected: %d)", pathname, ret, (int) sizeof (mdirents_file_t));
        errno = ENOENT;
        goto error;
    }


    /*
     ** sector 2  :
     ** Go through the hash table bucket an allocated memory for buckets that are no empty:
     **  there are   256 hash buckets of 16 bits
     */
    {
        int i;
        int j = 1000;

        mdirents_hash_ptr_t *hash_bucket_p = (mdirents_hash_ptr_t*) & dirent_file_p->sect2;
        printf("BUCKETS:\n");
        if (bucket == -1) {
            printf("    |   0  |   1  |  2   |  3   |  4   |  5   |  6   |  7   |  8   |  9   |");
            for (i = 0; i < MDIRENTS_HASH_TB_INT_SZ; i++, hash_bucket_p++) {
                if (j > 9) {
                    printf("\n%3d |", i);
                    j = 0;
                }
                j++;
                printf(" %s%3.3d |", print_type(hash_bucket_p->type), hash_bucket_p->idx);
            }
        } else {
            printf(" %3d %s%3.3d\n", bucket, print_type(hash_bucket_p[bucket].type), hash_bucket_p[bucket].idx);
        }
    }
    /*
     ** sector 3 : hash entries
     */

    {
        int i;
        int j = 1000;
        printf("\n...............................................................\nHASH ENTRIES:\n");

        mdirents_hash_entry_t *hash_entry_p = (mdirents_hash_entry_t*) & dirent_file_p->sect3;
        int bucket_idx;
        if (bucket == -1) {
            printf("    |    0     |    1     |   2      |   3      |   4      |   5      |   6      |   7      |   8      |   9      |");
            for (i = 0; i < MDIRENTS_ENTRIES_COUNT; i++, hash_entry_p++) {
                bucket_idx = DIRENT_HASH_ENTRY_GET_BUCKET_IDX(hash_entry_p);
                if (j > 9) {
                    printf("\n%3d |", i);
                    j = 0;
                }
                j++;
                printf(" %3d %s%3.3d |", bucket_idx,
                        print_type(hash_entry_p->next.type), hash_entry_p->next.idx);
            }
        } else {
            for (i = 0; i < MDIRENTS_ENTRIES_COUNT; i++, hash_entry_p++) {
                bucket_idx = DIRENT_HASH_ENTRY_GET_BUCKET_IDX(hash_entry_p);
                if (bucket_idx != bucket) continue;
                printf(" %3d bucket %d %s%3.3d\n", i, bucket_idx,
                        print_type(hash_entry_p->next.type), hash_entry_p->next.idx);
            }
        }
        printf("\n");
    }

out:
    if (fd != -1)
        close(fd);
    if (dirent_file_p != NULL)
        free(dirent_file_p);
    return;

error:
    if (dirent_file_p != NULL)
        free(dirent_file_p);
    if (fd != -1)
        close(fd);
    return;
}

/*_______________________________________________________________________
 */
int main(int argc, char **argv) {
    int idx;

    idx = read_parameters(argc, argv);

    if (scoll_idx != -1) {
        set_coll_idx(scoll_idx, argv[idx], 1);
        exit(0);
    }

    if (rcoll_idx != -1) {
        set_coll_idx(rcoll_idx, argv[idx], 0);
        exit(0);
    }


    while (idx < argc) {
        printf("___________________________________________________%s\n", argv[idx]);
        read_mdirents_file(argv[idx]);
        idx++;
    }

    exit(0);
}
