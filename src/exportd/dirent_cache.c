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
#define _XOPEN_SOURCE 500
#endif

#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <rozofs/rozofs.h>
#include <rozofs/common/log.h>
#include <rozofs/common/xmalloc.h>

#include "mdir.h"
#include "mdirent.h"

/**
 *  debug flags
 */
int fdl_debug_file_idx_trace = 0;
uint32_t hash_debug_trc = 0;
int fdl_debug_hash_idx = 274;
int fdl_debug_dirent_file_idx = 3048;
int fdl_debug_dirent_file_idx1 = 1;
int fdl_debug_bucket_idx = 203;
/**
 *  Global Data
 */
#define DIRENT_WRITE_DEBUG 0

#if DIRENT_WRITE_DEBUG
#warning DIRENT_WRITE_DEBUG flag enabled
#endif
mdirent_indirect_ptr_template_t mdirent_cache_indirect_coll_distrib = {
        MDIRENTS_MAX_COLLS_IDX, /**< last_idx                       */
        sizeof(mdirent_cache_ptr_t), /**< size of the element in bytes   */
        MDIRENTS_CACHE_DIRENT_COLL_INDIRECT_CNT, /**< number of indirections         */
        { MDIRENTS_CACHE_DIRENT_COLL_LVL0_NB_BIT, // 16,
                MDIRENTS_CACHE_DIRENT_COLL_LVL1_NB_BIT, // 4
                MDIRENTS_CACHE_DIRENT_COLL_LVL2_NB_BIT // 32
        } };

mdirent_indirect_ptr_template_t mdirent_cache_name_ptr_distrib = {
        MDIRENTS_NAME_PTR_MAX, /**< last_idx                       */
        sizeof(mdirent_cache_ptr_t), /**< size of the element in bytes   */
        MDIRENTS_CACHE_CHUNK_INDIRECT_CNT, /**< number of indirections         */
        { MDIRENTS_NAME_PTR_LVL0_NB_BIT, // 8,
                MDIRENTS_NAME_PTR_LVL1_NB_BIT, // 16
        } };

/**
 * statistics
 */
uint32_t check_bytes_call = 0;
uint32_t check_bytes_val_call = 0;
uint32_t dirent_skip_64_cnt = 0;
uint32_t dirent_skip_32_cnt = 0;
uint32_t dirent_skip_16_cnt = 0;

uint64_t malloc_size = 0;
uint32_t malloc_size_tb[DIRENT_MEM_MAX_IDX];

uint64_t dirent_coll_stats_tb[DIRENT_COLL_STAT_MAX_IDX];
uint64_t dirent_coll_stats_cumul_lookups;
uint64_t dirent_coll_stats_cumul_collisions;
uint64_t dirent_coll_stats_hash_name_collisions;

uint64_t dirent_read_bytes_count; /**< cumulative number of bytes read */
uint64_t dirent_pread_count; /**< cumaltive number of read requests  */
uint64_t dirent_write_bytes_count; /**< cumulative number of bytes written */
uint64_t dirent_write_count; /**< cumaltive number of write requests  */

uint64_t dirent_readdir_stats_call_count; /**< number of time read has been called */
uint64_t dirent_readdir_stats_file_count; /**< total numebr of file read by readdir */

#if 0
/**
 * hashing function used to find the index within the dirent hash table
 *@param key : pointer to the fid of the parent directory

 @retval : value of the hash
 */
static inline uint32_t dirent_primary_hash(void *key) {
    uint32_t hash = 0;
    uint8_t *c;
    uint8_t len = sizeof(mdirents_cache_key_t);

    for (c = key; c != key + len; c++)
    hash = *c + (hash << 6) + (hash << 16) - hash;
    return hash;
}

/**
 *  Hash function for a name of mdirentry
 *
 * @param *key: pointer to the name of the mdirentry
 *
 * @retval hash_value
 */
static inline uint32_t dirent_hash_name(void *key) {
    uint32_t hash = 0;
    char *d;

    for (d = key; *d != '\0'; d++)
    hash = *d + (hash << 6) + (hash << 16) - hash;

    return hash;
}

/*
 **______________________________________________________________________________
 */
/*
 *  comparaison on the primay key. The key is a fid
 @param k1,k2 : pointer to fid

 @retval 0: there is a match
 */
static inline int dirent_primary_cmp(void *k1, void *k2)
{
    mdirents_cache_key_t *key1 = (mdirents_cache_key_t*) k1;
    mdirents_cache_key_t *key2 = (mdirents_cache_key_t*) k2;

    if( uuid_compare(key1->fid, key2->fid) != 0) return 0;
    if (key1->dirent_root_idx == key2->dirent_root_idx) return 0;
    return 1;
}

/** initialize an  empty dirent cache
 *
 * @param cache: the cache to initialize
 */
void dirent_cache_initialize(dirent_cache_t *cache) {
    cache->max = DIRENT_CACHE_MAX_ENTRIES;
    cache->size = 0;
    list_init(&cache->entries);
    htable_initialize(&cache->htable, DIRENT_CACHE_BUKETS, dirent_primary_hash, dirent_primary_cmp);
}
#endif

/*
 **______________________________________________________________________________
 */
/**
 * Allocation of a dirent file cache entry

 @param dirent_hdr_p : pointer to the dirent header that contains its reference

 @retval  <>NULL  pointer to the dirent cache entry
 @retval NULL : out of cache entries
 */
mdirents_cache_entry_t *dirent_cache_allocate_entry(
        mdirents_header_new_t *dirent_hdr_p) {
    mdirents_cache_entry_t *p;
    int i;

    p = (mdirents_cache_entry_t*) DIRENT_MALLOC(sizeof(mdirents_cache_entry_t))
    ;
    if (p == (mdirents_cache_entry_t*) NULL ) {
        /*
         ** out of memory
         */
        return NULL ;
    }
    /*
     ** clear the memory entry
     */
    memset(p, 0, sizeof(mdirents_cache_entry_t));
//#warning assert dirty bit for bucket hash table and hash entry table

    for (i = 0; i < MDIRENTS_HASH_TB_CACHE_MAX_IDX; i++) {
        p->hash_tbl_p[i].s.dirty = 1;
    }
    for (i = 0; i < MDIRENTS_HASH_CACHE_MAX_IDX; i++) {
        p->hash_entry_p[i].s.dirty = 1;
    }
    /*
     ** set the mandatory sector in dirty mode
     */
    /*
     ** linked list init
     */
    list_init(&p->cache_link);
    list_init(&p->coll_link);
    /*
     ** copy the header that contains the reference of the dirent cache entry
     **
     */
    memcpy(&p->header, dirent_hdr_p, sizeof(mdirents_header_new_t));
    return p;
}

/*
 **______________________________________________________________________________
 */
/**
 *  API to create a dirent entry from scratch
 *
 @param dirent_hdr_p : pointer to the dirent header that contains its reference

 @retval  <>NULL  pointer to the dirent cache entry
 @retval NULL : out of cache entries
 */
mdirents_cache_entry_t *dirent_cache_create_entry(
        mdirents_header_new_t *dirent_hdr_p) {
    mdirents_cache_entry_t *p = NULL;
    uint64_t val;

    /*
     ** Force the current version of the dirent file
     */
    dirent_hdr_p->version = MDIRENT_FILE_VERSION_0;
    dirent_hdr_p->max_number_of_hash_entries = MDIRENTS_ENTRIES_COUNT;
    dirent_hdr_p->sector_offset_of_name_entry = DIRENT_HASH_NAME_BASE_SECTOR;

    p = dirent_cache_allocate_entry(dirent_hdr_p);
    if (p == NULL ) {
        /*
         ** out of memory
         */
        return (mdirents_cache_entry_t*) NULL ;
    }
    /*
    ** allocate the save bitmap for root entry only
    */
    p->bucket_safe_bitmap_p = DIRENT_MALLOC(MDIRENTS_HASH_TB_INT_SZ/8);
    if (p->bucket_safe_bitmap_p == NULL ) {
        /*
         ** out of memory
         */
	 goto error;
    }
    memset( p->bucket_safe_bitmap_p,0,MDIRENTS_HASH_TB_INT_SZ/8);
     /*
     ** Init of the various bitmap
     */
    /*
     ** Sector 0 init
     */
    {
        mdirent_sector0_not_aligned_t *sect0_p;
        sect0_p = DIRENT_MALLOC(sizeof( mdirent_sector0_not_aligned_t))
        ;
        if (sect0_p == NULL )
            goto error;
        memcpy(&sect0_p->header, dirent_hdr_p, sizeof(mdirents_header_new_t));
        memset(&sect0_p->coll_bitmap, 0xff,
                sizeof(mdirents_btmap_coll_dirent_t));
        memset(&sect0_p->hash_bitmap, 0xff, sizeof(mdirents_btmap_free_hash_t));
        /*
         ** store the virtual pointer
         */
        val = (uint64_t) (uintptr_t) sect0_p;
        p->sect0_p.s.val = val;

    }
    {
        mdirents_btmap_coll_dirent_t *coll_bitmap_hash_full_p;
        coll_bitmap_hash_full_p =
                DIRENT_MALLOC(sizeof( mdirents_btmap_coll_dirent_t))
        ;
        if (coll_bitmap_hash_full_p == NULL )
            goto error;
        memset(coll_bitmap_hash_full_p, 0,
                sizeof(mdirents_btmap_coll_dirent_t));
        /*
         ** store the virtual pointer
         */
        val = (uint64_t) (uintptr_t) coll_bitmap_hash_full_p;
        p->coll_bitmap_hash_full_p.s.val = val;

    }
    /*
     ** Sector 1: name bitmap
     */
    {
        mdirents_btmap_free_chunk_t *name_bitmap_p;
        name_bitmap_p = DIRENT_MALLOC(sizeof( mdirents_btmap_free_chunk_t))
        ;
        if (name_bitmap_p == NULL )
            goto error;
        memset(name_bitmap_p, 0xff, sizeof(mdirents_btmap_free_chunk_t));
        /*
         ** store the virtual pointer
         */
        val = (uint64_t) (uintptr_t) name_bitmap_p;
        p->name_bitmap_p.s.val = val;
    }

    /*
     ** All is fine
     */
    return p;

    error:
    /*
     ** Release the dirent entry
     */
    if (p != NULL) dirent_cache_release_entry(p);
    return NULL ;

}

/*
 **______________________________________________________________________________
 */
/**
 * Release of a dirent file cache entry

 @param dirent_entry_p : pointer to the dirent cache entry

 @retval  -1 on error
 @retval 0 on success
 */
int dirent_cache_release_entry(mdirents_cache_entry_t *dirent_entry_p) {
    int coll_idx = 0;
    int next_coll_idx = 0;
    uint8_t chunk_u8_idx;
    int bit_idx;
    mdirent_sector0_not_aligned_t *sect0_p;
    //uint8_t *hash_bitmap_p;
    uint8_t *coll_bitmap_p;
    int loop_cnt = 0;
    mdirents_cache_entry_t *dirent_coll_entry_p = NULL;
    uint8_t *mem_p;
    
#if 0    
    if (DIRENT_IS_ROOT_UPDATE_REQ(dirent_entry_p))
      severe("FDL_DEBUG dirent file has not been updated on disk %s [%d_%d]",
            (dirent_entry_p->header.level_index==0)?"ROOT":"COLL",
	    dirent_entry_p->header.dirent_idx[0],
	    dirent_entry_p->header.dirent_idx[1]);
#endif
    /*
     ** Get the pointer to the bitmap of the free hash entries of the parent dirent entry
     */
    sect0_p = DIRENT_VIRT_TO_PHY_OFF(dirent_entry_p,sect0_p)
    ;
    if (sect0_p == (mdirent_sector0_not_aligned_t*) NULL ) 
    {
        DIRENT_SEVERE(" dirent_cache_release_entry error at line %d\n",__LINE__);
    }
    else
    {

      coll_bitmap_p = (uint8_t*) &sect0_p->coll_bitmap;
      /*
       ** check if there is some collision dirent cache entry associated with the current entry
       */
      while (coll_idx < MDIRENTS_MAX_COLLS_IDX) {
  #if 1
          if (coll_idx % 8 == 0) {
              next_coll_idx = check_bytes_val(coll_bitmap_p, coll_idx,
                      MDIRENTS_MAX_COLLS_IDX, &loop_cnt, 1);
              if (next_coll_idx < 0)
                  break;
              coll_idx = next_coll_idx;
          }
  #endif
          chunk_u8_idx = coll_idx / 8;
          bit_idx = coll_idx % 8;

          /*
           ** there is collision dirent entry
           */
          if ((coll_bitmap_p[chunk_u8_idx] & (1 << bit_idx)) != 0) {
              /*
               ** that collision entry is free-> check the next one
               */
              coll_idx++;
              continue;
          }
          /*
           ** the entry exist, so get the associated dirent cache entry pointer
           */
          dirent_coll_entry_p = dirent_cache_get_collision_ptr(dirent_entry_p,
                  (uint32_t) coll_idx);
          if (dirent_coll_entry_p == NULL ) {
              /*
               ** This error is accepted, it addresses the case of a collision file reading error
	       ** In that case we keep the associated bit in the collision file bitmap of the
	       ** root entry, but we do not allocated memory for the collision file
               */
  //            DIRENT_SEVERE("dirent_cache_release_entry error for collision file  %d",coll_idx);

              coll_idx++;
              continue;
          }
          /*
           ** delete the pointer associated with the collision entry (not the collision entry itself
           */
          if (dirent_cache_del_collision_ptr(dirent_entry_p,
                  dirent_coll_entry_p) != NULL) {
              /*
               ** something wrong in the release
               */
              DIRENT_SEVERE("dirent_cache_release_entry error for collision file  %d",coll_idx);
          }
          /*
           ** Release that collision dirent entry
           */
          if (dirent_cache_release_entry(dirent_coll_entry_p) < 0) {
              /*
               ** something wrong in the release
               */
              DIRENT_SEVERE("dirent_cache_release_entry error for collision file  %d",coll_idx);
          }

          /*
           ** check next entry
           */
          coll_idx++;
      }
    }    
    /*
    ** Check for hash backet safe bitmap memory
    */
    if (dirent_entry_p->bucket_safe_bitmap_p != NULL)
          DIRENT_FREE(dirent_entry_p->bucket_safe_bitmap_p); 

    /*
     ** all the collision dirent cache entries have been released, start releasing the
     ** hash entries
     */
    dirent_cache_release_all_hash_entries_memory(dirent_entry_p);
    /*
     ** release all the name entries blocks
     */
    dirent_cache_release_name_all_memory(dirent_entry_p);
    /*
     ** release the sector_0 array
     */
    if (sect0_p != NULL) 
    {
      DIRENT_FREE(sect0_p);
    }
    DIRENT_CLEAR_VIRT_PTR(dirent_entry_p->sect0_p);

    /*
     ** release the memory allocated for collision bitmap
     */
    mem_p = DIRENT_VIRT_TO_PHY(dirent_entry_p->coll_bitmap_hash_full_p)
    ;
    if (mem_p == NULL ) {
        /*
         ** something wrong in the release
         */
        DIRENT_SEVERE("dirent_cache_release_entry error at line %d\n",__LINE__);
    }
    else
    {
      DIRENT_FREE(mem_p);
    }
    DIRENT_CLEAR_VIRT_PTR(dirent_entry_p->coll_bitmap_hash_full_p);

    /*
     ** release the memory allocated for name bitmap
     */
    mem_p = DIRENT_VIRT_TO_PHY(dirent_entry_p->name_bitmap_p);
    if (mem_p == NULL ) {
        /*
         ** something wrong in the release
         */
        DIRENT_SEVERE("dirent_cache_release_entry error at line %d\n",__LINE__);
    }
    else
    {
      DIRENT_FREE(mem_p);
    }
    DIRENT_CLEAR_VIRT_PTR(dirent_entry_p->name_bitmap_p);
    /*
     ** release the memory allocated for hash buckets
     */
    dirent_cache_release_all_hash_tbl_memory(dirent_entry_p);

//  printf("dirent_cache_release_entry not yet implemented\n");

    DIRENT_FREE(dirent_entry_p);
    return 0;
}

/*
 **______________________________________________________________________________
 */
/**
 *  API to allocate an entry in a dirent cache entry associated with the dirent file.

 hash_p:
 the type is set to EOF if no entry has been found:
 - idx and level are not significant

 the type is set to LOC if the entry has been found in the parent dirent entry:
 - idx :reference of the hash entry within the parent dirent file
 - level: no significant

 the type is set to COLL if the entry is found is a dirent cache entry that is different from parent dirent entry:
 - idx :reference of the collision dirent file
 - level: level of the dirent collision file (max is 1 today)

 The reference of the local hash entry is always found in the content of local_idx_p as output argument. It corresponds
 to the allocated hash entry with the dirent file cache pointer provided as returned argument.


 @param parent : pointer to the parent dirent entry
 @param hash_p : pointer to the result of the search
 @param bucket_idx : index of the hash bucket : taken from the lower 8 bits of the hash value applied to the name of the directory/file
 @param local_idx_p : pointer to the bitmap of the free chunks

 @retval <> NULL: the entry has been allocated and the returned pointer is the pointer to the dirent cache entry of the allocation
 @retval NULL: out of entries
 */
mdirents_cache_entry_t *dirent_cache_alloc_name_entry_idx(
        mdirents_cache_entry_t *parent, int bucket_idx,
        mdirents_hash_ptr_t *hash_p, int *local_idx_p) {
    uint8_t chunk_u8_idx;
    int coll_idx = 0;
    int next_coll_idx = 0;
    //int start_idx = -1;
    int loop_cnt = 0;
    int free_coll_idx = -1;
    int bit_idx;
    int i;
    //uint8_t *hash_bitmap_p;
    uint8_t *coll_bitmap_p;
    uint8_t *coll_bitmap_hash_full_p;
    mdirents_hash_entry_t *hash_entry_p;
    mdirents_hash_ptr_t *hash_bucket_p;

    mdirent_sector0_not_aligned_t *sect0_p;
    int hash_entry_bit_idx;

    /*
     ** set no reference in the returned hash pointer and set returned pointer to NULL
     */
    hash_p->type = MDIRENTS_HASH_PTR_EOF;
    mdirents_cache_entry_t *dirent_entry = NULL;
    *local_idx_p = -1;
    /*
     ** Get the pointer to the bitmap of the free hash entries of the parent dirent entry
     */
    sect0_p = DIRENT_VIRT_TO_PHY_OFF(parent,sect0_p)
    ;
    if (sect0_p == (mdirent_sector0_not_aligned_t*) NULL ) {
        return NULL ;
    }
    coll_bitmap_hash_full_p =
            DIRENT_VIRT_TO_PHY_OFF(parent,coll_bitmap_hash_full_p)
    ;
    if (coll_bitmap_hash_full_p == (uint8_t*) NULL ) {
        return NULL ;
    }
    //hash_bitmap_p = (uint8_t*) &sect0_p->hash_bitmap;
    coll_bitmap_p = (uint8_t*) &sect0_p->coll_bitmap;
    /*
     ** Check if the parent is not full
     */
    if (parent->hash_entry_full == 0) {

        /*
         ** There is at least one free entry on the parent ->allocate the free entry from the parent dirent
         */
        hash_entry_bit_idx =
                DIRENT_CACHE_ALLOCATE_HASH_ENTRY_IDX(&sect0_p->hash_bitmap)
        ;
        if (hash_entry_bit_idx >= 0) {
            /*
             ** there is a local free entry available, so udpate the hash_p value and return
             */
            hash_p->type = MDIRENTS_HASH_PTR_LOCAL;
//        hash_p->level = parent->header.level_index;
            hash_p->idx = hash_entry_bit_idx;
            *local_idx_p = hash_entry_bit_idx;
//        printf("FDL BUG DIRENT_CACHE_ALLOCATE_HASH_ENTRY_ARRAY line %d for idx %d\n",__LINE__,hash_entry_bit_idx);
            hash_entry_p =
                    DIRENT_CACHE_ALLOCATE_HASH_ENTRY_ARRAY(parent,hash_entry_bit_idx)
            ;
            if (hash_entry_p == NULL ) {
                /*
                 ** something wrong, we run out of memory
                 */
                return NULL ;
            }
            hash_bucket_p =
                    DIRENT_CACHE_ALLOCATE_BUCKET_ARRAY(parent,bucket_idx)
            ;
            if (hash_bucket_p == NULL ) {
                /*
                 ** something wrong, we run out of memory
                 */
                return NULL ;
            }
#if 1
            if (hash_entry_bit_idx == (MDIRENTS_ENTRIES_COUNT - 1)) {
                /*
                 ** all the entries of the parent are busy
                 */
                parent->hash_entry_full = 1;
            }
#endif
            return parent;
        }
        /*
         ** the hash_entry full flag of the parent was not asserted and there was no free entry: That
         ** situation happens when the root file is read for the first time because the system
         ** does not perform any check on the bitmap. So it must just happen on the first call
         */
        parent->hash_entry_full = 1;
    }
    /*
     ** all the entries of the parent are busy, search among any potential collision dirent file
     ** that are reference within the current
     */
    while (coll_idx < MDIRENTS_MAX_COLLS_IDX) {
#if 1
        if (coll_idx % 8 == 0) {
            next_coll_idx = check_bytes_val(coll_bitmap_hash_full_p, coll_idx,
                    MDIRENTS_MAX_COLLS_IDX, &loop_cnt, 1);
            if (next_coll_idx < 0)
                break;
            coll_idx = next_coll_idx;
        }
#endif
        chunk_u8_idx = coll_idx / 8;
        bit_idx = coll_idx % 8;
        /*
         ** check if the collision file has some free entry
         */
#if 1
        if ((coll_bitmap_hash_full_p[chunk_u8_idx] & (1 << bit_idx)) != 0) {
            /*
             ** the collision entry is full->next
             */
            coll_idx++;
            continue;
        }
#endif

        /*
         ** there is no collision dirent entry or the collision dirent entry exist and is not full
         */
        if ((coll_bitmap_p[chunk_u8_idx] & (1 << bit_idx)) != 0) {
            /*
             ** remenber that the idx was free: that index could be used if no entry is found among
             ** the existing collision dirent cache entries
             */
            if (free_coll_idx == -1)
                free_coll_idx = coll_idx;
            /*
             ** check if the index matches with chunk_u64_idx+1
             ** in that case we exit the while loop if no free bit has not yet been found
             */
            if (coll_idx % 8 == 0) {
                next_coll_idx = check_bytes_val(coll_bitmap_p, coll_idx,
                        MDIRENTS_MAX_COLLS_IDX, &loop_cnt, 1);
                if (next_coll_idx < 0)
                    break;
                /*
                 ** next  chunk
                 */
                if (next_coll_idx == coll_idx)
                    coll_idx++;
                else
                    coll_idx = next_coll_idx;
                continue;
            }
            /*
             ** next chunk
             */
            coll_idx++;
            continue;
        }
        /*
         ** the collision dirent cache entry exists, so get the associated dirent cache entry pointer
         */
        dirent_entry = dirent_cache_get_collision_ptr(parent,
                (uint32_t) coll_idx);
        if (dirent_entry == NULL ) {
            /*
             ** not normal, the dirent entry is not present in the cache
             */
	     DIRENT_SEVERE("Collision pointer not found inmemory for collision file %d",coll_idx);
            break;
        }
        sect0_p = DIRENT_VIRT_TO_PHY_OFF(dirent_entry,sect0_p)
        ;
        if (sect0_p == (mdirent_sector0_not_aligned_t*) NULL ) {
            return NULL ;
        }
        /*
         ** OK, now try to allocate an entry from the local dirent entry
         */
        hash_entry_bit_idx =
                DIRENT_CACHE_ALLOCATE_HASH_ENTRY_IDX(&sect0_p->hash_bitmap)
        ;
        if (hash_entry_bit_idx < 0) {
            /*
             ** there is no free entry, try the next collision file of the parent;
             ** indicates that collision dirent entry is full
             */
            DIRENT_CACHE_SET_COLL_ENTRY_FULL(coll_bitmap_hash_full_p, coll_idx);
            coll_idx++;
            continue;
        }
        /*
         ** OK, we have found it so set start_idx with the local reference of the collision file within
         ** the parent dirent cache entry
         */
        hash_p->type = MDIRENTS_HASH_PTR_COLL;
        // hash_p->level = parent->header.level_index;
        hash_p->idx = coll_idx;
        *local_idx_p = hash_entry_bit_idx;
        //start_idx = coll_idx;
        /*
         ** assert full for the collision file if hash_entry_bit_idx is the last allocatable index
         */
#if 1
        if (hash_entry_bit_idx == (MDIRENTS_ENTRIES_COUNT - 1)) {
            DIRENT_CACHE_SET_COLL_ENTRY_FULL(coll_bitmap_hash_full_p, coll_idx);
        }
#endif
        /*
         ** check if the hash entry section associated with the local index needs to be created
         */
        hash_entry_p = DIRENT_CACHE_ALLOCATE_HASH_ENTRY_ARRAY(dirent_entry,
                hash_entry_bit_idx)
        ;
        if (hash_entry_p == NULL ) {
            /*
             ** something wrong, we run out of memory
             */
            return NULL ;
        }
        hash_bucket_p =
                DIRENT_CACHE_ALLOCATE_BUCKET_ARRAY(dirent_entry,bucket_idx)
        ;
        if (hash_bucket_p == NULL ) {
            /*
             ** something wrong, we run out of memory
             */
            return NULL ;

        }

        return dirent_entry;
    }
    /*
     ** No free entry has been found among the existing collision file, so we need to create a new collision file
     */
    if (free_coll_idx == -1) {
        coll_idx = DIRENT_CACHE_ALLOCATE_COLL_ENTRY_IDX(coll_bitmap_p)
        ;
    } else {
        coll_idx = free_coll_idx;
        DIRENT_CACHE_SET_ALLOCATED_COLL_ENTRY_IDX(coll_bitmap_p, coll_idx);
    }
    if (coll_idx < 0) {
        /*
         ** out of collision index-> the 2048 collision files are full
         */
        return NULL ;
    }
    /*
     ** indicates that the parent (root) image on disk must be updated
     */
    DIRENT_ROOT_UPDATE_REQ(parent);
    /*
     ** OK, now create a new dirent cache entry for that new dirent collision file
     */
    {
        mdirents_header_new_t header;

        header.level_index = parent->header.level_index + 1;
        for (i = 0; i < header.level_index; i++)
            header.dirent_idx[i] = parent->header.dirent_idx[i];
        header.dirent_idx[header.level_index] = coll_idx;

        dirent_entry = dirent_cache_create_entry(&header);
        if (dirent_entry == NULL ) {
            /*
             ** out of memory
             */
            DIRENT_CACHE_RELEASE_COLL_ENTRY_IDX(coll_bitmap_p, coll_idx);
            return NULL ;
        }
        /*
         ** store the new dirent pointer in the collision table of the parent
         */
        if (dirent_cache_store_collision_ptr(parent, dirent_entry) != NULL ) {
            /*
             ** something wrong: either runs out of memory or there is already a pointer here
             */
            DIRENT_CACHE_RELEASE_COLL_ENTRY_IDX(coll_bitmap_p, coll_idx);
            printf(
                    "FDL ->%s:%d ->Put code to release the dirent_entry context \n",
                    __FILE__, __LINE__);
            return NULL ;
        }
        /*
         ** OK now allocate on entry
         */
        sect0_p = DIRENT_VIRT_TO_PHY_OFF(dirent_entry,sect0_p)
        ;
        if (sect0_p == (mdirent_sector0_not_aligned_t*) NULL ) {
            return NULL ;
        }
        //hash_bitmap_p = (uint8_t*) &sect0_p->hash_bitmap;
        coll_bitmap_p = (uint8_t*) &sect0_p->coll_bitmap; // not used since only one level is handled
        /*
         ** Try to allocate the free entry from the parent dirent
         */
        hash_entry_bit_idx =
                DIRENT_CACHE_ALLOCATE_HASH_ENTRY_IDX(&sect0_p->hash_bitmap)
        ;
        if (hash_entry_bit_idx >= 0) {
            /*
             ** there is a local free entry available, so udpate the hash_p value and return
             */
            hash_p->type = MDIRENTS_HASH_PTR_COLL;
//         hash_p->level = parent->header.level_index;
            hash_p->idx = coll_idx;
            *local_idx_p = hash_entry_bit_idx;
            /*
             ** check if the hash entry section associated with the local index needs to be created
             */
//        printf("FDL BUG DIRENT_CACHE_ALLOCATE_HASH_ENTRY_ARRAY line %d for idx %d\n",__LINE__,hash_entry_bit_idx);
            hash_entry_p =
                    DIRENT_CACHE_ALLOCATE_HASH_ENTRY_ARRAY(dirent_entry,hash_entry_bit_idx)
            ;
            if (hash_entry_p == NULL ) {
                /*
                 ** something wrong, we run out of memory
                 */
                return NULL ;
            }
            hash_bucket_p =
                    DIRENT_CACHE_ALLOCATE_BUCKET_ARRAY(dirent_entry,bucket_idx)
            ;
            if (hash_bucket_p == NULL ) {
                /*
                 ** something wrong, we run out of memory
                 */
                return NULL ;

            }
            return dirent_entry;
        }
        /*
         ** If there is no entry within the new dirent cache entry, this can be considered as a fatal
         ** error since the system must provide the first available entry that is in this case 0
         */
        return NULL ;

    }
}

/*
 **______________________________________________________________________________
 */
/**
 *   That API is private. It update the pointer to a free dirent cache entry if none has been found

 @param p : pointer to a dirent cache entry
 @param **free_p : pointer to a pointer that contains the pointer to the current dirent cacha entry that has an free hash entry
 @param *coll_bitmap_hash_full_p: pointer to the collision dirent full bitmap (from root dirent cache entry)

 @retval none
 */
static inline void dirent_cache_udpate_free_entry(mdirents_cache_entry_t *p,
        mdirents_cache_entry_t **free_p, uint8_t *coll_bitmap_hash_full_p) {

    if (free_p == NULL ) {
        if (p->header.level_index == 0) {
            /*
             ** case of the root dirent file
             */
            if (p->hash_entry_full != 0)
                return;
            *free_p = p;
            return;
        }
        if (dirent_test_chunk_bit(p->header.dirent_idx[1],
                coll_bitmap_hash_full_p))
            return;
        *free_p = p;
        return;
    }
}
#define UPDATE_FREE_ENTRY(p) dirent_cache_udpate_free_entry(p, \
                           &cache_entry_free_p,\
                           coll_bitmap_hash_full_p);

//#define UPDATE_FREE_ENTRY_IDX(p,idx) { if ((cache_entry_free_p != NULL) && (first_entry_idx_in_free != (uint16_t)-1)) first_entry_idx_in_free = (uint16_t)idx;}
/**
 * Macro to register the pointer to the cache entry that has the EOF
 */
//#define SET_LAST_ENTRY(p) { cache_entry_last_p =p; last_entry_idx_in_last = -1; }
//#define SET_LAST_ENTRY_IDX(idx) { last_entry_idx_in_last = idx;}


/*
 **______________________________________________________________________________
 */
/**
 *   delete  a hash entry in the linked associated with a bucket entry, the key being the value of the hash
 *

 @param  dir_fd : file descriptor of the parent directory
 @param root : pointer to the root dirent entry
 @param hash_value : hash value to search
 @param bucket_idx : index of the hash bucket : taken from the lower 8 bits of the hash value applied to the name of the directory/file
 @param hash_entry_match_idx_p : pointer to an array where the local idx of the hash entry will be returned
 @param cache_entry_prev_ret : pointer to an array where the pointer to the previous cache entry will be stored, a NULL
 pointer indicates that the previous dirent cache is not impacted by the removing of the hash entry
 @param name : pointer to the name to search or NULL if lookup hash is requested only
 @param len  : len of name
 @param fid: pointer to array used for storing fid of the entry to remove (nULL if not expected to be returned)
 @param mode_p: pointer to the array where mode will be copied

 @retval <> NULL: pointer to the dirent cache entry where hash entry can be found, local idx is found in hash_entry_match_idx_p
 @retval NULL: not found
 */
mdirents_cache_entry_t *dirent_cache_delete_hash_entry(int dir_fd,
        mdirents_cache_entry_t *root, int bucket_idx, uint32_t hash_value,
        int *hash_entry_match_idx_p,
        mdirents_cache_entry_t **cache_entry_prev_ret, uint8_t *name,
        uint16_t len, void *fid, uint32_t *mode_p) {
    mdirents_hash_ptr_t *hash_bucket_p;
    mdirents_hash_ptr_t *hash_bucket_prev_p;
    mdirents_hash_entry_t *hash_entry_cur_p;
    mdirents_cache_entry_t *cache_entry_cur;
    mdirents_cache_entry_t *cache_entry_prev;
    uint32_t coll_cnt = 0;
    int bucket_idx_used;
    uint8_t *coll_bitmap_hash_full_p;
    int cur_hash_entry_idx;
    int repair = 0;
    dirent_file_repair_cause_e    cause;



reloop: 
    *hash_entry_match_idx_p = -1;
    *cache_entry_prev_ret = NULL;
    mdirent_cache_ptr_t *bucket_virt_p = NULL;
    mdirent_cache_ptr_t *bucket_prev_virt_p;
    mdirent_cache_ptr_t *hash_virt_p = NULL;
    mdirent_cache_ptr_t *hash_prev_virt_p = NULL;
    cur_hash_entry_idx = -1;   
    cache_entry_cur = root;
    cache_entry_prev = root;
    hash_bucket_p =
            DIRENT_CACHE_GET_BUCKET_PTR_WITH_VIRT(cache_entry_cur,bucket_idx,&bucket_virt_p)
    ;
    hash_bucket_prev_p = hash_bucket_p;
    bucket_prev_virt_p = bucket_virt_p;

    /*
     ** need the pointer to the bitmap of the collision dirent to release the bit when an entry from one
     ** collision file is released
     */
    coll_bitmap_hash_full_p =
            DIRENT_VIRT_TO_PHY_OFF(root,coll_bitmap_hash_full_p)
    ;
    if (coll_bitmap_hash_full_p == (uint8_t*) NULL ) {
        DIRENT_SEVERE("FDL dirent_cache_delete_hash_entry: coll_bitmap_fill is NULL line %d\n",__LINE__)
        ;
        return NULL ;
    }
    /*
     ** indicate that the index used for getting hash_bucket_p is bucket_idx
     */

     bucket_idx_used = 1;
    /*
     ** some comments:
     **  A) hash_bucket_p corresponds either to an mdirents_hash_ptr_t pointer that points either to
     **     the bucket_idx entry when bucket_idx_used is asserted or to the previous mdirents_hash_ptr_t of a hash_entry.
     **
     **  B) hash_entry_cur_p is the pointer to the current hash entry.
     **
     **  C) bucket_idx_used is asserted each time we move from one collision dirent cache entry to a new one
     **    By default it is asserted when we start from root.
     */
    while (1) {
        coll_cnt += 1;
        if (hash_bucket_p == NULL ) {
            /*
             ** There is no entry for that hash
             */
            DIRENT_CACHE_LOOKUP_UPDATE_STATS(coll_cnt);
            return NULL ;
        }
	/*
	**_______________________________________________
	**        LOOP DETECTION PROCEDURE:
	**
	**  attempt to repair if it was not already done.
	** Then restart from the beginning.
	**_______________________________________________
	*/
	if (coll_cnt >= DIRENT_MAX_SUPPORTED_COLL)
	{
          /*
          ** check if the linked list has already been repaired, because if it was not the case
          ** we attempt to do it and then we retry from the beginning
          */
          if (repair == 0)
          {
            dirent_file_repair(dir_fd,root,bucket_idx,DIRENT_REPAIR_LOOP);
            repair = 1;
            goto reloop;
          }
          /*
          ** repair has already been done, so exit
          */
          DIRENT_SEVERE("dirent_cache_delete_hash_entry: collision counter exhausted for bucket_idx %d dirent[%d.%d]\n",
                       bucket_idx,cache_entry_cur->header.dirent_idx[0],cache_entry_cur->header.dirent_idx[1]);
          return NULL;
	}


        /*
	**_______________________________________________
	**        HASH ENTRY validity control
	**
	**  Check if the current entry is valid
	*   If it is not the case attempt a repair
	**  and then reloop
	** If repair has already been done, exit
	** with NULL
	**_______________________________________________
	*/
	if (repair == 0)
	{
          cause =  dirent_cache_check_repair_needed(root,cache_entry_cur,hash_bucket_p,bucket_idx);
          if (cause != DIRENT_REPAIR_NONE)
          {
            dirent_file_repair(dir_fd,root,bucket_idx,cause);
            repair = 1;
            goto reloop;
          }
        }


        if (hash_bucket_p->type == MDIRENTS_HASH_PTR_EOF) {
            /*
             ** This the end of list and not entry match with the requested hash value
             */
            DIRENT_CACHE_LOOKUP_UPDATE_STATS(coll_cnt);
            return NULL ;
        }
        if (hash_bucket_p->type == MDIRENTS_HASH_PTR_LOCAL) {
            /*
             ** the index is local
             */
            cur_hash_entry_idx = hash_bucket_p->idx;

            hash_entry_cur_p =
                    (mdirents_hash_entry_t*) DIRENT_CACHE_GET_HASH_ENTRY_PTR_WITH_VIRT(cache_entry_cur,cur_hash_entry_idx,&hash_virt_p)
            ;
            if (hash_entry_cur_p == NULL ) {
                /*
                 ** something wrong!! (either the index is out of range and the memory array has been released
                 */
                printf(
                        "dirent_cache_delete_hash_entry error at %d cur_hash_entry_idx %d \n",
                        __LINE__, cur_hash_entry_idx);
                DIRENT_CACHE_LOOKUP_UPDATE_STATS(coll_cnt);
                return NULL ;
            }
#if 0 // FDL_DEBUG
            if ((hash_entry_cur_p->hash == 4104964088) &&
                    (cache_entry_cur->header.dirent_idx[0] == 1528))

            {
                printf("Delete found  hash_idx %u -> in dirent file %s idx %d.%d (level %d)\n",
                        cur_hash_entry_idx,name,
                        cache_entry_cur->header.dirent_idx[0],
                        cache_entry_cur->header.dirent_idx[1],
                        cache_entry_cur->header.level_index);

            }

#endif
            /*
             ** Check if there is a match with that value
             */
            if (hash_entry_cur_p->hash
                    == (hash_value & DIRENT_ENTRY_HASH_MASK)) {
                uint8_t *hash_bitmap_p;
                mdirent_sector0_not_aligned_t *sect0_p;
                /*
                 ** get the pointer to the name entry
                 */
#if 1  // set to 0 for hash lookup only
                if (name != NULL ) {
                    mdirents_name_entry_t *name_entry_p;
                    int k;
                    name_entry_p =
                            (mdirents_name_entry_t*) dirent_get_entry_name_ptr(
                                    dir_fd, cache_entry_cur,
                                    hash_entry_cur_p->chunk_idx,
                                    DIRENT_CHUNK_NO_ALLOC);
                    if (name_entry_p == (mdirents_name_entry_t*) NULL ) {
                        /*
                         ** something wrong that must not occur
                         */
                        DIRENT_SEVERE("dirent_cache_delete_hash_entry: pointer does not exist at line %d\n",__LINE__)
                        ;
                        return NULL ;
                    }
                    /*
                     ** check if the entry match with name
                     */
                    if (len != name_entry_p->len) {
                        /*
                         ** try the next entry
                         */
//             printf("lookup hash+name hash found %u but bad len %d ->%s %d->%s \n",
//                      hash_value,len,name,name_entry_p->len,
//                      name_entry_p->name);
                        /*
                         ** the hash bucket pointer is a pointer to a hash entry at that level
                         */
                        hash_bucket_prev_p = hash_bucket_p;
                        //prev_hash_entry_idx = cur_hash_entry_idx;
                        /*
                         ** store the current hash entry virtual pointer reference in case the next is the good one
                         */
                        hash_prev_virt_p = hash_virt_p;
                        bucket_idx_used = 0;
                        hash_bucket_p = &hash_entry_cur_p->next;
                        continue;
                    }
                    int not_found = 0;
                    for (k = 0; k < len; k++) {
                        if (name_entry_p->name[k] != (char) name[k]) {
                            /*
                             ** not the right entry
                             */
#if 0 // FDL_DEBUG
                            printf("lookup hash+name hash found %u (chunk %d) but entry %s (%s)of dirent file idx %d.%d (level %d)\n",
                                    hash_value,hash_entry_cur_p->chunk_idx,name,name_entry_p->name,
                                    cache_entry_cur->header.dirent_idx[0],
                                    cache_entry_cur->header.dirent_idx[1],
                                    cache_entry_cur->header.level_index);
                            exit(0);
#endif
                            not_found = 1;
                            break;

                        }
                    }
                    if (not_found) {
                        /*
                         ** try the next entry
                         */
                        hash_bucket_prev_p = hash_bucket_p;

                        //prev_hash_entry_idx = cur_hash_entry_idx;
                        hash_prev_virt_p = hash_virt_p;

                        bucket_idx_used = 0;
                        hash_bucket_p = &hash_entry_cur_p->next;
                        continue;
                    }
                    /*
                     ** OK, we have the exact Match
                     */
                    if (fid != NULL )
                        memcpy(fid, name_entry_p->fid, sizeof(fid_t));
                    if (mode_p != NULL )
                        *mode_p = name_entry_p->type;
                    /*
                     ** release the bits relative to the neme entry
                     */
                    dirent_cache_del_entry_name(cache_entry_cur,
                            hash_entry_cur_p->chunk_idx,
                            hash_entry_cur_p->nb_chunk);
                }
#endif

                /*
                 ** entry has been found-> remove it
                 ** hash_virt_p -> it the reference of the array that contains the entry that will be updated with the pnext
                 **                of the entry that will be removed. That assertion is true if the bucket_idx_used flag is nit asserted
                 *                 When the flag is asserted it indicates that hash_virt_p is the pointer that is currently removed
                 */
                sect0_p = DIRENT_VIRT_TO_PHY_OFF(cache_entry_cur,sect0_p)
                ;
                if (sect0_p == (mdirent_sector0_not_aligned_t*) NULL ) {
                    DIRENT_SEVERE("FDL dirent_cache_release_entry error at line %d\n",__LINE__)
                    ;
                    return NULL ;
                }
                hash_bitmap_p = (uint8_t*) &sect0_p->hash_bitmap;
                /*
                 ** check if the entry to remove is the one reference at the bucket level
                 */
                if (bucket_idx_used) {
                    /*
                     ** A) check if it is the last entry of the linked list associated with bucket idx:
                     ** in that case we need to update the pointer on the previous collision entry
                     ** Notice that for the case of the root there is no previous cache entry because
                     ** it is the root. To avoid testing that the previous cache entry is root, we set
                     ** hash_bucket_p and hash_bucket_prev_p to point to the same bucket idx on the
                     ** dirent root cache entry at initialization time
                     **
                     ** B) There is the same situation when the next belongs to a collision file
                     **   in that case it does not make sense to keep the entry in that dirent file
                     **   since there is hash entry allocated for that bucket index in it.
                     **   So in order to be able to release a current collision dirent on which
                     **   we remove the last entry of a bucket, we need to update the next pointer
                     **   of the previous dirent collision file
                     */
                    if ((hash_entry_cur_p->next.type == MDIRENTS_HASH_PTR_EOF)
                            || (hash_entry_cur_p->next.type
                                    == MDIRENTS_HASH_PTR_COLL)) {

                        /*
                         ** indicates that previous cache entry must be also updated
                         */
                        *cache_entry_prev_ret =
                                (cache_entry_cur == root) ?
                                        NULL : cache_entry_prev;
                        DIRENT_ROOT_UPDATE_REQ(cache_entry_prev);
			if (hash_entry_cur_p->next.type == MDIRENTS_HASH_PTR_EOF)
			{
                          hash_bucket_prev_p->type = hash_entry_cur_p->next.type;
			}
			else
			{
                          *hash_bucket_prev_p = hash_entry_cur_p->next;			
			}
			/*
			** it was the last entry for that bucket the collision file, so set EOF in the 
			** current bucket: this applies to collision files only
			*/
			if (cache_entry_cur != root)
			{
			  hash_bucket_p->type = MDIRENTS_HASH_PTR_EOF;
			}
                        /*
                         ** assert the dirty bit for the memory array associated with the bucket index
                         */
                        bucket_prev_virt_p->s.dirty = 1;
                        /*
                         ** notice that we do not assert the dirty bit for the current removed entry. Eventhough that entry
                         ** is removed, we leave its conta=ent untouched, we rather just clear the corresponding bit in the
                         ** hash entry bitmap (that bitmap is always re-written)
                         */
                        *hash_entry_match_idx_p = cur_hash_entry_idx;
                        DIRENT_CACHE_LOOKUP_UPDATE_STATS(coll_cnt);
                        /*
                         ** clear the hash_entry_full flag if cache_entry_cur is the root
                         */
                        if (cache_entry_cur == root)
                            root->hash_entry_full = 0;
                        else {
                            /*
                             ** case of a collision file: get the index from the header (it corresponds to the index level 1 (level 0,
                             ** is always the level associated with the root)
                             */
                            DIRENT_CACHE_SET_COLL_ENTRY_NOT_FULL(
                                    coll_bitmap_hash_full_p,
                                    cache_entry_cur->header.dirent_idx[1]);
                        }
                        /*
                         ** release the hash entry on bitmap and eventually release the memory if it was the last
                         */
                        DIRENT_CACHE_RELEASE_HASH_ENTRY_IDX(hash_bitmap_p,
                                cur_hash_entry_idx);
                        DIRENT_CACHE_RELEASE_HASH_ENTRY_ARRAY(cache_entry_cur,
                                hash_bitmap_p, cur_hash_entry_idx);
                        DIRENT_ROOT_UPDATE_REQ(cache_entry_cur);
                        return cache_entry_cur;
                    }
                }
                /*
                 ** OK it was not the entry reference at bucket_idx of the current cache entry so
                 ** just update the previous hash_entry next pointer
                 */
                *hash_bucket_p = hash_entry_cur_p->next;
                /*
                 ** assert the dirty bit on the previous hash entry. There is no need to update the hash bucket
                 ** memory array since the previous is a hash entry.
                 */
                if (bucket_idx_used == 0) {
                    if (hash_prev_virt_p == 0) {
                        /*
                         ** that case MUST not happen-> so we will crash if it happens
                         */
                        DIRENT_SEVERE("dirent_cache_delete_hash_entry (%d) hash_prev_virt_p is NULL \n",__LINE__)
                        ;

                    }
                    hash_prev_virt_p->s.dirty = 1;
                }
                /*
                 ** return to the caller the local reference of the hash entry
                 */
                *hash_entry_match_idx_p = cur_hash_entry_idx;
                /*
                 ** clear the hash_entry_full flag if cache_entry_cur is the root
                 */
                if (cache_entry_cur == root)
                    root->hash_entry_full = 0;
                else {
                    /*
                     ** case of a collision file: get the index from the header (it corresponds to the index level 1 (level 0,
                     ** is always the level associated with the root)
                     */
                    DIRENT_CACHE_SET_COLL_ENTRY_NOT_FULL(
                            coll_bitmap_hash_full_p,
                            cache_entry_cur->header.dirent_idx[1]);
                }
                /*
                 ** release the hash entry on bitmap and eventually release the memory if it was the last
                 */
                DIRENT_CACHE_RELEASE_HASH_ENTRY_IDX(hash_bitmap_p,
                        cur_hash_entry_idx);
                DIRENT_CACHE_RELEASE_HASH_ENTRY_ARRAY(cache_entry_cur,
                        hash_bitmap_p, cur_hash_entry_idx);
                DIRENT_ROOT_UPDATE_REQ(cache_entry_cur);

                DIRENT_CACHE_LOOKUP_UPDATE_STATS(coll_cnt);
                return cache_entry_cur;
            }
            /*
             ** try the next entry
             */
            hash_bucket_prev_p = hash_bucket_p;
            //prev_hash_entry_idx = cur_hash_entry_idx;
            hash_prev_virt_p = hash_virt_p;

            bucket_idx_used = 0;
            hash_bucket_p = &hash_entry_cur_p->next;
            continue;
        }
        /*
         ** the current cache entry is now the previous cache entry
         */
#if 0 // FDL_DEBUG
        if ((bucket_idx == fdl_debug_bucket_idx) && (cache_entry_cur->header.dirent_idx[0] == fdl_debug_dirent_file_idx))
        {
            printf("Match for fdl_dbg_bucket_idx %d name %s\n",bucket_idx,name);

        }

#endif
        cache_entry_prev = cache_entry_cur;
        /*
         ** The next entry belongs to a dirent collision entry: need to get the pointer to that collision
         ** entry and to read the content of the hash bucket associated with the bucket idx
         */
        cache_entry_cur = dirent_cache_get_collision_ptr(root,
                hash_bucket_p->idx);
        if (cache_entry_cur == NULL ) {
            /*
             ** something is rotten in the cache since the pointer to the collision dirent cache
             ** does not exist
             */
            DIRENT_SEVERE("FDL error at %d\n",__LINE__)
            ;
            DIRENT_CACHE_LOOKUP_UPDATE_STATS(coll_cnt);
            return NULL ;
        }
        hash_bucket_prev_p = hash_bucket_p;
        bucket_prev_virt_p = bucket_virt_p;
        hash_bucket_p =
                DIRENT_CACHE_GET_BUCKET_PTR_WITH_VIRT(cache_entry_cur,bucket_idx,&bucket_virt_p)
        ;
        bucket_idx_used = 1;
        continue;
    }

    return NULL ;
}

/*
 **______________________________________________________________________________
 */
/**
 *   Dispaly a  bucket_idx linked list starting for root cache entry
 *

 @param root : pointer to the root dirent entry
 @param hash_value : hash value to search
 @param bucket_idx : index of the hash bucket : taken from the lower 8 bits of the hash value applied to the name of the directory/file
 @param hash_entry_match_idx_p : pointer to an array where the local idx of the hash entry will be returned

 @retval <> NULL: pointer to the dirent cache entry where hash entry can be found, local idx is found in hash_entry_match_idx_p
 @retval NULL: not found
 */
mdirents_cache_entry_t *dirent_cache_print_bucket_list(
        mdirents_cache_entry_t *root, int bucket_idx) {
    mdirents_hash_ptr_t *hash_bucket_p;
//   mdirents_hash_entry_t *hash_entry_p;
    mdirents_hash_entry_t *hash_entry_cur_p;
    mdirents_cache_entry_t *cache_entry_cur;

    printf("Display bucket Idx %d linked list\n", bucket_idx);

    int cur_hash_entry_idx = -1;
    cache_entry_cur = root;
    hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR(cache_entry_cur,bucket_idx)
    ;
    while (1) {
        if (hash_bucket_p == NULL ) {
            /*
             ** There is no entry for that hash
             */
            printf("\nEnd of List at Line %d\n", __LINE__);
            return NULL ;
        }
        if (hash_bucket_p->type == MDIRENTS_HASH_PTR_EOF) {
            /*
             ** This the end of list and not entry match with the requested hash value
             */
            printf("\nEnd of List at Line %d\n", __LINE__);
            return NULL ;
        }
        if (hash_bucket_p->type == MDIRENTS_HASH_PTR_LOCAL) {
            /*
             ** the index is local
             */
            cur_hash_entry_idx = hash_bucket_p->idx;
            hash_entry_cur_p =
                    (mdirents_hash_entry_t*) DIRENT_CACHE_GET_HASH_ENTRY_PTR(cache_entry_cur,cur_hash_entry_idx)
            ;
            if (hash_entry_cur_p == NULL ) {
                /*
                 ** something wrong!! (either the index is out of range and the memory array has been released
                 */
                printf("FDL error at %d cur_hash_entry_idx %d \n", __LINE__,
                        cur_hash_entry_idx);
                printf("\nAbnormal End of List at Line %d\n", __LINE__);
                return NULL ;
            }
            /*
             ** Display the entry
             */
            if (hash_entry_cur_p->next.type == MDIRENTS_HASH_PTR_EOF) {
                printf("%3.3d :hash :%8.8d next->EOF \n", cur_hash_entry_idx,
                        hash_entry_cur_p->hash);

            } else {
                printf("%3.3d: hash :%8.8d next->%s:%4.4d\n",
                        cur_hash_entry_idx, hash_entry_cur_p->hash,
                        (hash_entry_cur_p->next.type == MDIRENTS_HASH_PTR_LOCAL) ?
                                "LOC" : "COL", hash_entry_cur_p->next.idx);
            }
            /*
             ** try the next entry
             */
            hash_bucket_p = &hash_entry_cur_p->next;
            continue;
        }
        /*
         ** The next entry belongs to a dirent collision entry: need to get the pointer to that collision
         ** entry and to read the content of the hash bucket associated with the bucket idx
         */
        printf("\n");
        cache_entry_cur = dirent_cache_get_collision_ptr(root,
                hash_bucket_p->idx);
        if (cache_entry_cur == NULL ) {
            /*
             ** something is rotten in the cache since the pointer to the collision dirent cache
             ** does not exist
             */
            printf("FDL error at %d\n", __LINE__);
            printf("\nAbnormal End of List at Line %d\n", __LINE__);
            return NULL ;
        }
        hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR(cache_entry_cur,bucket_idx)
        ;
        continue;
    }

    printf("\nAbnormal End of List at Line %d\n", __LINE__);
    return NULL ;
}

/**
 *_________________________________________________________________________________
 F I L E    S E R V I C E S
 *_________________________________________________________________________________
 */
/**
*   SET  -2  ->FOR ROOT FILE	
*   SET -1 -> INACTIVE
*   any other value is the index of the collision file
*/
int fdl_debug_coll_idx = -1;
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
mdirents_cache_entry_t * read_mdirents_file(int dirfd,
        mdirents_header_new_t *dirent_hdr_p) {
    int fd = -1;
    int flag = O_RDONLY;
    mdirents_cache_entry_t * dirent_p = NULL;
    char pathname[64];
    char *path_p;
    off_t offset;
    uint64_t val;
    mdirents_file_t *dirent_file_p = NULL;
    mdirent_sector0_not_aligned_t *sect0_p = NULL;
    int ret;
    /*
    ** clear errno
    */
    errno = 0;

    /*
     ** build the filename of the dirent file to read
     */
    path_p = dirent_build_filename(dirent_hdr_p, pathname);
    if (path_p == NULL ) {
        /*
         ** something wrong that must not happen
         */
        DIRENT_SEVERE("Cannot build pathname line %d\n",__LINE__);
        goto error;
    }
    
#if 0
#warning EIO debugging (collision file reading)
    if (fdl_debug_coll_idx != -1)
    {
       if ((fdl_debug_coll_idx == -2) && (dirent_hdr_p->level_index == 0))
       {
	     errno = EIO;
	     goto error;       
       
       }
       
        if (dirent_hdr_p->level_index == 1)
	{

	   if (dirent_hdr_p->dirent_idx[1] == fdl_debug_coll_idx)
	   {
	     errno = EIO;
	     goto error;
	   }

	}
    
    }
#endif
    
    if ((fd = openat(dirfd, path_p, flag, S_IRWXU)) == -1) {
        //DIRENT_SEVERE("Cannot open the file %s, error %s at line %d\n",path_p,strerror(errno),__LINE__);
	/*
	** check if the file exists. If might be possible that the file does not exist, it can be considered
	** as a normal error since the exportd might have crashed just after the deletion of the collision dirent
	** file but before  the update of the dirent root file.
	*/
	if (errno == ENOENT)
	{
	  goto out;
	
	}
	/*
	** fatal error on file opening
	*/
	
	DIRENT_SEVERE("Cannot open the file %s, error %s at line %d\n",path_p,strerror(errno),__LINE__);
	errno = EIO;
        goto error;
    }
    /*
     ** Allocate a fresh free mdirent cache entry
     */
    dirent_p = dirent_cache_allocate_entry(dirent_hdr_p);
    if (dirent_p == NULL ) {
        /*
         ** the system runs out of memory
         */
	errno = ENOMEM;
        DIRENT_SEVERE("Out of Memory at line %d", __LINE__)
        ;
        goto error;
    }
    /*
     ** allocate a working array for storing the content of the file except the part
     ** that contains the name/fid and mode
     */
    dirent_file_p = DIRENT_MALLOC(sizeof(mdirents_file_t))
    ;
    if (dirent_file_p == NULL ) {
        /*
         ** the system runs out of memory
         */
	errno = ENOMEM;
        DIRENT_SEVERE("Out of Memory at line %d", __LINE__)
        ;
        goto error;
    }
    /*
     ** read the fixed part of the dirent file
     */
    offset = DIRENT_HEADER_BASE_SECTOR * MDIRENT_SECTOR_SIZE;
    ret = DIRENT_PREAD(fd, dirent_file_p, sizeof(mdirents_file_t), offset);
    if (ret <  0)
    {
      /*
      ** need to figure out what need to be done since this might block a chunk of file
      */
      DIRENT_SEVERE("pread failed in file %s: %s", pathname, strerror(errno));
      errno = EIO;
      goto error;   
    }
    if (ret != sizeof(mdirents_file_t)) 
    {
       
	/*
	** we consider that error as the case of the file that does not exist. By ignoring that file
	** we just lose potentially one file at amx
	*/
        DIRENT_SEVERE("incomplete pread in file %s %d (expected: %d)", pathname,ret,(int)sizeof(mdirents_file_t));
	errno = ENOENT;
	/*
	** release the allocated cache context
	*/
        dirent_cache_release_entry(dirent_p);
	dirent_p = NULL;
        goto out;
    }

    /*
     ** fill sector 0 in dirent cache entry
     */
    /*
     ** Sector 0 init
     */
    {
        sect0_p = DIRENT_MALLOC(sizeof( mdirent_sector0_not_aligned_t))
        ;
        if (sect0_p == NULL )
	{
	    severe("out of memroy while reading %s file ",pathname);
	    errno = ENOMEM;
            goto error;
	}

        memcpy(sect0_p, &dirent_file_p->sect0,
                sizeof(mdirent_sector0_not_aligned_t));
        /*
         ** store the virtual pointer
         */
        val = (uint64_t) (uintptr_t) sect0_p;
        dirent_p->sect0_p.s.val = val;

    }
    /*
     ** init of the bitmap that handles the case of the dirent collision file full
     */
    {
        mdirents_btmap_coll_dirent_t *coll_bitmap_hash_full_p;
        coll_bitmap_hash_full_p =
                DIRENT_MALLOC(sizeof( mdirents_btmap_coll_dirent_t))
        ;
        if (coll_bitmap_hash_full_p == NULL )
	{
	    severe("out of memroy while reading %s file ",pathname);
	    errno = ENOMEM;
            goto error;
	}
        memset(coll_bitmap_hash_full_p, 0,
                sizeof(mdirents_btmap_coll_dirent_t));
        /*
         ** store the virtual pointer
         */
        val = (uint64_t) (uintptr_t) coll_bitmap_hash_full_p;
        dirent_p->coll_bitmap_hash_full_p.s.val = val;

    }
    /*
     ** Sector 1: name bitmap
     */
    {
        mdirents_btmap_free_chunk_t *name_bitmap_p;
        name_bitmap_p = DIRENT_MALLOC(sizeof( mdirents_btmap_free_chunk_t))
        ;
        if (name_bitmap_p == NULL ) {
	    severe("out of memroy while reading %s file ",pathname);
	    errno = ENOMEM;
            goto error;
        }
        memcpy(name_bitmap_p, &dirent_file_p->sect1,
                sizeof(mdirents_btmap_free_chunk_t));

        /*
         ** store the virtual pointer
         */
        val = (uint64_t) (uintptr_t) name_bitmap_p;
        dirent_p->name_bitmap_p.s.val = val;
    }
    /*
     ** sector 2  :
     ** Go through the hash table bucket an allocated memory for buckets that are no empty:
     **  there are   256 hash buckets of 16 bits
     */
    {
        mdirent_cache_ptr_t *hash_tbl_p;
        int i, k;
        uint64_t val = 0;
        uint8_t *elem_p;

        uint16_t *file_ptr = (uint16_t*) &dirent_file_p->sect2;
        hash_tbl_p = dirent_p->hash_tbl_p;

        for (i = 0; i < MDIRENTS_HASH_TB_CACHE_MAX_IDX; i++) {
            for (k = 0; k < MDIRENTS_HASH_TB_CACHE_MAX_ENTRY; k++) {
                if (file_ptr[k] == 0)
                    continue;
                /*
                 ** there is a valid entry so need to allocate the memory array to store it
                 */
                elem_p =DIRENT_MALLOC(sizeof(mdirents_hash_ptr_t)*MDIRENTS_HASH_TB_CACHE_MAX_ENTRY)
                ;
                if (elem_p == NULL ) {
                    /*
                     ** out of memory
                     */
		    severe("out of memroy while reading %s file ",pathname);
		    errno = ENOMEM;
        	    goto error;                    ;
                }
                val = (uint64_t) (uintptr_t) elem_p;
                hash_tbl_p[i].s.val = val;
                hash_tbl_p[i].s.dirty = 0;
                hash_tbl_p[i].s.rd = 0;
                /*
                 ** copy the array into the memory
                 */
                memcpy(elem_p, file_ptr,
                        sizeof(mdirents_hash_ptr_t)
                                * MDIRENTS_HASH_TB_CACHE_MAX_ENTRY);
                break;

            }
            file_ptr += MDIRENTS_HASH_TB_CACHE_MAX_ENTRY;
        }
    }

    /*
     ** Go through the hash entry bitmap to fill up the valid entries in the cache
     */
    {
        /*
         ** the system checks thet hash entry bitmap by block of 64 entries since on
         ** memory array can contain up to 64 hash entries
         */
        uint64_t *p64;
        int index;
        uint64_t val = 0;
        uint64_t bitmap_val = 0;
        uint8_t *elem_p;
        mdirent_cache_ptr_t *hash_entry_p;
        uint8_t *membyte_p = (uint8_t*) dirent_file_p;

        hash_entry_p = dirent_p->hash_entry_p;

        bitmap_val = ~bitmap_val;

        p64 = (uint64_t*) &sect0_p->hash_bitmap;

        for (index = 0; index < MDIRENTS_HASH_CACHE_MAX_IDX; index++) {
            if (p64[index] == bitmap_val)
                continue;
            /*
             ** there at least one entry that is used: need to allocate a free array
             */
            elem_p =
                    DIRENT_MALLOC(sizeof(mdirents_hash_entry_t)*MDIRENTS_HASH_CACHE_MAX_ENTRY)
            ;
            if (elem_p == NULL ) {
                /*
                 ** out of memory
                 */
		severe("out of memroy while reading %s file ",pathname);
		errno = ENOMEM;
        	goto error;
            }
            val = (uint64_t) (uintptr_t) elem_p;
            hash_entry_p[index].s.val = val;
            hash_entry_p[index].s.dirty = 0;
            hash_entry_p[index].s.rd = 0;
            /*
             ** copy the array into the memory
             */
            memcpy(elem_p,
                    &membyte_p[(DIRENT_HASH_ENTRIES_BASE_SECTOR + index)
                            * MDIRENT_SECTOR_SIZE], MDIRENT_SECTOR_SIZE);
        }

    }
    /*
     ** Process the case of the collision file: only for a root dirent file only
     */
    if (dirent_p->header.level_index == 0) {
        /*
         ** this is a root dirent file, so process the collision file
         */
        int coll_idx = 0;
        int next_coll_idx = 0;
        uint8_t chunk_u8_idx;
        int bit_idx;
        int loop_cnt = 0;
        mdirents_cache_entry_t *dirent_coll_entry_p = NULL;
        mdirents_header_new_t dirent_hdr;
        uint8_t *coll_bitmap_p = (uint8_t*) &sect0_p->coll_bitmap;
	
	/*
	** allocate the save bitmap for root entry only
	*/
	dirent_p->bucket_safe_bitmap_p = DIRENT_MALLOC(MDIRENTS_HASH_TB_INT_SZ/8);
	if (dirent_p->bucket_safe_bitmap_p == NULL ) {
            /*
             ** out of memory
             */
	     errno = ENOMEM;
	     goto error;
	}
        memset( dirent_p->bucket_safe_bitmap_p,0,MDIRENTS_HASH_TB_INT_SZ/8);
	
        while (coll_idx < MDIRENTS_MAX_COLLS_IDX) {

            chunk_u8_idx = coll_idx / 8;
            bit_idx = coll_idx % 8;
            /*
             ** there is no collision dirent entry or the collision dirent entry exist and is not full
             */
            if ((coll_bitmap_p[chunk_u8_idx] & (1 << bit_idx)) != 0) {
                /*
                 ** That entry is free, need to find out the next entry that is busy (0: busy, 1:free)
                 */
                if (coll_idx % 8 == 0) {
                    next_coll_idx = check_bytes_val(coll_bitmap_p, coll_idx,
                            MDIRENTS_MAX_COLLS_IDX, &loop_cnt, 1);
                    if (next_coll_idx < 0)
                        break;
                    /*
                     ** next  chunk
                     */
                    if (next_coll_idx == coll_idx)
                        coll_idx++;
                    else
                        coll_idx = next_coll_idx;
                    continue;
                }
                /*
                 ** next chunk
                 */
                coll_idx++;
                continue;
            }
            /*
             ** the collision file with index coll_idx exist, so need to load it up in memory
             */
            dirent_hdr.type = MDIRENT_CACHE_FILE_TYPE;
            dirent_hdr.level_index = 1;
            dirent_hdr.dirent_idx[0] = dirent_p->header.dirent_idx[0];
            dirent_hdr.dirent_idx[1] = coll_idx;

            dirent_coll_entry_p = read_mdirents_file(dirfd, &dirent_hdr);
            if (dirent_coll_entry_p == NULL ) 
	    {
	       /*
	       ** check the case of the collision  file that does not exist
	       ** if the collision file does not exist, then we clear the collision bit in the dirent root
	       ** file
	       */
	       if (errno == ENOENT)
	       {
	         errno = 0;
	         coll_bitmap_p[chunk_u8_idx] |=(1 << bit_idx);
		 /*
		 ** assert the flag that indicates that dirent root file must be udpate on disk
		 */
		 DIRENT_ROOT_UPDATE_REQ(dirent_p);
                 /*
                 ** next chunk
                 */
                 coll_idx++;
                 continue;	    	       
	       }
	       else if (errno == EIO)
	       {
		/*
		** goal : skip collision file on read disk error
		*/
     	         /*
		 ** keep the collision file in the bitmap of the allocated collision file but 
		 ** need to assert its associated bit in the hash_collision_bitmap_full set to
		 ** avoid the allocation of a hash_entry
		 */
                 uint8_t *coll_bitmap_hash_full_p = DIRENT_VIRT_TO_PHY_OFF(dirent_p,coll_bitmap_hash_full_p)	       
                 DIRENT_CACHE_SET_COLL_ENTRY_FULL(coll_bitmap_hash_full_p, coll_idx);
                 coll_idx++;
                 continue;
	       }
	       else
	       {
                /*
                 ** fatal error
                 */
                DIRENT_SEVERE("error while reading collision file at line %d\n",__LINE__);
                goto error;
	       }

            }
            /*
             ** store the collision entry in the parent
             */
            if (dirent_cache_store_collision_ptr(dirent_p,
                    dirent_coll_entry_p) != NULL) {
                DIRENT_SEVERE("error while storing collision cache pointer at line %d\n",__LINE__);
                goto error;
            }
            /*
             ** check next bit
             */
            coll_idx++;
        }
    }
    /*
     **  go through the name entry bitmap to find out what are the memory array for which
     **  there the presence of some valid chunk. The name entries are not loaded from disk
     ** we just assert the corresponding bit in the associated presence bitmap. This is
     ** needed to figure out if disk must be read after the allocation of a memory array
     ** used for storing the name entries
     */
#if 1
    {
        uint32_t *p32 = DIRENT_VIRT_TO_PHY_OFF(dirent_p,name_bitmap_p)
        ;
        int first_chunk_of_array = 0;

        for (first_chunk_of_array = 0;
                first_chunk_of_array < MDIRENTS_NAME_CHUNK_MAX_CNT;
                first_chunk_of_array +=
                        MDIRENTS_CACHE_NB_CHUNK_PER_CHUNK_ARRAY) {

            /*
             ** check if there is at least one chunk that is used
             */
            int i;

            for (i = 0; i < MDIRENTS_CACHE_NB_CHUNK_PER_CHUNK_ARRAY / 32; i++) {
                if (p32[(first_chunk_of_array / 32) + i] != 0xffffffff) {
                    /*
                     ** at least one chunk is in use, so do not release the memory array
                     */
                    dirent_set_chunk_bit(
                            first_chunk_of_array
                                    / MDIRENTS_CACHE_NB_CHUNK_PER_CHUNK_ARRAY,
                            dirent_p->name_entry_array_btmap_presence);
                    break;
                }
            }
        }
    }
#endif    
out: 
    if (fd != -1)
        close(fd);
    if (dirent_file_p != NULL )
        DIRENT_FREE(dirent_file_p);
    return dirent_p;
    
error: 
    DIRENT_ROOT_SET_READ_ONLY();
    if (dirent_p != NULL ) {
        dirent_cache_release_entry(dirent_p);
    }
    if (dirent_file_p != NULL )
        DIRENT_FREE(dirent_file_p);
    if (fd != -1)
        close(fd);
    return NULL ;
}

#if 0
#warning WRITE_MDIRENTS_FILE  not optimized
/*
 **______________________________________________________________________________
 */
/**
 * Write a mdirents file on disk
 *
 * @param dirfd: file descriptor of the parent directory
 * @param *dirent_cache_p: pointer to cache entry to re-write
 *
 * @retval 0 on success
 * @retval -1 on error
 */
int write_mdirents_file(int dirfd,mdirents_cache_entry_t *dirent_cache_p)
{
    int fd = -1;
    int flag = O_WRONLY | O_CREAT | O_NOATIME;
    char pathname[64];
    char *path_p;
    mdirents_file_t *dirent_file_p = NULL;

    /*
     ** build the filename of the dirent file to read
     */
    path_p = dirent_build_filename(&dirent_cache_p->header,pathname);
    if (path_p == NULL)
    {
        /*
         ** something wrong that must not happen
         */
        DIRENT_SEVERE("Cannot build filename( line %d\n)",__LINE__);
        goto error;
    }
#if DIRENT_WRITE_DEBUG
    printf("write file %s:\n",path_p);
#endif

    if ((fd = openat(dirfd, path_p, flag, S_IRWXU)) == -1)
    {
        DIRENT_SEVERE("Cannot open file( line %d\n)",__LINE__);
        goto error;
    }
    /*
     ** allocate a working array for storing the content of the file except the part
     ** that contains the name/fid and mode
     */
    dirent_file_p = DIRENT_MALLOC(sizeof(mdirents_file_t));
    if (dirent_file_p == NULL)
    {
        /*
         ** the system runs out of memory
         */
        DIRENT_SEVERE("Out of memory( line %d\n)",__LINE__);
        goto error;
    }
//    memset(dirent_file_p,0xbb,sizeof(mdirents_file_t));

    /*
     ** fill the sector 0 to the dirent file buffer
     */
    {
        mdirent_sector0_not_aligned_t *sect0_p;

        sect0_p = DIRENT_VIRT_TO_PHY_OFF(dirent_cache_p,sect0_p);
        if (sect0_p == (mdirent_sector0_not_aligned_t*)NULL)
        {
            DIRENT_SEVERE("sector 0 ptr does not exist( line %d\n)",__LINE__);
            goto error;
        }
        memcpy(&dirent_file_p->sect0,sect0_p,sizeof(mdirent_sector0_not_aligned_t));
    }

    /*
     ** Copy  the sector 1 ( name/fid/type bitmap) to the dirent file buffer
     */
    {
        mdirents_btmap_free_chunk_t *sect1_p;

        sect1_p = DIRENT_VIRT_TO_PHY_OFF(dirent_cache_p,name_bitmap_p);
        if (sect1_p == (mdirents_btmap_free_chunk_t*)NULL)
        {
            DIRENT_SEVERE("sector 0 ptr does not exist( line %d\n)",__LINE__);
            goto error;
        }
        memcpy(&dirent_file_p->sect1,sect1_p,sizeof(mdirents_btmap_free_chunk_t));
    }

    /*
     ** Copy  the sector 2 ( 256 hash buckets of 16 bits) to the dirent file buffer
     */
    {
        mdirents_hash_ptr_t *hash_tbl_p;
        int i;
        mdirents_hash_ptr_t *file_ptr = (mdirents_hash_ptr_t*)&dirent_file_p->sect2;
        for (i = 0; i < MDIRENTS_HASH_TB_CACHE_MAX_IDX; i++)
        {
#if DIRENT_WRITE_DEBUG
            printf("hash_tbl_p[%d]: rd:%d dirty:%d val:%llx\n",i,dirent_cache_p->hash_tbl_p[i].s.rd,
                    dirent_cache_p->hash_tbl_p[i].s.dirty,
                    (unsigned long long int)dirent_cache_p->hash_tbl_p[i].s.val);
#endif
            hash_tbl_p = DIRENT_VIRT_TO_PHY_OFF(dirent_cache_p,hash_tbl_p[i]);
            if (hash_tbl_p == (mdirents_hash_ptr_t*)NULL)
            {
                /*
                 ** apply the default value
                 */
                memset(file_ptr,0,sizeof(mdirents_hash_ptr_t)*MDIRENTS_HASH_TB_CACHE_MAX_ENTRY);
            }
            else
            {
                memcpy(file_ptr,hash_tbl_p,sizeof(mdirents_hash_ptr_t)*MDIRENTS_HASH_TB_CACHE_MAX_ENTRY);
            }
            file_ptr += MDIRENTS_HASH_TB_CACHE_MAX_ENTRY;
            /*
             ** clear the dirty bit
             */
            dirent_cache_p->hash_tbl_p[i].s.dirty = 0;
        }
    }
    /*
     ** Copy the sectors 3&4 that contain the hash entries
     */
    {
        mdirents_hash_entry_t *hash_entry_p;
        int i;
        mdirents_hash_entry_t *file_ptr = (mdirents_hash_entry_t*)&dirent_file_p->sect3;
        for (i = 0; i < MDIRENTS_HASH_CACHE_MAX_IDX; i++)
        {
#if DIRENT_WRITE_DEBUG
            printf("hash_entry_p[%d]: rd:%d dirty:%d val:%llx\n",i,dirent_cache_p->hash_entry_p[i].s.rd,
                    dirent_cache_p->hash_entry_p[i].s.dirty,
                    (unsigned long long int)dirent_cache_p->hash_entry_p[i].s.val);

#endif
            hash_entry_p = DIRENT_VIRT_TO_PHY_OFF(dirent_cache_p,hash_entry_p[i]);
            if (hash_entry_p == (mdirents_hash_entry_t*)NULL)
            {
                /*
                 ** apply the default value
                 */
                memset(file_ptr,0,sizeof(mdirents_hash_entry_t)*MDIRENTS_HASH_CACHE_MAX_ENTRY);
            }
            else
            {
                memcpy(file_ptr,hash_entry_p,sizeof(mdirents_hash_entry_t)*MDIRENTS_HASH_CACHE_MAX_ENTRY);
            }
            /*
             ** clear the dirty bit
             */
            dirent_cache_p->hash_entry_p[i].s.dirty = 0;
            file_ptr += MDIRENTS_HASH_CACHE_MAX_ENTRY;
        }

    }
    /*
     ** OK now lest's write the mandatory sectors
     */
    if (DIRENT_PWRITE(fd, dirent_file_p, sizeof (mdirents_file_t), 0) != (sizeof (mdirents_file_t))) {
        DIRENT_SEVERE("pwrite failed for file %s: %s at line %d", pathname, strerror(errno),__LINE__);
        goto error;
    }
    /*
     ** OK now check if there is some name entry array to write on disk: for this we check the
     ** name_entry_array_btmap_wr_req bitmap. Each bit asserted indicates that 2 virtual sectors
     ** of 512 bytes have to be re-write on disk
     */
    {
        int first_chunk_of_array;
        uint8_t *mem_p;
        int bit_idx = 0;
        int next_bit_idx;
        int loop_cnt;

        int ret;
        while (bit_idx < MDIRENTS_NAME_PTR_MAX)
        {
#if 1
            if (bit_idx%8 == 0)
            {
                next_bit_idx = check_bytes(dirent_cache_p->name_entry_array_btmap_wr_req,bit_idx,MDIRENTS_NAME_PTR_MAX,&loop_cnt);
                if (next_bit_idx < 0) break;
                /*
                 ** next  chunk
                 */
                bit_idx = next_bit_idx;
            }
#endif
            first_chunk_of_array = bit_idx*MDIRENTS_CACHE_NB_CHUNK_PER_CHUNK_ARRAY;
            ret = dirent_test_chunk_bit(bit_idx,dirent_cache_p->name_entry_array_btmap_wr_req);
            if (ret == 0)
            {
                /*
                 ** nothing to re-write->next entry
                 */
                bit_idx ++;
                continue;
            }
            /*
             ** get the pointer to the beginning of the memory array
             */
            mem_p = (uint8_t*) dirent_get_entry_name_ptr(fd,dirent_cache_p,first_chunk_of_array,DIRENT_CHUNK_NO_ALLOC);
            if (mem_p == NULL)
            {
                /*
                 ** something wrong that must not occur
                 */
                DIRENT_SEVERE("write_mdirents_file: out of chunk error at line %d\n",__LINE__);
                goto error;
            }
            /*
             ** let's write it on disk
             */

#if 0

            if (DIRENT_PWRITE(fd, mem_p, MDIRENTS_CACHE_CHUNK_ARRAY_SZ,
                            DIRENT_HASH_NAME_BASE_SECTOR*MDIRENT_SECTOR_SIZE +MDIRENTS_CACHE_CHUNK_ARRAY_SZ*bit_idx) != MDIRENTS_CACHE_CHUNK_ARRAY_SZ) {
                DIRENT_SEVERE("pwrite failed for file %s: %s, at line %d", pathname, strerror(errno),__LINE__);
                goto error;
            }
#else
#warning Name entry write is disabled
#endif
            /*
             ** clear the write request and assert the presence of data in that array
             */
            dirent_clear_chunk_bit(bit_idx,dirent_cache_p->name_entry_array_btmap_wr_req);
            dirent_set_chunk_bit(bit_idx,dirent_cache_p->name_entry_array_btmap_presence);

#if 0
#warning need more testing
            /*
             ** release the associated memory pointer
             */
            uint8_t *free_p = mem_p;
            mem_p = (uint8_t*)dirent_cache_del_ptr(&dirent_cache_p->name_entry_lvl0_p[0],
                    &mdirent_cache_name_ptr_distrib,
                    bit_idx,(void *)mem_p);
            if (mem_p != NULL)
            {
                /*
                 ** that case must not happen because we just get it before calling deletion
                 */
                DIRENT_SEVERE("dirent_cache_del_entry_name_ptr error at line %d\n",__LINE__);
                goto error;

            }
            /*
             ** release the memory
             */
            DIRENT_FREE((void*)free_p);

#endif
            /*
             ** next bit
             */
            bit_idx ++;

        }
    }

    /*
     ** Indicate that the disk update has been done
     */
    DIRENT_ROOT_UPDATE_DONE(dirent_cache_p);

    /*
     ** release the resources
     */
    close(fd);
    DIRENT_FREE(dirent_file_p);

    return 0;

    error:
    if (fd != -1)
    close(fd);
    if (dirent_file_p != NULL) DIRENT_FREE(dirent_file_p);
    return -1;
}

#else

/*
** Debug 
*/
int fdl_debug_first_chunk_of_array_write = -1;

//#warning WRITE_MDIRENTS_FILE  with writeback cache

/*
 **______________________________________________________________________________
 */
/**
 * Write a mdirents file on disk
 *
 * @param dirfd: file descriptor of the parent directory
 * @param *dirent_cache_p: pointer to cache entry to re-write
 *
 * @retval 0 on success
 * @retval -1 on error
 */
int write_mdirents_file(int dirfd, mdirents_cache_entry_t *dirent_cache_p) {
    int fd = -1;
    int flag = O_WRONLY | O_CREAT;
#ifdef __linux__
		flag |= O_NOATIME;
#endif
    char pathname[64];
    char *path_p;
    mdirents_file_t *dirent_file_p = NULL;
    int writeback_cache = 0;
    int write_len = 0;

    /*
     ** Feature around the write back cache: -> try to allocate or retrieve a writeback cache
     */
    {
        dirent_file_p = writebck_cache_bucket_get_entry(
                &dirent_cache_p->writeback_ref,
                dirent_cache_p->header.dirent_idx[0]);
        if (dirent_file_p != NULL ) {
            writeback_cache = 1;
        }
    }
#ifndef DIRENT_SKIP_DISK
    /*
     ** build the filename of the dirent file to read
     */
    path_p = dirent_build_filename(&dirent_cache_p->header, pathname);
    if (path_p == NULL ) {
        /*
        ** something wrong that must not happen
        */
	errno = EIO;
        DIRENT_SEVERE("Cannot build filename");
        goto error;
    }
#if DIRENT_WRITE_DEBUG
    printf("write file %s:\n",path_p);
#endif

    if ((fd = openat(dirfd, path_p, flag, S_IRWXU)) == -1) {
        DIRENT_SEVERE("Cannot open file %s (%s)",path_p,strerror(errno));
	errno = EIO;
        goto error;
    }
    /*
     ** allocate a working array for storing the content of the file except the part
     ** that contains the name/fid and mode-> needed only if the buffer is not provided by the writeback cache
     */
    if (writeback_cache == 0) {
        dirent_file_p = DIRENT_MALLOC(sizeof(mdirents_file_t));
        if (dirent_file_p == NULL ) {
          /*
          ** the system runs out of memory
          */
	  errno = ENOMEM;
          DIRENT_SEVERE("Out of memory for file %s current  malloc_size %llu",path_p,(unsigned long long int)malloc_size);
          goto error;
        }
    }
//    memset(dirent_file_p,0xbb,sizeof(mdirents_file_t));

    /*
     ** fill the sector 0 to the dirent file buffer
     */
    {
        mdirent_sector0_not_aligned_t *sect0_p;

        sect0_p = DIRENT_VIRT_TO_PHY_OFF(dirent_cache_p,sect0_p)
        ;
        if (sect0_p == (mdirent_sector0_not_aligned_t*) NULL ) {
            DIRENT_SEVERE("sector 0 ptr does not exist( file %s)",path_p);
	    errno = ENOMEM;
            goto error;
        }
        memcpy(&dirent_file_p->sect0, sect0_p,
                sizeof(mdirent_sector0_not_aligned_t));
    }

    /*
     ** Copy  the sector 1 ( name/fid/type bitmap) to the dirent file buffer
     */
    {
        mdirents_btmap_free_chunk_t *sect1_p;

        sect1_p = DIRENT_VIRT_TO_PHY_OFF(dirent_cache_p,name_bitmap_p)
        ;
        if (sect1_p == (mdirents_btmap_free_chunk_t*) NULL ) {
            DIRENT_SEVERE("sector 1 ptr does not exist( file %s)",path_p);
	    errno = ENOMEM;
            goto error;
        }
        memcpy(&dirent_file_p->sect1, sect1_p,
                sizeof(mdirents_btmap_free_chunk_t));
    }

    /*
     ** Copy  the sector 2 ( 256 hash buckets of 16 bits) to the dirent file buffer
     */
    {
        mdirents_hash_ptr_t *hash_tbl_p;
        int i;
        mdirents_hash_ptr_t *file_ptr =
                (mdirents_hash_ptr_t*) &dirent_file_p->sect2;
        for (i = 0; i < MDIRENTS_HASH_TB_CACHE_MAX_IDX; i++) {
#if DIRENT_WRITE_DEBUG
            printf("hash_tbl_p[%d]: rd:%d dirty:%d val:%llx\n",i,dirent_cache_p->hash_tbl_p[i].s.rd,
                    dirent_cache_p->hash_tbl_p[i].s.dirty,
                    (unsigned long long int)dirent_cache_p->hash_tbl_p[i].s.val);
#endif
            hash_tbl_p = DIRENT_VIRT_TO_PHY_OFF(dirent_cache_p,hash_tbl_p[i])
            ;
            if (hash_tbl_p == (mdirents_hash_ptr_t*) NULL ) {
                /*
                 ** apply the default value
                 */
                memset(file_ptr, 0,
                        sizeof(mdirents_hash_ptr_t)
                                * MDIRENTS_HASH_TB_CACHE_MAX_ENTRY);
            } else {
                memcpy(file_ptr, hash_tbl_p,
                        sizeof(mdirents_hash_ptr_t)
                                * MDIRENTS_HASH_TB_CACHE_MAX_ENTRY);
            }
            file_ptr += MDIRENTS_HASH_TB_CACHE_MAX_ENTRY;
            /*
             ** clear the dirty bit
             */
            dirent_cache_p->hash_tbl_p[i].s.dirty = 0;
        }
    }
    /*
     ** update the length of the first part
     */
    write_len = sizeof(mdirent_sector0_t) + sizeof(mdirent_sector1_t)
            + sizeof(mdirent_sector2_t);
    /*
     ** Copy the sectors 3&4 that contain the hash entries
     */
    {
        mdirents_hash_entry_t *hash_entry_p;
        int i;
        int hash_entry_last_sector_idx = 0;
        mdirents_hash_entry_t *file_ptr =
                (mdirents_hash_entry_t*) &dirent_file_p->sect3;
        /*
         ** Check if there is some sectors that are empty and then adjust the index of
         ** the last sector to write
         */
        for (i = 0; i < MDIRENTS_HASH_CACHE_MAX_IDX; i++) {
            hash_entry_p =
                    DIRENT_VIRT_TO_PHY_OFF(dirent_cache_p,hash_entry_p[i])
            ;
            if (hash_entry_p != (mdirents_hash_entry_t*) NULL )
                hash_entry_last_sector_idx++;
        }
//#warning All Management sector are re-written on disk
        /* issue #33 Dirent rewrite on disk is truncated because of bad length computation */
        hash_entry_last_sector_idx = MDIRENTS_HASH_CACHE_MAX_IDX;
        for (i = 0; i < hash_entry_last_sector_idx; i++) {
#if DIRENT_WRITE_DEBUG
            printf("hash_entry_p[%d]: rd:%d dirty:%d val:%llx\n",i,dirent_cache_p->hash_entry_p[i].s.rd,
                    dirent_cache_p->hash_entry_p[i].s.dirty,
                    (unsigned long long int)dirent_cache_p->hash_entry_p[i].s.val);

#endif
            hash_entry_p =
                    DIRENT_VIRT_TO_PHY_OFF(dirent_cache_p,hash_entry_p[i])
            ;
            if (hash_entry_p == (mdirents_hash_entry_t*) NULL ) {
                /*
                 ** apply the default value
                 */
                memset(file_ptr, 0,
                        sizeof(mdirents_hash_entry_t)
                                * MDIRENTS_HASH_CACHE_MAX_ENTRY);
            } else {
                memcpy(file_ptr, hash_entry_p,
                        sizeof(mdirents_hash_entry_t)
                                * MDIRENTS_HASH_CACHE_MAX_ENTRY);
            }
            /*
             ** clear the dirty bit
             */
            dirent_cache_p->hash_entry_p[i].s.dirty = 0;
            file_ptr += MDIRENTS_HASH_CACHE_MAX_ENTRY;
            write_len += sizeof(mdirents_hash_entry_t)
                    * MDIRENTS_HASH_CACHE_MAX_ENTRY;
        }
    }
#endif
    /*
     ** return back to the writeback cache to figure out if the content must be flushed on disk
     */
    if (writeback_cache != 0) {
        int ret = writebck_cache_bucket_is_write_needed(
                &dirent_cache_p->writeback_ref,
                dirent_cache_p->header.dirent_idx[0]);
        if (ret == 1) {
            if (DIRENT_PWRITE(fd, dirent_file_p, write_len, 0) != write_len) {
                DIRENT_SEVERE("pwrite (mgt sectors) failed for file %s: %s", pathname, strerror(errno));
                errno =  EIO;
                goto error;
            }
        }
    } else {
        /*
         ** There is no write backbuffer ->OK now lest's write the mandatory sectors
         */
        if (DIRENT_PWRITE(fd, dirent_file_p, write_len, 0) != write_len) {
            DIRENT_SEVERE("pwrite (mgt sectors) failed for file %s: %s", pathname, strerror(errno));
            errno  = EIO;
            goto error;
        }
    }
#ifndef DIRENT_SKIP_DISK
    /*
     ** OK now check if there is some name entry array to write on disk: for this we check the
     ** name_entry_array_btmap_wr_req bitmap. Each bit asserted indicates that 2 virtual sectors
     ** of 512 bytes have to be re-write on disk
     */
    {
        int first_chunk_of_array;
        uint8_t *mem_p;
        int bit_idx = 0;
        int next_bit_idx;
        int loop_cnt;

        int ret;
        while (bit_idx < MDIRENTS_NAME_PTR_MAX) {
            if (bit_idx % 8 == 0) {
                next_bit_idx = check_bytes(
                        dirent_cache_p->name_entry_array_btmap_wr_req, bit_idx,
                        MDIRENTS_NAME_PTR_MAX, &loop_cnt);
                if (next_bit_idx < 0)
                    break;
                /*
                 ** next  chunk
                 */
                bit_idx = next_bit_idx;
            }
            first_chunk_of_array = bit_idx
                    * MDIRENTS_CACHE_NB_CHUNK_PER_CHUNK_ARRAY;
            ret = dirent_test_chunk_bit(bit_idx,
                    dirent_cache_p->name_entry_array_btmap_wr_req);
            if (ret == 0) {
                /*
                 ** nothing to re-write->next entry
                 */
                bit_idx++;
                continue;
            }
            /*
             ** get the pointer to the beginning of the memory array
             */
            mem_p = (uint8_t*) dirent_get_entry_name_ptr(fd, dirent_cache_p,
                    first_chunk_of_array, DIRENT_CHUNK_NO_ALLOC);
            if (mem_p == NULL ) {
                /*
                 ** something wrong that must not occur
                 */
                DIRENT_SEVERE("data sector pointer (%d) does not exist: file %s",first_chunk_of_array,pathname)
                errno = ENOMEM;
                goto error;
            }
            /*
             ** let's write it on disk
             */

	    if (fdl_debug_first_chunk_of_array_write != -1)
	    {

	      if (first_chunk_of_array > fdl_debug_first_chunk_of_array_write)
	      {
		errno = EIO;
		DIRENT_SEVERE("Debug write error for file %s  data sector %d",pathname,first_chunk_of_array); 
		goto error;
	      }    
	    }

#if 1
            if (DIRENT_PWRITE(fd, mem_p, MDIRENTS_CACHE_CHUNK_ARRAY_SZ,
                    DIRENT_HASH_NAME_BASE_SECTOR * MDIRENT_SECTOR_SIZE
                            + MDIRENTS_CACHE_CHUNK_ARRAY_SZ
                                    * bit_idx) != MDIRENTS_CACHE_CHUNK_ARRAY_SZ) {
                DIRENT_SEVERE("data sector %d write failed for file %s: %s",bit_idx, pathname, strerror(errno));
                errno = EIO;
                goto error;
            }
#else
#warning Name entry write is disabled
#endif
            /*
             ** clear the write request and assert the presence of data in that array
             */
            dirent_clear_chunk_bit(bit_idx,
                    dirent_cache_p->name_entry_array_btmap_wr_req);
            dirent_set_chunk_bit(bit_idx,
                    dirent_cache_p->name_entry_array_btmap_presence);

#if 0
#warning need more testing
            /*
             ** release the associated memory pointer
             */
            uint8_t *free_p = mem_p;
            mem_p = (uint8_t*)dirent_cache_del_ptr(&dirent_cache_p->name_entry_lvl0_p[0],
                    &mdirent_cache_name_ptr_distrib,
                    bit_idx,(void *)mem_p);
            if (mem_p != NULL)
            {
                /*
                 ** that case must not happen because we just get it before calling deletion
                 */
                DIRENT_SEVERE("dirent_cache_del_entry_name_ptr error at line %d\n",__LINE__);
                goto error;

            }
            /*
             ** release the memory
             */
            DIRENT_FREE((void*)free_p);

#endif
            /*
             ** next bit
             */
            bit_idx++;

        }
    }
#endif // SKIP_DISK
    /*
     ** Indicate that the disk update has been done
     */
    DIRENT_ROOT_UPDATE_DONE(dirent_cache_p);

    /*
     ** release the resources
     */
    if (fd != -1)
        close(fd);
    if ((dirent_file_p != NULL )&& (writeback_cache== 0))DIRENT_FREE(dirent_file_p);

    return 0;

error: 
    DIRENT_ROOT_SET_READ_ONLY();
    if (fd != -1)
        close(fd);
    if ((dirent_file_p != NULL )&& (writeback_cache== 0))DIRENT_FREE(dirent_file_p);
    return -1;
}

#endif



/*
 **______________________________________________________________________________
 */
/**
 * Write a name entry array on disk. That API is intended to be called when
 there no change on the management sector of a dirent file. It is typically
 the case when the entry needs to be updated when the fid of the name entry is
 changed. In fact that change does not impact the chunks that have been previously
 allocated for the name entry
 *
 * @param dirfd: file descriptor of the parent directory
 * @param *dirent_cache_p: pointer to cache entry to re-write
 * @param start_chunk_idx : reference of the first chunk associated with the name entry
 *
 * @retval 0 on success
 * @retval -1 on error
 */
int dirent_write_name_array_to_disk(int dirfd,
        mdirents_cache_entry_t *dirent_cache_p, uint16_t start_chunk_idx) {
    int fd = -1;
    int flag = O_WRONLY;
    char pathname[64];
    char *path_p;
    off_t offset;
    int ret;

    /*
     ** build the filename of the dirent file to read
     */
    path_p = dirent_build_filename(&dirent_cache_p->header, pathname);
    if (path_p == NULL ) {
        /*
         ** something wrong that must not happen
         */
        DIRENT_SEVERE("Cannot build filename( line %d\n)",__LINE__);
	errno = EIO;
        goto error;
    }

    if ((fd = openat(dirfd, path_p, flag, S_IRWXU)) == -1) {
        DIRENT_SEVERE("Cannot open file:%s error %s)",path_p,strerror(errno));
	errno = EIO;
        goto error;
    }

    /*
     ** OK now check if there is some name entry array to write on disk: for this we check the
     ** name_entry_array_btmap_wr_req bitmap. Each bit asserted indicates that 2 virtual sectors
     ** of 512 bytes have to be re-write on disk
     */
    {
        int first_chunk_of_array;
        uint8_t *mem_p;

        first_chunk_of_array = start_chunk_idx
                / MDIRENTS_CACHE_NB_CHUNK_PER_CHUNK_ARRAY;
        /*
         ** get the pointer to the beginning of the memory array
         */

        mem_p = (uint8_t*) dirent_get_entry_name_ptr(dirfd, dirent_cache_p,
                first_chunk_of_array * MDIRENTS_CACHE_NB_CHUNK_PER_CHUNK_ARRAY,
                DIRENT_CHUNK_NO_ALLOC);
        if (mem_p == NULL ) {
            /*
             ** something wrong that must not occur
             */
	    errno = ENOMEM;
            DIRENT_SEVERE("write_mdirents_name_entry_sectors: out of chunk error at line %d\n",__LINE__)
            ;
            goto error;
        }


        /*
         ** let's write it on disk
         */
#if 1

        offset = DIRENT_HASH_NAME_BASE_SECTOR * MDIRENT_SECTOR_SIZE
                + MDIRENTS_CACHE_CHUNK_ARRAY_SZ * (first_chunk_of_array);

	if (fdl_debug_first_chunk_of_array_write != -1)
	{

	  if (first_chunk_of_array > fdl_debug_first_chunk_of_array_write)
	  {
	    errno = EIO;
	    DIRENT_SEVERE("Debug write error for file %s offset %x chunk idx %d",pathname,(unsigned int)offset,first_chunk_of_array); 
	    goto error;
	  }    
	}
        ret = DIRENT_PWRITE(fd, mem_p, MDIRENTS_CACHE_CHUNK_ARRAY_SZ,offset);
	if (ret < 0)
	{
	  DIRENT_SEVERE("writing error for file %s (%s)", path_p,strerror(errno));
	  errno = EIO;
	  goto error;
	
	}
        if (ret != MDIRENTS_CACHE_CHUNK_ARRAY_SZ) 
	{
	   DIRENT_SEVERE("writing error for file %s (%s)", path_p,strerror(errno));
	   errno = EIO;
           goto error;
        }
#endif
    }
    /*
     ** Indicate that the disk update has been done
     */
    DIRENT_ROOT_UPDATE_DONE(dirent_cache_p);
    /*
     ** release the resources
     */
    close(fd);
    return 0;

error: 
    DIRENT_ROOT_SET_READ_ONLY();
    if (fd != -1)
        close(fd);
    return -1;
}

/*
 *______________________________________________________________________________
 M D I R E N T    U S E R   A P I
 *______________________________________________________________________________
 */

/*
** Debug 
*/
int fdl_debug_first_chunk_of_array_read = -1;
int fdl_debug_errno_read = 0;
/*
 **______________________________________________________________________________
 */
/**
 * Read the sectors associated with a chunk array from disk
 *
 * @param dirfd: file descriptor of the parent directory
 * @param *pathname: pointer to the pathname to read
 *
 * @retval 0 success
 * @retval -1 error
 */

int dirent_read_name_array_from_disk(int dirfd,
        mdirents_cache_entry_t *dirent_p, int first_chunk_of_array)

{
    int fd = -1;
    int flag = O_RDONLY;
    mdirents_header_new_t *dirent_hdr_p = &dirent_p->header;
    char pathname[64];
    char *path_p;
    off_t offset;
    uint8_t *mem_p;
    int ret;

    /*
     ** build the filename of the dirent file to read
     */
    path_p = dirent_build_filename(dirent_hdr_p, pathname);
    if (path_p == NULL ) {
        /*
         ** something wrong that must not happen
         */
        DIRENT_SEVERE("Cannot build pathname ");
	errno = EIO;
        goto error;
    }
    /*
     ** OK, let's allocate memory for storing the name entries into chunks
     */
    mem_p = DIRENT_MALLOC(MDIRENTS_CACHE_CHUNK_ARRAY_SZ)
    ;
    if (mem_p == NULL ) {
        /*
         ** fatal error we run out of memory
         */
	 errno = ENOMEM;
        DIRENT_SEVERE("out of memory for file %s",pathname);
        goto error;
    }
    /*
    ** clean up the memory array to address the case of a truncated dirent file 
    */
    memset(mem_p,0,MDIRENTS_CACHE_CHUNK_ARRAY_SZ);
    
    if (dirent_cache_store_ptr(&dirent_p->name_entry_lvl0_p[0],
            &mdirent_cache_name_ptr_distrib, first_chunk_of_array,
            (void *) mem_p) != NULL ) {
        /*
        ** fatal error
        */
	errno = ENOMEM;
        DIRENT_SEVERE("out of memory for file %s",pathname);
        goto error;
    }

    if ((fd = openat(dirfd, path_p, flag, S_IRWXU)) == -1) {
        DIRENT_SEVERE("Cannot open the file %s (%s)",path_p,strerror(errno));
	errno = EIO;
        goto error;
    }
    /*
     ** read the fixed part of the dirent file
     */
    offset = DIRENT_HASH_NAME_BASE_SECTOR * MDIRENT_SECTOR_SIZE
            + MDIRENTS_CACHE_CHUNK_ARRAY_SZ * (first_chunk_of_array);
    ret = DIRENT_PREAD(fd, mem_p, MDIRENTS_CACHE_CHUNK_ARRAY_SZ,offset);
    if (ret < 0)
    {
      DIRENT_SEVERE("pread failed in file %s: %s", pathname, strerror(errno));
      errno = EIO;
      goto error;        
    }
    if (fdl_debug_first_chunk_of_array_read != -1)
    {
    
      if (first_chunk_of_array > fdl_debug_first_chunk_of_array_read)
      {
        memset(mem_p,0,MDIRENTS_CACHE_CHUNK_ARRAY_SZ);
        if (fdl_debug_errno_read != 0)
	{
	  errno = EIO;
	  DIRENT_SEVERE("Debug read error for file %s offset %x chunk idx %d",pathname,(unsigned int)offset,first_chunk_of_array); 
	  goto error;
	}
	DIRENT_SEVERE("Debug empty chunk array for file %s offset %x chunk idx %d",pathname,(unsigned int)offset,first_chunk_of_array); 
        ret = 0;
      }    
    }
    /*
    ** read is successful, check the length, the file can be truncated because an exportd
    ** restart before the file system can push the data on disk
    */
    if (ret != MDIRENTS_CACHE_CHUNK_ARRAY_SZ)
    {
        DIRENT_SEVERE("name array pread failed in file %s at offset %d: len read %d", pathname, (int)offset,ret);    
    }
    /*
    ** indicate that the memory array exists
    */
    dirent_set_chunk_bit(first_chunk_of_array,dirent_p->name_entry_array_btmap_presence);
    /*
     ** that's OK
     */
    if (fd != -1)
        close(fd);
    return 0;

error: 
    DIRENT_ROOT_SET_READ_ONLY();
    if (fd != -1)
        close(fd);
    return -1;
}



/*
 *______________________________________________________________________________
 
    SAFE CONTROL: check the bucket list when the dirent file are loaded from disk
 *______________________________________________________________________________
 */
 dirent_file_repair_cause_e dirent_cache_check_repair_needed_dbg(
        mdirents_cache_entry_t *root, mdirents_cache_entry_t *cache_entry_p,
        mdirents_hash_ptr_t *hash_bucket_p, int bucket_idx)

{
    mdirents_cache_entry_t *cache_coll_entry = NULL;
    dirent_file_repair_cause_e cause = DIRENT_REPAIR_NONE;
    mdirent_sector0_not_aligned_t *sect0_p;
    uint8_t *hash_bitmap_p;
    mdirents_hash_entry_t *hash_entry_next_p = NULL;
    int hash_entry_bucket_idx_next;
    int cur_hash_entry_idx_repair = -1;

    sect0_p = DIRENT_VIRT_TO_PHY_OFF(cache_entry_p,sect0_p)
    ;
    hash_bitmap_p = (uint8_t*) &sect0_p->hash_bitmap;
    /*
     ** Check if the entry is valid on not by checking the bitmap
     */
    switch (hash_bucket_p->type) {
    case MDIRENTS_HASH_PTR_LOCAL:
        /*
         ** need to check the state of the hash entry in the bitmap
         */
        cur_hash_entry_idx_repair = hash_bucket_p->idx;
        if (dirent_test_chunk_bit(cur_hash_entry_idx_repair, hash_bitmap_p)
                != 0) {
            cause = DIRENT_REPAIR_FREE;
            break;
        }
        /*
         ** check if the next belongs to the same list
         */
        hash_entry_next_p =
                (mdirents_hash_entry_t*) DIRENT_CACHE_GET_HASH_ENTRY_PTR(cache_entry_p,cur_hash_entry_idx_repair)
        ;
        if (hash_entry_next_p == NULL ) {
            cause = DIRENT_REPAIR_FREE;
            break;
        }

        hash_entry_bucket_idx_next =
                DIRENT_HASH_ENTRY_GET_BUCKET_IDX(hash_entry_next_p);
        if (hash_entry_bucket_idx_next != bucket_idx) {
            cause = DIRENT_REPAIR_BUCKET_IDX_MISMATCH;
            break;
        }
        break;

    case MDIRENTS_HASH_PTR_COLL:
        /*
         ** need to check if the collision file exist and then check the state of the hash entry in the
         ** collision file
         */
        cache_coll_entry = dirent_cache_get_collision_ptr(root,
                hash_bucket_p->idx);
        if (cache_coll_entry == NULL ) {
            /*
             ** something is rotten in the cache since the pointer to the collision dirent cache
             ** does not exist
             */
            DIRENT_WARN("dirent_cache_check_repair_needed(%d) collision entry does not exist %d \n",bucket_idx,hash_bucket_p->idx)
            ;
            cause = DIRENT_REPAIR_NO_COLL_FILE;
            break;
        }

        /**
         * all is fine !!!
         */
        break;

    case MDIRENTS_HASH_PTR_EOF:
        /*
         ** No issue there, nothing to control--> will not OCCUR !!!!!!
         */
        break;

    case MDIRENTS_HASH_PTR_FREE:
         /*
	 ** this could happen while attempt to insert a hash entry
         */
         cause = DIRENT_REPAIR_FREE;
         break;
	 
    default:
        /*
         ** force EOF
         */
        DIRENT_WARN("dirent_cache_check_repair_needed(%d) wrong type for to EOF %d \n",__LINE__,hash_bucket_p->type)
        ;
        hash_bucket_p->type = MDIRENTS_HASH_PTR_EOF;
        break;
    }
    return cause;
}

/**
*
*/
dirent_file_repair_cause_e dirent_scan_dirent_cache_entry (mdirents_cache_entry_t *root,
                                                           mdirents_cache_entry_t *cache_entry_cur,
                                                           int bucket_idx,
							   int coll_idx,
							   uint8_t *coll_scanned_list_in_p,
							   uint8_t *coll_scanned_bitmap_p)
{
	mdirents_hash_ptr_t   *hash_bucket_p;
	mdirent_cache_ptr_t *bucket_virt_p = NULL;
	mdirent_cache_ptr_t *hash_cur_virt_p= NULL;
	int cur_hash_entry_idx = -1;
        dirent_file_repair_cause_e    cause = DIRENT_REPAIR_NONE;
	int bit_idx;
	int chunk_u8_idx;
	int loop_cnt = 0;
	mdirents_hash_entry_t *hash_entry_cur_p;
	int bucket_u8_idx;
	int bucket_bit_idx; 
	
        chunk_u8_idx = coll_idx / 8;
        bit_idx = coll_idx % 8;

	hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR_WITH_VIRT(cache_entry_cur,bucket_idx,&bucket_virt_p);
	if (hash_bucket_p == NULL) {
	  /* 
	  ** We may be processing the 1rst insertion in this bucket
	  ** and the bucket pointer may not be allocated at this time
	  */
	  return cause;
	}
	cause =  dirent_cache_check_repair_needed_dbg(root,cache_entry_cur,hash_bucket_p,bucket_idx);
	if (cause != DIRENT_REPAIR_NONE) return cause;
	
	if (cache_entry_cur != root)
	{
	  if (hash_bucket_p->type == MDIRENTS_HASH_PTR_LOCAL) 
	  {
	    /* 
	    ** this bucket goes through this collision file
	    */
	    coll_scanned_list_in_p[chunk_u8_idx] |= (1<<bit_idx);
	  }
	}	
	/*
	** Find the next collision file that this files points to
	*/
	while(loop_cnt < MDIRENTS_IN_BLOCK_LOOPCNT )
	{
          loop_cnt++;
	  
	  cause =  dirent_cache_check_repair_needed_dbg(root,cache_entry_cur,hash_bucket_p,bucket_idx);
	  if (cause != DIRENT_REPAIR_NONE) return cause;
	  /*
	  **----------------------------------------------------------------------
	  ** E O F  We have found an EOF however the hash entry idx MUST be valid
	  **----------------------------------------------------------------------
	  */
	  if (hash_bucket_p->type == MDIRENTS_HASH_PTR_EOF)
	  {
	    /*
	    ** end of list has been found so, exit the while loop
	    */
	    return cause;
	  }
	  /*
	  **----------------------------------------------------------------------
	  **LOCAL index case -> go to the next entry
	  **----------------------------------------------------------------------
	  */
	  if (hash_bucket_p->type == MDIRENTS_HASH_PTR_LOCAL)
	  {
	    cur_hash_entry_idx = hash_bucket_p->idx;

	    hash_entry_cur_p = (mdirents_hash_entry_t*)DIRENT_CACHE_GET_HASH_ENTRY_PTR_WITH_VIRT(cache_entry_cur,cur_hash_entry_idx,&hash_cur_virt_p);

	    hash_bucket_p = &hash_entry_cur_p->next;
	    continue;
	  }
	  /*
	  **----------------------------------------------------------------------
	  **COLL index case -> check loop
	  **----------------------------------------------------------------------
	  */
	  if ((cache_entry_cur != root) && (hash_bucket_p->idx == coll_idx))
	  {
	    cause = DIRENT_REPAIR_LOOP;
	    return cause;
	  }
	  bucket_u8_idx = hash_bucket_p->idx / 8;
	  bucket_bit_idx = hash_bucket_p->idx % 8;    
	  if ((coll_scanned_bitmap_p[bucket_u8_idx] & (1 << bucket_bit_idx)) != 0) 
	  {
	     /*
	     ** already scanned
	     */
	     cause = DIRENT_REPAIR_LOOP;
	     return cause;
	  }
	  /*
	  ** assert the corresponding bit 
	  */
	  coll_scanned_bitmap_p[bucket_u8_idx] |= (1 << bucket_bit_idx);
	  return cause;

	}
	/*
	** loop detected
	*/
	cause = DIRENT_REPAIR_LOOP;
	return cause;

}


/**
*
*/
static uint64_t coll_scanned_list_in[MDIRENTS_MAX_COLLS_IDX/sizeof(uint64_t)];
static uint64_t coll_scanned_bitmap[MDIRENTS_MAX_COLLS_IDX/sizeof(uint64_t)];


int dirent_cache_is_bucket_idx_safe ( int dir_fd,
                                  mdirents_cache_entry_t *root,
                                  int       bucket_idx)
{
    int coll_idx = 0;
    int loop_cnt = 0;
    int next_coll_idx = 0;
    int bit_idx;
    int chunk_u8_idx;
    uint8_t *coll_bitmap_p;
    uint8_t *bucket_safe_bitmap_p;
    mdirent_sector0_not_aligned_t *sect0_p;
    sect0_p = DIRENT_VIRT_TO_PHY_OFF(root,sect0_p)    
    int bucket_u8_idx;
    int bucket_bit_idx; 
    mdirents_cache_entry_t *cache_entry_cur;    
    dirent_file_repair_cause_e    cause;
    uint64_t *coll_scanned_list_in_p = coll_scanned_list_in;
    uint64_t *coll_scanned_bitmap_p = coll_scanned_bitmap;
    
    if (dirent_cache_safe_enable == 0) return -1;
    
    if (sect0_p == (mdirent_sector0_not_aligned_t*) NULL ) {
        return -1 ;
    };
    bucket_safe_bitmap_p = root->bucket_safe_bitmap_p;
    if (bucket_safe_bitmap_p == NULL)
    {
      /*
      ** don't care about safe state
      */
      return -1;
    }
    /*
    ** check bitmap for that bucket
    */
    bucket_u8_idx = bucket_idx / 8;
    bucket_bit_idx = bucket_idx % 8;    
    if ((bucket_safe_bitmap_p[bucket_u8_idx] & (1 << bucket_bit_idx)) != 0) 
    {
     /*
     ** safe !!
     */
     return 0;
    }
    memset(coll_scanned_list_in_p,0,MDIRENTS_MAX_COLLS_IDX/8);
    memset(coll_scanned_bitmap_p,0,MDIRENTS_MAX_COLLS_IDX/8);
    
    bucket_safe_bitmap_p[bucket_u8_idx] |= (1 << bucket_bit_idx);
    
    /*
    ** scan the root
    */
    cause = dirent_scan_dirent_cache_entry(root,root,
	                                   bucket_idx,0,
					   (uint8_t*) coll_scanned_list_in_p,
					   (uint8_t*) coll_scanned_bitmap_p);
    if (cause != DIRENT_REPAIR_NONE) goto repair;

    coll_bitmap_p = (uint8_t*) &sect0_p->coll_bitmap;

    while (coll_idx < MDIRENTS_MAX_COLLS_IDX) 
    {

        chunk_u8_idx = coll_idx / 8;
        bit_idx = coll_idx % 8;
        /*
         ** there is no collision dirent entry or the collision dirent entry exist and is not full
         */
        if ((coll_bitmap_p[chunk_u8_idx] & (1 << bit_idx)) != 0) {
            /*
             ** That entry is free, need to find out the next entry that is busy (0: busy, 1:free)
             */
            if (coll_idx % 8 == 0) {
                next_coll_idx = check_bytes_val(coll_bitmap_p, coll_idx,
                        MDIRENTS_MAX_COLLS_IDX, &loop_cnt, 1);
                if (next_coll_idx < 0)
                    break;
                /*
                 ** next  chunk
                 */
                if (next_coll_idx == coll_idx)
                    coll_idx++;
                else
                    coll_idx = next_coll_idx;
                continue;
            }
            /*
             ** next chunk
             */
            coll_idx++;
            continue;
        }

	cache_entry_cur = dirent_cache_get_collision_ptr(root,coll_idx);
	if (cache_entry_cur == NULL)
	{
           /*
           ** something is rotten in the cache since the pointer to the collision dirent cache
           ** does not exist (notice that it was there at the time of the control for repair !!!
           */
	   //dirent_file_repair(dir_fd,root,bucket_idx,DIRENT_REPAIR_NO_COLL_FILE);	
           //return 0;
	   
          coll_idx++;
          continue;	   
	}
        cause = dirent_scan_dirent_cache_entry(root,cache_entry_cur,
	                                       bucket_idx,coll_idx,
					       (uint8_t*) coll_scanned_list_in_p,
					       (uint8_t*) coll_scanned_bitmap_p);
	if (cause != DIRENT_REPAIR_NONE) goto repair;
					       
	/*
	** next collision entry
	*/
	coll_idx++;
    }
    /*
    ** All the collision files have been scanned without error for that bucket idx
    */
    int loop;
    for (loop = 0; loop < (MDIRENTS_MAX_COLLS_IDX/sizeof(uint64_t)); loop++)
    {
      if ( coll_scanned_list_in_p[loop] != coll_scanned_bitmap_p[loop])
      {
	 cause = DIRENT_REPAIR_BUCKET_IDX_MISMATCH;
	 goto repair;
      }
    }
    /*
    ** no issue
    */
    return 0;
  
repair:

   dirent_file_repair(dir_fd,root,bucket_idx,cause);

   return 0;
}
