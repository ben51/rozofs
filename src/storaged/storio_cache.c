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
#include <rozofs/common/mattr.h>
#include <rozofs/core/com_cache.h>

#include "storio_cache.h"
#include "storio_bufcache.h"

/*
**______________________________________________________________________________

      Global data
**______________________________________________________________________________
*/
com_cache_main_t  *storio_cache_cache_p= NULL;  /**< pointer to the fid cache  */
uint32_t storio_cache_enable_flag = 0;          /**< assert to 1 when the mode block cache is enable */
uint64_t storio_cache_key;
storio_cache_stats_t  storio_cache_stats;
/*
**______________________________________________________________________________
*/
/**
* allocate an entry for the  block cache

  @param fid : fid associated with the file
  
  @retval <>NULL: pointer to the cache entry
  @retval NULL : out of memory
*/
com_cache_entry_t *storio_cache_alloc_entry(fid_t fid)
{
  storio_cache_entry_t  *p;
  storio_cache_key_t    *key_p;
  /*
  ** allocate an entry for the context
  */
  p = malloc(sizeof(storio_cache_entry_t));
  if (p == NULL)
  {
    return NULL;
  }
  /*
  ** clear the structure
  */
  memset(p,0,sizeof(storio_cache_entry_t));
  p->cache.usr_entry_p = p;
  p->cache.usr_key_p   = &p->key;
  list_init(&p->cache.global_lru_link);
  list_init(&p->cache.bucket_lru_link);
  p->cache.dirty_bucket_counter = 0;
  p->cache.dirty_main_counter = 0;
  /*
  ** copy the informationn of the key
  */
  key_p = &p->key;
  memcpy(key_p->fid,fid,sizeof(fid_t));

  return &p->cache;
}

/*
**______________________________________________________________________________
*/
/**
* release an entry of the fid cache

  @param p : pointer to the user cache entry 
  
*/
void storio_cache_release_entry(void *entry_p)
{
  storio_cache_entry_t  *p = (storio_cache_entry_t*) entry_p;
  storio_cache_pool_entry_t *pool_p;
  int i;

  /*
  ** release the array used for storing the name
  */
  list_remove(&p->cache.global_lru_link);
  list_remove(&p->cache.bucket_lru_link);
  /*
  ** release all the allocated pointers (pools)
  */
  for (i = 0; i < STORIO_CACHE_MX_POOL; i++)
  {
    pool_p = p->pool[i];
    if (pool_p == NULL) continue;
    free(pool_p);
  }
  free(p);
}
/*
**______________________________________________________________________________
*/
/**
*  hash computation from parent fid and filename (directory name or link name)

  @param h : initial hash value
  @param key2:  fid
  
  @retval hash value
*/
static inline uint32_t uuid_hash_fnv(uint32_t h,void *key) {

    unsigned char *d;

    if (h == 0) h = 2166136261U;
 
    /*
     ** hash on fid
     */
    d = (unsigned char *) key;
    for (d = key; d != key + 16; d++) {
        h = (h * 16777619)^ *d;

    }
    return h;
}

/*
**______________________________________________________________________________
*/
/**
* fid entry hash compute 

  @param p : pointer to the user cache entry 
  
  @retval hash value
  
*/
uint32_t storio_cache_hash_compute(void *usr_key)
{
  storio_cache_key_t  *key_p = (storio_cache_key_t*) usr_key;
  uint32_t hash;
  
   hash = uuid_hash_fnv(0, key_p->fid);
   return hash;
}
/*
**______________________________________________________________________________
*/
/**
* fid hash match function

  @param key1 : pointer to the key associated with the entry from cache 
  
  @retval 0 on match
  @retval <> 0  no match  
*/

uint32_t storio_cache_exact_match(void *key1, void *key2)
{
  storio_cache_key_t  *key1_p = (storio_cache_key_t*) key1;
  storio_cache_key_t  *key2_p = (storio_cache_key_t*) key2;
  

  if (uuid_compare(key1_p->fid, key2_p->fid) != 0)  
  {
    return 1;
  }
  /*
  ** Match !!
  */
  return 0;
}

/*
**______________________________________________________________________________
*/
/**
* allocate a pool of cache enties
  @param none
  
  @retval <> NULL pointer to the pool
  @retval NULL out of memory
*/
storio_cache_pool_entry_t *storio_cache_pool_alloc()
{
  storio_cache_pool_entry_t *p;
  
  p = malloc(sizeof(storio_cache_pool_entry_t));
  if (p==NULL) return NULL;
  memset(p,0,sizeof(storio_cache_pool_entry_t));
  return p;
}
/*
**______________________________________________________________________________
*/
/**
*  Insert a set a timestamp associated with projections

  @param fid : fid associated with the file
  @param bid: first projection index in the timestamp table 
  @param nblocks: number of projections to insert
  @param relative_ts_idx: bid relative idx in the timestamp table
  @param timestamp_tb_p : pointer to the data array that contains the timestamps associated with the projections
  
  
  @retval 0 on success
  @retval -1 on error
*/
int storio_cache_insert(fid_t fid,uint64_t bid,uint32_t nblocks,uint64_t *timestamp_tb_p,int relative_ts_idx)
{
    storio_cache_key_t key;
    storio_cache_entry_t *cache_entry_p;
    storio_cache_buffer_entry_t *cache_data_p;
    storio_cache_buffer_entry_t *cache_data_free_p = NULL;
    com_cache_entry_t  *com_cache_entry_p;
    storio_cache_pool_entry_t *pool_p;
    int i;
    uint8_t *buf_cache_ts_p = NULL;
    int cur_nblocks;
    uint64_t cur_bid;
    uint64_t bid_cache;
    uint64_t *cur_buf_p;
    int pool_idx;
    int local_buffer_idx;
    uint64_t key_buf;
    
    cur_nblocks = nblocks;
    /*
    ** append the relative offset: note (relative_ts_idx + nblocks) must be less than STORIO_CACHE_BUFFER_BSIZE
    */
    cur_bid   = bid + relative_ts_idx;
    cur_buf_p = timestamp_tb_p +relative_ts_idx;
    /*
    ** check if the cache is enabled
    */    
    if (storio_cache_enable_flag != STORIO_TSCACHE_ENABLE) return -1;
        
    if (nblocks == 0) return -1;


    if (storio_cache_cache_p == NULL) return -1;
    /*
    ** OK, the fid is handled by this server, so attempt to get the child fid
    ** from the cache
    */
    storio_cache_fid_build_key(&key,(unsigned char *)fid);
   /*
   ** Perform a lookup on the mode block cache
   */
   cache_entry_p = com_cache_bucket_search_entry(storio_cache_cache_p,&key); 
   if (cache_entry_p == NULL)
   {
      /*
      ** allocate an entry
      */
      com_cache_entry_p = storio_cache_alloc_entry(fid); 
      if (com_cache_entry_p == NULL) return -1;
      if (com_cache_bucket_insert_entry(storio_cache_cache_p, com_cache_entry_p) < 0)
      {
         severe("error on fid insertion"); 
         storio_cache_release_entry(com_cache_entry_p->usr_entry_p);
         return -1;
      }
      cache_entry_p = com_cache_entry_p->usr_entry_p;         
   }
   while (cur_nblocks > 0)
   {
     /*
     **  Get the index to the array where data will be stored:
     **  compute bid_cache from cur_bid in order to have a bid_cache
     **  that is always aligned on a 32 projections boundary
     */
     bid_cache = cur_bid/STORIO_CACHE_BUFFER_BSIZE;
     pool_idx = bid_cache%(STORIO_CACHE_MX_POOL);
     bid_cache = bid_cache*STORIO_CACHE_BUFFER_BSIZE;
     /*
     ** check if there is an available pool
     */
     if (cache_entry_p->pool[pool_idx] == NULL)
     {
        /*
        ** need to allocated a pool
        */
        cache_entry_p->pool[pool_idx] = storio_cache_pool_alloc();   
     }
     pool_p = cache_entry_p->pool[pool_idx];
     if (pool_p == NULL)
     {
       /*
       ** out of memory-> do not cache
       */
       return -1;
     }
     /*
     ** OK now search for the array where we can fill the data
     */
     cache_data_p = pool_p->entry;
     cache_data_free_p = NULL;
     int found = 0;
     /*
     ** search for a free entry or an exact match
     */
     for (i = 0; i < STORIO_CACHE_MX_BUF_PER_POOL ; i++,cache_data_p++) 
     {
       if (cache_data_p->off_bufidx_st.s.state == 0)
       {
         if (cache_data_free_p!= NULL) continue;
         cache_data_free_p = cache_data_p;
         continue;
       }
       if (cache_data_p->off_bufidx_st.s.off == bid_cache) 
       {
         cache_data_free_p = cache_data_p;
         found = 1;
         break;     
       }
     }
     /*
     ** check if one entry has been found (empty or not empty: for not empty, it 
     ** corresponds to an exact match
     */
     if (cache_data_free_p == NULL)
     {
       /*
       ** no free entry so take one that is already in use
       */
       storio_cache_stats.put_cache_coll++;
       i = (cur_bid/STORIO_CACHE_BCOUNT)%STORIO_CACHE_MX_BUF_PER_POOL;
       cache_data_p =  &pool_p->entry[i];   
       cache_data_p->off_bufidx_st.s.off   = bid_cache;
       cache_data_p->off_bufidx_st.s.state = 1;
       key_buf = storio_cache_alloc_key_buf();
       cache_data_p->owner_key = key_buf;     

     }
     else
     {
       cache_data_p = cache_data_free_p;
       if (found  == 0)
       {
         /*
         ** this is a new entry
         */
         storio_cache_stats.put_cache_miss++;
         cache_data_p->off_bufidx_st.s.off   = bid_cache;
         cache_data_p->off_bufidx_st.s.state = 1;
         key_buf = storio_cache_alloc_key_buf();
         cache_data_p->owner_key = key_buf;                  
       }
       else
       {
         storio_cache_stats.put_cache_hit++;       
       }
     }
     /*
     ** check if we have a valid buffer for storing the data
     */
     if (cache_data_p->off_bufidx_st.s.buf_idx_valid == 0) 
     {
       /*
       ** need to allocate one
       */
       buf_cache_ts_p = storio_bufcache_alloc_buf_ts(fid,cur_bid,&local_buffer_idx);
       if (buf_cache_ts_p != NULL) 
       {
        cache_data_p->off_bufidx_st.s.buf_idx_valid = 1;
        cache_data_p->off_bufidx_st.s.buf_idx = local_buffer_idx;
       }
     }
     else
     {   
       buf_cache_ts_p = storio_bufcache_get_buf_ts_from_idx(fid,cur_bid,cache_data_p->off_bufidx_st.s.buf_idx);   
     }
     if (buf_cache_ts_p == NULL) 
     {
       /*
       ** that case can happen after a flush of the global cache: all the 256K buffer have been release so
       ** we need to reallocate them.
       */
       buf_cache_ts_p = storio_bufcache_alloc_buf_ts(fid,cur_bid,&local_buffer_idx);
       if (buf_cache_ts_p == NULL) return -1;
       /*
       ** update the entry with the new buffer reference
       */
       cache_data_p->off_bufidx_st.s.buf_idx_valid = 1;
       cache_data_p->off_bufidx_st.s.buf_idx = local_buffer_idx;     
     }
     /*
     ** OK, let's fill the timestamp in the timestamp cache buffer: caution need to adjust the
     ** offset of the destination buffer as well as the length to fill since the buffer to copy can
     ** span on two different cache entries.
     **
     **   cur_bid : index of the first projection for which the timestamp must be inserted
     **   len2copy: number of timestamps that can be loaded in the cache buffer
     **   skip : relative index of the block within the fid/timestamp cache
     **   cur_buf_p: pointer to the timestamp array provided by the application
     */
     int skip;
     int len2copy;
     skip = cur_bid-bid_cache;
     if ((skip+cur_nblocks)> STORIO_CACHE_BCOUNT)
     {
       len2copy = STORIO_CACHE_BCOUNT - skip;
     }
     else len2copy = cur_nblocks;

     storio_bufcache_write(buf_cache_ts_p,cur_bid,cur_buf_p,len2copy,
                           cache_data_p->owner_key,
                           &cache_data_p->empty_block_bitmap,
                           &cache_data_p->presence_block_bitmap
                           );
     /*
     ** update the pointers
     */
     storio_cache_stats.put_cache_blocks+= len2copy ;
     /*
     ** skip the timestamps that have already been loaded in the cache
     ** update the index of the nex block to insert (cur_bid).
     ** update the number of the blocks that remains to fill in (cur_nblocks)
     */
     cur_buf_p +=len2copy;
     cur_nblocks -= len2copy;
     cur_bid += len2copy;
     
   }
   return 0;
   
}
/*
**______________________________________________________________________________
*/
/**
*  get the list of the timestamps associated with a set of projections

  @param fid : fid associated with the file
  @param bid: index of the first projection
  @param nb_blocks: number of projections
  @param src_bufp : pointer to the source timestamp buffer
  @param dst_bufp : pointer to the destination timestamp buffer
  
  @retval number of hits
*/
int storio_cache_get(fid_t fid,uint64_t bid,uint32_t nb_blocks,uint64_t *src_bufp,uint64_t *dst_bufp)
{
    storio_cache_key_t key;
    storio_cache_entry_t *cache_entry_p;
    storio_cache_buffer_entry_t *cache_data_p;
    storio_cache_buffer_entry_t *cache_data_free_p = NULL;
    storio_cache_pool_entry_t *pool_p;
    int i;
    uint8_t *buf_cache_ts_p = NULL;
    int cur_nblocks;
    uint64_t cur_bid;
    uint64_t bid_cache;
    uint64_t *cur_dst_ts_buf_p;
    uint64_t *cur_src_ts_buf_p;
    int pool_idx;
    int total_block_hit = 0;
    int skip;
    int len2copy;    
    
    cur_nblocks = (int)nb_blocks;
    cur_bid = bid;
    cur_dst_ts_buf_p = dst_bufp;
    cur_src_ts_buf_p = src_bufp;

    /*
    ** check if the cache is enabled, if the size is not null and if
    ** the cache data has been allocated
    */    
    if (storio_cache_enable_flag != STORIO_TSCACHE_ENABLE) return total_block_hit;        
    if (nb_blocks == 0) return total_block_hit;
    if (storio_cache_cache_p == NULL) return total_block_hit;
    /*
    ** stats
    */
    storio_cache_stats.get_cache_req_blocks+=nb_blocks;
    /*
    ** clear the destination timestamp buffer
    */
    uint64_t inval_ts = 0;
    inval_ts -=1;
    for (i=0; i< nb_blocks;i++)
    {
      dst_bufp[i] = inval_ts;
    }
    /*
    ** OK, the fid is handled by this server, so attempt to get the child fid
    ** from the cache
    */
    storio_cache_fid_build_key(&key,(unsigned char *)fid);
   /*
   ** Perform a lookup on the mode block cache
   */
   cache_entry_p = com_cache_bucket_search_entry(storio_cache_cache_p,&key); 
   if (cache_entry_p == NULL)
   {
      /*
      ** entry not found
      */
      storio_cache_stats.get_cache_miss++;
      return total_block_hit;  
   }
   while (cur_nblocks > 0)
   {
     /*
     ** Get the index to the array where data will be stored
     */
     bid_cache = cur_bid/STORIO_CACHE_BUFFER_BSIZE;
     pool_idx = bid_cache%(STORIO_CACHE_MX_POOL);
     bid_cache = bid_cache*STORIO_CACHE_BUFFER_BSIZE;
     /*
     ** check if there is an available pool
     */
     if (cache_entry_p->pool[pool_idx] == NULL)
     {
        /*
        ** nothing for that offset
        */
        storio_cache_stats.get_cache_miss++;
        goto next;
     }
     pool_p = cache_entry_p->pool[pool_idx];
     /*
     ** OK now search for the array where we can fill the data
     */
     cache_data_p = pool_p->entry;
     cache_data_free_p = NULL;
     //int found = 0;
     /*
     ** search for  an exact match
     */
     for (i = 0; i < STORIO_CACHE_MX_BUF_PER_POOL ; i++,cache_data_p++) 
     {
       if (cache_data_p->off_bufidx_st.s.state == 0)
       {
         continue;
       }
       if (cache_data_p->off_bufidx_st.s.off == bid_cache) 
       {
         cache_data_free_p = cache_data_p;
         //found = 1;
         break;     
       }
     }
     /*
     ** check if one entry has been found (empty or not empty: for not empty, it 
     ** corresponds to an exact match
     */
     if (cache_data_free_p == NULL)
     {
       /*
       ** no entry
       */
       storio_cache_stats.get_cache_miss++;
       goto next;
     }

     /*
     ** check if we have a valid buffer for storing the data
     */
     if (cache_data_p->off_bufidx_st.s.buf_idx_valid == 0) 
     {
       /*
       ** no buffer !!
       */
       storio_cache_stats.get_cache_miss++;
       goto next;
     }
     /*
     ** get the pointer to the timestamp buffer
     */
     buf_cache_ts_p = storio_bufcache_get_buf_ts_from_idx(fid,cur_bid,cache_data_p->off_bufidx_st.s.buf_idx);   
     
     if (buf_cache_ts_p == NULL) 
     {
       /*
       ** that case can happen after a flush of the global cache where all the timestamp buffers have been released
       ** in that case we invalidate the enty in terms a buffer validity
       */
       cache_data_p->off_bufidx_st.s.buf_idx_valid = 0;
       storio_cache_stats.get_cache_miss++;
     }
     /*
     ** OK, let's fill the data in the buffer: caution need to adjust the
     ** offset of the source buffer as well as the length to fill since the buffer to copy can
     ** span on two different cache entries.
     */
 next:
     skip = cur_bid-bid_cache;
     if ((skip+cur_nblocks)> STORIO_CACHE_BCOUNT)
     {
       len2copy = STORIO_CACHE_BCOUNT -skip;
     }
     else len2copy =cur_nblocks;
     if (buf_cache_ts_p != 0)
     {
       total_block_hit += storio_bufcache_read(buf_cache_ts_p,cur_src_ts_buf_p,cur_bid,
                            cur_dst_ts_buf_p,len2copy,cache_data_p->owner_key,
                            cache_data_p->empty_block_bitmap,cache_data_p->presence_block_bitmap);
     }
     /*
     ** update the total read length
     */
     /*
     ** update the pointers
     */
     buf_cache_ts_p = NULL;
     cur_dst_ts_buf_p += len2copy;
     cur_src_ts_buf_p += len2copy;
     cur_nblocks -= len2copy;
     cur_bid += len2copy;
     
   }
   if (total_block_hit != 0) storio_cache_stats.get_cache_hit++;
   storio_cache_stats.get_cache_hit_bytes+=total_block_hit;
   return total_block_hit;
}
/*
**______________________________________________________________________________
*/
/**
*  remove a fid from the mode block cache

  @param fid : fid associated with the file
  
  @retval none
*/
void storio_cache_remove(fid_t fid)
{
    storio_cache_key_t key;
    
    /*
    ** check if the cache is enabled
    */    
    if (storio_cache_enable_flag != STORIO_TSCACHE_ENABLE) return;        

    if (storio_cache_cache_p == NULL) return ;
    
    /*
    ** OK, the fid is handled by this server, so attempt to get the child fid
    ** from the cache
    */
    storio_cache_fid_build_key(&key,(unsigned char *)fid);
   /*
   ** remove the entry from the cache
   */
   com_cache_bucket_remove_entry(storio_cache_cache_p,&key); 

}

/*
**______________________________________________________________________________
*/

#define SHOW_STAT_MBCACHE(probe) pChar += sprintf(pChar,"%-28s :  %10llu\n","  "#probe ,(long long unsigned int) stat_p->probe);


char *storio_cache_stats_display(char *buffer)
{
 char *pChar = buffer;
 storio_cache_stats_t *stat_p =&storio_cache_stats;
 SHOW_STAT_MBCACHE(put_cache_hit);
 SHOW_STAT_MBCACHE(put_cache_coll);
 SHOW_STAT_MBCACHE(put_cache_miss);
 SHOW_STAT_MBCACHE(put_cache_blocks);


 SHOW_STAT_MBCACHE(get_cache_miss);
 SHOW_STAT_MBCACHE(get_cache_hit);
 SHOW_STAT_MBCACHE(get_cache_req_blocks);
 SHOW_STAT_MBCACHE(get_cache_hit_bytes);
 pChar += sprintf(pChar,"\n\n");
 return pChar;
}
/*
**______________________________________________________________________________
*/
/**
* service for clearing the mode block cache statistics

  @param none
  retval none
*/
void storio_cache_stats_clear()
{
  memset(&storio_cache_stats,0,sizeof(storio_cache_stats));
}

/*
**______________________________________________________________________________
*/
/**
* service to enable the mode block cache

  @param none
  retval none
*/
void storio_cache_enable()
{
  if (storio_cache_enable_flag !=  STORIO_TSCACHE_ENABLE)
  {
    storio_cache_enable_flag = STORIO_TSCACHE_ENABLE;
    /*
    ** request for flushing the 256K buffer cache pool
    */
    storio_bufcache_flush();  
  }
}

/*
**______________________________________________________________________________
*/
/**
* service to disable the mode block cache

  @param none
  retval none
*/
void storio_cache_disable()
{
  if (storio_cache_enable_flag !=  STORIO_TSCACHE_DISABLE)
  {
    storio_cache_enable_flag = STORIO_TSCACHE_DISABLE;
    /*
    ** request for flushing the 256K buffer cache pool
    */
    storio_bufcache_flush();  
  }
}
/*
**______________________________________________________________________________
*/
/**
* creation of the block mode cache
 That API is intented to be called during the initialization of the module
 
 The max number of entries is given the STORIO_TSCACHE_MAX_ENTRIES constant
 and the size of the level 0 entry set is given by STORIO_TSCACHE_LVL0_SZ_POWER_OF_2 constant
 
 @param init_state : initial state of the cache 1: enable , 0 disable
 retval 0 on success
 retval < 0 on error
*/
 
uint32_t storio_cache_cache_init(uint32_t init_state)
{

  com_cache_usr_fct_t callbacks;
  
  if (storio_cache_cache_p != NULL)
  {
    return 0;
  }
  callbacks.usr_exact_match_fct = storio_cache_exact_match;
  callbacks.usr_hash_fct        = storio_cache_hash_compute;
  callbacks.usr_delete_fct      = storio_cache_release_entry;
  
  storio_cache_cache_p = com_cache_create(STORIO_TSCACHE_LVL0_SZ_POWER_OF_2,
                                      STORIO_TSCACHE_MAX_ENTRIES,
                                      &callbacks);
  if (storio_cache_cache_p == NULL)
  {
    /*
    ** we run out of memory
    */
    return -1;  
  }
  /*
  ** init of the key
  */
  storio_cache_key = 1;
  /*
  ** clear the stats
  */
  memset(&storio_cache_stats,0,sizeof(storio_cache_stats));
  /*
  ** init of the mode block cache state
  */
  storio_cache_enable_flag = init_state;
  
  return 0;
}
