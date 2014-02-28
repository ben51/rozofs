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
#include "expgw_fid_cache.h"



/*
**______________________________________________________________________________

      FID LOOKUP SECTION
**______________________________________________________________________________
*/
com_cache_main_t  *expgw_fid_cache_p= NULL; /**< pointer to the fid cache  */
/*
**______________________________________________________________________________
*/
/**
* allocate an entry for the fid cache

  @param pfid : fid of the parent
  @param name: name to search within the parent
  @param fid : fid associated with <pfid,name>
  
  @retval <>NULL: pointer to the cache entry
  @retval NULL : out of memory
*/
com_cache_entry_t *expgw_fid_alloc_entry(fid_t pfid,char *name,unsigned char *fid)
{
  expgw_fid_cache_t  *p;
  expgw_fid_key_t    *key_p;
  /*
  ** allocate an entry for the context
  */
  p = malloc(sizeof(expgw_fid_cache_t));
  if (p == NULL)
  {
    return NULL;
  }
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
  key_p->name_len = strlen(name);
  key_p->name = malloc(key_p->name_len+1);
  if (key_p->name == NULL)
  {
     /*
     ** out of memory: release the main context
     */
     free(p);
     return NULL;  
  }
  memcpy(key_p->name,name,key_p->name_len+1);
  memcpy(key_p->pfid,pfid,sizeof(fid_t));
  if (fid != NULL)
  {
    /*
    ** copy the child fid
    */
    memcpy(p->fid,fid,sizeof(fid_t));
  }
  return &p->cache;
}

/*
**______________________________________________________________________________
*/
/**
* release an entry of the fid cache

  @param p : pointer to the user cache entry 
  
*/
void expgw_fid_release_entry(void *entry_p)
{
  expgw_fid_cache_t  *p = (expgw_fid_cache_t*) entry_p;
  /*
  ** release the array used for storing the name
  */
  free(p->key.name);
  list_remove(&p->cache.global_lru_link);
  list_remove(&p->cache.bucket_lru_link);
  free(p);
}
/*
**______________________________________________________________________________
*/
/**
*  hash computation from parent fid and filename (directory name or link name)

  @param h : initial hahs value
  @param key1: filename or directory name
  @param key2: parent fid
  @param hash2 : pointer to the secondary hash (optional)
  @param len: pointer to an array that return the length of the filename string
  
  @retval hash value
*/
static inline uint32_t filename_uuid_hash_fnv(uint32_t h, void *key1, void *key2, uint32_t *hash2, int *len) {

    unsigned char *d = (unsigned char *) key1;
    int i = 0;

    if (h == 0) h = 2166136261U;
    /*
     ** hash on name
     */
    for (d = key1; *d != '\0'; d++, i++) {
        h = (h * 16777619)^ *d;

    }
    *len = i;

    *hash2 = h;
    /*
     ** hash on fid
     */
    d = (unsigned char *) key2;
    for (d = key2; d != key2 + 16; d++) {
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
uint32_t expgw_fid_hash_compute(void *usr_key)
{
  expgw_fid_key_t  *key_p = (expgw_fid_key_t*) usr_key;
  uint32_t hash;
  uint32_t hash2;
  int len;
  
   hash = filename_uuid_hash_fnv(0, key_p->name, key_p->pfid, &hash2, &len);
   return hash;
}



/*
**______________________________________________________________________________
*/
/**
* fid entry hash compute 

  @param key1 : pointer to the key associated with the entry from cache 
  @param key2 : pointer to array that contains the searching key
  
  @retval 0 on match
  @retval <> 0  no match  
*/

uint32_t expgw_fid_exact_match(void *key1, void *key2)
{
  expgw_fid_key_t  *key1_p = (expgw_fid_key_t*) key1;
  expgw_fid_key_t  *key2_p = (expgw_fid_key_t*) key2;
  
  if (key1_p->name_len != key2_p->name_len)
  {
    return 1;
  }
  if (strncmp((const char*)key1_p->name,(const char*)key2_p->name,key1_p->name_len)!= 0)
  {
    return 1;
  }
  if (uuid_compare(key1_p->pfid, key2_p->pfid) != 0)  
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
 
 The max number of entries is given the EXPGW_FID_CACHE_MAX_ENTRIES constant
 and the size of the level 0 entry set is given by EXPGW_FID_CACHE_LVL0_SZ_POWER_OF_2 constant
 
 retval 0 on success
 retval < 0 on error
*/
 
uint32_t expgw_fid_cache_init()
{

  com_cache_usr_fct_t callbacks;
  
  if (expgw_fid_cache_p != NULL)
  {
    return 0;
  }
  callbacks.usr_exact_match_fct = expgw_fid_exact_match;
  callbacks.usr_hash_fct        = expgw_fid_hash_compute;
  callbacks.usr_delete_fct      = expgw_fid_release_entry;
  
  expgw_fid_cache_p = com_cache_create(EXPGW_FID_CACHE_LVL0_SZ_POWER_OF_2,
                                      EXPGW_FID_CACHE_MAX_ENTRIES,
                                      &callbacks);
  if (expgw_fid_cache_p == NULL)
  {
    /*
    ** we run out of memory
    */
    return -1;  
  }
  return 0;
}
