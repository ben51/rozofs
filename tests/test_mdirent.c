#ifdef __linux__
#define _XOPEN_SOURCE 700
#endif
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <inttypes.h>

#include <rozofs/common/mattr.h>
#include <rozofs/rpc/export_profiler.h>
#include <rozofs/rpc/epproto.h>

#include "mdirent.h"

uint64_t my_global;
export_one_profiler_t  * export_profiler[EXPGW_EXPORTD_MAX_IDX] = { 0 };
uint32_t		 export_profiler_eid = 0;


#define NUMBER_OF_BLOCKS 1
#define NUMBER_OF_ENTRIES 384
#define MAX_CHUNK (NUMBER_OF_BLOCKS*NUMBER_OF_ENTRIES)

void faccessat_test(char *dir_path);

/**
 *    MESSAGE : message to print out
 *   root_count : number of dirent root entries
 *   loop_count  number of entries per root count
 */
#define PRINT_TIME(root_cnt,loop_count,MESSAGE) \
   gettimeofday(&tv_stop,NULL); \
   { \
      uint64_t stop_time; \
      stop_time = tv_stop.tv_sec; \
      stop_time = stop_time*1000000; \
      stop_time += tv_stop.tv_usec; \
      uint64_t start_time; \
      start_time = tv_start.tv_sec; \
      start_time = start_time*1000000; \
      start_time += tv_start.tv_usec; \
      uint64_t delay_us; \
      uint64_t delay_ns;\
      delay_us = (stop_time - start_time)/(loop_count*root_cnt); \
      delay_ns = (stop_time - start_time) -(delay_us*(loop_count*root_cnt)); \
      printf( #MESSAGE " %d\n",loop_count*root_cnt);\
      printf("Delay %llu.%llu us\n", (long long unsigned int)(stop_time - start_time)/(loop_count*root_cnt),\
          (long long unsigned int)(delay_ns*10)/(loop_count*root_cnt));   \
      printf("Delay %llu us ( %llu s)\n", (long long unsigned int)(stop_time - start_time),   \
            (long long unsigned int)(stop_time - start_time)/1000000);\
      printf("Start Delay %u s %u us\n", (unsigned int)tv_start.tv_sec,(unsigned int)tv_start.tv_usec);   \
      printf("stop Delay %u s %u us\n", (unsigned int)tv_stop.tv_sec,(unsigned int)tv_stop.tv_usec);   \
   }

uint8_t bitmap[MAX_CHUNK / 8];

mdirents_hash_entry_t hash_entry_tab[NUMBER_OF_ENTRIES];

void print_sector_offset() {
    printf("Former mdirent file size              : %d \n", (int) sizeof (mdirents_file_t));
    printf("section 0 size in 512 sectors unit    : %d \n", (int) sizeof (mdirent_sector0_t) / MDIRENT_SECTOR_SIZE);
    printf("section 1 size in 512 sectors unit    : %d \n", (int) sizeof (mdirent_sector1_t) / MDIRENT_SECTOR_SIZE);
    printf("section 2 size in 512 sectors unit    : %d \n", (int) sizeof (mdirent_sector2_t) / MDIRENT_SECTOR_SIZE);
    printf("section 3 size in 512 sectors unit    : %d \n", (int) sizeof (mdirent_sector3_t) / MDIRENT_SECTOR_SIZE);


    printf("DIRENT_HEADER_BASE_SECTOR         :%x \n", DIRENT_HEADER_BASE_SECTOR * 512);
    printf("COLL_BITMAP_BASE_SECTOR           :%x \n", (unsigned int) (DIRENT_HEADER_BASE_SECTOR * 512 + sizeof (mdirents_header_new_t)));
    printf("HASH_BITMAP_BASE_SECTOR           :%x\n", (unsigned int) (DIRENT_HEADER_BASE_SECTOR * 512 + sizeof (mdirents_btmap_coll_dirent_t) +
            sizeof (mdirents_header_new_t)));
    printf("DIRENT_NAME_BITMAP_BASE_SECTOR    :%x \n", (unsigned int) (DIRENT_NAME_BITMAP_BASE_SECTOR * 512));
    printf("DIRENT_HASH_BUCKET_BASE_SECTOR    :%x \n", (unsigned int) (DIRENT_HASH_BUCKET_BASE_SECTOR * 512));
    printf("DIRENT_HASH_ENTRIES_BASE_SECTOR   :%x \n", (unsigned int) (DIRENT_HASH_ENTRIES_BASE_SECTOR * 512));
    printf("DIRENT_HASH_NAME_BASE_SECTOR      :%x \n", (unsigned int) (DIRENT_HASH_NAME_BASE_SECTOR * 512));


}

void test_print_constants() {
    printf("mattr_t  size                    : %d\n", (int) sizeof (mattr_t));
    printf("Hash entry variables:\n");
    printf("  Number of hash entries         : %d\n", MDIRENTS_ENTRIES_COUNT);
    printf("  hash entries bitmap size       : %d bytes\n", MDIRENTS_BITMAP_FREE_HASH_SZ);
    printf("  hash entry context size        : %d bytes\n", (int) sizeof (mdirents_hash_entry_t));
    printf("  free hash entry bitmap tb size : %d bytes\n", (int) sizeof (mdirents_btmap_free_hash_t));
    printf("\n");
    printf("Hash table variables:\n");
    printf("  Number of buckets       : %d\n", MDIRENTS_HASH_TB_INT_SZ);
    printf("  hash table size         : %d bytes\n", (int) sizeof (mdirents_hash_tab_t));
    printf("\n");
    printf("Name entry variables:\n");
    printf("  chunk size                          : %d bytes\n", MDIRENTS_NAME_CHUNK_SZ);
    printf("  Max number of chunks per name entry : %d\n", (int) MDIRENTS_NAME_CHUNK_MAX);
    printf("  Number of chunks                    : %d\n", (int) MDIRENTS_NAME_CHUNK_MAX_CNT);
    printf("  chunk bitmap size                   : %d bytes\n", (int) MDIRENTS_BITMAP_FREE_NAME_SZ);
    printf("  last chunk index                    : %d \n", (int) MDIRENTS_BITMAP_FREE_NAME_LAST_BIT_IDX);
    printf("  name entry max context size         : %d bytes\n", (int) sizeof (mdirents_name_entry_t));
    printf("  free name entry bitmap tb size      : %d bytes\n", (int) sizeof (mdirents_btmap_free_chunk_t));

    printf("\n");
    printf("Dirent file min size                  : %d bytes\n", (int) sizeof (mdirents_file_t));
    printf("Dirent header size                    : %d bytes\n", (int) sizeof (mdirents_header_new_t));
    printf("section 0 not aligned                 : %d bytes\n", (int) sizeof (mdirent_sector0_not_aligned_t));
    {
        //mdirent_sector0_not_aligned_t *p = malloc(mdirent_sector0_not_aligned_t);
        //uint8_t *q;
        //q = (uint8_t*)p;
        mdirent_sector0_not_aligned_t *p = NULL;
        uint64_t offset = (uint64_t) PTR2INT (&p->coll_bitmap);
        printf(" -mdirents_btmap_coll_dirent_t offset : %llx modulo %d\n", (long long unsigned int) offset, (int) offset % 8);
        offset = (uint64_t) PTR2INT (&p->hash_bitmap);

        printf(" -hash_bitmap  offset                 : %llx modulo %d\n", (long long unsigned int) offset, (int) offset % 8);

    }
    printf("section 0 size in 512 sectors unit    : %d \n", (int) sizeof (mdirent_sector0_t) / MDIRENT_SECTOR_SIZE);
    printf("section 1 size in 512 sectors unit    : %d \n", (int) sizeof (mdirent_sector1_t) / MDIRENT_SECTOR_SIZE);
    printf("section 2 size in 512 sectors unit    : %d \n", (int) sizeof (mdirent_sector2_t) / MDIRENT_SECTOR_SIZE);
    printf("section 3 size in 512 sectors unit    : %d \n", (int) sizeof (mdirent_sector3_t) / MDIRENT_SECTOR_SIZE);

    printf("\n");
    printf("CACHE SECTION:\n");
    printf("\n");
#if 0

    typedef struct _mdirents_cache_entry_t {
        list_t cache_link; /**< linked list of the root dirent cache entries   */
        list_t coll_link; /**< linked list of the dirent collision entries    */
        //    void    *hash_ptr;    /**< pointer to the hash entry                      */
        mdirents_cache_key_t key; /**<  primary key */
        mdirents_header_new_t header; ///< header of the dirent file: mainly management information
        //    mdirent_cache_ptr_t     hash_bitmap_p;  ///< bitmap of the free hash entries
        mdirent_cache_ptr_t sect0_p; ///< header+coll entry bitmap+hash entry bitmap
        mdirent_cache_ptr_t coll_bitmap_hash_full_p; ///< bitmap of the collision dirent for which all hash entries are busy
        mdirent_cache_ptr_t name_bitmap_p; ///< bitmap of the free name/fid/type entries
        mdirent_cache_ptr_t hash_tbl_p[MDIRENTS_HASH_TB_CACHE_MAX_IDX]; ///< set of pointer to an cache array of 64 hash logical pointers
        mdirent_cache_ptr_t hash_entry_p[MDIRENTS_HASH_CACHE_MAX_IDX]; ///< table of hash entries
        mdirent_cache_ptr_t name_entry_lvl0_p[MDIRENTS_NAME_PTR_LVL0_NB_PTR]; ///< table of hash entries
        mdirent_cache_ptr_t dirent_coll_lvl0_p[MDIRENTS_CACHE_DIRENT_COLL_LVL0_NB_PTR]; ///< dirent collision file array pointers */

    } mdirents_cache_entry_t;
#endif

    printf("Cache Header part:\n");
    printf("  cache_link                          :%d\n", (int) sizeof (list_t));
    printf("  coll_link                           :%d\n", (int) sizeof (list_t));
    printf("  key                                 :%d\n", (int) sizeof (mdirents_cache_key_t));
    printf("  header                              :%d\n", (int) sizeof (mdirents_header_new_t));
    printf("\n");
    printf("Hash table bucket cache :\n");
    printf("  Dirent cache bucket size            : %d\n", DIRENT_CACHE_BUKETS);
    printf("  Max entries per cache array         : %d\n", MDIRENTS_HASH_TB_CACHE_MAX_ENTRY);
    printf("  number of entries for  cache        : %d\n", MDIRENTS_HASH_TB_CACHE_MAX_IDX);
    printf("\n");
    printf("Hash name cache :\n");
    printf("  Max entries per cache array         : %d\n", MDIRENTS_HASH_CACHE_MAX_ENTRY);
    printf("  number of entries for     cache     : %d\n", MDIRENTS_HASH_CACHE_MAX_IDX);
    printf("\n");

    printf("name entry cache :\n");
    printf("  Number of pointers per level0       : %d\n", MDIRENTS_NAME_PTR_LVL1_NB_PTR);
    printf("  Number of chunks per cache array    : %d\n", MDIRENTS_CACHE_NB_CHUNK_PER_CHUNK_ARRAY);
    printf("  number of chunks per level0 array   : %d\n", MDIRENTS_NAME_PTR_LVL0_NB_PTR);
    printf("  number of level0 index              : %d\n", MDIRENTS_NAME_PTR_LVL0_NB_PTR);
    printf("\n");

    printf("collision dirent cache ptrs :\n");
    printf("  Number of pointers per level0       : %d\n", MDIRENTS_CACHE_DIRENT_COLL_LVL0_NB_PTR);
    printf("  Number of pointers per level1       : %d\n", MDIRENTS_CACHE_DIRENT_COLL_LVL1_NB_PTR);
    printf("  Number of pointers per level2       : %d\n", MDIRENTS_CACHE_DIRENT_COLL_LVL2_NB_PTR);
    printf("\n");


    printf("size of the base dirent cache entry   : %d\n", (int) sizeof (mdirents_cache_entry_t));
    printf("virtual pointer size                  : %d\n", (int) sizeof (mdirent_cache_ptr_t));
    printf("\n");

}

/**
 *   Print out the distribution of sector associated with a dirent file
 */

void sector_distribution_print() {
    printf("DIRENT FILE layout:\n");
    printf(" -max sectors: %d\n", (int) DIRENT_FILE_MAX_SECTORS);
    printf(" -header                : %d/%d\n", (int) DIRENT_HEADER_BASE_SECTOR, (int) DIRENT_HEADER_SECTOR_CNT);
    printf(" -coll/hash bitmaps     : %d/%d\n", (int) DIRENT_NAME_BITMAP_BASE_SECTOR, (int) DIRENT_NAME_BITMAP_SECTOR_CNT);
    printf(" - hash buckets         : %d/%d\n", (int) DIRENT_HASH_BUCKET_BASE_SECTOR, (int) DIRENT_HASH_BUCKET_SECTOR_CNT);
    printf(" - hash entries         : %d/%d\n", (int) DIRENT_HASH_ENTRIES_BASE_SECTOR, (int) DIRENT_HASH_ENTRIES_SECTOR_CNT);
    printf(" - name entries         : %d/%d\n", (int) DIRENT_HASH_NAME_BASE_SECTOR, (int) DIRENT_HASH_NAME_SECTOR_CNT);

    printf("\n");




}

void test_dirent_filename() {
    char buffer[128];
    char *buf;

    printf("\ntest_dirent_filename start\n\n");
    mdirents_header_new_t header;
    header.level_index = 0;
    header.dirent_idx[0] = 1;

    buf = dirent_build_filename(&header, buffer);
    if (buf == NULL) {
        printf("test_dirent_filename error line %d\n", __LINE__);
        return;

    }
    printf("filename : %s\n", buf);
    header.level_index = 1;
    header.dirent_idx[0] = 1;
    header.dirent_idx[1] = 2;
    buf = dirent_build_filename(&header, buffer);
    if (buf == NULL) {
        printf("test_dirent_filename error line %d\n", __LINE__);
        return;

    }
    printf("filename : %s\n", buf);
    header.level_index = 2;
    header.dirent_idx[0] = 1;
    header.dirent_idx[1] = 2;
    header.dirent_idx[2] = 3;
    buf = dirent_build_filename(&header, buffer);
    if (buf == NULL) {
        printf("test_dirent_filename error line %d\n", __LINE__);
        return;

    }
    printf("filename : %s\n", buf);
    printf("\ntest_dirent_filename end\n");

}

/*
 *_____________________________________________________
   DIRENT CACHE TEST START
 *_____________________________________________________
 */
mdirents_cache_entry_t *root = NULL;

/**
 * allocation test
 */
void test_dirent_cache_allocation() {

    mdirents_header_new_t header;
    mdirents_cache_entry_t *child1, *child1_read;
    mdirents_cache_entry_t *child2;
    mdirents_cache_entry_t **child_table = NULL;
    mdirents_cache_entry_t *returned_ptr;
    //   int ret;
    int i;
    int k;

    child_table = malloc(MDIRENTS_MAX_COLLS_IDX * sizeof (mdirents_cache_entry_t*));
    if (child_table == NULL) {
        printf("test_dirent_cache_allocation: failure at %d\n", __LINE__);
        exit(0);
    }
    memset(child_table, 0, MDIRENTS_MAX_COLLS_IDX * sizeof (mdirents_cache_entry_t*));


    /*
     ** root file case
     */
    header.level_index = 0;
    header.dirent_idx[0] = 10;

    root = dirent_cache_allocate_entry(&header);
    if (root == NULL) {
        printf("test_dirent_cache_allocation: failure at %d\n", __LINE__);
        exit(0);
    }
    /*
     ** create a collision dirent cache entry
     */
    header.level_index = 1;
    header.dirent_idx[0] = 10;
    header.dirent_idx[1] = 20;
    child1 = dirent_cache_allocate_entry(&header);
    if (child1 == NULL) {
        printf("test_dirent_cache_allocation: failure at %d\n", __LINE__);
        exit(0);
    }
    /*
     ** create a collision dirent cache entry
     */
    header.level_index = 2;
    header.dirent_idx[0] = 10;
    header.dirent_idx[1] = 20;
    header.dirent_idx[2] = 30;
    child2 = dirent_cache_allocate_entry(&header);
    if (child2 == NULL) {
        printf("test_dirent_cache_allocation: failure at %d\n", __LINE__);
        exit(0);
    }

    /*
     ** store the collision index
     */
    returned_ptr = dirent_cache_store_collision_ptr(root, child1);
    if (returned_ptr != NULL) {
        printf("test_dirent_cache_allocation: failure at %d\n", __LINE__);
        exit(0);
    }
    child1_read = dirent_cache_get_collision_ptr(root, child1->header.dirent_idx[1]);
    if (child1_read == (mdirents_cache_entry_t*) NULL) {
        printf("test_dirent_cache_allocation: failure at %d\n", __LINE__);
        exit(0);
    }
    /*
     **
     check that the pointers are the same
     */
    if (child1_read != child1) {
        printf("test_dirent_cache_allocation: failure at %d: %p expect %p\n", __LINE__, child1_read, child1);
        exit(0);
    }
    /*
     ** check out of limit insertion
     */
    {

        header.level_index = 1;
        header.dirent_idx[1] = MDIRENTS_MAX_COLLS_IDX;
        child_table[0] = dirent_cache_allocate_entry(&header);
        if (child_table[0] == NULL) {
            printf("test_dirent_cache_allocation: failure at %d\n", __LINE__);
            exit(0);
        }

        returned_ptr = dirent_cache_store_collision_ptr(root, child_table[0]);
        if (returned_ptr != NULL) {
            if (returned_ptr != child_table[0]) {
                printf("test_dirent_cache_allocation: failure at %d ptr %p\n", __LINE__, returned_ptr);
                exit(0);
            }
        }
    }
    /*
     ** remove child1
     */
    child1_read = dirent_cache_del_collision_ptr(root, child1);
    if (child1_read != NULL) {
        printf("test_dirent_cache_allocation: failure at %d bad ptr %p at idx %d\n", __LINE__,
                child1_read, 0);
        exit(0);
    }


    for (i = 0; i < MDIRENTS_MAX_COLLS_IDX; i++) {
        header.level_index = 1;
        header.dirent_idx[1] = i;
        child_table[i] = dirent_cache_allocate_entry(&header);
        if (child_table[i] == NULL) {
            printf("test_dirent_cache_allocation: failure at %d\n", __LINE__);
            exit(0);
        }

        returned_ptr = dirent_cache_store_collision_ptr(root, child_table[i]);
        if (returned_ptr != NULL) {
            printf("test_dirent_cache_allocation: failure at %d for idx %d\n", __LINE__, i);
            exit(0);
        }

        for (k = 0; k < i; k++) {
            if (child_table[k]->header.dirent_idx[1] != k) {
                printf("test_dirent_cache_allocation: failure at %d idx1 %d idx2 %d ref %d\n", __LINE__,
                        i, k, child_table[k]->header.dirent_idx[1]);
                exit(0);

            }
        }

    }
    /*
     ** check all the reference of the dirent entries
     */
    for (i = 0; i < MDIRENTS_MAX_COLLS_IDX; i++) {
        if (child_table[i]->header.dirent_idx[1] != i) {
            printf("test_dirent_cache_allocation: failure at %d idx %d ref %d\n", __LINE__,
                    i, child_table[i]->header.dirent_idx[1]);
            exit(0);

        }
        if (child_table[0]->header.dirent_idx[0] != 10) {
            printf("test_dirent_cache_allocation: failure at %d idx %d ref %d\n", __LINE__,
                    10, child_table[i]->header.dirent_idx[0]);
            exit(0);

        }
    }
    /*
     ** retrieve all the pointers and check them
     */
    for (i = 0; i < MDIRENTS_MAX_COLLS_IDX; i++) {
        child1_read = dirent_cache_get_collision_ptr(root, child_table[i]->header.dirent_idx[1]);
        if (child1_read == (mdirents_cache_entry_t*) NULL) {
            printf("test_dirent_cache_allocation: failure at %d idx %d ref %d\n", __LINE__,
                    i, child_table[i]->header.dirent_idx[1]);
            exit(0);
        }
        if (child1_read != child_table[i]) {
            printf("test_dirent_cache_allocation: failure at %d: %p expect %p\n", __LINE__, child1_read, child_table[i]);
            exit(0);
        }

    }

    for (i = 0; i < MDIRENTS_MAX_COLLS_IDX; i++) {
        child1_read = dirent_cache_get_collision_ptr(root, child_table[i]->header.dirent_idx[1]);
        if (child1_read == (mdirents_cache_entry_t*) NULL) {
            printf("test_dirent_cache_allocation: failure at %d idx %d ref %d\n", __LINE__,
                    i, child_table[i]->header.dirent_idx[1]);
            exit(0);
        }
        if (child1_read != child_table[i]) {
            printf("test_dirent_cache_allocation: failure at %d: %p expect %p\n", __LINE__, child1_read, child_table[i]);
            exit(0);
        }
        child1_read = dirent_cache_del_collision_ptr(root, child_table[i]);
        if (child1_read != NULL) {
            printf("test_dirent_cache_allocation: failure at %d bad ptr %p at idx %d\n", __LINE__,
                    child1_read, i);
            exit(0);
        }
    }
    /*
     ** It  looks good , check that the virtual pointer of root are NULL
     */
    for (i = 0; i < MDIRENTS_CACHE_DIRENT_COLL_LVL0_NB_PTR; i++) {
        if (root->dirent_coll_lvl0_p[i].s.val != 0) {
            printf("test_dirent_cache_allocation: failure at %d level0 virt. ptr is not NULL at idx %d\n", __LINE__,
                    i);
            exit(0);

        }
    }






    printf("test_dirent_cache_allocation: success\n");


}

/*
 *_____________________________________________________
   file/directy insertion in dirent file (cache part)
 *_____________________________________________________
 */
/*
 ** Bucket Index statistics
 */
#define BUCKET_MODULO 256

uint32_t bucket_idx_tb[BUCKET_MODULO];

void clear_bucket_idx_array() {
    memset(bucket_idx_tb, 0, BUCKET_MODULO * sizeof (uint32_t));
}

void print_bucket_idx_stats() {
    int i;
    printf("Bucket Idx statistics:\n");
    for (i = 0; i < BUCKET_MODULO; i++) {
        if (bucket_idx_tb[i] != 0) printf("bucket %3.3d: %d\n", i, bucket_idx_tb[i]);

    }
}

void update_bucket_idx_stats(uint32_t entry) {
    bucket_idx_tb[entry % BUCKET_MODULO] += 1;
}

typedef struct _dirent_alloc_t {
    mdirents_hash_ptr_t hash; /**< reference of the hash entry             */
    int local_idx; /**< local index within a dirent cache entry */
    int hash_value; /**< value of the inserted hash             */
    int root_idx; /**< index of the root file                 */
    int bucket_idx; /**< index of bucket                 */
    mdirents_cache_entry_t *returned_entry; /**< pointer to the dirent entry   */
} dirent_alloc_t;

#if 0
void dirent_cache_alloc_name_entry_idx_test2(char *dir_path) {
    mdirents_header_new_t header;
    header.level_index = 0;
    header.dirent_idx[0] = 10;
    int i;
    struct timeval tv_start;
    struct timeval tv_stop;
    dirent_alloc_t *dirent_alloc_p = NULL;
    int loop_count = 384 * 60;
    mdirents_cache_entry_t *root;
    int root_cnt = 1;
    mdirents_cache_entry_t **root_tb = NULL;
    int root_idx;
    //   dirent_cache_search_alloc_t  cache_alloc;
    int fd_dir = -1;
    mdirents_name_entry_t name_entry;
    mdirents_name_entry_t *name_entry_p = &name_entry;

    sprintf(name_entry_p->name, "myfiletreqsdqsss_test.c");
    name_entry_p->len = strlen(name_entry_p->name);

    /*
     ** Open the directory
     */
    if ((fd_dir = open(dir_path, O_RDONLY, S_IRWXU)) < 0) {
        printf("Error on directory Open %s error %s\n", dir_path, strerror(errno));
        exit(0);
    }

    root_tb = malloc(sizeof (mdirents_cache_entry_t*) * root_cnt);

    dirent_alloc_p = malloc(sizeof (dirent_alloc_t) * loop_count * root_cnt);
    memset(dirent_alloc_p, 0xff, sizeof (dirent_alloc_t) * loop_count * root_cnt);
    printf("root_tb  size       %d MBytes\n", (int) sizeof (mdirents_cache_entry_t*) * root_cnt / 1000000);
    printf("dirent_alloc_p size %d MBytes\n", (int) sizeof (dirent_alloc_t) * loop_count * root_cnt / 1000000);
    /*
     ** create the root dirent
     */
    DIRENT_MALLOC_SIZE_CLEAR
    dirent_mem_clear_stats();
    for (root_idx = 0; root_idx < root_cnt; root_idx++) {
        header.dirent_idx[0] = root_idx;
        root = dirent_cache_create_entry(&header);
        if (root == NULL) {
            printf("dirent_cache_alloc_name_entry_idx_test2: failure at %d\n", __LINE__);
            exit(0);
        }
        root_tb[root_idx] = root;
    }
    DIRENT_MALLOC_SIZE_PRINT;
    printf("dirent_cache_alloc_name_entry_idx_test2: root dirent creation OK at %d\n", __LINE__);
    DIRENT_SKIP_STATS_CLEAR
    //   DIRENT_MALLOC_SIZE_CLEAR

    //   goto out;



    gettimeofday(&tv_start, NULL);
    clear_bucket_idx_array();
    printf("____________________________________________________________________\n");
    printf("        --->DIRENT_CACHE_ALLOCATE/INSERT/SEARCH ENTRY() test start\n");
    for (root_idx = 0; root_idx < root_cnt; root_idx++) {
        root = root_tb[root_idx];

        //     int last_coll_idx = -1;
        //     int last_local_idx = -1;
        //      printf("stats call  no_val:%d val:%d \n",check_bytes_call,check_bytes_val_call);

        for (i = 0; i < loop_count; i++) {
            //        mdirents_hash_ptr_t hash;
            int entry_idx = loop_count * root_idx + i;
            //       int local_idx;
            int bucket_idx = entry_idx % BUCKET_MODULO;
            mdirents_cache_entry_t *returned_entry;
            int bucket_idx_trace = -1;

            update_bucket_idx_stats(entry_idx);

            if (entry_idx == 416) {
                printf("FDL bug\n");
            }
            dirent_alloc_p[entry_idx].returned_entry = dirent_cache_alloc_name_entry_idx(root, bucket_idx, &dirent_alloc_p[entry_idx].hash,
                    &dirent_alloc_p[entry_idx].local_idx);
            if (dirent_alloc_p[entry_idx].returned_entry == NULL) {
                printf("dirent_cache_alloc_name_entry_idx_test: failure at %d for index %d root %d\n", __LINE__, i, root_idx);
                DIRENT_MALLOC_SIZE_PRINT;
                exit(0);
            }
            dirent_alloc_p[entry_idx].root_idx = root_idx;
            dirent_alloc_p[entry_idx].hash_value = entry_idx;
            if (bucket_idx == bucket_idx_trace) {
                printf(" entry_idx %d : root_idx %d bucket idx %d --> local_idx %d hash(%d/%d)\n",
                        entry_idx, root_idx, bucket_idx, dirent_alloc_p[entry_idx].local_idx,
                        dirent_alloc_p[entry_idx].hash.type, dirent_alloc_p[entry_idx].hash.idx);
            }
            /*
             ** insert it in the linked list
             */
            returned_entry = dirent_cache_insert_hash_entry(-1, root,
                    dirent_alloc_p[entry_idx].returned_entry,
                    bucket_idx,
                    &dirent_alloc_p[entry_idx].hash,
                    dirent_alloc_p[entry_idx].local_idx);
            if (returned_entry == NULL) {
                printf("dirent_cache_alloc_name_entry_idx_test: failure at %d for index %d root %d\n", __LINE__, i, root_idx);
                DIRENT_MALLOC_SIZE_PRINT;
                exit(0);
            }
            {
                /**
                 ** insert the value of the hash
                 */
                mdirents_hash_entry_t *hash_entry_p;
                hash_entry_p = (mdirents_hash_entry_t*) DIRENT_CACHE_GET_HASH_ENTRY_PTR(dirent_alloc_p[entry_idx].returned_entry,
                        dirent_alloc_p[entry_idx].local_idx);
                if (hash_entry_p == NULL) {
                    /*
                     ** something wrong!! (either the index is out of range and the memory array has been released
                     */
                    printf("dirent_cache_alloc_name_entry_idx_test: failure at %d for index %d root %d\n", __LINE__, i, root_idx);
                    DIRENT_MALLOC_SIZE_PRINT;
                    exit(0);
                }
                hash_entry_p->hash = dirent_alloc_p[entry_idx].hash_value;
#if 0
                {
                    /*
                     ** insert the name entry
                     */
                    uint8_t *p8;

                    p8 = (uint8_t*) dirent_create_entry_name(fd_dir, dirent_alloc_p[entry_idx].returned_entry,
                            name_entry_p,
                            hash_entry_p);
                    if (p8 == NULL) {

                        printf("dirent_create_entry_name: failure at %d for index %d root %d\n", __LINE__, i, root_idx);
                        DIRENT_MALLOC_SIZE_PRINT;
                        exit(0);
                    }
                }
#endif
            }
            {
#if 1
                int ret;
                /*
                 ** Write the file on disk: it might the root and collision file, root only, collision file only
                 */
                /*
                 ** write the dirent file on which the entry has been inserted:
                 */
                ret = write_mdirents_file(fd_dir, dirent_alloc_p[entry_idx].returned_entry);
                if (ret < 0) {
                    printf("Error on writing file at line %d\n", __LINE__);
                    exit(0);

                }
                /*
                 ** check if another dirent cache entry  needs to be re-written on disk
                 */
                if (returned_entry != dirent_alloc_p[entry_idx].returned_entry) {
                    /*
                     ** write the root file:
                     */
                    // printf("Write Root file\n");
                    ret = write_mdirents_file(fd_dir, returned_entry);
                    if (ret < 0) {
                        printf("Error on writing file at line %d\n", __LINE__);
                        exit(0);

                    }
                }
                /*
                 ** Check if root needs to be re-written on disk :
                 **   - adding a new collision file
                 **   - update of a bucket entry
                 **   - update of a hash entry (pnext)
                 */
                if (DIRENT_IS_ROOT_UPDATE_REQ(root)) {
                    ret = write_mdirents_file(fd_dir, root);
                    if (ret < 0) {
                        printf("Error on writing file at line %d\n", __LINE__);
                        exit(0);
                    }
                }
#endif

            }
            /**
             * search back the entry
             */
            {
                int local_idx;

                returned_entry = dirent_cache_search_hash_entry(fd_dir, root,
                        bucket_idx,
                        dirent_alloc_p[entry_idx].hash_value,
                        &local_idx,
                        NULL, // Name
                        0, // len
                        NULL, // name_entry_p,
                        NULL // hash_entry_p
                        );
                if (returned_entry == NULL) {
                    printf(" dirent_cache_search_hash_entry test error on line %d for index %d \n", __LINE__, entry_idx);
                    printf(" entry_idx %d : root_idx %d bucket idx %d --> local_idx %d hash(%d/%d)\n",
                            entry_idx, root_idx, bucket_idx, dirent_alloc_p[entry_idx].local_idx,
                            dirent_alloc_p[entry_idx].hash.type, dirent_alloc_p[entry_idx].hash.idx);
                    exit(0);
                }
                if (returned_entry != dirent_alloc_p[entry_idx].returned_entry) {
                    printf(" dirent_cache_search_hash_entry test error on line %d for index %d\n", __LINE__, i);
                    printf(" entry_idx %d : root_idx %d bucket idx %d --> local_idx %d hash(%d/%d)\n",
                            entry_idx, root_idx, bucket_idx, dirent_alloc_p[entry_idx].local_idx,
                            dirent_alloc_p[entry_idx].hash.type, dirent_alloc_p[entry_idx].hash.idx);
                    exit(0);
                }
            }

            /*
                  if (dirent_alloc_p[i].hash.type == MDIRENTS_HASH_PTR_COLL)
                  {
                    if (last_coll_idx != dirent_alloc_p[i].hash.idx)
                    {
                      if (last_coll_idx != -1)
                      {
                         printf("collision idx %d last_local_idx %d\n",last_coll_idx,last_local_idx);

                      }
                      last_coll_idx = dirent_alloc_p[i].hash.idx;
                    }
                    else
                    {
                      last_local_idx = dirent_alloc_p[i].local_idx;
                    }
                  }
                  if (i%(384*2) == 0) printf("%d\n",i);
             */
            //        if (i%(4000) == 0){ printf("%d\n",i); DIRENT_MALLOC_SIZE_PRINT; }

            //      printf("%d root %p ret %p lcl_idx %d type %d idx %d\n",i,root,dirent_alloc_p[i].returned_entry,
            //                dirent_alloc_p[i].local_idx,dirent_alloc_p[i].hash.type,dirent_alloc_p[i].hash.idx);

        }
        //     printf("Next Root index\n");
    }
    PRINT_TIME(root_cnt, loop_count, Number of entries created);


    printf("stats call  no_val:%d val:%d \n", check_bytes_call, check_bytes_val_call);

    printf("stats 64:%d 32:%d  16:%d\n", dirent_skip_64_cnt, dirent_skip_32_cnt, dirent_skip_16_cnt);
    DIRENT_MALLOC_SIZE_PRINT;
    printf(" Number of bytes per entry %llu\n", (long long unsigned int) malloc_size / (loop_count * root_cnt));
    dirent_mem_print_stats_per_size();

    //    print_bucket_idx_stats();

    /*
     ** now proceed with the control of the pointers
     */
    int empty_entries = 0;
    int local_entries = 0;
    mdirents_cache_entry_t *child_read;
    for (i = 0; i < loop_count * root_cnt; i++) {
        root = root_tb[dirent_alloc_p[i].root_idx];
        if (dirent_alloc_p[i].local_idx == -1) {
            empty_entries++;
            continue;
        }
        if (dirent_alloc_p[i].hash.type == MDIRENTS_HASH_PTR_LOCAL) {
            local_entries++;
            continue;
        }
        if (dirent_alloc_p[i].hash.type != MDIRENTS_HASH_PTR_COLL) {
            printf(" dirent_cache_alloc_name_entry_idx_test error on line %d\n", __LINE__);
            exit(0);
        }
#if 0
        if (dirent_alloc_p[i].hash.level != 0) {
            printf(" dirent_cache_alloc_name_entry_idx_test error on line %d: hash level %d entry %d \n", __LINE__,
                    dirent_alloc_p[i].hash.level, i);
            exit(0);
        }
#endif
        child_read = dirent_cache_get_collision_ptr(root, dirent_alloc_p[i].hash.idx);
        if (child_read == (mdirents_cache_entry_t*) NULL) {
            printf(" dirent_cache_alloc_name_entry_idx_test error on line %d\n", __LINE__);
            exit(0);
        }
        if (child_read != dirent_alloc_p[i].returned_entry) {
            printf(" dirent_cache_alloc_name_entry_idx_test error on line %d\n", __LINE__);
            exit(0);
        }

    }
    printf("dirent_cache_alloc_name_entry_idx_test: success \n");
    /*
     **______________________________________________________________________________
     */
    /*
     ** Now proceed with the search within the cache
     */
    /*
     **______________________________________________________________________________
     */
    dirent_cache_lookup_clear_stats();
    printf("____________________________________________________________________\n");
    printf("        --->DIRENT_CACHE_SEARCH_HASH_ENTRY() test start\n");
    gettimeofday(&tv_start, NULL);

    {
        //      int empty_entries = 0;
        //      int local_entries = 0;
        //      int coll_entries  = 0;
        mdirents_cache_entry_t *returned_entry;
        int local_idx;
        int bucket_idx;
        for (i = 0; i < loop_count * root_cnt; i++) {
            root = root_tb[dirent_alloc_p[i].root_idx];
            bucket_idx = i % BUCKET_MODULO;
#if 0
            returned_entry = dirent_cache_search_hash_entry(fd, root,
                    bucket_idx,
                    dirent_alloc_p[i].hash_value,
                    &local_idx,
                    NULL, // Name
                    0, // len
                    NULL, // name_entry_p,
                    NULL // hash_entry_p
                    );

#else
            returned_entry = dirent_cache_search_and_alloc_hash_entry(root,
                    bucket_idx,
                    dirent_alloc_p[i].hash_value,
                    &local_idx,
                    NULL);
#endif

#if 1
            if (returned_entry == NULL) {
                printf(" dirent_cache_search_hash_entry test error on line %d for index %d \n", __LINE__, i);
                exit(0);
            }
            if (returned_entry != dirent_alloc_p[i].returned_entry) {
                printf(" dirent_cache_search_hash_entry test error on line %d for index %d\n", __LINE__, i);
                exit(0);
            }
#endif
        }

        PRINT_TIME(root_cnt, loop_count, Number of entries searched);
        dirent_cache_lookup_print_stats();
        printf("--->dirent_cache_search_hash_entry() test end\n\n");


    }

    /*
     **______________________________________________________________________________
     */
    /*
     ** Now proceed with the removing of the hash entry
     */
    /*
     **______________________________________________________________________________
     */
    //     dirent_cache_print_bucket_list(root_tb[0],127);

    goto remove_entries;

    dirent_cache_lookup_clear_stats();
    printf("____________________________________________________________________\n");
    printf("      --->DIRENT_CACHE_DELETE_HASH_ENTRY() test start\n");
    gettimeofday(&tv_start, NULL);

    {


        mdirents_cache_entry_t *returned_entry;
        mdirents_cache_entry_t *returned_prev_entry;
        int local_idx;
        int idx2check;
        int bucket_idx;
        for (i = 0; i < loop_count * root_cnt; i++) {
            idx2check = (loop_count * root_cnt - 1) - i;
            //        idx2check = i;

            root = root_tb[dirent_alloc_p[idx2check].root_idx];
            bucket_idx = idx2check % BUCKET_MODULO;
            if (dirent_alloc_p[idx2check].hash_value == 3904) {
                printf("Bug delete\n");

            }
            /*        if (bucket_idx == 127)
                    {
                       printf("hash value %d\n",idx2check);

                    }
             */
            returned_entry = dirent_cache_delete_hash_entry(fd_dir, root,
                    bucket_idx,
                    dirent_alloc_p[idx2check].hash_value,
                    &local_idx,
                    &returned_prev_entry,
                    NULL, // name
                    0, // len
                    (void*) NULL, // fid
                    NULL // mode
                    );
            if (returned_entry == NULL) {
                printf(" dirent_cache_search_hash_entry test error on line %d for index %d (local %d) \n", __LINE__, dirent_alloc_p[idx2check].hash_value, i);
                printf("     --> Bucket Idx = %d\n", bucket_idx);
                printf("     --> Hash value = %d\n", dirent_alloc_p[idx2check].hash_value);
                printf("     --> Local Idx  = %d\n", dirent_alloc_p[idx2check].local_idx);

                dirent_cache_print_bucket_list(root_tb[0], 127); // FDL debug
                exit(0);
            }
            if (returned_entry != dirent_alloc_p[idx2check].returned_entry) {
                printf(" dirent_cache_delete_hash_entry test error on line %d for index %d\n", __LINE__, idx2check);
                exit(0);
            }
            if (dirent_alloc_p[idx2check].hash_value == -1) {
                dirent_cache_print_bucket_list(root_tb[0], 127);
            }

#if 1
            int ret;
            mdirent_sector0_not_aligned_t *sect0_p;
            sect0_p = DIRENT_VIRT_TO_PHY_OFF(returned_entry, sect0_p);
            /*
             ** check if the dirent cache entry from which the entry has been removed is now empty
             */
            ret = dirent_cache_entry_check_empty(returned_entry);
            switch (ret) {
                case 0:
                    /*
                     ** not empty
                     */
                    break;
                case 1:
                    /*
                     ** empty
                     */
                    //               printf("Entry is level %d  ref: %d_%d is empty\n",sect0_p->header.level_index,
                    //                                                                 sect0_p->header.dirent_idx[0],
                    //                                                                 sect0_p->header.dirent_idx[1]);
                    /*
                     ** if the entry is not root: we need to update the bitmap of the root
                     */
                    if (returned_entry != root) {

                        if (dirent_cache_del_collision_ptr(root, returned_entry) != NULL) {
                            printf(" ERROR  while deleting the collision ptr\n");
                            exit(0);
                        }
                    }

                    /*
                     ** OK, now release the associated memory
                     */
                    if (dirent_cache_release_entry(returned_entry) < 0) {
                        printf(" ERROR  dirent_cache_release_entry\n");
                        exit(0);
                    }

                    /*
                     ** remove the file
                     */
                {
                    char pathname[64];
                    char *path_p;
                    int flags = 0;
                    int ret;

                    /*
                     ** build the filename of the dirent file to read
                     */
                    path_p = dirent_build_filename(&returned_entry->header, pathname);
                    if (path_p == NULL) {
                        /*
                         ** something wrong that must not happen
                         */
                        DIRENT_SEVERE("Cannot build filename( line %d\n)", __LINE__);
                        exit(0);
                    }
#if 1
                    ret = unlinkat(fd_dir, path_p, flags);
                    if (ret < 0) {
                        DIRENT_SEVERE("Cannot remove file %s: %s( line %d\n)", path_p, strerror(errno), __LINE__);
                        exit(0);
                    }
#endif
                }
                    if (returned_entry == root) root = NULL;
                    returned_entry = NULL;
                    break;

                default:
                    printf("Error on line %d : sector0 pointer is wrong\n", __LINE__);
                    exit(0);
                    break;

            }
            /*
             ** Write the file on disk: it might the root and collision file, root only, collision file only
             */
            /*
             ** write the dirent file on which the entry has been inserted:
             */
            if ((returned_entry != NULL) && (DIRENT_IS_ROOT_UPDATE_REQ(returned_entry))) {
                ret = write_mdirents_file(fd_dir, returned_entry);
                if (ret < 0) {
                    printf("Error on writing file at line %d\n", __LINE__);
                }
            }
            /*
             ** check if another dirent cache entry  needs to be re-written on disk
             */
            if ((returned_prev_entry != NULL) && (DIRENT_IS_ROOT_UPDATE_REQ(returned_prev_entry))) {
                /*
                 ** write the root file:
                 */
                // printf("Write Root file\n");
                ret = write_mdirents_file(fd_dir, returned_prev_entry);
                if (ret < 0) {
                    printf("Error on writing file at line %d\n", __LINE__);

                }
            }
            /*
             ** Check if root needs to be re-written on disk :
             **   - adding a new collision file
             **   - update of a bucket entry
             **   - update of a hash entry (pnext)
             */
            if ((root != NULL) && (DIRENT_IS_ROOT_UPDATE_REQ(root))) {
                ret = write_mdirents_file(fd_dir, root);
                if (ret < 0) {
                    printf("Error on writing file at line %d\n", __LINE__);
                }
            }
#endif


        }
        PRINT_TIME(root_cnt, loop_count, Number of entries deleted);
        dirent_cache_lookup_print_stats();
        printf("--->dirent_cache_delete_hash_entry() test end\n\n");
    }
    /*
     **______________________________________________________________________________
     */
    /*
     ** Control that all the bucket are empty
     */
#if 0
    printf("____________________________________________________________________\n");
    printf("      --->DIRENT_CACHE_DELETE_HASH_ENTRY() : empty Bucket control\n");
    {
        int bucket_id = 0;
        for (i = 0; i < root_cnt; i++) {
            for (bucket_id = 0; bucket_id < 256; bucket_id++) {
                dirent_cache_print_bucket_list(root_tb[i], bucket_id);
            }

        }
    }
#endif
    /*
     **______________________________________________________________________________
     */
    //  printf("FDL temporay End of test\n");
    //   exit(0);

#if 0
    /*
     **______________________________________________________________________________
     */
    /*
     ** Lookup of the almost last index
     */
    /*
     **______________________________________________________________________________
     */
    {
        dirent_cache_lookup_clear_stats();

        mdirents_cache_entry_t *returned_entry;
        int local_idx;
        int bucket_idx;
        i = loop_count * root_cnt - 100;

        printf(" Lookup for entry %d\n", i);

        root = root_tb[dirent_alloc_p[i].root_idx];
        bucket_idx = i % BUCKET_MODULO;

        returned_entry = dirent_cache_search_hash_entry(root,
                bucket_idx,
                dirent_alloc_p[i].hash_value,
                &local_idx,
                NULL, // Name
                0, // len
                NULL, // name_entry_p,
                NULL // hash_entry_p
                );
        if (returned_entry == NULL) {
            printf(" dirent_cache_search_hash_entry test error on line %d for index %d \n", __LINE__, i);
            exit(0);
        }
        if (returned_entry != dirent_alloc_p[i].returned_entry) {
            printf(" dirent_cache_search_hash_entry test error on line %d for index %d\n", __LINE__, i);
            exit(0);
        }
        dirent_cache_lookup_print_stats();

    }
#endif
    /*
     **______________________________________________________________________________
     */
    /*
     ** Deletion of a dirent cache entry
     **
     ** The goal of the test is to remove al the cache entry by removing the
     ** root cache entry first
     */
    /*
     **______________________________________________________________________________
     */
remove_entries:
    {

        dirent_cache_lookup_clear_stats();
        printf("____________________________________________________________________\n");
        printf("      --->DIRENT_REMOVE_ALL_CACHE_ENTRIES() test start\n");
        gettimeofday(&tv_start, NULL);

        int ret;
        for (root_idx = 0; root_idx < root_cnt; root_idx++) {
            root = root_tb[root_idx];

            ret = dirent_cache_release_entry(root);
            if (ret < 0) {
                printf(" dirent_cache_release_entry test error on line %d for index %d \n", __LINE__, root_idx);
                exit(0);
            }
        }
    }
    PRINT_TIME(root_cnt, 1, Number of entries removed from cache);
    printf("--->dirent_cache_remove_entry() test end\n\n");

    //out:
    if (root_tb != NULL) free(root_tb);
    if (dirent_alloc_p != NULL) free(dirent_alloc_p);

    if (fd_dir != -1) close(fd_dir);
}
#endif

/**
 *__________________________________________________________________
 *
 *   TEST 3
 *__________________________________________________________________
 *
 */

#if 0
void dirent_cache_alloc_name_entry_idx_test3(char *dir_path) {
    mdirents_header_new_t header;
    header.level_index = 0;
    header.dirent_idx[0] = 10;
    int i;
    struct timeval tv_start;
    struct timeval tv_stop;
    dirent_alloc_t *dirent_alloc_p = NULL;
    int loop_count = 384 * 24;
    mdirents_cache_entry_t *root;
    int root_cnt = 1024;
    mdirents_cache_entry_t **root_tb = NULL;
    int root_idx;
    //   dirent_cache_search_alloc_t  cache_alloc;
    int fd_dir = -1;
    mdirents_name_entry_t name_entry;
    mdirents_name_entry_t *name_entry_p = &name_entry;

    sprintf(name_entry_p->name, "myfiletreqsdqsss_test.c");
    name_entry_p->len = strlen(name_entry_p->name);

    /*
     ** Open the directory
     */
    if ((fd_dir = open(dir_path, O_RDONLY, S_IRWXU)) < 0) {
        printf("Error on directory Open %s error %s\n", dir_path, strerror(errno));
        exit(0);
    }

    root_tb = malloc(sizeof (mdirents_cache_entry_t*) * root_cnt);

    dirent_alloc_p = malloc(sizeof (dirent_alloc_t) * loop_count * root_cnt);
    memset(dirent_alloc_p, 0xff, sizeof (dirent_alloc_t) * loop_count * root_cnt);
    printf("root_tb  size       %d MBytes\n", (int) sizeof (mdirents_cache_entry_t*) * root_cnt / 1000000);
    printf("dirent_alloc_p size %d MBytes\n", (int) sizeof (dirent_alloc_t) * loop_count * root_cnt / 1000000);
    /*
     ** create the root dirent
     */
    DIRENT_MALLOC_SIZE_CLEAR
    dirent_mem_clear_stats();
    for (root_idx = 0; root_idx < root_cnt; root_idx++) {
        header.dirent_idx[0] = root_idx;
        root = dirent_cache_create_entry(&header);
        if (root == NULL) {
            printf("dirent_cache_alloc_name_entry_idx_test2: failure at %d\n", __LINE__);
            exit(0);
        }
        root_tb[root_idx] = root;
    }
    DIRENT_MALLOC_SIZE_PRINT;
    printf("dirent_cache_alloc_name_entry_idx_test2: root dirent creation OK at %d\n", __LINE__);
    DIRENT_SKIP_STATS_CLEAR
    //   DIRENT_MALLOC_SIZE_CLEAR

    //   goto out;



    gettimeofday(&tv_start, NULL);
    clear_bucket_idx_array();
    printf("____________________________________________________________________\n");
    printf("        --->DIRENT_CACHE_ALLOCATE/INSERT/SEARCH ENTRY() test start\n");
    for (root_idx = 0; root_idx < root_cnt; root_idx++) {
        root = root_tb[root_idx];

        //     int last_coll_idx = -1;
        //     int last_local_idx = -1;
        //      printf("stats call  no_val:%d val:%d \n",check_bytes_call,check_bytes_val_call);

        for (i = 0; i < loop_count; i++) {
            //        mdirents_hash_ptr_t hash;
            int entry_idx = loop_count * root_idx + i;
            //       int local_idx;
            int bucket_idx = entry_idx % BUCKET_MODULO;
            mdirents_cache_entry_t *returned_entry;
            int bucket_idx_trace = -1;

            update_bucket_idx_stats(entry_idx);

            if (entry_idx == 416) {
                printf("FDL bug\n");
            }
            dirent_alloc_p[entry_idx].returned_entry = dirent_cache_alloc_name_entry_idx(root, bucket_idx, &dirent_alloc_p[entry_idx].hash,
                    &dirent_alloc_p[entry_idx].local_idx);
            if (dirent_alloc_p[entry_idx].returned_entry == NULL) {
                printf("dirent_cache_alloc_name_entry_idx_test: failure at %d for index %d root %d\n", __LINE__, i, root_idx);
                DIRENT_MALLOC_SIZE_PRINT;
                exit(0);
            }
            dirent_alloc_p[entry_idx].root_idx = root_idx;
            dirent_alloc_p[entry_idx].hash_value = entry_idx;
            if (bucket_idx == bucket_idx_trace) {
                printf(" entry_idx %d : root_idx %d bucket idx %d --> local_idx %d hash(%d/%d)\n",
                        entry_idx, root_idx, bucket_idx, dirent_alloc_p[entry_idx].local_idx,
                        dirent_alloc_p[entry_idx].hash.type, dirent_alloc_p[entry_idx].hash.idx);
            }
            /*
             ** insert it in the linked list
             */
            returned_entry = dirent_cache_insert_hash_entry(-1, root,
                    dirent_alloc_p[entry_idx].returned_entry,
                    bucket_idx,
                    &dirent_alloc_p[entry_idx].hash,
                    dirent_alloc_p[entry_idx].local_idx);
            if (returned_entry == NULL) {
                printf("dirent_cache_alloc_name_entry_idx_test: failure at %d for index %d root %d\n", __LINE__, i, root_idx);
                DIRENT_MALLOC_SIZE_PRINT;
                exit(0);
            }
            mdirents_hash_entry_t *hash_entry_p;
            {
                /**
                 ** insert the value of the hash
                 */
                hash_entry_p = (mdirents_hash_entry_t*) DIRENT_CACHE_GET_HASH_ENTRY_PTR(dirent_alloc_p[entry_idx].returned_entry,
                        dirent_alloc_p[entry_idx].local_idx);
                if (hash_entry_p == NULL) {
                    /*
                     ** something wrong!! (either the index is out of range and the memory array has been released
                     */
                    printf("dirent_cache_alloc_name_entry_idx_test: failure at %d for index %d root %d\n", __LINE__, i, root_idx);
                    DIRENT_MALLOC_SIZE_PRINT;
                    exit(0);
                }
                hash_entry_p->hash = dirent_alloc_p[entry_idx].hash_value;
#if 1
                {
                    /*
                     ** insert the name entry
                     */
                    uint8_t *p8;

                    p8 = (uint8_t*) dirent_create_entry_name(fd_dir, dirent_alloc_p[entry_idx].returned_entry,
                            name_entry_p,
                            hash_entry_p);
                    if (p8 == NULL) {

                        printf("dirent_create_entry_name: failure at %d for index %d root %d\n", __LINE__, i, root_idx);
                        DIRENT_MALLOC_SIZE_PRINT;
                        exit(0);
                    }
                }
#endif
            }



            {
#if 1
                int ret;
                /*
                 ** Write the file on disk: it might the root and collision file, root only, collision file only
                 */
                /*
                 ** write the dirent file on which the entry has been inserted:
                 */
                ret = write_mdirents_file(fd_dir, dirent_alloc_p[entry_idx].returned_entry);
                if (ret < 0) {
                    printf("Error on writing file at line %d\n", __LINE__);

                }
                /*
                 ** check if another dirent cache entry  needs to be re-written on disk
                 */
                if (returned_entry != dirent_alloc_p[entry_idx].returned_entry) {
                    /*
                     ** write the root file:
                     */
                    // printf("Write Root file\n");
                    ret = write_mdirents_file(fd_dir, returned_entry);
                    if (ret < 0) {
                        printf("Error on writing file at line %d\n", __LINE__);

                    }
                }
                /*
                 ** Check if root needs to be re-written on disk :
                 **   - adding a new collision file
                 **   - update of a bucket entry
                 **   - update of a hash entry (pnext)
                 */
                if (DIRENT_IS_ROOT_UPDATE_REQ(root)) {
                    ret = write_mdirents_file(fd_dir, root);
                    if (ret < 0) {
                        printf("Error on writing file at line %d\n", __LINE__);
                    }
                }
#endif

            }
            /**
             * search back the entry
             */
            {
                int local_idx;

                returned_entry = dirent_cache_search_hash_entry(fd_dir, root,
                        bucket_idx,
                        dirent_alloc_p[entry_idx].hash_value,
                        &local_idx,
                        NULL, // Name
                        0, // len
                        NULL, // name_entry_p,
                        NULL // hash_entry_p
                        );
                if (returned_entry == NULL) {
                    printf(" dirent_cache_search_hash_entry test error on line %d for index %d \n", __LINE__, entry_idx);
                    printf(" entry_idx %d : root_idx %d bucket idx %d --> local_idx %d hash(%d/%d)\n",
                            entry_idx, root_idx, bucket_idx, dirent_alloc_p[entry_idx].local_idx,
                            dirent_alloc_p[entry_idx].hash.type, dirent_alloc_p[entry_idx].hash.idx);
                    exit(0);
                }
                if (returned_entry != dirent_alloc_p[entry_idx].returned_entry) {
                    printf(" dirent_cache_search_hash_entry test error on line %d for index %d\n", __LINE__, i);
                    printf(" entry_idx %d : root_idx %d bucket idx %d --> local_idx %d hash(%d/%d)\n",
                            entry_idx, root_idx, bucket_idx, dirent_alloc_p[entry_idx].local_idx,
                            dirent_alloc_p[entry_idx].hash.type, dirent_alloc_p[entry_idx].hash.idx);
                    exit(0);
                }
            }
        }
        //     printf("Next Root index\n");
    }
    PRINT_TIME(root_cnt, loop_count, Number of entries created);


    printf("stats call  no_val:%d val:%d \n", check_bytes_call, check_bytes_val_call);

    printf("stats 64:%d 32:%d  16:%d\n", dirent_skip_64_cnt, dirent_skip_32_cnt, dirent_skip_16_cnt);
    DIRENT_MALLOC_SIZE_PRINT;
    printf(" Number of bytes per entry %d\n", (int) malloc_size / (loop_count * root_cnt));
    dirent_mem_print_stats_per_size();

    //    print_bucket_idx_stats();

    /*
     ** now proceed with the control of the pointers
     */
    int empty_entries = 0;
    int local_entries = 0;
    mdirents_cache_entry_t *child_read;
    for (i = 0; i < loop_count * root_cnt; i++) {
        root = root_tb[dirent_alloc_p[i].root_idx];
        if (dirent_alloc_p[i].local_idx == -1) {
            empty_entries++;
            continue;
        }
        if (dirent_alloc_p[i].hash.type == MDIRENTS_HASH_PTR_LOCAL) {
            local_entries++;
            continue;
        }
        if (dirent_alloc_p[i].hash.type != MDIRENTS_HASH_PTR_COLL) {
            printf(" dirent_cache_alloc_name_entry_idx_test error on line %d\n", __LINE__);
            exit(0);
        }
#if 0
        if (dirent_alloc_p[i].hash.level != 0) {
            printf(" dirent_cache_alloc_name_entry_idx_test error on line %d: hash level %d entry %d \n", __LINE__,
                    dirent_alloc_p[i].hash.level, i);
            exit(0);
        }
#endif
        child_read = dirent_cache_get_collision_ptr(root, dirent_alloc_p[i].hash.idx);
        if (child_read == (mdirents_cache_entry_t*) NULL) {
            printf(" dirent_cache_alloc_name_entry_idx_test error on line %d\n", __LINE__);
            exit(0);
        }
        if (child_read != dirent_alloc_p[i].returned_entry) {
            printf(" dirent_cache_alloc_name_entry_idx_test error on line %d\n", __LINE__);
            exit(0);
        }

    }
    printf("dirent_cache_alloc_name_entry_idx_test: success \n");

    /*
     **______________________________________________________________________________
     */
    /*
     ** Now remove all the entries
     */
    /*
     **______________________________________________________________________________
     */
    {

        dirent_cache_lookup_clear_stats();
        printf("____________________________________________________________________\n");
        printf("      --->DIRENT_REMOVE_ALL_CACHE_ENTRIES() test start\n");
        gettimeofday(&tv_start, NULL);

        int ret;
        for (root_idx = 0; root_idx < root_cnt; root_idx++) {
            root = root_tb[root_idx];

            ret = dirent_cache_release_entry(root);
            if (ret < 0) {
                printf(" dirent_cache_release_entry test error on line %d for index %d \n", __LINE__, root_idx);
                exit(0);
            }
        }
    }
    PRINT_TIME(root_cnt, loop_count, Number of entries removed from cache);
    printf("--->dirent_cache_remove_entry() test end\n\n");



    /*
     **______________________________________________________________________________
     */
    /*
     ** Now re-read from disk
     */
    /*
     **______________________________________________________________________________
     */
    {

        dirent_cache_lookup_clear_stats();
        mdirents_header_new_t dirent_hdr;
        uint64_t size_before;
        uint64_t size_after;
        size_before = DIRENT_MALLOC_GET_CURRENT_SIZE();

        printf("____________________________________________________________________\n");
        printf("      --->RE-READ DIRENT FROM DISK() test start\n");
        gettimeofday(&tv_start, NULL);

        //int  ret;
        for (root_idx = 0; root_idx < root_cnt; root_idx++) {
            dirent_hdr.type = MDIRENT_CACHE_FILE_TYPE;
            dirent_hdr.level_index = 0;
            dirent_hdr.dirent_idx[0] = root_idx;
            dirent_hdr.dirent_idx[1] = 0;
            root_tb[root_idx] = read_mdirents_file(fd_dir, &dirent_hdr);
            if (root_tb[root_idx] == NULL) {
                printf(" re-read from disk error test error on line %d for index %d \n", __LINE__, root_idx);
                exit(0);
            }
        }
        size_after = DIRENT_MALLOC_GET_CURRENT_SIZE();
        if (loop_count / 384 == 0) {
            PRINT_TIME(root_cnt, 1, Number of dirent root re_read from disk);
        } else {
            PRINT_TIME(root_cnt, loop_count / 384, Number of dirent root re_read from disk);
        }
        if (root_cnt == 1) {
            printf("Total Memory for %d entries : %llu KBytes (%llu Bytes)\n", loop_count,
                    (long long unsigned int) (size_after - size_before) / (1024),
                    (long long unsigned int) (size_after - size_before));

        }
    }
    printf("--->dirent re_read from disk test end\n\n");

    /*
     **______________________________________________________________________________
     */
    /*
     ** Now proceed with the search within the cache
     */
    /*
     **______________________________________________________________________________
     */
    dirent_cache_lookup_clear_stats();
    printf("____________________________________________________________________\n");
    printf("        --->DIRENT_CACHE_SEARCH_HASH_ENTRY() test start\n");
    gettimeofday(&tv_start, NULL);

    {
        //      int empty_entries = 0;
        //      int local_entries = 0;
        //      int coll_entries  = 0;
        mdirents_cache_entry_t *returned_entry;
        int local_idx;
        int bucket_idx;
        for (i = 0; i < loop_count * root_cnt; i++) {
            root = root_tb[dirent_alloc_p[i].root_idx];
            bucket_idx = i % BUCKET_MODULO;
#if 1
            returned_entry = dirent_cache_search_hash_entry(fd_dir, root,
                    bucket_idx,
                    dirent_alloc_p[i].hash_value,
                    &local_idx,
                    NULL, // Name
                    0, // len
                    NULL, // name_entry_p,
                    NULL // hash_entry_p
                    );

#else
            returned_entry = dirent_cache_search_and_alloc_hash_entry(root,
                    bucket_idx,
                    dirent_alloc_p[i].hash_value,
                    &local_idx,
                    NULL);
#endif

#if 1
            if (returned_entry == NULL) {
                printf(" dirent_cache_search_hash_entry test error on line %d for index %d \n", __LINE__, i);
                exit(0);
            }
#if 0
            if (returned_entry != dirent_alloc_p[i].returned_entry) {
                printf(" dirent_cache_search_hash_entry test error on line %d for index %d\n", __LINE__, i);
                exit(0);
            }
#endif
#endif
        }

        PRINT_TIME(root_cnt, loop_count, Number of entries searched);
        dirent_cache_lookup_print_stats();
        printf("--->dirent_cache_search_hash_entry() test end\n\n");


    }

    /*
     **______________________________________________________________________________
     */
    /*
     ** Now proceed with the removing of the hash entry
     */
    /*
     **______________________________________________________________________________
     */
    //     dirent_cache_print_bucket_list(root_tb[0],127);

    goto remove_entries;

    dirent_cache_lookup_clear_stats();
    printf("____________________________________________________________________\n");
    printf("      --->DIRENT_CACHE_DELETE_HASH_ENTRY() test start\n");
    gettimeofday(&tv_start, NULL);

    {


        mdirents_cache_entry_t *returned_entry;
        mdirents_cache_entry_t *returned_prev_entry;
        int local_idx;
        int idx2check;
        int bucket_idx;
        for (i = 0; i < loop_count * root_cnt; i++) {
            idx2check = (loop_count * root_cnt - 1) - i;
            //        idx2check = i;

            root = root_tb[dirent_alloc_p[idx2check].root_idx];
            bucket_idx = idx2check % BUCKET_MODULO;
            if (dirent_alloc_p[idx2check].hash_value == 3904) {
                printf("Bug delete\n");

            }
            /*        if (bucket_idx == 127)
                    {
                       printf("hash value %d\n",idx2check);

                    }
             */
            returned_entry = dirent_cache_delete_hash_entry(fd_dir, root,
                    bucket_idx,
                    dirent_alloc_p[idx2check].hash_value,
                    &local_idx,
                    &returned_prev_entry,
                    NULL, //name
                    0, // len
                    (void*) NULL, // fid
                    NULL // mode
                    );
            if (returned_entry == NULL) {
                printf(" dirent_cache_delete_hash_entry test error on line %d for index %d (local %d) \n", __LINE__, dirent_alloc_p[idx2check].hash_value, i);
                printf("     --> Bucket Idx = %d\n", bucket_idx);
                printf("     --> Hash value = %d\n", dirent_alloc_p[idx2check].hash_value);
                printf("     --> Local Idx  = %d\n", dirent_alloc_p[idx2check].local_idx);

                dirent_cache_print_bucket_list(root_tb[0], 127); // FDL debug
                exit(0);
            }
            if (returned_entry != dirent_alloc_p[idx2check].returned_entry) {
                printf(" dirent_cache_delete_hash_entry test error on line %d for index %d\n", __LINE__, idx2check);
                exit(0);
            }
            if (dirent_alloc_p[idx2check].hash_value == -1) {
                dirent_cache_print_bucket_list(root_tb[0], 127);
            }

#if 1
            int ret;
            mdirent_sector0_not_aligned_t *sect0_p;
            sect0_p = DIRENT_VIRT_TO_PHY_OFF(returned_entry, sect0_p);
            /*
             ** check if the dirent cache entry from which the entry has been removed is now empty
             */
            ret = dirent_cache_entry_check_empty(returned_entry);
            switch (ret) {
                case 0:
                    /*
                     ** not empty
                     */
                    break;
                case 1:
                    /*
                     ** empty
                     */
                    //               printf("Entry is level %d  ref: %d_%d is empty\n",sect0_p->header.level_index,
                    //                                                                 sect0_p->header.dirent_idx[0],
                    //                                                                 sect0_p->header.dirent_idx[1]);
                    /*
                     ** if the entry is not root: we need to update the bitmap of the root
                     */
                    if (returned_entry != root) {

                        if (dirent_cache_del_collision_ptr(root, returned_entry) != NULL) {
                            printf(" ERROR  while deleting the collision ptr\n");
                            exit(0);
                        }
                    }

                    /*
                     ** OK, now release the associated memory
                     */
                    if (dirent_cache_release_entry(returned_entry) < 0) {
                        printf(" ERROR  dirent_cache_release_entry\n");
                        exit(0);
                    }

                    /*
                     ** remove the file
                     */
                {
                    char pathname[64];
                    char *path_p;
                    int flags = 0;
                    int ret;

                    /*
                     ** build the filename of the dirent file to read
                     */
                    path_p = dirent_build_filename(&returned_entry->header, pathname);
                    if (path_p == NULL) {
                        /*
                         ** something wrong that must not happen
                         */
                        DIRENT_SEVERE("Cannot build filename( line %d\n)", __LINE__);
                        exit(0);
                    }
#if 1
                    ret = unlinkat(fd_dir, path_p, flags);
                    if (ret < 0) {
                        DIRENT_SEVERE("Cannot remove file %s: %s( line %d\n)", path_p, strerror(errno), __LINE__);
                        exit(0);
                    }
#endif
                }
                    if (returned_entry == root) root = NULL;
                    returned_entry = NULL;
                    break;

                default:
                    printf("Error on line %d : sector0 pointer is wrong\n", __LINE__);
                    exit(0);
                    break;

            }
            /*
             ** Write the file on disk: it might the root and collision file, root only, collision file only
             */
            /*
             ** write the dirent file on which the entry has been inserted:
             */
            if ((returned_entry != NULL) && (DIRENT_IS_ROOT_UPDATE_REQ(returned_entry))) {
                ret = write_mdirents_file(fd_dir, returned_entry);
                if (ret < 0) {
                    printf("Error on writing file at line %d\n", __LINE__);
                }
            }
            /*
             ** check if another dirent cache entry  needs to be re-written on disk
             */
            if ((returned_prev_entry != NULL) && (DIRENT_IS_ROOT_UPDATE_REQ(returned_prev_entry))) {
                /*
                 ** write the root file:
                 */
                // printf("Write Root file\n");
                ret = write_mdirents_file(fd_dir, returned_prev_entry);
                if (ret < 0) {
                    printf("Error on writing file at line %d\n", __LINE__);

                }
            }
            /*
             ** Check if root needs to be re-written on disk :
             **   - adding a new collision file
             **   - update of a bucket entry
             **   - update of a hash entry (pnext)
             */
            if ((root != NULL) && (DIRENT_IS_ROOT_UPDATE_REQ(root))) {
                ret = write_mdirents_file(fd_dir, root);
                if (ret < 0) {
                    printf("Error on writing file at line %d\n", __LINE__);
                }
            }
#endif


        }
        PRINT_TIME(root_cnt, loop_count, Number of entries deleted);
        dirent_cache_lookup_print_stats();
        printf("--->dirent_cache_delete_hash_entry() test end\n\n");
    }
    /*
     **______________________________________________________________________________
     */
    /*
     ** Control that all the bucket are empty
     */
#if 0
    printf("____________________________________________________________________\n");
    printf("      --->DIRENT_CACHE_DELETE_HASH_ENTRY() : empty Bucket control\n");
    {
        int bucket_id = 0;
        for (i = 0; i < root_cnt; i++) {
            for (bucket_id = 0; bucket_id < 256; bucket_id++) {
                dirent_cache_print_bucket_list(root_tb[i], bucket_id);
            }

        }
    }
#endif
    /*
     **______________________________________________________________________________
     */
    //  printf("FDL temporay End of test\n");
    //   exit(0);

#if 0
    /*
     **______________________________________________________________________________
     */
    /*
     ** Lookup of the almost last index
     */
    /*
     **______________________________________________________________________________
     */
    {
        dirent_cache_lookup_clear_stats();

        mdirents_cache_entry_t *returned_entry;
        int local_idx;
        int bucket_idx;
        i = loop_count * root_cnt - 100;

        printf(" Lookup for entry %d\n", i);

        root = root_tb[dirent_alloc_p[i].root_idx];
        bucket_idx = i % BUCKET_MODULO;

        returned_entry = dirent_cache_search_hash_entry(root,
                bucket_idx,
                dirent_alloc_p[i].hash_value,
                &local_idx);
        if (returned_entry == NULL) {
            printf(" dirent_cache_search_hash_entry test error on line %d for index %d \n", __LINE__, i);
            exit(0);
        }
        if (returned_entry != dirent_alloc_p[i].returned_entry) {
            printf(" dirent_cache_search_hash_entry test error on line %d for index %d\n", __LINE__, i);
            exit(0);
        }
        dirent_cache_lookup_print_stats();

    }
#endif
    /*
     **______________________________________________________________________________
     */
    /*
     ** Deletion of a dirent cache entry
     **
     ** The goal of the test is to remove al the cache entry by removing the
     ** root cache entry first
     */
    /*
     **______________________________________________________________________________
     */
remove_entries:
    {

        dirent_cache_lookup_clear_stats();
        printf("____________________________________________________________________\n");
        printf("      --->DIRENT_REMOVE_ALL_CACHE_ENTRIES() test start\n");
        gettimeofday(&tv_start, NULL);

        int ret;
        for (root_idx = 0; root_idx < root_cnt; root_idx++) {
            root = root_tb[root_idx];

            ret = dirent_cache_release_entry(root);
            if (ret < 0) {
                printf(" dirent_cache_release_entry test error on line %d for index %d \n", __LINE__, root_idx);
                exit(0);
            }
        }
    }
    if (root_cnt * loop_count / 384 == 0) {
        PRINT_TIME(1, 1, Number of entries removed from cache);
    } else {
        PRINT_TIME(root_cnt * loop_count / 384, 1, Number of entries removed from cache);
    }
    printf("--->dirent_cache_remove_entry() test end\n\n");

    //out:
    if (root_tb != NULL) free(root_tb);
    if (dirent_alloc_p != NULL) free(dirent_alloc_p);

    if (fd_dir != -1) close(fd_dir);
}
#endif



/*
 *_____________________________________________________
   DIRENT CACHE TEST END
 *_____________________________________________________
 */


/*
 *_____________________________________________________
   DIRENT FILE TEST START
 *_____________________________________________________
 */
#ifdef UBUNTU
#warning compiled for ubuntu path :
#define DIR_PATHNAME "/home/didier/fizians/rozofs/dir_test"
#else
#define DIR_PATHNAME "/root/dir_test"
#endif

int test_dirent_create_fake_file(char *dir_path, mdirents_header_new_t *header) {
    int fd_dir = -1;
    int fd_file = -1;
    int flag = O_WRONLY | O_CREAT;
    char buffer[128];
    char *buf;
    mdirents_file_t *file_p = NULL;


    file_p = malloc(sizeof (mdirents_file_t));
    if (file_p == NULL) {
        return -1;
    }
    buf = dirent_build_filename(header, buffer);
    if (buf == NULL) {
        free(file_p);
        return -1;
    }
    /*
     ** Open the directory
     */
    if ((fd_dir = open(dir_path, O_RDONLY, S_IRWXU)) < 0) {
        goto out;
    }
    if ((fd_file = openat(fd_dir, buf, flag, S_IRWXU)) == -1)
        goto out;

    if (pwrite(fd_file, file_p, sizeof (mdirents_file_t), 0) != (sizeof (mdirents_file_t))) {
        DIRENT_SEVERE("pwrite failed in file %s: %s", buffer, strerror(errno));
        goto out;
    }
    free(file_p);
    close(fd_dir);
    close(fd_file);
    return 0;

out:
    free(file_p);
    if (fd_dir != -1)
        close(fd_dir);
    if (fd_file != -1)
        close(fd_file);
    return -1;
}

/**
 *_____________________________________________________
 * test_dirent_read_from_file

 */

void test_dirent_read_from_file() {
    mdirents_header_new_t header;
    int ret;
    mdirents_cache_entry_t *cache_entry;
    int fd_dir = -1;
    int i;
    struct timeval tv_start;
    int root_cnt = 1;
    struct timeval tv_stop;
    int loop_count = 200000;

    /*
     ** create a fake file
     */
    printf("test_dirent_read_from_file start\n");
    printf("sect 1 size %d bitmap size %d\n", (int) sizeof (mdirent_sector1_t), (int) sizeof (mdirents_btmap_free_chunk_t));
    header.level_index = 0;
    header.dirent_idx[0] = 1;
    ret = test_dirent_create_fake_file(DIR_PATHNAME, &header);
    if (ret == -1) {
        printf("test_dirent_read_from_file error at line %d\n", __LINE__);
        exit(0);
    }
    /*
     ** open the directory
     */
    if ((fd_dir = open(DIR_PATHNAME, O_RDONLY, S_IRWXU)) < 0) {
        printf("test_dirent_read_from_file error at line %d\n", __LINE__);
        exit(0);
    }
    gettimeofday(&tv_start, NULL);
    for (i = 0; i < loop_count; i++) {
        cache_entry = read_mdirents_file(fd_dir, &header);
        if (cache_entry == NULL) {
            printf("test_dirent_read_from_file error at line %d\n", __LINE__);
            exit(0);
        }
    }

    PRINT_TIME(root_cnt, loop_count, Number of files read);


    printf("test_dirent_read_from_file error success\n");
}

/*
 *_____________________________________________________
   DIRENT FILE TEST END
 *_____________________________________________________
 */


void readdir_test(char * dir_path);

/*
 *_____________________________________________________
   PUT_MDIRENTRY test 1
 *_____________________________________________________
 */
extern int dirent_append_entry;
extern int dirent_update_entry;
//extern uint32_t hash_debug_trc;
extern int fdl_debug_file_idx_trace;

void put_mdirentry_test1(char *dir_path) {
    int i;
    struct timeval tv_start;
    struct timeval tv_stop;
    int fd_dir = -1;
    int lost_count = 0;
    char bufall[256];
    fid_t fid_parent;
    fid_t fid_chid;
    int loop_count = 384 * 300;
    //char previous_file[128];
    char search_file[128];
    //  int loop_count = 320*5;

    //   int loop_count = 64*384*10*4*4;

    int root_cnt = 1;
    uint32_t mode;
    int ret;
    dirent_disk_clear_stats();
    dirent_file_repair_stats_clear();

    memset(fid_parent, 0, sizeof (fid_t));

    //   uuid_generate(fid_parent);

    dirent_cache_level0_initialize();

    writebck_cache_level0_initialize();

    uuid_generate(fid_chid);
    if ((fd_dir = open(dir_path, O_RDONLY, S_IRWXU)) < 0) {
        printf("Error on directory Open %s error %s\n", dir_path, strerror(errno));
        exit(0);
    }
#if 1
    printf("file_test_put_mdirentry with file descriptor %d\n", fd_dir);
    printf("____________________________________________________________________\n");
    printf("      --->PUT_MDIRENTRY_TEST1 test start\n");
    gettimeofday(&tv_start, NULL);
    /*
     ** Open the directory
     */

    for (i = 0; i < loop_count; i++) {
        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        if ((i % 1000) == 0) {
            printf("index %10.10d\r", i), fflush(stdout);
        }
        ret = put_mdirentry(fd_dir, fid_parent, bufall, fid_chid, 0);
        if (ret < 0) {
            printf("Error on put_mdirentry_test1 line %d for index %d\n", __LINE__, i);
            exit(0);

        }


    }
    PRINT_TIME(root_cnt, loop_count, dirent_append_entry);
    dirent_cache_lookup_print_stats();
    DIRENT_MALLOC_SIZE_PRINT;
    dirent_disk_print_stats();
    writebck_cache_print_stats();
    //     writebck_cache_print_access_stats();
    //     writebck_cache_print_per_count_stats();
    dirent_file_repair_stats_print();

    printf("dirent_append_entry  %d dirent_update_entry %d\n", dirent_append_entry, dirent_update_entry);
    dirent_cache_bucket_print_stats();

#endif
    fid_t fid_read;
    printf("____________________________________________________________________\n");
    printf("      --->GET_MDIRENTRY_TEST1 test start\n");
    gettimeofday(&tv_start, NULL);
    for (i = 0; i < loop_count; i++) {
        if (i == 1034852) {
            fdl_debug_file_idx_trace = 1;
        } else {
            fdl_debug_file_idx_trace = 0;
        }
        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        ret = get_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
        if (ret < 0) {
            //printf("Error on get_mdirentry_test1 line %d for index %d\n",__LINE__,i);
            //exit(0);
            lost_count++;

        }


    }
    PRINT_TIME(root_cnt, loop_count, Number of entries searched);
    dirent_cache_lookup_print_stats();
    dirent_cache_bucket_print_stats();

    DIRENT_MALLOC_SIZE_PRINT;
    dirent_disk_print_stats();
    printf("Number of files lost %d\n", lost_count);


#if 0
    printf("file_test_put_mdirentry with file descriptor %d\n", fd_dir);
    printf("____________________________________________________________________\n");
    printf("      --->PUT_MDIRENTRY_TEST1 step 2 test start\n");
    gettimeofday(&tv_start, NULL);
    /*
     ** Open the directory
     */

    for (i = 0; i < loop_count; i++) {
        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        ret = put_mdirentry(fd_dir, fid_parent, bufall, fid_chid, 0);
        if (ret < 0) {
            printf("Error on put_mdirentry_test1 line %d for index %d\n", __LINE__, i);
            exit(0);

        }


    }
    PRINT_TIME(root_cnt, loop_count, dirent_append_entry);
    dirent_cache_lookup_print_stats();
    DIRENT_MALLOC_SIZE_PRINT;
    printf("dirent_append_entry  %d dirent_update_entry %d\n", dirent_append_entry, dirent_update_entry);
#endif

    /*
     ** Access Test
     */
    readdir_test(dir_path);
    lost_count = 0;

#if 0
    {
        /*
         ** Get the root idx 0 and perform a repair for bucket_idx 5
         */
        mdirents_cache_entry_t *root_entry_p;
        root_entry_p = dirent_get_root_entry_from_cache(fid_parent, 0);
        if (root_entry_p == NULL) {
            /*
             ** dirent file is not in the cache need to read it from disk
             */
            printf("ERROR root_idx 0 is not in the cache !!!\n");
            return;
        }
        dirent_repair_print_enable = 1;
        //     dirent_file_check( fd_dir , root_entry_p,5);
        dirent_repair_print_enable = 0;
    }
#endif

#if 1
    printf("____________________________________________________________________\n");
    printf("      --->DEL_MDIRENTRY_TEST1 test start\n");
    gettimeofday(&tv_start, NULL);
    int idx_search = 778;
    for (i = 0; i < loop_count; i++) {

        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        ret = del_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
        if (ret < 0) {
            // printf("Error on del_mdirentry test1 line %d for index %d\n",__LINE__,i);
            //  exit(0);
            lost_count++;

        }
        /*
         ** check if the search if is still there
         */
        if (i < idx_search) {
            sprintf(search_file, "file_test_put_mdirentry_778");
            if (strcmp(search_file, bufall) == 0) continue;
            ret = get_mdirentry(fd_dir, fid_parent, search_file, fid_read, &mode);
            if (ret < 0) {
                printf("searched file lost while removing %s\n", bufall);
                exit(0);
                //lost_count++;

            }
        }


    }
    PRINT_TIME(root_cnt, loop_count, Number of entries removed);
    dirent_cache_bucket_print_stats();
    dirent_cache_lookup_print_stats();
    dirent_mem_print_stats_per_size();
    dirent_disk_print_stats();
    printf("Number of files lost %d\n", lost_count);
    dirent_file_repair_stats_print();

#endif

}

void put_mdirentry_test3(char *dir_path) {
    int i;
    struct timeval tv_start;
    struct timeval tv_stop;
    int fd_dir = -1;
    int lost_count = 0;
    char bufall[256];
    fid_t fid_parent;
    fid_t fid_chid;
    int loop_count = 384 * 300;
    //  int loop_count = 320*5;

    //   int loop_count = 64*384*10*4*4;

    int root_cnt = 1;
    uint32_t mode;
    int ret;
    dirent_disk_clear_stats();
    dirent_file_repair_stats_clear();

    memset(fid_parent, 0, sizeof (fid_t));

    //   uuid_generate(fid_parent);

    dirent_cache_level0_initialize();

    writebck_cache_level0_initialize();

    uuid_generate(fid_chid);
    if ((fd_dir = open(dir_path, O_RDONLY, S_IRWXU)) < 0) {
        printf("Error on directory Open %s error %s\n", dir_path, strerror(errno));
        exit(0);
    }
#if 0
    printf("file_test_put_mdirentry with file descriptor %d\n", fd_dir);
    printf("____________________________________________________________________\n");
    printf("      --->PUT_MDIRENTRY_TEST3 test start\n");
    gettimeofday(&tv_start, NULL);
    /*
     ** Open the directory
     */

    for (i = 0; i < loop_count; i++) {
        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        if ((i % 1000) == 0) {
            printf("index %10.10d\r", i), fflush(stdout);
        }
        ret = put_mdirentry(fd_dir, fid_parent, bufall, fid_chid, 0);
        if (ret < 0) {
            printf("Error on put_mdirentry_test1 line %d for index %d\n", __LINE__, i);
            exit(0);

        }


    }
    PRINT_TIME(root_cnt, loop_count, dirent_append_entry);
    dirent_cache_lookup_print_stats();
    DIRENT_MALLOC_SIZE_PRINT;
    dirent_disk_print_stats();
    writebck_cache_print_stats();
    //     writebck_cache_print_access_stats();
    //     writebck_cache_print_per_count_stats();
    dirent_file_repair_stats_print();

    printf("dirent_append_entry  %d dirent_update_entry %d\n", dirent_append_entry, dirent_update_entry);
    dirent_cache_bucket_print_stats();
    lost_count = 0;
    {
        /*
         ** Get the root idx 0 and perform a repair for bucket_idx 5
         */
        mdirents_cache_entry_t *root_entry_p;
        root_entry_p = dirent_get_root_entry_from_cache(fid_parent, 0);
        if (root_entry_p == NULL) {
            /*
             ** dirent file is not in the cache need to read it from disk
             */
            printf("ERROR root_idx 0 is not in the cache !!!\n");
            return;
        }
        //     dirent_repair_print_enable = 1;
        dirent_file_repair(fd_dir, root_entry_p, 5, DIRENT_REPAIR_BUCKET_IDX_MISMATCH);
        //     dirent_repair_print_enable = 0;
    }


#endif
    fid_t fid_read;
    printf("____________________________________________________________________\n");
    printf("      --->GET_MDIRENTRY_TEST3 test start\n");
    gettimeofday(&tv_start, NULL);
    for (i = 0; i < loop_count; i++) {
        if (i == 1034852) {
            fdl_debug_file_idx_trace = 1;
        } else {
            fdl_debug_file_idx_trace = 0;
        }
        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        ret = get_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
        if (ret < 0) {
            //printf("Error on get_mdirentry_test1 line %d for index %d\n",__LINE__,i);
            //exit(0);
            lost_count++;

        }


    }
    PRINT_TIME(root_cnt, loop_count, Number of entries searched);
    dirent_cache_lookup_print_stats();
    dirent_cache_bucket_print_stats();

    DIRENT_MALLOC_SIZE_PRINT;
    dirent_disk_print_stats();
    printf("Number of files lost %d\n", lost_count);
    dirent_file_repair_stats_print();


#if 0
    printf("file_test_put_mdirentry with file descriptor %d\n", fd_dir);
    printf("____________________________________________________________________\n");
    printf("      --->PUT_MDIRENTRY_TEST1 step 2 test start\n");
    gettimeofday(&tv_start, NULL);
    /*
     ** Open the directory
     */

    for (i = 0; i < loop_count; i++) {
        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        ret = put_mdirentry(fd_dir, fid_parent, bufall, fid_chid, 0);
        if (ret < 0) {
            printf("Error on put_mdirentry_test1 line %d for index %d\n", __LINE__, i);
            exit(0);

        }


    }
    PRINT_TIME(root_cnt, loop_count, dirent_append_entry);
    dirent_cache_lookup_print_stats();
    DIRENT_MALLOC_SIZE_PRINT;
    printf("dirent_append_entry  %d dirent_update_entry %d\n", dirent_append_entry, dirent_update_entry);
#endif

    /*
     ** Access Test
     */
    readdir_test(dir_path);
    lost_count = 0;
#if 0
    {
        /*
         ** Get the root idx 0 and perform a repair for bucket_idx 5
         */
        mdirents_cache_entry_t *root_entry_p;
        root_entry_p = dirent_get_root_entry_from_cache(fid_parent, 0);
        if (root_entry_p == NULL) {
            /*
             ** dirent file is not in the cache need to read it from disk
             */
            printf("ERROR root_idx 0 is not in the cache !!!\n");
            return;
        }
        dirent_file_repair(fd_dir, root_entry_p, 5, DIRENT_REPAIR_BUCKET_IDX_MISMATCH);
    }
#endif

#if 0
    printf("____________________________________________________________________\n");
    printf("      --->DEL_MDIRENTRY_TEST1 test start\n");
    gettimeofday(&tv_start, NULL);
    for (i = 0; i < loop_count; i++) {

        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        ret = del_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
        if (ret < 0) {
            // printf("Error on del_mdirentry test1 line %d for index %d\n",__LINE__,i);
            //  exit(0);
            lost_count++;

        }


    }
    PRINT_TIME(root_cnt, loop_count, Number of entries removed);
    dirent_cache_bucket_print_stats();
    dirent_cache_lookup_print_stats();
    dirent_mem_print_stats_per_size();
    dirent_disk_print_stats();
    printf("Number of files lost %d\n", lost_count);

#endif

}



/*
 *_____________________________________________________
   PUT_MDIRENTRY test 2 (ramdom)
 *_____________________________________________________
 */
extern int dirent_append_entry;
extern int dirent_update_entry;
//extern uint32_t hash_debug_trc;
extern int fdl_debug_file_idx_trace;

void put_mdirentry_test2(char *dir_path) {
    int i;
    struct timeval tv_start;
    struct timeval tv_stop;
    int fd_dir = -1;
    char bufall[256];
    fid_t fid_parent;
    fid_t fid_chid;
    //   int loop_count = 800;
    int loop_count = 64 * 384 * 10 * 4;

    int root_cnt = 1;
    uint32_t mode;
    int ret;
    dirent_disk_clear_stats();

    memset(fid_parent, 0, sizeof (fid_t));

    //   uuid_generate(fid_parent);

    dirent_cache_level0_initialize();

    writebck_cache_level0_initialize();

    uuid_generate(fid_chid);
    if ((fd_dir = open(dir_path, O_RDONLY, S_IRWXU)) < 0) {
        printf("Error on directory Open %s error %s\n", dir_path, strerror(errno));
        exit(0);
    }
#if 1
    printf("file_test_put_mdirentry with file descriptor %d\n", fd_dir);
    printf("____________________________________________________________________\n");
    printf("      --->PUT_MDIRENTRY_TEST2 test start\n");
    printf(" Number of entries to insert : %d\n", loop_count);
    gettimeofday(&tv_start, NULL);
    /*
     ** Open the directory
     */

    for (i = 0; i < loop_count; i++) {
        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        if ((i % 1000) == 0) {
            printf("index %10.10d\r", i), fflush(stdout);
        }
        ret = put_mdirentry(fd_dir, fid_parent, bufall, fid_chid, 0);
        if (ret < 0) {
            printf("Error on put_mdirentry_test1 line %d for index %d\n", __LINE__, i);
            exit(0);

        }


    }
    PRINT_TIME(root_cnt, loop_count, dirent_append_entry);
    dirent_cache_lookup_print_stats();
    DIRENT_MALLOC_SIZE_PRINT;
    dirent_disk_print_stats();
    writebck_cache_print_stats();
    writebck_cache_print_access_stats();


    printf("dirent_append_entry  %d dirent_update_entry %d\n", dirent_append_entry, dirent_update_entry);
    dirent_cache_bucket_print_stats();

#endif
    fid_t fid_read;
    printf("____________________________________________________________________\n");
    printf("      --->GET_MDIRENTRY_TEST1 test start\n");
    gettimeofday(&tv_start, NULL);
    for (i = 0; i < loop_count; i++) {
        if (i == 1034852) {
            fdl_debug_file_idx_trace = 1;
        } else {
            fdl_debug_file_idx_trace = 0;
        }
        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        ret = get_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
        if (ret < 0) {
            printf("Error on get_mdirentry_test1 line %d for index %d\n", __LINE__, i);
            exit(0);

        }


    }
    PRINT_TIME(root_cnt, loop_count, Number of entries searched);
    dirent_cache_lookup_print_stats();
    dirent_cache_bucket_print_stats();

    DIRENT_MALLOC_SIZE_PRINT;
    dirent_disk_print_stats();

    /*
     ** Access Test
     */
    readdir_test(dir_path);

#if 1
    printf("____________________________________________________________________\n");
    printf("      --->DEL/GET/PUT/GET/MDIRENTRY_TEST1 test start\n");
    gettimeofday(&tv_start, NULL);
    int increment;
    for (increment = 0; increment < 128; increment++) {
        for (i = 0; i < loop_count; i++) {
            if ((i % 1000) == 0) {
                printf("increment %3.3d index %10.10d\r", increment, i), fflush(stdout);
            }
            sprintf(bufall, "file_test_put_mdirentry_%d", (i + increment) % loop_count);
            ret = del_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
            if (ret < 0) {
                printf("Error on del_mdirentry test1 line %d for index %d\n", __LINE__, (i + increment) % loop_count);
                exit(0);
            }
            /*
             ** get MUST fail
             */
            ret = get_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
            if (ret == 0) {
                printf("Error on get_mdirentry_test1 line %d for index %d\n", __LINE__, (i + increment) % loop_count);
                exit(0);
            }
            /*
             ** re-insert the entry
             */
            ret = put_mdirentry(fd_dir, fid_parent, bufall, fid_chid, 0);
            if (ret < 0) {
                printf("Error on put_mdirentry_test1 line %d for index %d\n", __LINE__, (i + increment) % loop_count);
                exit(0);
                /*
                 ** get MUST succeed
                 */
            }
            ret = get_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
            if (ret < 0) {
                printf("Error on get_mdirentry_test1 line %d for index %d\n", __LINE__, (i + increment) % loop_count);
                exit(0);

            }
        }
    }
    printf("\n");
    PRINT_TIME(root_cnt, loop_count * 128, Number of entries removed);
    dirent_cache_bucket_print_stats();
    dirent_cache_lookup_print_stats();
    dirent_mem_print_stats_per_size();
    dirent_disk_print_stats();

#endif
#if 1
    printf("____________________________________________________________________\n");
    printf("      --->DEL_MDIRENTRY_TEST1 test start\n");
    gettimeofday(&tv_start, NULL);
    for (i = 0; i < loop_count; i++) {

        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        ret = del_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
        if (ret < 0) {
            printf("Error on del_mdirentry test1 line %d for index %d\n", __LINE__, i);
            exit(0);

        }


    }
    PRINT_TIME(root_cnt, loop_count, Number of entries removed);
    dirent_cache_bucket_print_stats();
    dirent_cache_lookup_print_stats();
    dirent_mem_print_stats_per_size();
    dirent_disk_print_stats();

#endif
}

/*
 *_____________________________________________________
   FACCESAT test
 *_____________________________________________________
 */
void faccessat_test(char *dir_path) {

    int i;
    uint32_t file_found = 0;
    struct timeval tv_start;
    struct timeval tv_stop;
    int fd_dir = -1;
    char bufall[256];
    fid_t fid_parent;
    //   int loop_count = 64;
    int loop_count = 4096;

    int root_cnt = 40;
    int mode = F_OK;
    int ret;
    int k;
    dirent_disk_clear_stats();

    memset(fid_parent, 0, sizeof (fid_t));



    if ((fd_dir = open(dir_path, O_RDONLY, S_IRWXU)) < 0) {
        printf("Error on directory Open %s error %s\n", dir_path, strerror(errno));
        exit(0);
    }
    printf("____________________________________________________________________\n");
    printf("      --->FACCESSAT test start\n");
    gettimeofday(&tv_start, NULL);
    for (k = 0; k < root_cnt; k++) {
        for (i = 0; i < loop_count; i++) {

            sprintf(bufall, "d_%d", i);
            ret = faccessat(fd_dir, bufall, mode, 0);
            if (ret < 0) {
                //       printf("faccessat error %s for index %d\n",strerror(errno),i);
                //       exit(0);
                continue;

            }
            file_found++;
        }
    }
    PRINT_TIME(root_cnt, loop_count, Number of entries checked);
    printf("ACCESS number of files found : %d\n\n", file_found);

    if (fd_dir != -1) close(fd_dir);

}
/*
 *_____________________________________________________
   READDIR_TEST test
 *_____________________________________________________
 */

typedef struct ep_child_t *ep_children_t;

/*
struct ep_child_t {
        ep_name_t name;
        ep_uuid_t fid;
        ep_children_t next;
};
 */

void readdir_test(char *dir_path) {
    struct timeval tv_start;
    struct timeval tv_stop;
    int fd_dir = -1;
    //   char bufall[256];
    fid_t fid_parent;
    child_t *children;
    uint64_t cookie = 0;
    uint8_t eof = 0;
    memset(fid_parent, 0, sizeof (fid_t));
    int loop_count = 0;
    int root_cnt = 1;

    dirent_readdir_stats_clear();

    if ((fd_dir = open(dir_path, O_RDONLY, S_IRWXU)) < 0) {
        printf("Error on directory Open %s error %s\n", dir_path, strerror(errno));
        exit(0);
    }
    printf("____________________________________________________________________\n");
    printf("      --->READDIR test start\n");
    gettimeofday(&tv_start, NULL);
    while (eof == 0) {
        list_mdirentries(fd_dir, fid_parent, &children, &cookie, &eof);
        if (eof == 0) loop_count += MAX_DIR_ENTRIES;
    }
    if (loop_count == 0) loop_count = 1;
    //   printf ("loop_count %d\n",loop_count);
    PRINT_TIME(root_cnt, loop_count, Number of entries read);
    dirent_readdir_stats_print();
    if (fd_dir != -1) close(fd_dir);
}

/*
 *_____________________________________________________
   OPENAT test
 *_____________________________________________________
 */
void openat_test(char *dir_path) {

    int i;
    uint32_t file_found = 0;
    struct timeval tv_start;
    struct timeval tv_stop;
    int fd_dir = -1;
    char bufall[256];
    fid_t fid_parent;
    //   int loop_count = 64;
    int loop_count = 384 * 100;

    int root_cnt = 40;
    //   int mode = F_OK;
    int ret;
    int k;
    dirent_disk_clear_stats();

    memset(fid_parent, 0, sizeof (fid_t));



    if ((fd_dir = open(dir_path, O_RDONLY, S_IRWXU)) < 0) {
        printf("Error on directory Open %s error %s\n", dir_path, strerror(errno));
        exit(0);
    }
    printf("____________________________________________________________________\n");
    printf("      --->OPENAT test start\n");
    gettimeofday(&tv_start, NULL);
    for (k = 0; k < root_cnt; k++) {
        for (i = 0; i < loop_count; i++) {

            sprintf(bufall, "d_%d", i);
            ret = openat(fd_dir, bufall, O_RDONLY, S_IRWXU);
            if (ret < 0) {
                //       printf("faccessat error %s for index %d\n",strerror(errno),i);
                //       exit(0);
                continue;

            }
            close(ret);
            file_found++;
        }
    }
    PRINT_TIME(root_cnt, loop_count, Openat Number of entries checked);
    printf("Openat number of files found : %d\n\n", file_found);

    if (fd_dir != -1) close(fd_dir);

}

void mdirent_test_sylvain(char *dir_path) {
    int i;
    struct timeval tv_start;
    struct timeval tv_stop;
    int fd_dir = -1;
    int lost_count = 0;
    char bufall[256];
    fid_t fid_parent;
    fid_t fid_chid;
    int loop_count = 10000;

    int root_cnt = 1;
    uint32_t mode;
    int ret;

    dirent_disk_clear_stats();
    dirent_file_repair_stats_clear();

    memset(fid_parent, 0, sizeof (fid_t));

    //   uuid_generate(fid_parent);

    dirent_cache_level0_initialize();

    writebck_cache_level0_initialize();

    uuid_generate(fid_chid);

    /*
     ** Open the directory
     */
    if ((fd_dir = open(dir_path, O_RDONLY, S_IRWXU)) < 0) {
        printf("Error on directory Open %s error %s\n", dir_path, strerror(errno));
        exit(0);
    }

    printf("____________________________________________________________________\n");
    printf("      --->PUT_MDIRENTRY_TEST test start\n");
    gettimeofday(&tv_start, NULL);


    for (i = 0; i < loop_count; i++) {

        sprintf(bufall, "file_test_put_mdirentry_%d", i);

        if ((i % 1000) == 0) {
            printf("index %10.10d\r", i), fflush(stdout);
        }

        ret = put_mdirentry(fd_dir, fid_parent, bufall, fid_chid, 0);
        if (ret < 0) {
            printf("Error on put_mdirentry_test1 line %d for index %d\n", __LINE__, i);
            exit(0);

        }
    }

    PRINT_TIME(root_cnt, loop_count, PUT_MDIRENTRY_TEST);
    //dirent_cache_lookup_print_stats();
    //DIRENT_MALLOC_SIZE_PRINT;
    //dirent_disk_print_stats();
    //writebck_cache_print_stats();
    //dirent_file_repair_stats_print();

    //printf("dirent_append_entry  %d dirent_update_entry %d\n", dirent_append_entry, dirent_update_entry);
    //dirent_cache_bucket_print_stats();

    printf("____________________________________________________________________\n");
    printf("      --->GET_MDIRENTRY_TEST test start\n");


    fid_t fid_read;

    gettimeofday(&tv_start, NULL);

    for (i = 0; i < loop_count; i++) {

        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        ret = get_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
        if (ret < 0) {
            printf("Error on get_mdirentry_test line %d for index %d\n", __LINE__, i);
            exit(0);
            lost_count++;
        }


    }
    PRINT_TIME(root_cnt, loop_count, Number of entries searched);
    //dirent_cache_lookup_print_stats();
    //dirent_cache_bucket_print_stats();
    //DIRENT_MALLOC_SIZE_PRINT;
    //dirent_disk_print_stats();
    printf("Number of files lost %d\n", lost_count);

    printf("____________________________________________________________________\n");
    printf("      --->READDIR test start\n");

    child_t *children;
    child_t *iterator = NULL;
    uint64_t cookie = 0;
    uint64_t count_entries = 0;
    uint64_t count_request = 0;
    uint8_t eof = 0;
    dirent_readdir_stats_clear();

    gettimeofday(&tv_start, NULL);

    while (eof == 0) {
        ret = list_mdirentries(fd_dir, fid_parent, &children, &cookie, &eof);
        if (eof != 1) {
            iterator = children;
            count_request++;
            while (iterator != NULL) {
                count_entries++;
                iterator = iterator->next;
            }
        }
        //if (eof == 0) loop_count += MAX_DIR_ENTRIES;
    }

    PRINT_TIME(root_cnt, loop_count, Number of entries read);
    printf("Nb. of mdirentries %"PRIu64", Nb. of requests: %"PRIu64"\n", count_entries, count_request);
    //dirent_readdir_stats_print();


    printf("____________________________________________________________________\n");
    printf("      --->DEL_MDIRENTRY_TEST test start\n");

    gettimeofday(&tv_start, NULL);

    for (i = 0; i < loop_count; i++) {

        sprintf(bufall, "file_test_put_mdirentry_%d", i);
        ret = del_mdirentry(fd_dir, fid_parent, bufall, fid_read, &mode);
        if (ret < 0) {
            printf("Error on del_mdirentry test1 line %d for index %d\n", __LINE__, i);
            exit(0);
            lost_count++;

        }

    }
    PRINT_TIME(root_cnt, loop_count, Number of entries removed);
    //dirent_cache_bucket_print_stats();
    //dirent_cache_lookup_print_stats();
    //dirent_mem_print_stats_per_size();
    //dirent_disk_print_stats();
    printf("Number of files lost %d\n", lost_count);
    //dirent_file_repair_stats_print();

}


//extern void uuid_test();

int main() {
    //print_sector_offset();
    //test_print_constants();

    // hashtab_test();

    // uuid_test();


    //  test_dirent_filename();

    //  test_dirent_cache_allocation();

    //  dirent_cache_alloc_name_entry_idx_test2(DIR_PATHNAME);

    //  dirent_cache_alloc_name_entry_idx_test3(DIR_PATHNAME);

    //  sector_distribution_print();

    //  test_dirent_read_from_file();

    //put_mdirentry_test2(DIR_PATHNAME);
    //put_mdirentry_test3(DIR_PATHNAME);
    printf("####################################################################\n");
    //put_mdirentry_test1(DIR_PATHNAME);

    mdirent_test_sylvain(DIR_PATHNAME);

    // faccessat_test(DIR_PATHNAME);
    // openat_test(DIR_PATHNAME);

    DIRENT_MALLOC_SIZE_PRINT;
    return 0;

}
