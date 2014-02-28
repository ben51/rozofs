

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



/*
**______________________________________________________________________________
*/
/**
*  PRIVATE API: Check if the linked list for a bucket within a cache enty is safe or not

  @param target_cache_entry : pointer to target dirent cache entry that is associated with the local index
  @param bucket_idx : index of the hash bucket : taken from the lower 8 bits of the hash value applied to the name of the directory/file

  @retval 0 safe
  @retval -1 not safe
*/
static inline  int dirent_cache_is_bucket_idx_safe_for_cache_entry(mdirents_cache_entry_t *target_cache_entry,int bucket_idx)
{
  return -1;

}

/*
**______________________________________________________________________________
*/
/**
*  PRIVATE API: set safe the linked list for a bucket within a cache enty

  @param target_cache_entry : pointer to target dirent cache entry that is associated with the local index
  @param bucket_idx : index of the hash bucket : taken from the lower 8 bits of the hash value applied to the name of the directory/file

  @retval 0 safe
  @retval -1 not safe
*/
static inline  void dirent_cache_set_bucket_idx_safe_for_cache_entry(mdirents_cache_entry_t *target_cache_entry,int bucket_idx)
{

}

/**______________________________________________________________________________
*/
/**
*   FRIEND API: Search for a particular collision file index within a particular linked list
    This is archieved by starting from the root entry

  @param dir_fd : file handle of the parent directory
  @param root : pointer to the root dirent entry
  @param target_cache_entry : pointer to target dirent cache entry that is associated with the local index
  @param hash_p : pointer to the result of the search
  @param bucket_idx : index of the hash bucket : taken from the lower 8 bits of the hash value applied to the name of the directory/file
  @param local_idx : local hash entry index within the target_cache_entry cache entry

  @retval 0 if found
  @retval -1 if not found
*/
static inline int dirent_cache_search_for_coll_with_bucket_idx ( int dir_fd,
                                                             mdirents_cache_entry_t *root,
                                                             mdirents_cache_entry_t *target_cache_entry,
                                                             int                   bucket_idx,
                                                             int                   coll_idx)
{
   int  ret;
   mdirents_hash_entry_t *hash_entry_cur_p = NULL;
   mdirents_cache_entry_t *cache_entry_cur;
   mdirents_hash_ptr_t   *hash_bucket_p;
   mdirent_cache_ptr_t *bucket_virt_p = NULL;
   mdirent_cache_ptr_t *hash_cur_virt_p= NULL;
   int cur_hash_entry_idx = -1;
   int found = 0.;
   dirent_file_repair_cause_e    cause;
   /*
   ** Check if the target is safe, notice that the target can be null, so we do the hard way
   */
   if (target_cache_entry != NULL)
   {
     ret = dirent_cache_is_bucket_idx_safe_for_cache_entry(target_cache_entry,bucket_idx);
     if (ret == 0) return 0;
   }
   /*
   ** OK, no choice go through the linked list and search
   */
   cache_entry_cur = root;
   hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR_WITH_VIRT(cache_entry_cur,bucket_idx,&bucket_virt_p);
   while(1)
   {
     if (hash_bucket_p == NULL)
     {
       /*
       ** something wrong, we run out of memory
       */
       DIRENT_SEVERE("dirent_cache_search_for_coll_with_bucket_idx error at %d\n",__LINE__);
       return -1;
     }
     cause =  dirent_cache_check_repair_needed(root,cache_entry_cur,hash_bucket_p,bucket_idx);
     if (cause != DIRENT_REPAIR_NONE) goto repair;
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
       break;
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
       if (hash_entry_cur_p == NULL)
       {
         /*
         ** something wrong!! (either the index is out of range and the memory array has been released
         */
         DIRENT_SEVERE("dirent_cache_search_for_coll_with_bucket_idx error at %d cur_hash_entry_idx %d \n",__LINE__,cur_hash_entry_idx);
         return -1;
       }
       hash_bucket_p = &hash_entry_cur_p->next;
       continue;
     }
     /*
     **----------------------------------------------------------------------
     **COLL index case -> Check if is matches our collision index
     **----------------------------------------------------------------------
     */
     if (hash_bucket_p->idx == coll_idx)
     {
       found = 1;
       break;
     }
     /*
     ** Not yet found-> try the next
     */
     cache_entry_cur = dirent_cache_get_collision_ptr(root,hash_bucket_p->idx);
     if (cache_entry_cur == NULL)
     {
        /*
        ** something is rotten in the cache since the pointer to the collision dirent cache
        ** does not exist (notice that it was there at the time of the control for repair !!!
        */	
        DIRENT_SEVERE("dirent_cache_search_for_coll_with_bucket_idx error at %d\n",__LINE__);
        return -1;
     }
     hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR(cache_entry_cur,bucket_idx);
     continue;
   }
   if (found == 1) return 0;

   /*
   ** here we are in the situation where the new entry is inserted but none collision reference it so
   ** trigger a file repair for that linked list
   */
   cause = DIRENT_REPAIR_BUCKET_IDX_MISMATCH;
repair:

   dirent_file_repair(dir_fd,root,bucket_idx,cause);
   /*
   ** Set the link list safe
   */
   dirent_cache_set_bucket_idx_safe_for_cache_entry(cache_entry_cur,bucket_idx);
   return 0;

}
/*
**______________________________________________________________________________
*/
/**
   The function returns the pointer to the last cache entry that has been modified during the insertion
   in the cache entry provided as input argument. Because of the link list insertion, it might possible that
   another dirent cache entry might be modified.
*

  @param dir_fd : file handle of the parent directory
  @param root : pointer to the root dirent entry
  @param target_cache_entry : pointer to target dirent cache entry that is associated with the local index
  @param hash_p : pointer to the result of the search
  @param bucket_idx : index of the hash bucket : taken from the lower 8 bits of the hash value applied to the name of the directory/file
  @param local_idx : local hash entry index within the target_cache_entry cache entry

  @retval <> NULL: the entry has been inserted: the returned value is the pointer to the last modified cache entry
  @retval NULL: out of entries
  
  note: that function has been modified to take into account the case of a file repair while attempting to insert
        a hash_entry in a brend new collision cache entry. The previous code did not restart from the beginning
	and the insertiion failed when the bucket_p of the root cache entry is EOF.
*/

mdirents_cache_entry_t *dirent_cache_insert_hash_entry ( int dir_fd,
                                                         mdirents_cache_entry_t *root,
                                                         mdirents_cache_entry_t *target_cache_entry,
                                                         int                   bucket_idx,
                                                         mdirents_hash_ptr_t   *hash_p,
                                                         int                   local_idx)
{
   mdirents_hash_ptr_t   *hash_bucket_p = NULL;
   mdirents_hash_ptr_t   *hash_bucket_target_p = NULL;
   mdirents_hash_entry_t *hash_entry_p = NULL;
   mdirents_hash_entry_t *hash_entry_cur_p = NULL;
   mdirent_cache_ptr_t *bucket_virt_p = NULL;
   mdirent_cache_ptr_t *bucket_target_virt_p = NULL;
   mdirent_cache_ptr_t *hash_virt_p = NULL;
   mdirent_cache_ptr_t *hash_cur_virt_p= NULL;
   int found = 0;
   dirent_file_repair_cause_e    cause;
   int cur_hash_entry_idx;
     
   int loop_cnt = 0;
   
   

   /*
   ** 1- set the reference of the bucket as well as the type to FREE in case of repair
   */
   hash_entry_p = (mdirents_hash_entry_t*)DIRENT_CACHE_GET_HASH_ENTRY_PTR_WITH_VIRT(target_cache_entry,local_idx,&hash_virt_p);
   if (hash_entry_p == NULL)
   {
     /*
     ** something wrong!! (either the index is out of range and the memory array has been released
     */
     DIRENT_SEVERE("dirent_cache_insert_hash_entry error at %d\n",__LINE__);
     return NULL;
   }
   hash_entry_p->next.type = MDIRENTS_HASH_PTR_FREE;
   DIRENT_HASH_ENTRY_SET_BUCKET_IDX(hash_entry_p,bucket_idx);
   /*
   **________________________________________________________________________________________
   *    T A R G E  T   I S   T H E   R O O T
   **________________________________________________________________________________________
   */
   /*
   ** Check if the target_cache_entry  is the root. In that case, the local index is inserted at the
   ** top: it correspond to the head of the entry associated with the bucket_idx
   */
   if (root == target_cache_entry)
   {
     hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR_WITH_VIRT(target_cache_entry,bucket_idx,&bucket_virt_p);
     if (hash_bucket_p == NULL)
     {
       /*
       ** something wrong, we run out of memory
       */
       DIRENT_SEVERE("dirent_cache_insert_hash_entry error at %d\n",__LINE__);
       return NULL;
     }
    /*
    ** For the case of the repair it is necessary to get the hash entry bitmap array associated
    ** with the cache entry in order to be able to control the state of the local hash entry
    ** when we check the next pointer:
    **  That might be avoided in the insertion, since that case can also been detected while searching
    ** for an entry within the link list with the same bucket index;
    ** Furthermore, the fact that the next pointer is not valid has on impact on the entry that is
    ** currently inserted since the system inserts at the top.
    */
     /*
     ** a) update the pointer to the next hash entry in the hash entry that is inserted
     */
     hash_entry_p->next   =  *hash_bucket_p;
     DIRENT_HASH_ENTRY_SET_BUCKET_IDX(hash_entry_p,bucket_idx);

     /*
     ** b) set the new hash entry as the first of the list for that dirent cache entry
     */
     hash_bucket_p->type  = MDIRENTS_HASH_PTR_LOCAL;
     hash_bucket_p->idx   = local_idx;
     /*
     ** c) assert the dirty bit for hash entry and bucket entry:
     ** this just there for an optimization during disk write operation. However
     ** it is not used
     */
     bucket_virt_p->s.dirty = 1;
     hash_virt_p->s.dirty   = 1;
     /*
     ** c) entry is inserted
     */
     return root;
   }
   /*
   **________________________________________________________________________________________
   *    T A R G E  T   I S  N O T   T H E   R O O T
   **________________________________________________________________________________________
   */
   /*
   ** root and target are different, so target is a dirent collision file:
   ** Different cases are considered:
   **  1- the target cache entry is already involved in the linked list (type of first pointer is not EOF)
   **     In that case we just need to update the target cache entry as we did for the case where root and
   **     target cache entry are the same
   **
   **    For the case of the repaur, it is a little bit tricky since in case of failure, that
   **    collision file might no be referenced by any of the dirent file participating in the
   **    linked list associated with the bucket. In that case, if there is no control, we can
   **    face the situation where we insert an entry in a dirent file that is referenced by nobody
   **
   **    To solve that situation, the only was is to go through all the dirent file where there ara
   **    hash entries referencing that bucket and then to rebuild a safe linked list.
   */
   hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR_WITH_VIRT(target_cache_entry,bucket_idx,&bucket_virt_p);
   if (hash_bucket_p == NULL)
   {
     /*
     ** something wrong, we run out of memory
     */
     DIRENT_SEVERE("dirent_cache_insert_hash_entry error at %d\n",__LINE__);
     return NULL;
   }
   /**
   *  Create a virtual loop to address the case of a potential repair
   */
   loop_cnt = 0;
   /*
   ** Here we address the case where it is no the first element of that list that is inserted in that
   ** collision file. As for the case of the root, the entry is insert at the top
   */
   if ((hash_bucket_p->type == MDIRENTS_HASH_PTR_LOCAL) || ( hash_bucket_p->type == MDIRENTS_HASH_PTR_COLL))
   {
     /*
     ** update the pointer to the next hash entry in the hash entry that is inserted
     ** (note : do not forget to reload the information related to the bucket index since
     **  the hash pointer of the bucket might have it empty not handled at hash pointer level)
     */
     hash_entry_p->next   =  *hash_bucket_p;
     DIRENT_HASH_ENTRY_SET_BUCKET_IDX(hash_entry_p,bucket_idx);

     /*
     ** set the new hash entry as the first of the list for that dirent cache entry
     */
     hash_bucket_p->type  = MDIRENTS_HASH_PTR_LOCAL;
     hash_bucket_p->idx   = local_idx;
     /*
     ** assert the dirty bit for hash entry and bucket entry
     */
     bucket_virt_p->s.dirty = 1;
     hash_virt_p->s.dirty   = 1;
     /*
     ** Now check if the local link list is safe otherwise we need to go through the
     ** the full linked link to check if that collision block is reference within some
     ** other
     */
    if (dirent_cache_search_for_coll_with_bucket_idx (dir_fd,root,
                                                  target_cache_entry,bucket_idx,
                                                  target_cache_entry->header.dirent_idx[1]) != 0)
     {
       /*
       ** something wrong happens!!
       */
       return NULL;
     }
     /*
     ** all is fine
     */
     return target_cache_entry;
   }

   /*
   **________________________________________________________________________________________
   *    E O F  C A S E  O N   T H E   T A R G E T    C A C H E   E N T R Y
   **________________________________________________________________________________________
   */
   /*
   ** EOF case on the target cache entry: so it is the first entry inserted for that bucket in that
   ** dirent collision file: Need to check if it is also the first one inserted for that bucket
   ** at root level. In that case we do not need to go though the linked list until finding out
   ** the entry with EOF
   */
   hash_bucket_target_p = hash_bucket_p;
   bucket_target_virt_p = bucket_virt_p;
   hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR_WITH_VIRT(root,bucket_idx,&bucket_virt_p);
   if (hash_bucket_p == NULL)
   {
     /*
     ** something wrong, we run out of memory
     */
     DIRENT_SEVERE("dirent_cache_insert_hash_entry error at %d\n",__LINE__);
     return NULL;
   }
   if (hash_bucket_p->type == MDIRENTS_HASH_PTR_EOF)
   {
     /*
     ** it is the fist time that we insert a entry for that bucket idx (from root standpoint
     ** so we don't need to go to the while loop below
     */
     hash_bucket_target_p->type  = MDIRENTS_HASH_PTR_LOCAL;
     hash_bucket_target_p->idx   = local_idx;
     hash_entry_p->next.type  = MDIRENTS_HASH_PTR_EOF;
     /*
     ** update the head of list on the root entry by setting up the bucket idx pointer
     */
     hash_bucket_p->type  = MDIRENTS_HASH_PTR_COLL;
     hash_bucket_p->idx   = hash_p->idx;
     /*
     ** assert the dirty bit for hash entry and bucket entry
     */
     bucket_target_virt_p->s.dirty = 1;
     bucket_virt_p->s.dirty        = 1;
     hash_virt_p->s.dirty          = 1;
     return root;
   }
      


   /*
   **________________________________________________________________________________________
   *    H A R D  W A Y    C A S E
   **________________________________________________________________________________________
   */
   /**
   *  Here we need to go though the linked list of the bucket searching fro the EOF
   */
   /*
   ** EOF case: so it is the first entry that is inserted for that hash bucket index in that dirent collision file
   ** Here we need to go through the linked list associated with bucket idx by starting from root until finding out
   ** the hash entry that contains the EOF mark
   */
  {
     int repair = 0;

reloop:
     cur_hash_entry_idx = -1;
     hash_bucket_p = DIRENT_CACHE_GET_BUCKET_PTR_WITH_VIRT(root,bucket_idx,&bucket_virt_p);
     for (loop_cnt = 0; loop_cnt< MDIRENTS_ENTRIES_COUNT;loop_cnt++)
     {
       if (hash_bucket_p == NULL)
       {
         /*
         ** something wrong, we run out of memory
         */
         DIRENT_SEVERE("dirent_cache_insert_hash_entry error at %d\n",__LINE__);
         return NULL;
       }
       if (repair == 0)
       {
         cause =  dirent_cache_check_repair_needed(root,root,hash_bucket_p,bucket_idx);
          if (cause != DIRENT_REPAIR_NONE)
          {
            dirent_file_repair(dir_fd,root,bucket_idx,cause);
            repair = 1;
            goto reloop;
          }
        }
       /*
       **----------------------------------------------------------------------
       ** E O F  or collision -> so it the last element of the local list of the root cache entry 
       **----------------------------------------------------------------------
       */
       if ((hash_bucket_p->type == MDIRENTS_HASH_PTR_EOF) || (hash_bucket_p->type == MDIRENTS_HASH_PTR_COLL))
       {
         /*
         ** end of list has been found so, exit the while loop
         */
         found = 1;
         break;
       }
       /*
       **----------------------------------------------------------------------
       **LOCAL index case -> go to the next entry
       **----------------------------------------------------------------------
       */
       if (hash_bucket_p->type == MDIRENTS_HASH_PTR_LOCAL)
       {
         cur_hash_entry_idx = hash_bucket_p->idx;

         hash_entry_cur_p = (mdirents_hash_entry_t*)DIRENT_CACHE_GET_HASH_ENTRY_PTR_WITH_VIRT(root,cur_hash_entry_idx,&hash_cur_virt_p);
         if (hash_entry_cur_p == NULL)
         {
           if (repair == 0)
           {
             dirent_file_repair(dir_fd,root,bucket_idx,DIRENT_REPAIR_MEM);
             repair = 1;
            goto reloop;
           }
           /*
           ** something wrong!! (either the index is out of range and the memory array has been released
           */
           DIRENT_SEVERE("dirent_cache_insert_hash_entry error at %d cur_hash_entry_idx %d \n",__LINE__,cur_hash_entry_idx);
           return NULL;
         }
         hash_bucket_p = &hash_entry_cur_p->next;
         continue;
       }
     }
     
     if (found == 0)
     {
       /*
       ** check if repair has been called, if not attempt to do it and reloop: tis address the case of a loop in the list
       */
       if (repair == 0)
       {
         dirent_file_repair(dir_fd,root,bucket_idx,DIRENT_REPAIR_LOOP);
         repair = 1;
         goto reloop;
       }
       /*
       ** not found even after a repair
       */
       DIRENT_SEVERE("dirent_cache_insert_hash_entry error at %d  not found even after a repair\n",__LINE__);
       return NULL;

     }
   }

   /*
   **  We have found the last entry in the root cache entry, so we can build the linked list
   */
   /*
   ** update first the new entry by inserting the EOF
   */
   hash_bucket_target_p->type  = MDIRENTS_HASH_PTR_LOCAL;
   hash_bucket_target_p->idx   = local_idx;

   /*
   ** Now update the next pointer of the formaer last entry towards the new inserted entry
   */
   if (cur_hash_entry_idx == -1)
   {
     hash_entry_p->next = *hash_bucket_p;

     hash_bucket_p->type  = MDIRENTS_HASH_PTR_COLL;
     hash_bucket_p->idx   = hash_p->idx;   
     /*
     ** assert the dirty bit for hash entry and bucket entry
     */
     bucket_target_virt_p->s.dirty = 1;
     bucket_virt_p->s.dirty        = 1;
     hash_virt_p->s.dirty          = 1;   
   }
   else
   {
     hash_entry_p->next = hash_entry_cur_p->next;
     
     hash_entry_cur_p->next.type  = MDIRENTS_HASH_PTR_COLL;
     hash_entry_cur_p->next.idx   = hash_p->idx;
     hash_cur_virt_p->s.dirty     = 1;
     /*
     ** assert the dirty bit for hash entry and bucket entry
     */
     bucket_target_virt_p->s.dirty   = 1;
     hash_virt_p->s.dirty            = 1;

   }
   DIRENT_HASH_ENTRY_SET_BUCKET_IDX(hash_entry_p,bucket_idx);   
   return root;
}

