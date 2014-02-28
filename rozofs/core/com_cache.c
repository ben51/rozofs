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
#include <rozofs/common/list.h>
#include <rozofs/common/xmalloc.h>
#include <rozofs/common/profile.h>
#include "com_cache.h"




/*
 **______________________________________________________________________________
 */
/** @ingroup DIRENT_BITMAP
 *  That function go throught the bitmap of free chunk by skipping allocated chunks
 it returns the next chunk to check or -1 if there is no more free chunk



 @param p : pointer to the bitmap of the free chunk (must be aligned on a 8 bytes boundary)
 @param first_chunk : first chunk to test
 @param last_chunk: last allocated chunk
 @param loop_cnt : number of busy chunks that has been skipped (must be cleared by the caller
 @param empty :assert to 1 for searching for free chunk or 0 for allocated chunk

 @retval next_chunk < 0 : out of free chunk (last_chunk has been reached
 @retval next_chunk !=0 next free chunk bit to test
 */
static inline int check_bytes_val(uint8_t *p, int first_chunk, int last_chunk,
        int *loop_cnt, uint8_t empty) {
    int chunk_idx = first_chunk;
    int chunk_u8_idx = 0;
    int align;
    uint64_t val = 0;

    /*
     ** check if the search is for the next free array or the next busy array
     */
    if (empty)
        val = ~val;
//  *loop_cnt = 0;

    while (chunk_idx < last_chunk) {
        chunk_u8_idx = chunk_idx / 8;
        align = chunk_u8_idx & 0x07;
        switch (align) {
        case 0:
            if (((uint64_t*) p)[chunk_idx / 64] == val) {
                chunk_idx += 64;
                break;
            }
            if (((uint32_t*) p)[chunk_idx / 32] == (val & 0xffffffff)) {
                chunk_idx += 32;
                break;
            }
            if (((uint16_t*) p)[chunk_idx / 16] == (val & 0xffff)) {
                chunk_idx += 16;
                break;
            }
            if (((uint8_t*) p)[chunk_idx / 8] == (val & 0xff)) {
                chunk_idx += 8;
                break;
            }
            if ((((uint8_t*) p)[chunk_idx / 8] & 0xf) == (val & 0xf)) {
                chunk_idx += 4;
            }
            return chunk_idx;

        case 2:
        case 6:
            if (((uint16_t*) p)[chunk_idx / 16] == (val & 0xffff)) {
                chunk_idx += 16;
                break;
            }
            if (((uint8_t*) p)[chunk_idx / 8] == (val & 0xff)) {
                chunk_idx += 8;
                break;
            }
            if ((((uint8_t*) p)[chunk_idx / 8] & 0xf) == (val & 0xf)) {
                chunk_idx += 4;
            }
            return chunk_idx;

        case 4:
            if (((uint32_t*) p)[chunk_idx / 32] == (val & 0xffffffff)) {
                chunk_idx += 32;
                break;
            }
            if (((uint16_t*) p)[chunk_idx / 16] == (val & 0xffff)) {
                chunk_idx += 16;
                break;
            }
            if (((uint8_t*) p)[chunk_idx / 8] == (val & 0xff)) {
                chunk_idx += 8;
                break;
            }
            if ((((uint8_t*) p)[chunk_idx / 8] & 0xf) == (val & 0xf)) {
                chunk_idx += 4;
            }
            return chunk_idx;
        case 1:
        case 3:
        case 5:
        case 7:
            if (((uint8_t*) p)[chunk_idx / 8] == (val & 0xff)) {
                chunk_idx += 8;
                break;
            }
            if ((((uint8_t*) p)[chunk_idx / 8] & 0xf) == (val & 0xf)) {
                chunk_idx += 4;
            }
            return chunk_idx;
        }
        *loop_cnt += 1;
    }
    /*
     ** all the bits have been skipped
     */
    return -1;
}

/*
 **______________________________________________________________________________
 */
/** 
 *  clear a bit in a bitmap

 @param start_chunk : index of the entry that must be set
 @param *p  : pointer to the bitmap array

 @retval none
 */
static inline void com_clear_chunk_bit(int start_chunk, uint8_t *p) {
    (p[start_chunk / 8] &= ~(1 << start_chunk % 8));

}

/*
 **______________________________________________________________________________
 */
 /** 
 *  set a bit in a bitmap

 @param start_chunk : index of the entry that must be set
 @param *p  : pointer to the bitmap array

 @retval none
 */
static inline void com_set_chunk_bit(int start_chunk, uint8_t *p) {
    (p[start_chunk / 8] |= (1 << start_chunk % 8));
}

/*
 **______________________________________________________________________________
 */
/**
 *   Compute the hash values for the name and fid

 @param h :previous hash computation result
 @param key1 : pointer to the fid
 @param key2 : pointer to the file index

 @retval primary hash value
 */
static inline uint32_t com_cache_bucket_hash_fnv(uint32_t h, void *key1, uint16_t *key2) {

    unsigned char *d = (unsigned char *) key1;
    int i = 0;

    if (h == 0) h = 2166136261U;

    /*
     ** hash on fid
     */
    for (d = key1; d != key1 + 16; d++) {
        h = (h * 16777619)^ *d;

    }
    /*
     ** hash on index
     */
    d = (unsigned char *) key2;
    for (i = 0; i < sizeof (uint16_t); d++, i++) {
        h = (h * 16777619)^ *d;

    }

    return h;
}
/*
 **______________________________________________________________________________
 */
/**
 *  Creation a a cache entity
    That service is intended to allocate the management cache structure and to 
    return it as returned parameter.
 
 @param level0_sz: size of the level 0 in power of 2
 @param max: max number of entries supported by the cache before activing LRU

 @retval <> NULL : success
 @retval == NULL : error
 */
com_cache_main_t *com_cache_create(uint32_t level0_sz, uint32_t max,com_cache_usr_fct_t  *usr_fct) 
{

    com_cache_bucket_t *p;
    com_cache_main_t *cache;
    int i;
    
    cache = malloc(sizeof(com_cache_main_t));
    if (cache == NULL)
    {
      severe(" out of memory (%u)", (unsigned int) sizeof(com_cache_main_t));   
      return NULL;     
    }
    memset(cache,0,sizeof(com_cache_main_t));
    cache->max = max;
    cache->level0_sz   = 1<< level0_sz;
    cache->level0_mask = cache->level0_sz-1;
    cache->usr_exact_match_fct = usr_fct->usr_exact_match_fct;
    cache->usr_hash_fct        = usr_fct->usr_hash_fct;
    cache->usr_delete_fct      = usr_fct->usr_delete_fct;
    
    cache->size = 0;
    list_init(&cache->global_lru_link);
    /*
    ** Allocate the memory to handle the buckets
    */
    cache->htable = xmalloc(sizeof (com_cache_bucket_t)*(1 << COM_BUCKET_DEPTH_IN_BIT));
    if (cache->htable == NULL) 
    {
      severe("com_cache_level0_initialize out of memory (%u)", (unsigned int) sizeof (com_cache_bucket_t)*(1 << COM_BUCKET_DEPTH_IN_BIT));
      return NULL;     
    }
    /*
    ** init of the buckets
    */
    p = cache->htable;
    for (i = 0; i < (1 << COM_BUCKET_DEPTH_IN_BIT); i++, p++) {
        list_init(&p->bucket_lru_link);
        memset(&p->bucket_free_bitmap, 0xff, COM_BUCKET_MAX_COLLISIONS_BYTES);
        memset(&p->entry_tb, 0, COM_BUCKET_ENTRY_MAX_ARRAY * sizeof (void*));
        p->dirty_bucket_counter = 0;
    }
    return cache;
}

/*
 **______________________________________________________________________________
 */
/**
 *  Insert a user entry in the cache
 *  Note : the bitmap is aligned on a 8 byte boundary, so we can perform
  control by using a uint64_t

   @param cache : pointer to the main cache structure
   @param entry   : pointer to the entry to insert (needed to be a com_cache_entry_t structure)

   @retval 0 -> success
   @retval -1 -> failure, entry has not been inserted  .
 */

int com_cache_bucket_insert_entry(com_cache_main_t *cache, com_cache_entry_t *entry) {
    uint8_t *bitmap_p;
    com_cache_bucket_t *bucket_p;
    int coll_idx = 0;
    int next_coll_idx = 0;
    uint8_t chunk_u8_idx;
    int bit_idx;
    int loop_cnt;
    int bucket_entry_arrray_idx;
    com_cache_bucket_entry_t *cache_bucket_entry_p;
    int local_idx; 
    uint32_t hash_value;

    uint16_t hash_bucket;
    uint16_t hash_bucket_entry;
    
    /*
    ** split the hash value in 2 part: the lower part is used as the hash bucket index: level 0 entry
    ** the 16 highest bits are stored in the hash table to be used as a secundary before performing
    ** the exact match
    */
    hash_value = (*(cache->usr_hash_fct))(entry->usr_key_p);
    hash_bucket = hash_value & cache->level0_mask;
    hash_bucket_entry = (hash_value >> 16) ^ (hash_value & 0xffff);
    
    /*
    ** LRU handling: check for cache full condition: release one entry from the global link
    */
    if (cache->size >= cache->max)
    {
      int ret;
      com_cache_entry_t *cache_entry_lru_p = list_entry(cache->global_lru_link.prev, com_cache_entry_t, global_lru_link);
      ret = com_cache_bucket_remove_entry(cache,cache_entry_lru_p->usr_key_p);
      if (ret == -1) 
      {
        /*
        ** not really normal
        */
        cache->stats.com_bucket_cache_lru_global_error++;
        severe("Debug fail to Remove %p index %d ",cache_entry_lru_p,-1 );
        return -1;      
      }
      /*
      ** release the memory allocated for storing the dirent file
      */
      cache->stats.com_bucket_cache_lru_counter_global++;
// FDL      (*cache->usr_delete_fct)(cache_entry_lru_p->usr_entry_p);
    }
    /*
     ** set the pointer to the bucket and load up the pointer to the bitmap
     */
reloop:
    bucket_p = &cache->htable[hash_bucket];
    bitmap_p = bucket_p->bucket_free_bitmap;
    coll_idx = 0;
    next_coll_idx = 0;

    while (coll_idx < COM_BUCKET_MAX_COLLISIONS) {
        if (coll_idx % 8 == 0) {
            /*
             ** skip the entries that are alreadt allocated
             */
            next_coll_idx = check_bytes_val(bitmap_p, coll_idx, COM_BUCKET_MAX_COLLISIONS, &loop_cnt, 0);
            if (next_coll_idx < 0) break;
            coll_idx = next_coll_idx;
        }
        /*
         ** check if the return bit is free
         */
        chunk_u8_idx = coll_idx / 8;
        bit_idx = coll_idx % 8;
        if ((bitmap_p[chunk_u8_idx] & (1 << bit_idx)) == 0) {
            /*
             ** the entry is busy, check the next one
             */
            coll_idx++;
            continue;
        }
#if 1
        if (coll_idx > cache->stats.com_bucket_cache_max_level0_collisions) {
            cache->stats.com_bucket_cache_max_level0_collisions = coll_idx;
        }

#endif
        /*
         ** allocate the entry by clearing the associated bit
         */
        com_clear_chunk_bit(coll_idx, bitmap_p);
        /*
         **  OK we found one, check if the memory has been allocated to store the entry
         ** this will depend on the value of the coll_idx
         */
        bucket_entry_arrray_idx = coll_idx / COM_BUCKET_NB_ENTRY_PER_ARRAY;
        /*
         ** we need to allocated memory of the associated pointer is NULL
         */
        cache_bucket_entry_p = bucket_p->entry_tb[bucket_entry_arrray_idx];
        if (cache_bucket_entry_p == (com_cache_bucket_entry_t*) NULL) {
            cache_bucket_entry_p = (com_cache_bucket_entry_t*) malloc(sizeof (com_cache_bucket_entry_t));
            if (cache_bucket_entry_p == NULL) {
                warning(" out of memory");
                return -1;
            }
            bucket_p->entry_tb[bucket_entry_arrray_idx] = cache_bucket_entry_p;
            memset(cache_bucket_entry_p, 0, sizeof (com_cache_bucket_entry_t));
        }
        /*
         ** OK, now insert the entry
         */
        local_idx = coll_idx % COM_BUCKET_NB_ENTRY_PER_ARRAY;
        cache_bucket_entry_p->hash_value_table[local_idx] = hash_bucket_entry;
        cache_bucket_entry_p->entry_ptr_table[local_idx] = entry;
        /*
        ** update the dirty bits in the entry
        */
        entry->dirty_bucket_counter = bucket_p->dirty_bucket_counter;
        entry->dirty_main_counter   = cache->dirty_main_counter;
        /*
        ** do the job for LRU
        */
        {
          list_push_front(&cache->global_lru_link, &entry->global_lru_link);
          list_push_front(&bucket_p->bucket_lru_link, &entry->bucket_lru_link);
          cache->size++;
        }
        return 0;
    }
    /*
    ** Out of entries-> need to go through bucket LRU-> remove the oldest one
    */
    {
      int ret;
      com_cache_entry_t *cache_entry_lru_p = list_entry(bucket_p->bucket_lru_link.prev, com_cache_entry_t, bucket_lru_link);
      ret = com_cache_bucket_remove_entry(cache,cache_entry_lru_p->usr_key_p);
      if (ret == -1) 
      {
        /*
        ** not really normal
        */
        cache->stats.com_bucket_cache_lru_coll_error++;
        return -1;      
      }
      /*
      ** release the memory allocated for storing the dirent file
      */
      cache->stats.com_bucket_cache_lru_counter_coll++;
// FDL      (*cache->usr_delete_fct)(cache_entry_lru_p->usr_entry_p);
    }
    goto reloop;
}

/*
 **______________________________________________________________________________
 */
 /**
 *  Search an entry in the cache
 *
   @param cache : pointer to the main cache structure
   @param key_p : pointer to the key to search

   @retval <>NULL: pointer to the root dirent cache entry
   @retval ==NULL:no entry
 */
void *com_cache_bucket_search_entry(com_cache_main_t *cache, void *key_p) {
    uint8_t *bitmap_p;
    com_cache_bucket_t *bucket_p;
    int coll_idx = 0;
    int next_coll_idx = 0;
    int coll_idx_level1 = 0;
    uint8_t chunk_u8_idx;
    int bit_idx;
    int loop_cnt;
    int bucket_entry_arrray_idx;
    com_cache_bucket_entry_t *cache_bucket_entry_p;
    int local_idx;
    int ret;

    uint32_t hash_value;
    uint16_t hash_bucket;
    uint16_t hash_bucket_entry;

    /*
    ** split the hash value in 2 part: the lower part is used as the hash bucket index: level 0 entry
    ** the 16 highest bits are stored in the hash table to be used as a secundary before performing
    ** the exact match
    */
    hash_value = (*(cache->usr_hash_fct))(key_p);
    hash_bucket = hash_value & cache->level0_mask;
    hash_bucket_entry = (hash_value >> 16) ^ (hash_value & 0xffff);
    /*
     ** set the pointer to the bucket and load up the pointer to the bitmap
     */
    bucket_p = &cache->htable[hash_bucket];
    bitmap_p = bucket_p->bucket_free_bitmap;

    /*
     ** search among the bit that indicates a busy entry
     */
    while (coll_idx < COM_BUCKET_MAX_COLLISIONS) {
        if (coll_idx % 8 == 0) {
            /*
             ** skip the entries that are free
             */
            next_coll_idx = check_bytes_val(bitmap_p, coll_idx, COM_BUCKET_MAX_COLLISIONS, &loop_cnt, 1);
            if (next_coll_idx < 0) break;
            coll_idx = next_coll_idx;
        }
        /*
         ** check if the return bit is busy
         */
        chunk_u8_idx = coll_idx / 8;
        bit_idx = coll_idx % 8;
        if ((bitmap_p[chunk_u8_idx] & (1 << bit_idx)) != 0) {
            /*
             ** the entry is free, check the next one
             */
            coll_idx++;
            continue;
        }
        /*
         ** we have a busy entry: check the hash value of the entry
         */
        bucket_entry_arrray_idx = coll_idx / COM_BUCKET_NB_ENTRY_PER_ARRAY;
        cache_bucket_entry_p = bucket_p->entry_tb[bucket_entry_arrray_idx];
        if (cache_bucket_entry_p == NULL) {
            severe("com_cache_bucket_search_entry: busy entry but no pointer at line %d\n", __LINE__);
            return NULL;
        }
        /*
         ** OK, now check the entry
         */
        local_idx = coll_idx % COM_BUCKET_NB_ENTRY_PER_ARRAY;
        if (cache_bucket_entry_p->hash_value_table[local_idx] != hash_bucket_entry) {
            /*
             ** not the right hash value, check the next entry
             */
            cache->stats.com_bucket_cache_collision_level0_counter++;
            coll_idx++;
            continue;
        }
        /*
         ** There is match on the hash entry: check the fid and the index of the dirent cache entry reference in that
         ** bucket entry
         */
        com_cache_entry_t *cache_entry_p = cache_bucket_entry_p->entry_ptr_table[local_idx];
        if ((*cache->usr_exact_match_fct)(cache_entry_p->usr_key_p,key_p)!= 0)
        {
           /*
            ** not the right entry, check next one
            */
           coll_idx_level1++;
           cache->stats.com_bucket_cache_collision_counter++;
           coll_idx++;
           continue;
        }
        /*
        ** OK, we got the match check the dirty bits, if there is a match then return the pointer to the entry
        */
        if ((cache->dirty_main_counter == cache_entry_p->dirty_main_counter) && 
            (bucket_p->dirty_bucket_counter == cache_entry_p->dirty_bucket_counter))
        {
  #if 1
          if (coll_idx_level1 > cache->stats.com_bucket_cache_max_level1_collisions) {
              cache->stats.com_bucket_cache_max_level1_collisions = coll_idx_level1;
          }
  #endif
          cache->stats.com_bucket_cache_hit_counter++;
          /*
          ** do the job for LRU
          */
          {
            list_remove(&cache_entry_p->global_lru_link);
            list_remove(&cache_entry_p->bucket_lru_link);
            list_push_front(&cache->global_lru_link, &cache_entry_p->global_lru_link);
            list_push_front(&bucket_p->bucket_lru_link, &cache_entry_p->bucket_lru_link);
          }
          return cache_entry_p->usr_entry_p;
        }
        /*
        ** the entry is dirty, so remove it and return a NULL pointer
        */
        ret = com_cache_bucket_remove_entry(cache,cache_entry_p->usr_key_p);
        if (ret == -1) 
        {
          /*
          ** not really normal
          */
          cache->stats.com_bucket_cache_lru_coll_error++;
          return NULL;      
        }
        /*
        ** delete the entry
        */
// FDL        (*cache->usr_delete_fct)(cache_entry_p->usr_entry_p);
        cache->stats.com_bucket_cache_dirty_counter++;
    }
    cache->stats.com_bucket_cache_miss_counter++;
    return NULL;
}

/*
 **______________________________________________________________________________
 */
/**
 *  Remove a root dirent file reference from the cache
 *
   @param cache : pointer to the main cache structure
   @param key_p : key to search 

   @retval 0 : success
   @retval -1 : not found
 */
int com_cache_bucket_remove_entry(com_cache_main_t *cache, void *key_p) {
    uint8_t *bitmap_p;
    com_cache_bucket_t *bucket_p;
    int coll_idx = 0;
    int next_coll_idx = 0;
    uint8_t chunk_u8_idx;
    int bit_idx;
    int loop_cnt;
    int bucket_entry_arrray_idx;
    com_cache_bucket_entry_t *cache_bucket_entry_p;
    int local_idx;
    com_cache_entry_t *cache_entry_p = NULL;

    uint32_t hash_value;
    uint16_t hash_bucket;
    uint16_t hash_bucket_entry;
    /*
    ** split the hash value in 2 part: the lower part is used as the hash bucket index: level 0 entry
    ** the 16 highest bits are stored in the hash table to be used as a secundary before performing
    ** the exact match
    */
    hash_value = (*(cache->usr_hash_fct))(key_p);
    hash_bucket = hash_value & cache->level0_mask;
    hash_bucket_entry = (hash_value >> 16) ^ (hash_value & 0xffff);
    /*
     ** set the pointer to the bucket and load up the pointer to the bitmap
     */
    bucket_p = &cache->htable[hash_bucket];
    bitmap_p = bucket_p->bucket_free_bitmap;
    /*
     ** search among the bit that indicates a busy entry
     */

    while (coll_idx < COM_BUCKET_MAX_COLLISIONS) {
        if (coll_idx % 8 == 0) {
            /*
             ** skip the entries that are free
             */
            next_coll_idx = check_bytes_val(bitmap_p, coll_idx, COM_BUCKET_MAX_COLLISIONS, &loop_cnt, 1);
            if (next_coll_idx < 0) break;
            coll_idx = next_coll_idx;
        }
        /*
         ** check if the return bit is busy
         */
        chunk_u8_idx = coll_idx / 8;
        bit_idx = coll_idx % 8;
        if ((bitmap_p[chunk_u8_idx] & (1 << bit_idx)) != 0) {
            /*
             ** the entry is free, check the next one
             */
            coll_idx++;
            continue;
        }
        /*
         ** we have a busy entry: check the hash value of the entry
         */
        bucket_entry_arrray_idx = coll_idx / COM_BUCKET_NB_ENTRY_PER_ARRAY;
        cache_bucket_entry_p = bucket_p->entry_tb[bucket_entry_arrray_idx];
        if (cache_bucket_entry_p == NULL) {
            severe("com_cache_bucket_remove_entry: busy entry but no pointer at line %d\n", __LINE__);
            return -1;
        }
        /*
         ** OK, now check the entry
         */
        local_idx = coll_idx % COM_BUCKET_NB_ENTRY_PER_ARRAY;
        if (cache_bucket_entry_p->hash_value_table[local_idx] != hash_bucket_entry) {
            /*
             ** not the right hash value, check the next entry
             */
            cache->stats.com_bucket_cache_collision_level0_counter++;
            coll_idx++;
            continue;
        }
        /*
         ** There is match on the hash entry: check the fid and the index of the dirent cache entry reference in that
         ** bucket entry
         */
        cache_entry_p = cache_bucket_entry_p->entry_ptr_table[local_idx];
        if ((*cache->usr_exact_match_fct)(cache_entry_p->usr_key_p,key_p)!= 0)
        {
            /*
             ** not the right entry, check next one
             */
            cache->stats.com_bucket_cache_collision_counter++;
            coll_idx++;
            continue;
        }
        /*
        ** do the job for LRU
        */
        {
          list_remove(&cache_entry_p->global_lru_link);
          list_remove(&cache_entry_p->bucket_lru_link);  
          /*
          ** delete the entry
          */
          (*cache->usr_delete_fct)(cache_entry_p->usr_entry_p);        
          cache->size--;
        }
        /*
         **________________________________________________
         ** OK, we got the match, remove it from cache
         **________________________________________________
         */
        /*
         ** clear the entry by clearing the associated bit
         */
        com_set_chunk_bit(coll_idx, bitmap_p);
        cache_bucket_entry_p->hash_value_table[local_idx] = 0;
        /*
         **  check if the bucket array has to be released
         */
        int release_req = 1;
        int i;
        for (i = 0; i < COM_BUCKET_NB_ENTRY_PER_ARRAY / 8; i++) {
            if (bitmap_p[bucket_entry_arrray_idx + i] == 0xff) continue;
            release_req = 0;
            break;

        }
        if (release_req) {
            free(bucket_p->entry_tb[bucket_entry_arrray_idx]);
            bucket_p->entry_tb[bucket_entry_arrray_idx] = NULL;
        }
        cache->stats.com_bucket_cache_hit_counter++;

        return 0;
    }
    /*
     ** nothing found
     */
    cache->stats.com_bucket_cache_miss_counter++;
    return -1;
}

/*
**______________________________________________________________________________
*/
/**
*  update the bucket dirty "bits" of the cache

   @param cache : pointer to the main cache structure
   @param bucket_first_idx : index of the first bit in the dirty bitmap
   @param nb_entries : number of entries in the bitmap (modulo 8)
   @param bitmap_p : pointer to the bitmap
   
   @retval none

*/
void com_cache_dirty_bucket_update(com_cache_main_t *cache,uint32_t bucket_first_idx,uint32_t nb_entries,uint8_t *bitmap_p)
{
   uint16_t relative_bucket_idx = 0;
   int bucket_idx;
   com_cache_bucket_t *bucket_p;
   int loop_cnt;
   int next_coll_idx = 0;
   uint8_t chunk_u8_idx;
   int bit_idx;
    
    while (relative_bucket_idx < nb_entries) 
    {
        if (relative_bucket_idx % 8 == 0) 
        {
            /*
             ** skip the entries that are not asserted
             */
            next_coll_idx = check_bytes_val(bitmap_p, relative_bucket_idx,(int) nb_entries, &loop_cnt, 1);
            if (next_coll_idx < 0) break;
            relative_bucket_idx = next_coll_idx;
        }
        /*
        ** check if the return bit is free
        */
        chunk_u8_idx = relative_bucket_idx / 8;
        bit_idx = relative_bucket_idx % 8;
        if ((bitmap_p[chunk_u8_idx] & (1 << bit_idx)) == 0) 
        {
            /*
             ** the entry is busy, check the next one
             */
            relative_bucket_idx++;
            continue;
        }
        /*
        ** the dirty bit is asserted, so increment the bucket counter
        */
        bucket_idx = relative_bucket_idx+bucket_first_idx;
        if (bucket_idx >= (1<<cache->level0_sz)) 
        {
           /*
           ** bucket idx is out of range
           */
           severe("out of range bucket idx %d",bucket_idx);
           return;        
        }
        bucket_p = &cache->htable[bucket_idx];
        if (bucket_p != NULL)
        {
          bucket_p->dirty_bucket_counter++;
          cache->stats.com_bucket_cache_dirty_bucket_update++;
        }
        relative_bucket_idx++;
    }        
}


