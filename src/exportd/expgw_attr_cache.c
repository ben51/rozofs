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
#include <rozofs/rpc/export_profiler.h>
#include <rozofs/common/mattr.h>
#include <rozofs/core/com_cache.h>
#include "expgw_attr_cache.h"


/*
**______________________________________________________________________________

      Attributes LOOKUP SECTION
**______________________________________________________________________________
*/
com_cache_main_t  *expgw_attr_cache_p= NULL; /**< pointer to the fid cache  */
/*
**______________________________________________________________________________
*/
/**
* allocate an entry for the attributes cache

  @param pfid : fid of the parent
  @param name: name to search within the parent
  @param fid : fid associated with <pfid,name>
  
  @retval <>NULL: pointer to the cache entry
  @retval NULL : out of memory
*/
com_cache_entry_t *expgw_attr_alloc_entry(mattr_t *attr)
{
  expgw_attr_cache_t  *p;
  /*
  ** allocate an entry for the context
  */
  p = malloc(sizeof(expgw_attr_cache_t));
  if (p == NULL)
  {
    return NULL;
  }
  p->cache.usr_entry_p = p;
  memcpy(&p->attr,attr,sizeof(mattr_t));
  p->cache.usr_key_p   = p->attr.fid;
  list_init(&p->cache.global_lru_link);
  list_init(&p->cache.bucket_lru_link);
  p->cache.dirty_bucket_counter = 0;
  p->cache.dirty_main_counter = 0;
  return &p->cache;
}

/*
**______________________________________________________________________________
*/
/**
* release an entry of the attributes cache

  @param p : pointer to the user cache entry 
  
*/
void expgw_attr_release_entry(void *entry_p)
{
  expgw_attr_cache_t  *p = (expgw_attr_cache_t*) entry_p;

  list_remove(&p->cache.global_lru_link);
  list_remove(&p->cache.bucket_lru_link);
  free(p);
}
/*
**______________________________________________________________________________
*/
/**
*  hash computation from  fid 

  @param h : initial hahs value
  @param key2: parent fid
  @param hash2 : pointer to the secondary hash (optional)
  @param len: pointer to an array that return the length of the filename string
  
  @retval hash value
*/
static inline uint32_t uuid_hash_fnv(uint32_t h, void *key) {

    unsigned char *d ;

    if (h == 0) h = 2166136261U;

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
uint32_t expgw_attr_hash_compute(void *usr_key)
{
  uint32_t hash;

  hash = uuid_hash_fnv(0,usr_key);
  return hash;
}

/*
**______________________________________________________________________________
*/
/**
* attributes entry hash compute 

  @param key1 : pointer to the key associated with the entry from cache 
  @param key2 : pointer to array that contains the searching key
  
  @retval 0 on match
  @retval <> 0  no match  
*/

uint32_t expgw_attr_exact_match(void *key1, void *key2)
{
  if (uuid_compare(key1,key2) != 0)  
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
* creation of the FID cache
 That API is intented to be called during the initialization of the module
 
 The max number of entries is given the EXPGW_ATTR_CACHE_MAX_ENTRIES constant
 and the size of the level 0 entry set is given by EXPGW_ATTR_CACHE_LVL0_SZ_POWER_OF_2 constant
 
 retval 0 on success
 retval < 0 on error
*/
 
uint32_t expgw_attr_cache_init()
{

  com_cache_usr_fct_t callbacks;
  
  if (expgw_attr_cache_p != NULL)
  {
    return 0;
  }
  callbacks.usr_exact_match_fct = expgw_attr_exact_match;
  callbacks.usr_hash_fct        = expgw_attr_hash_compute;
  callbacks.usr_delete_fct      = expgw_attr_release_entry;
  
  expgw_attr_cache_p = com_cache_create(EXPGW_ATTR_CACHE_LVL0_SZ_POWER_OF_2,
                                      EXPGW_ATTR_CACHE_MAX_ENTRIES,
                                      &callbacks);
  if (expgw_attr_cache_p == NULL)
  {
    /*
    ** we run out of memory
    */
    return -1;  
  }
  return 0;
}
