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
#include "dirent_journal.h"

#define DIRENT_DEBUG_WRITEBACK 0
#if DIRENT_DEBUG_WRITEBACK
#warning DIRENT_DEBUG_WRITEBACK -> debug mode asserted
#endif

#define WRITE_BACK_MAX_CNT 64

#define WRITEBCK_BUCKET_DEPTH_IN_BIT   5
#define WRITEBCK_BUCKET_DEPTH_IN_BYTE   (1<<5)

#define WRITEBCK_BUCKET_DEPTH_MASK ((1<<WRITEBCK_BUCKET_DEPTH_IN_BIT)-1)
#define WRITEBCK_BUCKET_MAX_ROOT_DIRENT (2560000*2)


#define WRITEBCK_BUCKET_MAX_COLLISIONS  128  /**< number of collisions that can be supported by a bucket  */
#define WRITEBCK_BUCKET_MAX_COLLISIONS_BYTES  (WRITEBCK_BUCKET_MAX_COLLISIONS/8)  /**< number of collisions that can be supported by a bucket  */

typedef struct _writebck_cache_bucket_entry_t
{
     uint64_t      counter:16;   /**< number of time the entry has been used for the same entry */
     uint64_t      timestamp:32; /**< incremented each time the buffer is allocated            */
     uint64_t      buffer_id:16;    /**< reference of the write back buffer                       */
} writebck_cache_bucket_entry_t;


/**
*  dirent cache structure
*/
typedef struct _writebck_cache_bucket_t
{
    uint8_t bucket_free_bitmap[WRITEBCK_BUCKET_MAX_COLLISIONS_BYTES];  /**< bitmap of the free entries  */
    writebck_cache_bucket_entry_t entry[WRITEBCK_BUCKET_MAX_COLLISIONS];   /**< write back entries entries */

} writebck_cache_bucket_t;
/**
* Key used by the application to get the buffer from the write back cache
*/
typedef struct _writebck_cache_key_t
{
     uint64_t      timestamp:32; /**< incremented each time the buffer is allocated                                */
     uint64_t      local_id:8;   /**< local reference of the write back buffer in the writeback cache: local idx   */
     uint64_t      filler:24;   /**< filler   */
} writebck_cache_key_t;


typedef struct _writebck_cache_main_t
{

   uint32_t   max;       /**< maximum number of entries in the cache */
   uint32_t   size;      /**< current number of entries in the cache */
   list_t     entries;   /**< entries cached: used for LRU           */
   writebck_cache_bucket_t *htable;          /**< pointer to the bucket array of the cache */
   mdirents_file_t         *mdirents_file_p; /**< pointer to the buffers */
} writebck_cache_main_t;

/*
** Dirent level 0 cache
*/
writebck_cache_main_t   writebck_cache_level0;

uint32_t  writebck_cache_initialized = 0;
uint64_t writebck_cache_flush_req_counter = 0;
uint64_t writebck_cache_hit_counter       = 0;
uint64_t writebck_cache_miss_counter      = 0;
uint64_t writebck_cache_collision_counter = 0;
uint64_t writebck_cache_collision_level0_counter = 0;
 int writebck_cache_enable = 0;  /**< 1 : write back cache enabled */
int writebck_cache_max_level0_collisions = 0;   /**< max number of collision at level 0  */
int writebck_cache_max_level1_collisions = 0;   /**< max number of collision at level 1  */

uint64_t writebck_cache_access_tb[WRITEBCK_BUCKET_DEPTH_IN_BYTE]; /**< writeback access statistics    */
uint64_t writebck_cache_pending_tb[WRITE_BACK_MAX_CNT]; /**< number of entry     */

/*
** Print the dirent cache bucket statistics
*/
void writebck_cache_print_stats()
{
  printf("Write Back cache state %s\n",(writebck_cache_enable==1)?"Enabled":"Disabled");
  printf("Level 0 bucket number       :%d\n",(1<<WRITEBCK_BUCKET_DEPTH_IN_BIT));
  printf("Number of buffer per bucket :%d\n",WRITEBCK_BUCKET_MAX_COLLISIONS);
  printf("Buffer (count/size          :%d/%d Bytes\n",WRITEBCK_BUCKET_MAX_COLLISIONS*(1<<WRITEBCK_BUCKET_DEPTH_IN_BIT),
         (int)sizeof(mdirents_file_t));
  printf("Total Memory                :%d MBytes\n",
      (int)( ((sizeof(writebck_cache_bucket_t)*(1<<WRITEBCK_BUCKET_DEPTH_IN_BIT))+
             ((1<<WRITEBCK_BUCKET_DEPTH_IN_BIT)*WRITEBCK_BUCKET_MAX_COLLISIONS*sizeof(mdirents_file_t)))/1000000));
  printf("hit/miss: %llu/%llu\n",(long long unsigned int)writebck_cache_hit_counter,
                                (long long unsigned int)writebck_cache_miss_counter);
 printf("flush_req: %llu\n",(long long unsigned int)writebck_cache_flush_req_counter);
 }

/**
*  writeback access statistics
*/
void writebck_cache_print_access_stats()
{
  int i;
  printf("Write Back access statistics \n");
  for (i = 0; i < WRITEBCK_BUCKET_DEPTH_IN_BYTE; i++)
     printf("index[%3.3d]  :%llu\n",i,(long long unsigned int)writebck_cache_access_tb[i]);
 }

/**
*  writeback count statistics
*/
 void writebck_cache_print_per_count_stats()
{
  writebck_cache_bucket_t *p;
  writebck_cache_main_t *cache = &writebck_cache_level0;
  writebck_cache_bucket_entry_t *bucket_entry;
  int i,k;
  int cpt_idx;

  printf("Write Back per count statistics \n");
  memset(writebck_cache_pending_tb,0,WRITE_BACK_MAX_CNT*sizeof(uint64_t));

  p = cache->htable;
  for (i = 0; i <  (1<<WRITEBCK_BUCKET_DEPTH_IN_BIT); i++,p++)
  {
    bucket_entry = p->entry;
    for(k = 0; k < WRITEBCK_BUCKET_MAX_COLLISIONS; k++,bucket_entry++)
    {
      cpt_idx = bucket_entry->counter;
      if (cpt_idx >= WRITE_BACK_MAX_CNT) cpt_idx = WRITE_BACK_MAX_CNT-1;
      writebck_cache_pending_tb[cpt_idx]++;
    }
  }

  for (i = 0; i < WRITE_BACK_MAX_CNT; i++)
  {
    if (writebck_cache_pending_tb[i] != 0)
       printf("index[%3.3d]  :%llu\n",i,(long long unsigned int)writebck_cache_pending_tb[i]);
  }
 }

/*
**______________________________________________________________________________
*/
/**
*  API for init of the dirent level 0 cache
 @param cache: pointer to the cache descriptor

 @retval none
 */
void writebck_cache_level0_initialize()
{
    writebck_cache_bucket_t *p;
    writebck_cache_main_t *cache = &writebck_cache_level0;

    if (writebck_cache_initialized) return;
    cache->max = WRITEBCK_BUCKET_MAX_ROOT_DIRENT;
    cache->size = 0;
    list_init(&cache->entries);
    /*
    ** Allocate the memory to handle the buckets
    */
    cache->htable = xmalloc(sizeof(writebck_cache_bucket_t)*(1<<WRITEBCK_BUCKET_DEPTH_IN_BIT));
    if (cache->htable == NULL)
    {
      DIRENT_SEVERE("writebck_cache_level0_initialize out of memory (%u) at line %d\n",(unsigned int)sizeof(writebck_cache_bucket_t)*(1<<WRITEBCK_BUCKET_DEPTH_IN_BIT),
                     __LINE__);
      exit(0);
    }
    /*
    ** allocate the buffers
    */
    cache->mdirents_file_p = xmalloc(((1<<WRITEBCK_BUCKET_DEPTH_IN_BIT)*WRITEBCK_BUCKET_MAX_COLLISIONS*sizeof(mdirents_file_t)));
    /*
    ** init of the buckets
    */
    int i ;
    p = cache->htable;
    for (i = 0; i <  (1<<WRITEBCK_BUCKET_DEPTH_IN_BIT); i++,p++)
    {
      memset(&p->bucket_free_bitmap,0xff,WRITEBCK_BUCKET_MAX_COLLISIONS_BYTES);
      memset(&p->entry,0,WRITEBCK_BUCKET_MAX_COLLISIONS*sizeof(writebck_cache_bucket_entry_t));
    }
    /*
    ** clear statistics
    */
    memset(writebck_cache_access_tb,0,WRITEBCK_BUCKET_DEPTH_IN_BYTE*sizeof(uint64_t));
    memset(writebck_cache_pending_tb,0,WRITE_BACK_MAX_CNT*sizeof(uint64_t));

    writebck_cache_initialized = 1;

}



/**
*  Insert a root dirent file reference in the cache
*  Note : the bitmap is aligned on a 8 byte boundary, so we can perform
  control by using a uint64_t

   @param cache : pointer to the main cache structure
   @param index : index of the root dirent file
   @param key   : pointer of the key of the write back buffer

   @retval !NULL -> success
   @retval NULL -> na available buffer .
*/
mdirents_file_t *writebck_cache_bucket_get_entry(uint64_t *key_ext,uint16_t index)
{
   uint8_t *bitmap_p;
   writebck_cache_bucket_t *bucket_p;
   int coll_idx;
   int  next_coll_idx ;
   uint8_t  chunk_u8_idx ;
   int  bit_idx;
   int loop_cnt;

   if (writebck_cache_enable == 0) return NULL;


   uint16_t hash_bucket;
   writebck_cache_key_t *key = (writebck_cache_key_t*)key_ext;
   writebck_cache_main_t *cache =  &writebck_cache_level0;
//reloop:

   coll_idx = 0;
   next_coll_idx = 0 ;
   /*
   ** Get the index where application can get a write back buffer
   */
   hash_bucket = (uint16_t)(index&WRITEBCK_BUCKET_DEPTH_MASK);
   bucket_p = &cache->htable[hash_bucket];
   writebck_cache_access_tb[hash_bucket]++;

  /*
   ** set the pointer to the bucket and load up the pointer to the bitmap
   */
   bitmap_p = bucket_p->bucket_free_bitmap;
   /*
   ** Check if the application request a free writeback buffer or need to get the one that
   ** has been previously returned
   */
//   printf("root_idx %d timestamp %d local_id %d\n",index,key->timestamp,key->local_id);
   if (key->timestamp != 0)
   {
     /*
     ** the application has to re-use the previously allocated writeback buffer
     ** Check if the application is still the owner
     */
     if (bucket_p->entry[key->local_id].timestamp == key->timestamp)
     {
       /*
       ** that's OK-> increment just the usage counter
       */
       dirent_clear_chunk_bit(key->local_id,bitmap_p);
       writebck_cache_hit_counter++;
       return &cache->mdirents_file_p[(hash_bucket*WRITEBCK_BUCKET_MAX_COLLISIONS) +key->local_id];
     }
     /*
     ** unlucky, check if we can allocate another write back buffer
     */
   }



   while(coll_idx < WRITEBCK_BUCKET_MAX_COLLISIONS)
   {
     if (coll_idx%8 == 0)
     {
       /*
       ** skip the entries that are alreadt allocated
       */
       next_coll_idx = check_bytes_val(bitmap_p,coll_idx,WRITEBCK_BUCKET_MAX_COLLISIONS,&loop_cnt,0);
       if (next_coll_idx < 0) break;
       coll_idx = next_coll_idx;
     }
     /*
     ** check if the return bit is free
     */
     chunk_u8_idx = coll_idx/8;
     bit_idx      = coll_idx%8;
     if ((bitmap_p[chunk_u8_idx] & (1<<bit_idx)) == 0)
     {
       /*
       ** the entry is busy, check the next one
       */
       coll_idx++;
       continue;
     }
#if 1
     if (coll_idx > writebck_cache_max_level0_collisions)
     {
       writebck_cache_max_level0_collisions = coll_idx;
     }

#endif
     /*
     ** allocate the entry by clearing the associated bit
     */
     dirent_clear_chunk_bit(coll_idx,bitmap_p);
//     if (coll_idx > 32) printf("FDL_DEBUG coll_idx %d\n",coll_idx);
     /*
     **  OK we found one, check if the memory has been allocated to store the entry
     ** this will depend on the value of the coll_idx
     */
//     printf("coll_idx %d\n",coll_idx);
     bucket_p->entry[coll_idx].timestamp += 1;
     if (bucket_p->entry[coll_idx].timestamp == 0) bucket_p->entry[coll_idx].timestamp = 1;
     key->timestamp = bucket_p->entry[coll_idx].timestamp;
     key->local_id  = coll_idx;
 //    printf("FDL_DEBUG allocated idx %d\n",coll_idx);
     /*
     ** OK, now insert the entry
     */
     return &cache->mdirents_file_p[(hash_bucket*WRITEBCK_BUCKET_MAX_COLLISIONS) +key->local_id];   ;
   }
   /*
   ** Out of entries-> need to go through LRU-> TODO
   */
#if DIRENT_DEBUG_WRITEBACK
    {
      int k;
      writebck_cache_bucket_entry_t *bucket_entry = bucket_p->entry;
      for(k = 0; k < WRITEBCK_BUCKET_MAX_COLLISIONS; k++,bucket_entry++)
      {
        if (bucket_entry->counter!= 0) continue;
        printf("Free entry at idx %d\n",k);
        goto reloop;
        break;
      }
    }
#endif
   writebck_cache_miss_counter++;
   return NULL;
}




/**
*  Insert a root dirent file reference in the cache
*  Note : the bitmap is aligned on a 8 byte boundary, so we can perform
  control by using a uint64_t

   @param cache : pointer to the main cache structure
   @param index : index of the root dirent file
   @param key   : pointer of the key of the write back buffer

   @retval 0  -> no need
   @retval 1  -> write needed
   @retval -1 -> error .
*/
int writebck_cache_bucket_is_write_needed(uint64_t *key_ext,uint16_t index)
{
   uint8_t *bitmap_p;
   writebck_cache_bucket_t *bucket_p;
   writebck_cache_main_t *cache =  &writebck_cache_level0;

   uint16_t hash_bucket;
   writebck_cache_key_t *key = (writebck_cache_key_t*)key_ext;


   /*
   ** Get the index where application can get a write back buffer
   */
   hash_bucket = (uint16_t)(index&0x1f);
   bucket_p = &cache->htable[hash_bucket];
   bitmap_p = bucket_p->bucket_free_bitmap;

   /*
   ** Check if the application request a free writeback buffer or need to get the one that
   ** has been previously returned
   */
   if (key->timestamp != 0)
   {
     /*
     ** the application has to re-use the previously allocated writeback buffer
     ** Check if the application is still the owner
     */
     if (bucket_p->entry[key->local_id].timestamp == key->timestamp)
     {
       /*
       ** that's OK-> increment just the usage counter
       */
       bucket_p->entry[key->local_id].counter += 1;
       if (bucket_p->entry[key->local_id].counter == WRITE_BACK_MAX_CNT)
       {
         /*
         ** flush the entry
         */
         bucket_p->entry[key->local_id].counter = 0;
         writebck_cache_flush_req_counter++;
         /*
         ** clear the associated bit in the bitmap
         */
         dirent_set_chunk_bit(key->local_id,bitmap_p);
         return 1;
        }
        return 0;
     }
     /*
     ** unlucky, check if we can allocate another write back buffer
     */
   }
   return -1;
}


/**
*  Flush a write back buffer
*  Note : the bitmap is aligned on a 8 byte boundary, so we can perform
  control by using a uint64_t

   @param cache : pointer to the main cache structure
   @param index : index of the root dirent file
   @param key   : pointer of the key of the write back buffer

   @retval 0  -> success
   @retval -1 -> error .
*/
int writebck_cache_bucket_flush_entry(uint64_t *key_ext,uint16_t index)
{
   uint8_t *bitmap_p;
   writebck_cache_bucket_t *bucket_p;
   writebck_cache_main_t *cache =  &writebck_cache_level0;

   uint16_t hash_bucket;
   writebck_cache_key_t *key = (writebck_cache_key_t*)key_ext;


   /*
   ** Get the index where application can get a write back buffer
   */
   hash_bucket = (uint16_t)(index&0x1f);
   bucket_p = &cache->htable[hash_bucket];
   bitmap_p = bucket_p->bucket_free_bitmap;

   if (key->timestamp != 0)
   {
     /*
     ** the application has to re-use the previously allocated writeback buffer
     ** Check if the application is still the owner
     */
     if (bucket_p->entry[key->local_id].timestamp == key->timestamp)
     {
       /*
       ** that's OK-> increment just the usage counter
       */
       bucket_p->entry[key->local_id].counter = 0;
       /*
       ** clear the associated bit in the bitmap
       */
       dirent_set_chunk_bit(key->local_id,bitmap_p);
       return 0;

     }
     /*
     ** unlucky, check if we can allocate another write back buffer
     */
   }
   return -1;
}



