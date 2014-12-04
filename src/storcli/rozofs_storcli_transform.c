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

/* need for crypt */
#ifdef __linux__
#define _XOPEN_SOURCE 500
#endif
#define FUSE_USE_VERSION 26
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#include <rozofs/rozofs.h>
#include <rozofs/rozofs_srv.h>

#include "rozofs_storcli_transform.h"
#include "rozofs_storcli.h"
DECLARE_PROFILING(stcpp_profiler_t);

/**
* Local variables
*/

/*
**__________________________________________________________________________
*/
/**
*   API to update in the internal structure associated with the projection
    the header of each blocks
    That function is required since the read can return less blocks than expected
    so we might face the situation where the system check headers in memory
    on an array that has not be updated
    We need also to consider the case of the end of file as well as the 
    case where blocks has been reserved but not yet written (file with holes).
    For these two cases we might have a timestam of 0 so we need to use
    the effective length to discriminate between a hole (0's array on BSIZE length)
    and a EOF case where length is set to 0.
    
    @param prj_ctx_p : pointer to the projection context
    @param layout : layout associated with the file
    @param number_of_blocks_returned : number of blocks in the projection
    @param number_of_blocks_requested : number of blocks requested
    @param raw_file_size : raw file_size reported from a fstat on the projection file (on storage)
    
    @retval none
*/     
void rozofs_storcli_transform_update_headers(rozofs_storcli_projection_ctx_t *prj_ctx_p, 
                                             uint8_t  layout,
                                             uint32_t number_of_blocks_returned,
                                             uint32_t number_of_blocks_requested,
                                             uint64_t raw_file_size)
{

    int block_idx;
    
    prj_ctx_p->raw_file_size = raw_file_size;
                    
    for (block_idx = 0; block_idx < number_of_blocks_returned; block_idx++) 
    {
      /*
      ** Get the pointer to the beginning of the block and extract its header
      */
      rozofs_stor_bins_hdr_t *rozofs_bins_hdr_p = (rozofs_stor_bins_hdr_t*)(prj_ctx_p->bins 
                                            +((rozofs_get_max_psize(layout)+(sizeof(rozofs_stor_bins_hdr_t)/sizeof(bin_t))) * block_idx));
      if (rozofs_bins_hdr_p->s.timestamp == 0)
      {
        prj_ctx_p->block_hdr_tab[block_idx].s.timestamp = rozofs_bins_hdr_p->s.timestamp;
        prj_ctx_p->block_hdr_tab[block_idx].s.effective_length = ROZOFS_BSIZE;          
      }
      else
      {
        prj_ctx_p->block_hdr_tab[block_idx].s.timestamp = rozofs_bins_hdr_p->s.timestamp;
        prj_ctx_p->block_hdr_tab[block_idx].s.effective_length = rozofs_bins_hdr_p->s.effective_length;                 
      }    
    }
    /*
    ** clear the part that is after number of returned block (assume end of file)
    */
    for (block_idx = number_of_blocks_returned; block_idx < number_of_blocks_requested; block_idx++)
    {    
      prj_ctx_p->block_hdr_tab[block_idx].s.timestamp = 0;
      prj_ctx_p->block_hdr_tab[block_idx].s.effective_length = 0;      
    } 
}    




/*
**__________________________________________________________________________
*/
/**
    API to get the exact read size from an application standpoint.
     That function go through the effective length of each blocks and
     cumulates that length on the number of effective read blocks provided in the input arguments.
     
     if EOF is encountered, the eof flag is asserted
         
    @param prj_ctx_p : pointer to the projection context
    @param eof_p : pointer to the array where end of file state is reported (asserted to 1 if EOF is encountered
    
    @retval len of the data read
*/     
int  rozofs_storcli_transform_get_read_len_in_bytes(rozofs_storcli_inverse_block_t *inverse_res_p, 
                                                    uint32_t number_of_blocks_read,uint8_t *eof_p)
{

    int len = 0;
    
    *eof_p = 0;
    
    /*
    ** compute the number of 8K full
    */
    if (number_of_blocks_read != 0)
    {
      len = ROZOFS_BSIZE*(number_of_blocks_read);
    }                
    return len;
}    

/*
**__________________________________________________________________________
*/
/**
*
*/
int rozofs_storcli_transform_inverse_check_timestamp_tb(rozofs_storcli_projection_ctx_t *prj_ctx_p,  
                                       uint8_t layout,
                                       uint32_t block_idx, 
                                       uint8_t *prj_idx_tb_p,
                                       uint64_t *timestamp_p,
                                       uint16_t *effective_len_p)
{
    uint8_t prj_ctx_idx;
    uint8_t timestamp_entry;
    *timestamp_p = 0;
    uint8_t rozofs_inverse = rozofs_get_rozofs_inverse(layout);
    uint8_t rozofs_safe = rozofs_get_rozofs_safe(layout);
    rozofs_storcli_timestamp_ctx_t *p;
    int eof = 1;
    rozofs_storcli_timestamp_ctx_t rozofs_storcli_timestamp_tb[ROZOFS_SAFE_MAX];
    uint8_t  rozofs_storcli_timestamp_next_free_idx=0;

    for (prj_ctx_idx = 0; prj_ctx_idx < rozofs_safe; prj_ctx_idx++)
    {
      if (prj_ctx_p[prj_ctx_idx].prj_state != ROZOFS_PRJ_READ_DONE)
      {
        /*
        ** that projection context does not contain valid data, so skip it
        */
        continue;      
      }
      /*
      ** Get the pointer to the projection header
      */      
      rozofs_stor_bins_hdr_t *rozofs_bins_hdr_p = (rozofs_stor_bins_hdr_t*)&prj_ctx_p[prj_ctx_idx].block_hdr_tab[block_idx];
      /*
      ** check if the current block of the projection contains valid data. The block is invalid when the timestamp and the
      ** effective length are 0. That situation can occur when a storage was in fault at the writing time, so we can face
      ** the situation where the projections read on the different storages do not return the same number of block.
      */
      if ((rozofs_bins_hdr_p->s.timestamp == 0)&&(rozofs_bins_hdr_p->s.effective_length == 0))  continue;
      if (rozofs_storcli_timestamp_next_free_idx == 0)
      {
        /*
        ** first entry
        */
        eof = 0;
        p = &rozofs_storcli_timestamp_tb[rozofs_storcli_timestamp_next_free_idx];        
        p->timestamp     = rozofs_bins_hdr_p->s.timestamp;
        p->effective_length = rozofs_bins_hdr_p->s.effective_length;
        p->count         = 0;
        p->prj_idx_tb[p->count]= prj_ctx_idx;
        p->count++;
        rozofs_storcli_timestamp_next_free_idx++;
        continue;      
      }
      /*
      ** more than 1 entry in the timestamp table
      */
      for(timestamp_entry = 0; timestamp_entry < rozofs_storcli_timestamp_next_free_idx;timestamp_entry++)
      {
        p = &rozofs_storcli_timestamp_tb[timestamp_entry];        
        if ((rozofs_bins_hdr_p->s.timestamp != p->timestamp) || (rozofs_bins_hdr_p->s.effective_length != p->effective_length)) continue;
        /*
        ** same timestamp and length: register the projection index and check if we have reached rozofs_inverse projections
        ** to stop the search
        */
        p->prj_idx_tb[p->count]= prj_ctx_idx;
        p->count++;
        if (p->count == rozofs_inverse)
        {
          /*
          ** OK we have the right number of projection so we can leave
          */
          memcpy(prj_idx_tb_p,p->prj_idx_tb,rozofs_inverse);
          /*
          ** assert the timestamp that is common to all projections used to rebuild that block
          */
          *timestamp_p     = p->timestamp;
          *effective_len_p = p->effective_length;
          /*
          ** Mark the projection that MUST be rebuilt
          */
          rozofs_storcli_mark_projection2rebuild(prj_ctx_p,rozofs_storcli_timestamp_tb,timestamp_entry,rozofs_storcli_timestamp_next_free_idx);
          return 1;       
        }
        /*
        ** try next
        */
      }
      /*
      ** that timestamp does not exist, so create an entry for it
      */
      p = &rozofs_storcli_timestamp_tb[rozofs_storcli_timestamp_next_free_idx];        
      p->timestamp     = rozofs_bins_hdr_p->s.timestamp;
      p->effective_length = rozofs_bins_hdr_p->s.effective_length;
      p->count     = 0;
      p->prj_idx_tb[p->count]= prj_ctx_idx;
      p->count++;
      rozofs_storcli_timestamp_next_free_idx++;
    }
    /*
    ** take care of the case where we try to read after the end of file
    */
    if (eof) return 0;
    /*
    ** unlucky, we did not find rozof_inverse projections with the same timestamp
    ** we need to read one more projection unless we already attempt to read rozofs_safe
    ** projection or we run out of storage that are up among the set of rozofs_safe storage   
    */
    return -1;

}



/*
**__________________________________________________________________________
*/
/**
*
*/
int rozofs_storcli_transform_inverse_check(rozofs_storcli_projection_ctx_t *prj_ctx_p,  
                                       uint8_t layout,
                                       uint32_t block_idx, 
                                       uint8_t *prj_idx_tb_p,
                                       uint64_t *timestamp_p,
                                       uint16_t *effective_len_p)
{
    uint8_t prj_ctx_idx;
    uint8_t nb_projection_with_same_timestamp = 0;
    uint8_t rozofs_inverse = rozofs_get_rozofs_inverse(layout);
    uint8_t rozofs_safe = rozofs_get_rozofs_safe(layout);
    int ret;
    int eof = 1;
    *timestamp_p = 0;
    *effective_len_p = 0;
    rozofs_storcli_timestamp_ctx_t ref_ctx;        
    rozofs_storcli_timestamp_ctx_t *ref_ctx_p = &ref_ctx;        
    rozofs_storcli_timestamp_ctx_t rozofs_storcli_timestamp_tb[ROZOFS_SAFE_MAX];
    uint8_t  rozofs_storcli_timestamp_next_free_idx=0;
    
    ref_ctx_p->count = 0;
    /*
    ** clean data used for tracking projection to rebuild
    */
    rozofs_storcli_timestamp_ctx_t *p = &rozofs_storcli_timestamp_tb[rozofs_storcli_timestamp_next_free_idx];        
    p->timestamp = 0;
    p->count     = 0;

    for (prj_ctx_idx = 0; prj_ctx_idx < rozofs_safe; prj_ctx_idx++)
    {
      if (prj_ctx_p[prj_ctx_idx].prj_state != ROZOFS_PRJ_READ_DONE)
      {
        /*
        ** that projection context does not contain valid data, so skip it
        */
        continue;      
      }
      /*
      ** Get the pointer to the projection header
      */
      rozofs_stor_bins_hdr_t *rozofs_bins_hdr_p = (rozofs_stor_bins_hdr_t*)&prj_ctx_p[prj_ctx_idx].block_hdr_tab[block_idx];
      /*
      ** skip the invalid blocks
      */
      if ((rozofs_bins_hdr_p->s.timestamp == 0) && (rozofs_bins_hdr_p->s.effective_length==0)) continue;
      if (ref_ctx_p->count == 0)
      {
        /*
        ** first projection found
        */
        eof = 0;
        ref_ctx_p->timestamp     = rozofs_bins_hdr_p->s.timestamp;
        ref_ctx_p->effective_length = rozofs_bins_hdr_p->s.effective_length;
        ref_ctx_p->count++;
        prj_idx_tb_p[nb_projection_with_same_timestamp++] = prj_ctx_idx; 
        continue;            
      }
      /*
      ** the entry is not empty check if the timestamp and the effective length of the block belonging to 
      ** projection prj_ctx_idx matches
      */
      if ((rozofs_bins_hdr_p->s.timestamp == ref_ctx_p->timestamp) &&(rozofs_bins_hdr_p->s.effective_length == ref_ctx_p->effective_length))
      {
        /*
        ** there is a match, store the projection index and check if we have reach rozofs_inverse blocks with the 
        ** same timestamp and length
        */
        ref_ctx_p->count++;
        prj_idx_tb_p[nb_projection_with_same_timestamp++] = prj_ctx_idx; 

        if (nb_projection_with_same_timestamp == rozofs_inverse)
        {
          /*
          ** ok we have found all the projection for the best case
          */
          *timestamp_p     = ref_ctx_p->timestamp;
          *effective_len_p = ref_ctx_p->effective_length;
          /*
          ** Mark the projection that MUST be rebuilt
          */
          if (rozofs_storcli_timestamp_next_free_idx)
          {
             rozofs_storcli_mark_projection2rebuild(prj_ctx_p,
                                                    rozofs_storcli_timestamp_tb,
                                                    rozofs_storcli_timestamp_next_free_idx+1,
                                                    rozofs_storcli_timestamp_next_free_idx);
          }
          return (int)rozofs_inverse;        
        }
        continue;      
      }
      /*
      ** Either the length of the timestamp does not match
      ** log the reference of the projection index in order to address a potential rebuild of the
      ** projection
      */
      p->prj_idx_tb[p->count]= prj_ctx_idx;
      p->count++;
      if (rozofs_storcli_timestamp_next_free_idx == 0)
      {
         rozofs_storcli_timestamp_next_free_idx = 1;
      }        
    }
    /*
    ** check th eof case
    */
    if (eof) return 0;
    /*
    ** unlucky, we did not find rozof_inverse projections with the same timestamp
    ** so we have to find out the projection(s) that are out of sequence
    */
    ret =  rozofs_storcli_transform_inverse_check_timestamp_tb( prj_ctx_p,  
                                        layout,
                                        block_idx, 
                                        prj_idx_tb_p,
                                        timestamp_p,
                                        effective_len_p);
    return ret;
}

/*
**____________________________________________________________________________
*/
/**
* api for reading the cycles counter
*/

static __inline__ unsigned long long rdtsc(void)
{
  unsigned hi,lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)lo)| (((unsigned long long)hi)<<32);

}
/*
**__________________________________________________________________________
*/
/**
*  that procedure check if the received projections permit to rebuild
   the initial message

  @param *prj_ctx_p: pointer to the working array of the projection
  @param first_block_idx: index of the first block to transform
  @param number_of_blocks: number of blocks to write
  @param *number_of_blocks_p: pointer to the array where the function returns number of blocks on which the transform was applied
  @param *rozofs_storcli_prj_idx_table: pointer to the array used for storing the projections index for inverse process
 
  @return: the length written on success, -1 otherwise (errno is set)
*/
 int rozofs_storcli_transform_inverse_check_for_thread(rozofs_storcli_projection_ctx_t *prj_ctx_p,  
                                       uint8_t layout,
                                       uint32_t first_block_idx, 
                                       uint32_t number_of_blocks, 
                                       rozofs_storcli_inverse_block_t *block_ctx_p,
                                       uint32_t *number_of_blocks_p,
				       uint8_t  *rozofs_storcli_prj_idx_table) 

{

    int block_idx;
    int ret;

   
    *number_of_blocks_p = 0;
    
    for (block_idx = 0; block_idx < number_of_blocks; block_idx++) {
        if (block_ctx_p[block_idx].state == ROZOFS_BLK_TRANSFORM_DONE)
        {
          /*
	  ** that case must not occur!!
	  */
          continue;        
        }
        ret =  rozofs_storcli_transform_inverse_check(prj_ctx_p,layout,
                                                      block_idx, &rozofs_storcli_prj_idx_table[block_idx*ROZOFS_SAFE_MAX],
                                                      &block_ctx_p[block_idx].timestamp,
                                                      &block_ctx_p[block_idx].effective_length);
        if (ret < 0)
        {
          /*
          ** the set of projection that have been read does not permit to rebuild, need to read more
          */
          return -1;        
        } 
	/*
	** check for end of file
	*/
        if ((block_ctx_p[block_idx].timestamp == 0)  && (block_ctx_p[block_idx].effective_length == 0 ))
        {
          /*
          ** we have reached end of file
          */
          //block_ctx_p[block_idx].state = ROZOFS_BLK_TRANSFORM_DONE;
          *number_of_blocks_p = (block_idx++);
          
          return 0;        
        }      	
    }
    *number_of_blocks_p = (block_idx++);

    return 0;
}
/*
**__________________________________________________________________________
*/
/** 
  Apply the transform to a buffer starting at "data". That buffer MUST be ROZOFS_BSIZE
  aligned.
  The first_block_idx is the index of a ROZOFS_BSIZE array in the output buffer
  The number_of_blocks is the number of ROZOFS_BSIZE that must be transform
  Notice that the first_block_idx offset applies to the output transform buffer only
  not to the input buffer pointed by "data".
  
 * 
 * @param *prj_ctx_p: pointer to the working array of the projection
 * @param first_block_idx: index of the first block to transform
 * @param number_of_blocks: number of blocks to write
 * @param *data: pointer to the source data that must be transformed
   @param *number_of_blocks_p: pointer to the array where the function returns number of blocks on which the transform was applied
  @param *rozofs_storcli_prj_idx_table: pointer to the array used for storing the projections index for inverse process
 *
 * @return: the length written on success, -1 otherwise (errno is set)
 */
 int rozofs_storcli_transform_inverse(rozofs_storcli_projection_ctx_t *prj_ctx_p,  
                                       uint8_t layout,
                                       uint32_t first_block_idx, 
                                       uint32_t number_of_blocks, 
                                       rozofs_storcli_inverse_block_t *block_ctx_p,
                                       char *data,
                                       uint32_t *number_of_blocks_p,
				       uint8_t  *rozofs_storcli_prj_idx_table) 
 {

    projection_t *projections = NULL;
    projection_t rozofs_inv_projections[ROZOFS_SAFE_MAX]; 
    int block_idx;
    uint16_t projection_id = 0;
    int prj_ctx_idx;
    *number_of_blocks_p = 0;    
    uint8_t rozofs_inverse = rozofs_get_rozofs_inverse(layout);
    
    projections = rozofs_inv_projections;
        
    /*
    ** Proceed the inverse data transform for the nb_projections2read blocks.
    */
    for (block_idx = 0; block_idx < number_of_blocks; block_idx++) {
        if (block_ctx_p[block_idx].state == ROZOFS_BLK_TRANSFORM_DONE)
        {
          /*
          ** transformation has already been done for that block of ROZOFS_BSIZE siz
          ** check the next one
          */
          continue;        
        }
#if 0
        /*
        ** check if we can find out a set of rozofs_inverse projections that will permit to
        ** rebuild the current block of ROZOFS_BSIZE sise
        ** For this we check if we can find at least rozofs_inverse projections with the same
        ** time stamp and with different angles(projection id
        ** If there is no enough valid projection we need to read a new projection on the next
        ** storage in sequence that follows the index of the last valid storage on which a projection has been
        ** read.
        ** It might be possible that we run out of storage since rozofs_safe has been reached and we have not reached
        ** rozofs_inserse projection!!
        */

        ret =  rozofs_storcli_transform_inverse_check(prj_ctx_p,layout,
                                                      block_idx, rozofs_storcli_prj_idx_table,
                                                      &block_ctx_p[block_idx].timestamp,
                                                      &block_ctx_p[block_idx].effective_length);

        if (ret < 0)
        {
          /*
          ** the set of projection that have been read does not permit to rebuild, need to read more
          */
          return -1;        
        } 
#endif
        /*
        ** Check the case of the file that has no data (there is a hole in the file), this is indicated by
        ** reporting a timestamp of 0
        */
        if ((block_ctx_p[block_idx].timestamp == 0)  && (block_ctx_p[block_idx].effective_length == ROZOFS_BSIZE ))
        {
          /*
          ** clear the memory
          */
          ROZOFS_STORCLI_STATS(ROZOFS_STORCLI_EMPTY_READ);
          memset( data + (ROZOFS_BSIZE * (first_block_idx + block_idx)),0,ROZOFS_BSIZE);
          block_ctx_p[block_idx].state = ROZOFS_BLK_TRANSFORM_DONE;
          continue;
        
        }	                                                              
        if ((block_ctx_p[block_idx].timestamp == 0)  && (block_ctx_p[block_idx].effective_length == 0 ))
        {
          /*
          ** we have reached end of file
          */
          block_ctx_p[block_idx].state = ROZOFS_BLK_TRANSFORM_DONE;
          *number_of_blocks_p = (block_idx++);
          
          return 0;        
        }      
	
        /*
        ** Here we have to take care, since the index of the projection_id use to address
        ** prj_ctx_p is NOT the real projection_id. The projection ID is found in the header of
        ** each bins, so for a set of projections pointed by bins, we might have a different
        ** projection id in the header of the projections contains in the bins array that has
        ** been read!!
        */
        int prj_count = 0;
        for (prj_count = 0; prj_count < rozofs_inverse; prj_count++)
        {
           /*
           ** Get the pointer to the beginning of the projection and extract the projection Id
           */
	   
           prj_ctx_idx = rozofs_storcli_prj_idx_table[ROZOFS_SAFE_MAX*block_idx+prj_count];
         rozofs_stor_bins_hdr_t *rozofs_bins_hdr_p = (rozofs_stor_bins_hdr_t*)(prj_ctx_p[prj_ctx_idx].bins 
                                                 +((rozofs_get_max_psize(layout)+(sizeof(rozofs_stor_bins_hdr_t)/sizeof(bin_t))) * block_idx));
            
                                                 
           /*
           ** Extract the projection_id from the header
           ** and Fill the table of projections for the block block_idx
           **   For each meta-projection
           */
           projection_id = rozofs_bins_hdr_p->s.projection_id;
           projections[prj_count].angle.p = rozofs_get_angles_p(layout,projection_id);
           projections[prj_count].angle.q = rozofs_get_angles_q(layout,projection_id);
           projections[prj_count].size = rozofs_get_psizes(layout,projection_id);
           projections[prj_count].bins = (bin_t*)(rozofs_bins_hdr_p+1);                   
        }
        

        // Inverse data for the block (first_block_idx + block_idx)
        transform_inverse_inline((pxl_t *) (data + (ROZOFS_BSIZE * (first_block_idx + block_idx))),
                rozofs_inverse,
                ROZOFS_BSIZE / rozofs_inverse / sizeof (pxl_t),
                rozofs_inverse, projections);
        /*
        ** indicate that transform has been done for the projection
        */
        block_ctx_p[block_idx].state = ROZOFS_BLK_TRANSFORM_DONE;
        /*
        ** check the case of a block that is not full: need to zero's that part
        */
        if (block_ctx_p[block_idx].effective_length < ROZOFS_BSIZE)
        {
           /*
           ** clear the memory
           */
           char *raz_p = data + (ROZOFS_BSIZE * (first_block_idx + block_idx)) + block_ctx_p[block_idx].effective_length;
           memset( raz_p,0,(ROZOFS_BSIZE-block_ctx_p[block_idx].effective_length) );
        }
    }
    /*
    ** now the inverse transform is finished, release the allocated ressources used for
    ** rebuild
    */
    *number_of_blocks_p = number_of_blocks;
    return 0;   
}


/**
*  That function check if the user data block to transform is empty

   @param data: pointer to the user data block : must be aligned on a 8 byte boundary
   @param size: size of the data block (must be blocksize aligned)
  
   @retval 0 non empty
   @retval 1 empty
*/
static inline int rozofs_data_block_check_empty(char *data, int size)
{
  uint64_t *p64;
  int i;

  p64 = (uint64_t*) data;
  for (i = 0; i < (size/sizeof(uint64_t));i++,p64++)
  {
    if (*p64 != 0) return 0;
  }
  ROZOFS_STORCLI_STATS(ROZOFS_STORCLI_EMPTY_WRITE);
  return 1;
}

/** 
  Apply the transform to a buffer starting at "data". That buffer MUST be ROZOFS_BSIZE
  aligned.
  The first_block_idx is the index of a ROZOFS_BSIZE array in the output buffer
  The number_of_blocks is the number of ROZOFS_BSIZE that must be transform
  Notice that the first_block_idx offset applies to the output transform buffer only
  not to the input buffer pointed by "data".
  
 * 
 * @param *prj_ctx_p: pointer to the working array of the projection
 * @param first_block_idx: index of the first block to transform
 * @param number_of_blocks: number of blocks to write
 * @param timestamp: date in microseconds
   @param last_block_size: effective length of the last block
 * @param *data: pointer to the source data that must be transformed
 *
 * @return: the length written on success, -1 otherwise (errno is set)
 */
 int rozofs_storcli_transform_forward(rozofs_storcli_projection_ctx_t *prj_ctx_p,  
                                       uint8_t layout,
                                       uint32_t first_block_idx, 
                                       uint32_t number_of_blocks,
                                       uint64_t timestamp, 
                                       uint16_t last_block_size,
                                       char *data) 
 {
    projection_t rozofs_fwd_projections[ROZOFS_SAFE_MAX];
    projection_t *projections; // Table of projections used to transform data
    uint16_t projection_id = 0;
    uint32_t i = 0;    
    uint8_t rozofs_forward = rozofs_get_rozofs_forward(layout);
    uint8_t rozofs_inverse = rozofs_get_rozofs_inverse(layout);
    int empty_block = 0;

    projections = rozofs_fwd_projections;

    // For each projection
    for (projection_id = 0; projection_id < rozofs_forward; projection_id++) {
        projections[projection_id].angle.p =  rozofs_get_angles_p(layout,projection_id);
        projections[projection_id].angle.q =  rozofs_get_angles_q(layout,projection_id);
        projections[projection_id].size    =  rozofs_get_psizes(layout,projection_id);
    }

    /* Transform the data */
    // For each block to send
    for (i = 0; i < number_of_blocks; i++) 
    {
         empty_block = rozofs_data_block_check_empty(data + (i * ROZOFS_BSIZE), ROZOFS_BSIZE);

        // seek bins for each projection
        for (projection_id = 0; projection_id < rozofs_forward; projection_id++) 
        {
          /*
          ** Indicates the memory area where the transformed data must be stored
          */
          projections[projection_id].bins = prj_ctx_p[projection_id].bins +
                                           ((rozofs_get_max_psize(layout)+(sizeof(rozofs_stor_bins_hdr_t)/sizeof(bin_t)))* (first_block_idx+i));
          rozofs_stor_bins_hdr_t *rozofs_bins_hdr_p = (rozofs_stor_bins_hdr_t*)projections[projection_id].bins;
          /*
          ** check if the user data block is empty: if the data block is empty no need to transform
          */
          if (empty_block)
          {
            rozofs_bins_hdr_p->s.projection_id = 0;
            rozofs_bins_hdr_p->s.timestamp     = 0;          
            rozofs_bins_hdr_p->s.effective_length = 0;    
            continue;   
          }
          /*
          ** fill the header of the projection
          */
          rozofs_bins_hdr_p->s.projection_id = projection_id;
          rozofs_bins_hdr_p->s.timestamp     = timestamp;
          /*
          ** set the effective size of the block. It is always ROZOFS_BSIZE except for the last block
          */
          if (i == (number_of_blocks-1))
          {
            rozofs_bins_hdr_p->s.effective_length = last_block_size;
          }
          else
          {
            rozofs_bins_hdr_p->s.effective_length = ROZOFS_BSIZE;          
          } 
          /*
          ** update the pointer to point out the first bins
          */
          projections[projection_id].bins += sizeof(rozofs_stor_bins_hdr_t)/sizeof(bin_t);
                                                     
        }
        /*
        ** do not apply transform for empty block
        */
        if (empty_block == 0)
        {
          /*
          ** Apply the erasure code transform for the block i+first_block_idx
          */
          transform_forward((pxl_t *) (data + (i * ROZOFS_BSIZE)),
                  rozofs_inverse,
                  ROZOFS_BSIZE / rozofs_inverse / sizeof (pxl_t),
                  rozofs_forward, projections);
        }
    }

    return 0;
}
 
