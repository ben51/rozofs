

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
//#define DIRENT_LKUP_TRACKING 1
//#warning DIRENT_LKUP_TRACKING activated

/**
 * Dirent search tracing buffer structure
 */
typedef struct _dirent_cache_search_trace_t
{
  mdirents_hash_ptr_t  hash_p; /**< virtual next pointer */
  uint8_t              bucket_idx; /**< reference of the intial list */
} dirent_cache_search_trace_t;

/**
*  Dirent search tracing buffer
*/
dirent_cache_search_trace_t   dirent_cache_search_trace_b[DIRENT_MAX_SUPPORTED_COLL];

/*
**______________________________________________________________________________
*/
/**
 Debug API: that service is used upon a loop detection while searching
            for an entry. During that process, the system might trace the list
            and that function is the display of the tracing buffer

  @param coll_cnt: last collision counter (last scanned entry)
  @param bucket_idx : index of the traced linked list
  @param root_idx : index of the dirent root file in which we have the bucket idx (always relative to a root file)

  @retval none
*/
void dirent_cache_print_tracking_buf(uint32_t coll_cnt,int bucket_idx,int root_idx)
{
   int i;
   int root = -1;
   if (coll_cnt >= DIRENT_MAX_SUPPORTED_COLL) coll_cnt = DIRENT_MAX_SUPPORTED_COLL;
   printf("Tracking lkup Buffer for bucket %d of root_idx %d:\n",bucket_idx,root_idx);
   for (i = 0; i  < coll_cnt; i++)
   {
     if (root == -1) printf("ROOT ");
     else printf("C%3.3d ",root);
     printf("bucket %3.3d type %d index %4.4d\n",
            dirent_cache_search_trace_b[i].bucket_idx,
            dirent_cache_search_trace_b[i].hash_p.type,
            dirent_cache_search_trace_b[i].hash_p.idx);

     if (dirent_cache_search_trace_b[i].hash_p.type == MDIRENTS_HASH_PTR_COLL)
     {

       if ((root == -1) ||(dirent_cache_search_trace_b[i].hash_p.idx != root))
       {
         root = dirent_cache_search_trace_b[i].hash_p.idx;
         printf("\n");
       }
     }

   }
   printf("\n");
}

/*
**______________________________________________________________________________
*/
/**
*   Search a hash entry in the linked associated with a bucket entry, the key being the value of the hash
*
  @param  dir_fd : file descriptor of the parent directory
  @param root : pointer to the root dirent entry
  @param hash_value : hash value to search
  @param bucket_idx : index of the hash bucket : taken from the lower 8 bits of the hash value applied to the name of the directory/file
  @param hash_entry_match_idx_p : pointer to an array where the local idx of the hash entry will be returned
  @param name : pointer to the name to search or NULL if lookup hash is requested only
  @param len  : len of name
  @param user_name_entry_p: pointer provided by the caller that will be filled with the pointer to the name entry in the cache entru
                           in case of success

  @retval <> NULL: pointer to the dirent cache entry where hash entry can be found, local idx is found in hash_entry_match_idx_p
  @retval NULL: not found
*/


mdirents_cache_entry_t *dirent_cache_search_hash_entry (  int dir_fd,
                                                         mdirents_cache_entry_t *root,
                                                         int                    bucket_idx,
                                                         uint32_t               hash_value,
                                                         int                   *hash_entry_match_idx_p,
                                                         uint8_t               *name,
                                                         uint16_t              len,
                                                         mdirents_name_entry_t **user_name_entry_p,
                                                         mdirents_hash_entry_t **user_hash_entry_p)
{
   mdirents_hash_ptr_t           *hash_bucket_p;
   //mdirents_hash_ptr_t           *hash_bucket_coll_p = NULL;
   //mdirents_hash_ptr_t           *hash_bucket_prev_p;
   mdirents_hash_entry_t         *hash_entry_cur_p;
   mdirents_cache_entry_t        *cache_entry_cur;
   //mdirents_cache_entry_t        *cache_entry_prev;
   uint32_t                       coll_cnt = 0;
#ifdef DIRENT_LKUP_TRACKING
   int                            hash_entry_bucket_idx = bucket_idx ;
#endif
   int                            repair = 0;
   dirent_file_repair_cause_e    cause;

   *hash_entry_match_idx_p = -1;
   if (user_name_entry_p!= NULL)*user_name_entry_p = NULL;
   if (user_hash_entry_p!= NULL)*user_hash_entry_p = NULL;
   
   /*
   ** check if bucket list is safe, if unsafe rebuild the list
   */
   dirent_cache_is_bucket_idx_safe(dir_fd,root,bucket_idx);
   /*
   ** Big Loop for searching starts here
   */
  {
     int cur_hash_entry_idx = -1;
     /*
     ** entry point after a repair attempt
     */
reloop:
     cache_entry_cur  = root;
     //cache_entry_prev = root;
     hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR(cache_entry_cur,bucket_idx);
     //hash_bucket_prev_p = hash_bucket_p;
#ifdef DIRENT_LKUP_TRACKING
    /*
    ** When tracking is enabled, we stored the information related to the linked list
    */
    if (hash_bucket_p != NULL)
    {
      dirent_cache_search_trace_b[coll_cnt].hash_p     = *hash_bucket_p;
      dirent_cache_search_trace_b[coll_cnt].bucket_idx = bucket_idx;
    }
#endif
    while(1)
    {
      coll_cnt+=1;
      if (hash_bucket_p == NULL)
      {
        /*
        ** There is no entry for that hash
        */
        DIRENT_CACHE_LOOKUP_UPDATE_STATS(coll_cnt);
        return NULL;
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
#ifdef DIRENT_LKUP_TRACKING
        dirent_cache_print_tracking_buf(coll_cnt,bucket_idx,root->header.dirent_idx[0]);
#endif
        /*
        ** check if the linked list has already been repaired, because if it was not the case
        ** we attempt to do it and then we retry from the beginning
        */
        if (repair == 0)
        {
	  coll_cnt = 0;
          dirent_file_repair(dir_fd,root,bucket_idx,DIRENT_REPAIR_LOOP);
          repair = 1;
          goto reloop;
        }
        /*
        ** repair has already been done, so exit
        */
        DIRENT_SEVERE("dirent_cache_search_hash_entry: collision counter exhausted for bucket_idx %d dirent[%d.%d],%s",
                     bucket_idx,cache_entry_cur->header.dirent_idx[0],cache_entry_cur->header.dirent_idx[1],name);
        return NULL;
      }

#ifdef DIRENT_LKUP_TRACKING
      dirent_cache_search_trace_b[coll_cnt].hash_p     = *hash_bucket_p;
      dirent_cache_search_trace_b[coll_cnt].bucket_idx = hash_entry_bucket_idx;;
#endif
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
     /*
      **_______________________________________________
      **        HASH ENTRY processing
      ** normal processing according to the type
      ** of the current element
      **_______________________________________________
      */
      if (hash_bucket_p->type == MDIRENTS_HASH_PTR_EOF)
      {
        /*
        ** This the end of list and not entry match with the requested hash value
        */
        DIRENT_CACHE_LOOKUP_UPDATE_STATS(coll_cnt);
        return NULL;
      }
      if (hash_bucket_p->type == MDIRENTS_HASH_PTR_LOCAL)
      {
        /*
        ** the index is local
        */
        cur_hash_entry_idx = hash_bucket_p->idx;

        hash_entry_cur_p = (mdirents_hash_entry_t*)DIRENT_CACHE_GET_HASH_ENTRY_PTR(cache_entry_cur,cur_hash_entry_idx);
        if (hash_entry_cur_p == NULL)
        {
          /*
          ** Check for repair, normaly that must not happen after a control for repair since it will be detected
          ** at that time, however since repair might be optional we might fall in that situation
          */
          if (repair == 0)
          {
            dirent_file_repair(dir_fd,root,bucket_idx,DIRENT_REPAIR_MEM);
            repair = 1;
           goto reloop;
          }
          DIRENT_SEVERE("dirent_cache_search_hash_entry: hash_entry_cur_p NULL for bucket_idx %d dirent[%d.%d]\n",
                     bucket_idx,cache_entry_cur->header.dirent_idx[0],cache_entry_cur->header.dirent_idx[1]);
          return NULL;
        }
        /*
        ** get the index of the bucket for tracing purpose only
        */
#ifdef DIRENT_LKUP_TRACKING
        hash_entry_bucket_idx = DIRENT_HASH_ENTRY_GET_BUCKET_IDX(hash_entry_cur_p);
#endif
        /*
        ** Check if there is a match with that value
        */
        if (hash_entry_cur_p->hash == (hash_value &DIRENT_ENTRY_HASH_MASK))
        {
           /*
           ** entry has been found
           */
           *hash_entry_match_idx_p = cur_hash_entry_idx;
           /*
           ** get the pointer to the name entry
           */
#if 1 // FDL-> set to 0 if we want match on hash only
           if (name != NULL)
           {
             mdirents_name_entry_t *name_entry_p;
             int k;
             name_entry_p = (mdirents_name_entry_t*) dirent_get_entry_name_ptr(dir_fd,cache_entry_cur,hash_entry_cur_p->chunk_idx,DIRENT_CHUNK_NO_ALLOC);
             if (name_entry_p == (mdirents_name_entry_t*)NULL)
             {
               /*
               ** something wrong that must not occur
               */
               DIRENT_SEVERE("dirent_cache_search_hash_entry: pointer does not exist at line %d\n",__LINE__);
               return NULL;
             }
             /*
             ** check if the entry match with name
             */
             if (len != name_entry_p->len)
             {
               /*
               ** try the next entry
               */
               //hash_bucket_prev_p = hash_bucket_p;
               hash_bucket_p      = &hash_entry_cur_p->next;
               continue;
             }
             int not_found = 0;
             for (k = 0; k < len; k++)
             {
               if (name_entry_p->name[k] != (char) name[k])
               {
                 /*
                 ** not the right entry
                 */
                 dirent_coll_stats_hash_name_collisions++;
  //               printf("FDL BUG Collision target %s curr %s\n",name,name_entry_p->name);
#if 0// FDL_DEBUG
  {
                 printf("lookup hash+name hash found %u (chunk %d) but entry %s (%s)of dirent file idx %d.%d (level %d)\n",
                        hash_value,hash_entry_cur_p->chunk_idx,name,name_entry_p->name,
                        cache_entry_cur->header.dirent_idx[0],
                        cache_entry_cur->header.dirent_idx[1],
                        cache_entry_cur->header.level_index);
                        exit(0);
  }
#endif
                 not_found = 1;
                 break;
               }
             }
             if (not_found == 1)
             {
                /*
                ** try the next entry
                */
                //hash_bucket_prev_p = hash_bucket_p;
                hash_bucket_p = &hash_entry_cur_p->next;
                continue;
             }
#if 0 // FDL_DEBUG
  {
      if( strcmp(name,"file_test_put_mdirentry_4412148") == 0)
             {
                 printf("Search entry found  %u (chunk %d) hash_idx %u -> in dirent file %s idx %d.%d (level %d) %d\n",
                        hash_value,hash_entry_cur_p->chunk_idx,cur_hash_entry_idx,name,
                        cache_entry_cur->header.dirent_idx[0],
                        cache_entry_cur->header.dirent_idx[1],
                        cache_entry_cur->header.level_index,
                        strcmp(name,"file_test_put_mdirentry_1034852"));
             }
  }
#endif
             /*
             ** OK, we have the exact Match
             */
             if (user_name_entry_p!= NULL) *user_name_entry_p = name_entry_p;
             if (user_hash_entry_p!= NULL) *user_hash_entry_p = hash_entry_cur_p;
           }
#endif // FDL-> set to 0 if we want match on hash only
           DIRENT_CACHE_LOOKUP_UPDATE_STATS(coll_cnt);

           return cache_entry_cur;
        }
        /*
        ** try the next entry
        */
        //hash_bucket_prev_p = hash_bucket_p;
        hash_bucket_p = &hash_entry_cur_p->next;
        continue;
      }
      /*
      ** The next entry belongs to a dirent collision entry: need to get the pointer to that collision
      ** entry and to read the content of the hash bucket associated with the bucket idx
      */
      cache_entry_cur = dirent_cache_get_collision_ptr(root,hash_bucket_p->idx);
      if (cache_entry_cur == NULL)
      {
        /*
        ** That situtaion MUST not happen when repair feature is enabled since it is control at that time.
        ** However when repair is disabled, it might happen
        */
        if (repair == 0)
        {
          dirent_file_repair(dir_fd,root,bucket_idx,DIRENT_REPAIR_MEM);
          repair = 1;
          goto reloop;
        }
        DIRENT_SEVERE("dirent_cache_search_hash_entry error at %d for bucket_idx %d \n",__LINE__,bucket_idx);
        return NULL;
      }
      /*
      ** set the pointer to the bitmap of the next collision file
      */
      hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR(cache_entry_cur,bucket_idx);
      //hash_bucket_prev_p = hash_bucket_p;
      //hash_bucket_coll_p = hash_bucket_p;
      continue;
    }
  }


  return NULL;
}
