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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <assert.h>

#include <rozofs/rozofs.h>
#include "config.h"
#include <rozofs/common/log.h>
#include <rozofs/common/xmalloc.h>
//#include "rozofs_stats.h"
#include <rozofs/core/rozofs_tx_common.h>
#include <rozofs/core/rozofs_tx_api.h>
#include "rozofs_storcli.h"
#include "storage_proto.h"
#include <rozofs/core/af_unix_socket_generic_api.h>
#include <rozofs/rozofs_srv.h>
#include "rozofs_storcli_rpc.h"
#include <rozofs/rpc/sproto.h>
#include "storcli_main.h"
#include <rozofs/rozofs_timer_conf.h>
#include "rozofs_storcli_mojette_thread_intf.h"

int rozofs_storcli_get_position_of_first_byte2write();

DECLARE_PROFILING(stcpp_profiler_t);

/*
**__________________________________________________________________________
*/
/**
* PROTOTYPES
*/
int rozofs_storcli_internal_read_req(rozofs_storcli_ctx_t *working_ctx_p,rozofs_storcli_ingress_write_buf_t  *wr_proj_buf_p);


/**
* allocate a sequence number for the read. The sequence number is associated to
* the read context and is common to all the request concerning the projections of a particular set of distribution
 @retval sequence number
*/
extern uint32_t rozofs_storcli_allocate_read_seqnum();



/**
*  END PROTOTYPES
*/
/*
**__________________________________________________________________________
*/

/**
* Local prototypes
*/
void rozofs_storcli_write_req_processing_cbk(void *this,void *param) ;
void rozofs_storcli_write_req_processing(rozofs_storcli_ctx_t *working_ctx_p);

//int rozofs_storcli_remote_rsp_cbk(void *buffer,uint32_t socket_ref,void *user_param);


/*
**_________________________________________________________________________
*      LOCAL FUNCTIONS
**_________________________________________________________________________
*/

/**
* Check if all the forward transform are done and if the system can proceed with the sending
  of the projections towards the storages
  
  @param prj_cxt_p: pointer to the entry buffer subset used for building the projections
  
  @retval 1 : all the transformation are finish-> projections can be sent to storages
  @retval 0 : read in progress on some buffer subset
  @retval -1 : fatal error on a read-> buffer cannot be written
*/
static inline int rozofs_storcli_check_all_forward_transform_done(rozofs_storcli_ingress_write_buf_t *wr_proj_buf_p)
{
    int i;
    int status = 1;

    for (i = 0; i < ROZOFS_WR_MAX; i++)
    {
      if ( wr_proj_buf_p[i].state == ROZOFS_WR_ST_IDLE) continue;      
      if ( wr_proj_buf_p[i].state == ROZOFS_WR_ST_TRANSFORM_DONE) continue;      
      if ( wr_proj_buf_p[i].state == ROZOFS_WR_ST_ERROR) return -1;  
      status = 0;     
   }
   return status;
}
/*
**__________________________________________________________________________
*/
/**
* The purpose of that function is to return TRUE if there are enough projection received for
  rebuilding the associated initial message
  
  @param layout : layout association with the file
  @param prj_cxt_p: pointer to the projection context (working array)
  @param *distribution: pointer to the resulting distribution--> obsolete
  
  @retval 1 if there are enough received projection
  @retval 0 when there is enough projection
*/
static inline int rozofs_storcli_all_prj_write_check(uint8_t layout,rozofs_storcli_projection_ctx_t *prj_cxt_p,dist_t *distribution)
{
  /*
  ** Get the rozofs_forward value for the layout
  */
  uint8_t   rozofs_forward = rozofs_get_rozofs_forward(layout);
  int i;
  int received = 0;
  
  for (i = 0; i <rozofs_forward; i++,prj_cxt_p++)
  {
    if (prj_cxt_p->prj_state == ROZOFS_PRJ_WR_DONE) 
    {
      received++;
//      dist_set_true(*distribution, prj_cxt_p->stor_idx);
    }
    if (received == rozofs_forward) return 1;   
  }
  return 0;
}

/*
**_________________________________________________________________________
*      PUBLIC FUNCTIONS
**_________________________________________________________________________
*/

/** 
 *  The purpose of that function is to split the input buffer (data write part) in sections that are
    a multiple of ROZOFS_BSIZE.
    Since offset (off) might not start of a ROZOFS_BSIZE boundary in might be required to read the
    first block for making adjustement 
    In the off+len does not ends on a ROZOFS_BSIZE  boundary, it is needed to read the last block if
    off+len is less that file_size.
    All these operations are required because the transform applies on fixed buffer size (ROZOFS_BSIZE)
    
    In this particular case, we know that the file is empty; so there is no need to request data
    from the storage, we just need to padd the first block with 0
 * 
 * @param *working_ctx_p: pointer to the root transaction context 
 * @param off: offset to write from
 * @param len: length to write
 * @param bid_p: pointer  to the array when the function returns the index of the first block to write
 * @param nb_blocks_p: pointer  to the array when the function returns the number of blocks to write
 *
 * @return: none
 */
void rozofs_storcli_prepare2write_empty_file(rozofs_storcli_ctx_t *working_ctx_p, 
                                  uint64_t off, 
                                  uint32_t len,
                                  uint64_t *bid_p,
                                  uint32_t *nb_blocks_p) {
    int64_t length = -1;
    uint64_t first = 0;
    uint64_t last = 0;
    int fread = 0;
    uint16_t foffset = 0;
    uint16_t loffset = 0;
    int i;
    void * buffer;
    uint8_t * payload;   
    
   *nb_blocks_p = 0;
    
   rozofs_storcli_ingress_write_buf_t  *wr_proj_buf_p = working_ctx_p->wr_proj_buf;

    for (i = 0; i < ROZOFS_WR_MAX; i++)
    {
      wr_proj_buf_p[i].len = 0;
      wr_proj_buf_p[i].read_buf = NULL;
      wr_proj_buf_p[i].data     = NULL;
      wr_proj_buf_p[i].last_block_size = ROZOFS_BSIZE;
    }    
    length = len;
    /*
    **  Nb. of the first block to write
    */
    first = off / ROZOFS_BSIZE;
    /*
    ** store the index of the fisrt block to write
    */
    *bid_p = first;
    /*
    ** Offset (in bytes) for the first block
    */
    foffset = off % ROZOFS_BSIZE;    
    /* 
    ** Nb. of the last block to write
    */
    last = (off + length) / ROZOFS_BSIZE + ((off + length) % ROZOFS_BSIZE == 0 ? -1 : 0);
    /*
    **  Offset (in bytes) for the last block
    */
    loffset = (off + length) - last * ROZOFS_BSIZE;
    /*
    ** Is it neccesary to read the first block ?
    */
    if (foffset != 0)
        fread = 1;

    /*
    ** The 1rst block do not start on a BSIZE boundary, so we need to pad at beginning
    */
    if (fread == 1) {
        wr_proj_buf_p[ROZOFS_WR_FIRST].state = ROZOFS_WR_ST_TRANSFORM_REQ;
        wr_proj_buf_p[ROZOFS_WR_FIRST].off   = first * ROZOFS_BSIZE;            
	wr_proj_buf_p[ROZOFS_WR_FIRST].first_block_idx    = 0;            
	wr_proj_buf_p[ROZOFS_WR_FIRST].number_of_blocks   = 1;  
	*nb_blocks_p += 1;    
	
        /* Allocate a buffer as if it was received from a storage */
        buffer = ruc_buf_getBuffer(rozofs_tx_pool[_ROZOFS_TX_LARGE_RX_POOL]);
        wr_proj_buf_p[ROZOFS_WR_FIRST].read_buf = buffer;
        if (buffer == NULL) {
           severe("Out of ROZOFS_TX_LARGE_RX_POOL");
           return; 
        }
        payload = (uint8_t*) ruc_buf_getPayload(buffer);
        wr_proj_buf_p[ROZOFS_WR_FIRST].data = (char*)payload;

        /* Fill with 0 the beginning of the block and then put valid data */
        memset(payload,0,foffset);
        payload += foffset;
        if ((len + foffset) > ROZOFS_BSIZE) {
	  /* Fill up to the end of the block */
          memcpy(payload,working_ctx_p->data_write_p,ROZOFS_BSIZE - foffset);
          wr_proj_buf_p[ROZOFS_WR_FIRST].len = ROZOFS_BSIZE;
        }
        else {	 
	  /* copy the few given data */
          memcpy(payload,working_ctx_p->data_write_p,len); 
          wr_proj_buf_p[ROZOFS_WR_FIRST].len = len+foffset;
        }	   
        wr_proj_buf_p[ROZOFS_WR_FIRST].last_block_size = wr_proj_buf_p[ROZOFS_WR_FIRST].len;	  	    
	
	/*
	** No more block to add
	*/
	if (first == last) return;
	
	
	/*
	** Some more data are left
	*/	
		                        
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].state = ROZOFS_WR_ST_TRANSFORM_REQ;
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].off   = (first+1) * ROZOFS_BSIZE;            
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].len   = len-(ROZOFS_BSIZE-foffset);            
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].data   = working_ctx_p->data_write_p + (ROZOFS_BSIZE-foffset) ;
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].first_block_idx    = 1;            
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].number_of_blocks   = (wr_proj_buf_p[ROZOFS_WR_MIDDLE].len+ROZOFS_BSIZE-1)/ROZOFS_BSIZE;  
	wr_proj_buf_p[ROZOFS_WR_MIDDLE].last_block_size    = loffset;
        *nb_blocks_p += wr_proj_buf_p[ROZOFS_WR_MIDDLE].number_of_blocks;   
	return;       		 				 
    }
    
    /*
    ** The 1rst need no padding
    */
    wr_proj_buf_p[ROZOFS_WR_FIRST].state = ROZOFS_WR_ST_TRANSFORM_REQ;
    wr_proj_buf_p[ROZOFS_WR_FIRST].off   = first * ROZOFS_BSIZE;            
    wr_proj_buf_p[ROZOFS_WR_FIRST].len   = ((last - first) + 1)*ROZOFS_BSIZE;                  
    wr_proj_buf_p[ROZOFS_WR_FIRST].data  = working_ctx_p->data_write_p;
    wr_proj_buf_p[ROZOFS_WR_FIRST].first_block_idx    = 0;            
    wr_proj_buf_p[ROZOFS_WR_FIRST].number_of_blocks   = ((last - first) + 1); 
    wr_proj_buf_p[ROZOFS_WR_FIRST].last_block_size    = loffset;               
    *nb_blocks_p += wr_proj_buf_p[ROZOFS_WR_FIRST].number_of_blocks;
}

/** 
 *  The purpose of that function is to split the input buffer (data write part) in sections that are
    a multiple of ROZOFS_BSIZE.
    Since offset (off) might not start of a ROZOFS_BSIZE boundary in might be required to read the
    first block for making adjustement 
    In the off+len does not ends on a ROZOFS_BSIZE  boundary, it is needed to read the last block if
    off+len is less that file_size.
    All these operations are required because the transform applies on fixed buffer size (ROZOFS_BSIZE)
    
 * 
 * @param *working_ctx_p: pointer to the root transaction context 
 * @param off: offset to write from
 * @param len: length to write
 * @param bid_p: pointer  to the array when the function returns the index of the first block to write
 * @param nb_blocks_p: pointer  to the array when the function returns the number of blocks to write
 *
 * @return: none
 */
void rozofs_storcli_prepare2write(rozofs_storcli_ctx_t *working_ctx_p, 
                                  uint64_t off, 
                                  uint32_t len,
                                  uint64_t *bid_p,
                                  uint32_t *nb_blocks_p) {
    int64_t length = -1;
    uint64_t first = 0;
    uint64_t last = 0;
    int fread = 0;
    int lread = 0;
    uint16_t foffset = 0;
    uint16_t loffset = 0;
    int i;
    
   *nb_blocks_p = 0;
   //working_ctx_p->last_block_size = 0;       
    
   rozofs_storcli_ingress_write_buf_t  *wr_proj_buf_p = working_ctx_p->wr_proj_buf;

    for (i = 0; i < ROZOFS_WR_MAX; i++)
    {
      wr_proj_buf_p[i].len = 0;
      wr_proj_buf_p[i].read_buf = NULL;
      wr_proj_buf_p[i].data     = NULL;
      wr_proj_buf_p[i].last_block_size = ROZOFS_BSIZE;
    }    
    length = len;
    /*
    **  Nb. of the first block to write
    */
    first = off / ROZOFS_BSIZE;
    /*
    ** store the index of the fisrt block to write
    */
    *bid_p = first;
    /*
    ** Offset (in bytes) for the first block
    */
    foffset = off % ROZOFS_BSIZE;    
    /* 
    ** Nb. of the last block to write
    */
    last = (off + length) / ROZOFS_BSIZE + ((off + length) % ROZOFS_BSIZE == 0 ? -1 : 0);
    /*
    **  Offset (in bytes) for the last block
    */
    loffset = (off + length) - last * ROZOFS_BSIZE;
    /*
    ** Is it neccesary to read the first block ?
    */
//    if (first <= (file_size / ROZOFS_BSIZE) && foffset != 0)
    if (foffset != 0)
        fread = 1;
    /*
    ** Is it necesary to read the last block ?
    */
    if (loffset != ROZOFS_BSIZE)
//    if (last < (file_size / ROZOFS_BSIZE) && loffset != ROZOFS_BSIZE)
        lread = 1;

    /*
    **  it is not possible to know the last_block_size. So by default we set ROZOFS_BSIZE
    ** because the transform will be done on a MIDDLE buffer that has full projection size.
    */
//    working_ctx_p->last_block_size = ROZOFS_BSIZE;
    /*
    **  If we must write only one block
    */
    if (first == last) 
    {
        /*
        ** Reading block if necessary
        */
        if (fread == 1 || lread == 1) 
        {
            wr_proj_buf_p[ROZOFS_WR_FIRST].state = ROZOFS_WR_ST_RD_REQ;
            wr_proj_buf_p[ROZOFS_WR_FIRST].off   = first * ROZOFS_BSIZE;            
            wr_proj_buf_p[ROZOFS_WR_FIRST].len   = ROZOFS_BSIZE;
            wr_proj_buf_p[ROZOFS_WR_FIRST].first_block_idx    = 0;            
            wr_proj_buf_p[ROZOFS_WR_FIRST].number_of_blocks   = 1;  
            *nb_blocks_p += 1;          
            wr_proj_buf_p[ROZOFS_WR_FIRST].last_block_size = ROZOFS_BSIZE; 
            return;            
        } 
        /**
        * write the full block since it is ROZOFS_BSIZE
        */
        wr_proj_buf_p[ROZOFS_WR_FIRST].state = ROZOFS_WR_ST_TRANSFORM_REQ;
        wr_proj_buf_p[ROZOFS_WR_FIRST].off   = off;            
        wr_proj_buf_p[ROZOFS_WR_FIRST].len   = len;
        wr_proj_buf_p[ROZOFS_WR_FIRST].data  = working_ctx_p->data_write_p;
        wr_proj_buf_p[ROZOFS_WR_FIRST].first_block_idx    = 0;            
        wr_proj_buf_p[ROZOFS_WR_FIRST].number_of_blocks   = 1;            
        *nb_blocks_p += 1;          
        wr_proj_buf_p[ROZOFS_WR_FIRST].last_block_size = len;
        return;              
    }
    /*
    ** Here we must write more than one block
    */
    if (fread || lread) 
    {
      if (fread == 1)
      {
        wr_proj_buf_p[ROZOFS_WR_FIRST].state = ROZOFS_WR_ST_RD_REQ;
        wr_proj_buf_p[ROZOFS_WR_FIRST].off   = first * ROZOFS_BSIZE;            
        wr_proj_buf_p[ROZOFS_WR_FIRST].len   = ROZOFS_BSIZE;
        wr_proj_buf_p[ROZOFS_WR_FIRST].first_block_idx    = 0;            
        wr_proj_buf_p[ROZOFS_WR_FIRST].number_of_blocks   = 1;  
        *nb_blocks_p += 1;          
                        
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].state = ROZOFS_WR_ST_TRANSFORM_REQ;
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].off   = (first+1) * ROZOFS_BSIZE;            
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].len   = ((len-(ROZOFS_BSIZE-foffset))/ROZOFS_BSIZE)*ROZOFS_BSIZE;            
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].data   = working_ctx_p->data_write_p + (ROZOFS_BSIZE-foffset) ;
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].first_block_idx    = 1;            
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].number_of_blocks   = wr_proj_buf_p[ROZOFS_WR_MIDDLE].len/ROZOFS_BSIZE;  
        *nb_blocks_p += wr_proj_buf_p[ROZOFS_WR_MIDDLE].number_of_blocks;          
      }
      else
      {
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].state = ROZOFS_WR_ST_TRANSFORM_REQ;
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].off   = first * ROZOFS_BSIZE;            
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].len   = (len/ROZOFS_BSIZE)*ROZOFS_BSIZE;                  
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].data   = working_ctx_p->data_write_p;
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].first_block_idx    = 0;            
        wr_proj_buf_p[ROZOFS_WR_MIDDLE].number_of_blocks   = wr_proj_buf_p[ROZOFS_WR_MIDDLE].len/ROZOFS_BSIZE;  
        *nb_blocks_p += wr_proj_buf_p[ROZOFS_WR_MIDDLE].number_of_blocks;          
      }
      if (lread == 1)
      {
        wr_proj_buf_p[ROZOFS_WR_LAST].state = ROZOFS_WR_ST_RD_REQ;
        wr_proj_buf_p[ROZOFS_WR_LAST].off   = last * ROZOFS_BSIZE;            
        wr_proj_buf_p[ROZOFS_WR_LAST].len   = ROZOFS_BSIZE;
        wr_proj_buf_p[ROZOFS_WR_LAST].first_block_idx    = (last-first);            
        wr_proj_buf_p[ROZOFS_WR_LAST].number_of_blocks   = 1;  
        *nb_blocks_p += wr_proj_buf_p[ROZOFS_WR_LAST].number_of_blocks;

      }
      return;
    }
    /*
    ** all is aligned on ROZOFS_BSIZE
    */
    wr_proj_buf_p[ROZOFS_WR_FIRST].state = ROZOFS_WR_ST_TRANSFORM_REQ;
    wr_proj_buf_p[ROZOFS_WR_FIRST].off   = first * ROZOFS_BSIZE;            
    wr_proj_buf_p[ROZOFS_WR_FIRST].len   = ((last - first) + 1)*ROZOFS_BSIZE;                  
    wr_proj_buf_p[ROZOFS_WR_FIRST].data  = working_ctx_p->data_write_p;
    wr_proj_buf_p[ROZOFS_WR_FIRST].first_block_idx    = 0;            
    wr_proj_buf_p[ROZOFS_WR_FIRST].number_of_blocks   = ((last - first) + 1);            
    *nb_blocks_p += wr_proj_buf_p[ROZOFS_WR_FIRST].number_of_blocks;

}


/*
**__________________________________________________________________________
*/

/*
**__________________________________________________________________________
*/
/**
  Prepare to execute a write request: 
   that function is called either directly from from rozofs_storcli_write_req_init() if
   there is request with the same fid that is currently processed,
   or at the end of the processing of a request with the same fid (from rozofs_storcli_release_context()).
    
 @param working_ctx_p: pointer to the root context associated with the top level write request

 
   @retval : none
*/

void rozofs_storcli_write_req_processing_exec(rozofs_storcli_ctx_t *working_ctx_p)
{
   /*
   ** OK now check if we need to read some part, perform a transform...
   */
    rozofs_storcli_ingress_write_buf_t  *wr_proj_buf_p = working_ctx_p->wr_proj_buf;
    storcli_write_arg_no_data_t *storcli_write_rq_p = &working_ctx_p->storcli_write_arg;
    uint8_t layout = storcli_write_rq_p->layout;
    int i;
    int errcode=0;
    int ret;
    int read_req = 0;

    /*
    ** need to lock to avoid the sending a a direct reply error on internal reading
    */
    working_ctx_p->write_ctx_lock = 1;
    
    for (i = 0; i < ROZOFS_WR_MAX; i++)
    {
      if (wr_proj_buf_p[i].state == ROZOFS_WR_ST_RD_REQ)
      {
        read_req = 1;
        ret = rozofs_storcli_internal_read_req(working_ctx_p,&wr_proj_buf_p[i]);
         if (ret < 0)
         {
           working_ctx_p->write_ctx_lock = 0;
           errcode = errno;
           severe("fatal error on internal read");
           goto failure;        
         }
      } 
   }
   working_ctx_p->write_ctx_lock = 0;
   /*
   ** check if there is any direct error on internal reading
   */
   ret = rozofs_storcli_check_all_forward_transform_done(wr_proj_buf_p);
   if (ret < 0)
   {
      /*
      ** direct error while attempting to read
      */
      errcode = EFAULT;
      goto failure;   
   }
   /*
   ** check if there some read request pending: if it is the case
   ** do not use the thread
   */
   if ((rozofs_stcmoj_thread_write_enable) &&(read_req == 0)&& 
        (storcli_write_rq_p->len >rozofs_stcmoj_thread_len_threshold))
   {
     ret = rozofs_stcmoj_thread_intf_send(STORCLI_MOJETTE_THREAD_FWD,working_ctx_p,0);
     if (ret < 0) 
     {
        errno = EPROTO;
	goto failure;
     }
     return;   
   }
    /*
    ** Just to address the case of the buffer on which the fransform must apply
    */
    for (i = 0; i < ROZOFS_WR_MAX; i++)
    {
      if ( wr_proj_buf_p[i].state == ROZOFS_WR_ST_TRANSFORM_REQ)
      {
         STORCLI_START_KPI(storcli_kpi_transform_forward);

         ret = rozofs_storcli_transform_forward(working_ctx_p->prj_ctx,  
                                                 layout,
                                                 wr_proj_buf_p[i].first_block_idx, 
                                                 wr_proj_buf_p[i].number_of_blocks, 
                                                 working_ctx_p->timestamp,
                                                 wr_proj_buf_p[i].last_block_size,
                                                 wr_proj_buf_p[i].data);  
         wr_proj_buf_p[i].state =  ROZOFS_WR_ST_TRANSFORM_DONE; 
         STORCLI_STOP_KPI(storcli_kpi_transform_forward,0);
      }    
   }
   /*
   ** Check check if all the direct transformation is done
   */
   ret = rozofs_storcli_check_all_forward_transform_done(wr_proj_buf_p);
   if (ret == 0)
   {
      /*
      ** No, we have to wait for one or 2 read responses
      */
      return;   
   }
   /*
   ** All the transformation are finished so start sending the projection to the storages
   */
   return rozofs_storcli_write_req_processing(working_ctx_p);

    /*
    **_____________________________________________
    **  Exception cases
    **_____________________________________________
    */      
       
    /*
    ** there was a failure while attempting to allocate a memory ressource.
    */
failure:
     /*
     ** send back the response with the appropriated error code. 
     */
     rozofs_storcli_write_reply_error(working_ctx_p,errcode);
     STORCLI_STOP_NORTH_PROF(working_ctx_p,write,0);
     rozofs_storcli_release_context(working_ctx_p);
     return;
}



/*
**__________________________________________________________________________
*/
/**
  Initial write request


    
  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)
  @param rozofs_storcli_remote_rsp_cbk: callback for sending out the response
 
   @retval : TRUE-> xmit ready event expected
  @retval : FALSE-> xmit  ready event not expected
*/
void rozofs_storcli_write_req_init(uint32_t  socket_ctx_idx, void *recv_buf,rozofs_storcli_resp_pf_t rozofs_storcli_remote_rsp_cbk)
{
   rozofs_rpc_call_hdr_with_sz_t    *com_hdr_p;
   rozofs_storcli_ctx_t *working_ctx_p = NULL;
   int i;
   uint32_t  msg_len;  /* length of the rpc messsage including the header length */
   storcli_write_arg_no_data_t *storcli_write_rq_p = NULL;
   rozofs_rpc_call_hdr_t   hdr;   /* structure that contains the rpc header in host format */
   int      len;       /* effective length of application message               */
   uint8_t  *pmsg;     /* pointer to the first available byte in the application message */
   uint32_t header_len;
   XDR xdrs;
   int errcode = EINVAL;
   /*
   ** allocate a context for the duration of the write
   */
   working_ctx_p = rozofs_storcli_alloc_context();
   if (working_ctx_p == NULL)
   {
     /*
     ** that situation MUST not occur since there the same number of receive buffer and working context!!
     */
     severe("out of working read/write saved context");
     goto failure;
   }
   storcli_write_rq_p = &working_ctx_p->storcli_write_arg;
   STORCLI_START_NORTH_PROF(working_ctx_p,write,0);

   
   /*
   ** Get the full length of the message and adjust it the the length of the applicative part (RPC header+application msg)
   */
   msg_len = ruc_buf_getPayloadLen(recv_buf);
   msg_len -=sizeof(uint32_t);

   /*
   ** save the reference of the received socket since it will be needed for sending back the
   ** response
   */
   working_ctx_p->socketRef    = socket_ctx_idx;
   working_ctx_p->user_param   = NULL;
   working_ctx_p->recv_buf     = recv_buf;
   working_ctx_p->response_cbk = rozofs_storcli_remote_rsp_cbk;
   /*
   ** Get the payload of the receive buffer and set the pointer to the array that describes the write request
   */
   com_hdr_p  = (rozofs_rpc_call_hdr_with_sz_t*) ruc_buf_getPayload(recv_buf);   
   memcpy(&hdr,&com_hdr_p->hdr,sizeof(rozofs_rpc_call_hdr_t));
   /*
   ** swap the rpc header
   */
   scv_call_hdr_ntoh(&hdr);
   pmsg = rozofs_storcli_set_ptr_on_nfs_call_msg((char*)&com_hdr_p->hdr,&header_len);
   if (pmsg == NULL)
   {
     errcode = EFAULT;
     goto failure;
   }
   /*
   ** map the memory on the first applicative RPC byte available and prepare to decode:
   ** notice that we will not call XDR_FREE since the application MUST
   ** provide a pointer for storing the file handle
   */
   len = msg_len - header_len;    
   xdrmem_create(&xdrs,(char*)pmsg,len,XDR_DECODE); 
   /*
   ** store the source transaction id needed for the reply
   */
   working_ctx_p->src_transaction_id =  hdr.hdr.xid;
   /*
   ** decode the RPC message of the read request
   */
   if (xdr_storcli_write_arg_no_data_t(&xdrs,storcli_write_rq_p) == FALSE)
   {
      /*
      ** decoding error
      */
      errcode = EFAULT;
      severe("rpc read request decoding error");
      goto failure;
      
   }   
   /*
   ** set the pointer to the first valid data
   ** this depend on the presence of the shared memory or not.
   ** when the write is perfomed by using the shared memory, the upper bit of
   ** the length to write is asserted, the lower part of the length indicates the reference
   ** of the buffer in the shared memory
   */
   if (storcli_write_rq_p->len & 0x80000000)
   {
     if (storcli_rozofsmount_shared_mem.active == 0)
     {
       /*
       ** the shared memory is not active , so the write cannot take place
       ** trigger a fatal to restart the storcli
       */
       fatal("Shared memory is not configured");
     }
       /*
       ** set data_write_p to point to the shared memory
       */
       uint8_t *pbase = (uint8_t*)storcli_rozofsmount_shared_mem.data_p;
       int shared_mem_idx = (storcli_write_rq_p->len & 0xff);
       uint32_t buf_offset = shared_mem_idx*storcli_rozofsmount_shared_mem.buf_sz;
       uint32_t *pbuffer = (uint32_t*) (pbase + buf_offset);
       /*
       ** restore the length of the data to write in the write interface
       */
       storcli_write_rq_p->len = pbuffer[1];
       working_ctx_p->data_write_p  = (char*)&pbuffer[2]; 
       /*
       ** store the pointer to the beginning of the shared memory
       **  needed to control the timestamp just before sending the 
       ** data after the Mojette transform took place
       */
       working_ctx_p->shared_mem_p = pbuffer;
   }
   else
   {
     /*
     **  case of a write without using the shared memory
     */
     working_ctx_p->data_write_p = (char*)(pmsg+xdr_getpos(&xdrs));
   }
   /*
   ** init of the load balancing group/ projection association table:
   ** That table is ordered: the first corresponds to the storage associated with projection 0, second with 1, etc..
   ** When build that table, we MUST consider the value of the base which is associated with the distribution
   */

   
   uint8_t   rozofs_safe = rozofs_get_rozofs_safe(storcli_write_rq_p->layout);
   int lbg_in_distribution = 0;
   for (i = 0; i  <rozofs_safe ; i ++)
   {
    /*
    ** Get the load balancing group associated with the sid
    */
    int lbg_id = rozofs_storcli_get_lbg_for_sid(storcli_write_rq_p->cid,storcli_write_rq_p->dist_set[i]);
    if (lbg_id < 0)
    {
      /*
      ** there is no associated between the sid and the lbg. It is typically the case
      ** when a new cluster has been added to the configuration and the client does not
      ** know yet the configuration change
      */
      severe("sid is unknown !! %d\n",storcli_write_rq_p->dist_set[i]);
      continue;    
    }
     rozofs_storcli_lbg_prj_insert_lbg_and_sid(working_ctx_p->lbg_assoc_tb,lbg_in_distribution,
                                                lbg_id,
                                                storcli_write_rq_p->dist_set[i]);  

     rozofs_storcli_lbg_prj_insert_lbg_state(working_ctx_p->lbg_assoc_tb,
                                             lbg_in_distribution,
                                             NORTH_LBG_GET_STATE(working_ctx_p->lbg_assoc_tb[lbg_in_distribution].lbg_id));    
     lbg_in_distribution++;
     if (lbg_in_distribution == rozofs_safe) break;

   }
   /*
   ** allocate a small buffer that will be used for sending the response to the write request
   */
   working_ctx_p->xmitBuf = ruc_buf_getBuffer(ROZOFS_STORCLI_NORTH_SMALL_POOL);
   if (working_ctx_p == NULL)
   {
     /*
     ** that situation MUST not occur since there the same number of receive buffer and working context!!
     */
     errcode = ENOMEM;
     severe("out of small buffer");
     goto failure;
   }
   /*
   ** allocate a sequence number for the working context (same aas for read)
   */
   working_ctx_p->read_seqnum = rozofs_storcli_allocate_read_seqnum();
   /*
   ** set now the working variable specific for handling the write
   ** We need one large buffer per projection that will be written on storage
   */
   uint8_t forward_projection = rozofs_get_rozofs_forward(storcli_write_rq_p->layout);
   for (i = 0; i < forward_projection; i++)
   {
     working_ctx_p->prj_ctx[i].prj_state = ROZOFS_PRJ_READ_IDLE;
     working_ctx_p->prj_ctx[i].prj_buf   = ruc_buf_getBuffer(ROZOFS_STORCLI_SOUTH_LARGE_POOL);
     if (working_ctx_p->prj_ctx[i].prj_buf == NULL)
     {
       /*
       ** that situation MUST not occur since there the same number of receive buffer and working context!!
       */
       errcode = ENOMEM;
       severe("out of large buffer");
       goto failure;
     }
     /*
     ** increment inuse counter on each buffer since we might need to re-use that packet in case
     ** of retransmission
     */
     working_ctx_p->prj_ctx[i].inuse_valid = 1;
     ruc_buf_inuse_increment(working_ctx_p->prj_ctx[i].prj_buf);
     /*
     ** set the pointer to the bins
     */
     int position = rozofs_storcli_get_position_of_first_byte2write();
     uint8_t *pbuf = (uint8_t*)ruc_buf_getPayload(working_ctx_p->prj_ctx[i].prj_buf); 

     working_ctx_p->prj_ctx[i].bins       = (bin_t*)(pbuf+position);   
   }
   /*
   ** OK, now split the input buffer to figure out if we need either to read the first and/or last block
   ** That situation occurs when the data to write does not start on a ROZOFS_BSIZE boundary (first) or
   ** does not end of a ROZOFS_BSIZE boundary (last)
   */
   rozofs_storcli_prepare2write(working_ctx_p, 
                              storcli_write_rq_p->off , 
                              storcli_write_rq_p->len,
                              &working_ctx_p->wr_bid,
                              &working_ctx_p->wr_nb_blocks
                              );				
   /*
   ** Prepare for request serialization
   */
   memcpy(working_ctx_p->fid_key, storcli_write_rq_p->fid, sizeof (sp_uuid_t));
   working_ctx_p->opcode_key = STORCLI_WRITE;
   {
       int ret;
       uint64_t wr_bid;
       uint64_t wr_nb_blocks;
       if (storcli_write_rq_p->empty_file == 0)
       {
	 wr_bid = working_ctx_p->wr_bid;
	 wr_nb_blocks = working_ctx_p->wr_nb_blocks;
       }
       else
       {
	 wr_bid = 0;
	 wr_nb_blocks= 0;
	 wr_nb_blocks--;
       }
       ret = stc_rng_insert((void*)working_ctx_p,
                             STORCLI_WRITE,working_ctx_p->fid_key,
			     wr_bid,wr_nb_blocks,
			     &working_ctx_p->sched_idx);
       if (ret == 0)
       {
           /*
            ** there is a current request that is processed with the same fid and there is a collision
            */
           return;
       }
     /*
     ** no request pending with that fid, so we can process it right away
     */
     return rozofs_storcli_write_req_processing_exec(working_ctx_p);
   }

    /*
    **_____________________________________________
    **  Exception cases
    **_____________________________________________
    */      
       

    /*
    ** there was a failure while attempting to allocate a memory ressource.
    */
failure:
     /*
     ** send back the response with the appropriated error code. 
     ** note: The received buffer (rev_buf)  is
     ** intended to be released by this service in case of error or the TCP transmitter
     ** once it has been passed to the TCP stack.
     */
     rozofs_storcli_reply_error_with_recv_buf(socket_ctx_idx,recv_buf,NULL,rozofs_storcli_remote_rsp_cbk,errcode);
     /*
     ** check if the root context was allocated. Free it if is exist
     */
     if (working_ctx_p != NULL) 
     {
        /*
        ** remove the reference to the recvbuf to avoid releasing it twice
        */
       STORCLI_STOP_NORTH_PROF(working_ctx_p,write,0);
       working_ctx_p->recv_buf   = NULL;
       rozofs_storcli_release_context(working_ctx_p);
     }
     return;
}

/*
**__________________________________________________________________________
*/
/*
** That function is called when all the projection are ready to be sent

 @param working_ctx_p: pointer to the root context associated with the top level write request

*/
void rozofs_storcli_write_req_processing(rozofs_storcli_ctx_t *working_ctx_p)
{

  storcli_write_arg_no_data_t *storcli_write_rq_p = (storcli_write_arg_no_data_t*)&working_ctx_p->storcli_write_arg;
  uint8_t layout = storcli_write_rq_p->layout;
  uint8_t   rozofs_forward;
  uint8_t   rozofs_safe;
  uint8_t   projection_id;
  int       storage_idx;
  int       error=0;
  rozofs_storcli_lbg_prj_assoc_t  *lbg_assoc_p = working_ctx_p->lbg_assoc_tb;
  rozofs_storcli_projection_ctx_t *prj_cxt_p   = working_ctx_p->prj_ctx;   
  
  rozofs_forward = rozofs_get_rozofs_forward(layout);
  rozofs_safe    = rozofs_get_rozofs_safe(layout);
  
  /*
  ** check if the buffer is still valid: we might face the situation where the rozofsmount
  ** time-out and re-allocate the write buffer located in shared memory for another
  ** transaction (either read or write:
  ** the control must take place only where here is the presence of a shared memory for the write
  */
  if (working_ctx_p->shared_mem_p!= NULL)
  {
      uint32_t *xid_p = (uint32_t*)working_ctx_p->shared_mem_p;
      if (*xid_p !=  working_ctx_p->src_transaction_id)
      {
        /*
        ** the source has aborted the request
        */
        error = EPROTO;
        goto fail;
      }      
  }   
  /*
  ** set the current state of each load balancing group belonging to the rozofs_safe group
  */
  for (storage_idx = 0; storage_idx < rozofs_safe; storage_idx++) 
  {
    /*
    ** Check the state of the load Balancing group
    */
    rozofs_storcli_lbg_prj_insert_lbg_state(lbg_assoc_p,
                                            storage_idx,
                                            NORTH_LBG_GET_STATE(lbg_assoc_p[storage_idx].lbg_id));      
  }
  /*
  ** Now find out a selectable lbg_id for each projection
  */
  for (projection_id = 0; projection_id < rozofs_forward; projection_id++)
  {
    if (rozofs_storcli_select_storage_idx_for_write ( working_ctx_p,rozofs_forward, rozofs_safe,projection_id) < 0)
    {
       /*
       ** there is no enough valid storage !!
       */
       error = EIO;
       goto fail;
    }
  }  
  /*
  ** We have enough storage, so initiate the transaction towards the storage for each
  ** projection
  */
  for (projection_id = 0; projection_id < rozofs_forward; projection_id++)
  {
     sp_write_arg_no_bins_t *request; 
     sp_write_arg_no_bins_t  write_prj_args;
     void  *xmit_buf;  
     int ret;  
      
     xmit_buf = prj_cxt_p[projection_id].prj_buf;
     if (xmit_buf == NULL)
     {
       /*
       ** fatal error since the ressource control already took place
       */       
       error = EIO;
       goto fatal;     
     }
     /*
     ** fill partially the common header
     */
retry:
     request   = &write_prj_args;
     request->cid = storcli_write_rq_p->cid;
     request->sid = (uint8_t) rozofs_storcli_lbg_prj_get_sid(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);
     request->layout        = storcli_write_rq_p->layout;
     if (prj_cxt_p[projection_id].stor_idx >= rozofs_forward) request->spare = 1;
//     if (projection_id >= rozofs_forward) request->spare = 1;
     else request->spare = 0;
     memcpy(request->dist_set, storcli_write_rq_p->dist_set, ROZOFS_SAFE_MAX*sizeof (uint8_t));
     memcpy(request->fid, storcli_write_rq_p->fid, sizeof (sp_uuid_t));
     request->proj_id = projection_id;
     request->bid     = working_ctx_p->wr_bid;
     request->nb_proj = working_ctx_p->wr_nb_blocks;     
     /*
     ** set the length of the bins part.
     */
     int bins_len = ((rozofs_get_max_psize(layout)+(sizeof(rozofs_stor_bins_hdr_t)/sizeof(bin_t)))* request->nb_proj)*sizeof(bin_t);
     request->len = bins_len; /**< bins length MUST be in bytes !!! */
     uint32_t  lbg_id = rozofs_storcli_lbg_prj_get_lbg(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);
     STORCLI_START_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),write_prj,bins_len);
     /*
     ** caution we might have a direct reply if there is a direct error at load balancing group while
     ** ateempting to send the RPC message-> typically a disconnection of the TCP connection 
     ** As a consequence the response fct 'rozofs_storcli_write_req_processing_cbk) can be called
     ** prior returning from rozofs_sorcli_send_rq_common')
     ** anticipate the status of the xmit state of the projection and lock the section to
     ** avoid a reply error before returning from rozofs_sorcli_send_rq_common() 
     ** --> need to take care because the write context is released after the reply error sent to rozofsmount
     */
     working_ctx_p->write_ctx_lock = 1;
     prj_cxt_p[projection_id].prj_state = ROZOFS_PRJ_WR_IN_PRG;
     
     ret =  rozofs_sorcli_send_rq_common(lbg_id,ROZOFS_TMR_GET(TMR_STORAGE_PROGRAM),STORAGE_PROGRAM,STORAGE_VERSION,SP_WRITE,
                                         (xdrproc_t) xdr_sp_write_arg_no_bins_t, (caddr_t) request,
                                          xmit_buf,
                                          working_ctx_p->read_seqnum,
                                          (uint32_t) projection_id,
                                          bins_len,
                                          rozofs_storcli_write_req_processing_cbk,
                                         (void*)working_ctx_p);

     working_ctx_p->write_ctx_lock = 0;
     if (ret < 0)
     {
       /*
       ** the communication with the storage seems to be wrong (more than TCP connection temporary down
       ** attempt to select a new storage
       **
       */
       if (rozofs_storcli_select_storage_idx_for_write (working_ctx_p,rozofs_forward,rozofs_safe,projection_id) < 0)
       {
         /*
         ** Out of storage !!-> too many storages are down
         */
         goto fatal;
       } 
       /*
       ** retry for that projection with a new storage index: WARNING: we assume that xmit buffer has not been released !!!
       */
//#warning: it is assumed that xmit buffer has not been release, need to double check!!        
       goto retry;
     } 
     else
     {
       /*
       ** check if the state has not been changed: -> it might be possible to get a direct error
       */
       if (prj_cxt_p[projection_id].prj_state == ROZOFS_PRJ_WR_ERROR)
       {
          error = prj_cxt_p[projection_id].errcode;
          goto fatal;       
       }
     }

   }

  return;
  
fail:
     /*
     ** we fall in that case when we run out of  resource-> that case is a BUG !!
     */
     rozofs_storcli_write_reply_error(working_ctx_p,error);
     /*
     ** release the root transaction context
     */
     STORCLI_STOP_NORTH_PROF(working_ctx_p,write,0);
     rozofs_storcli_release_context(working_ctx_p);  
     return;

fatal:
     /*
     ** we fall in that case when we run out of  resource-> that case is a BUG !!
     */
     rozofs_storcli_write_reply_error(working_ctx_p,error);
     /*
     ** release the root transaction context
     */
     STORCLI_STOP_NORTH_PROF(working_ctx_p,write,0);
     rozofs_storcli_release_context(working_ctx_p);  

  return;

}


/*
**__________________________________________________________________________
*/
/**
* Projection read retry: that procedure is called upon the reading failure
  of one projection. The system attempts to read in sequence the next available
  projection if any. 
  The index of the next projection to read is given by redundancyStorageIdxCur
  
  @param  working_ctx_p : pointer to the root transaction context
  @param  projection_id : index of the projection
  @param same_storage_retry_acceptable : assert to 1 if retry on the same storage is acceptable
  
  @retval >= 0 : success, it indicates the reference of the projection id
  @retval< < 0 error
*/

void rozofs_storcli_write_projection_retry(rozofs_storcli_ctx_t *working_ctx_p,uint8_t projection_id,int same_storage_retry_acceptable)
{
    uint8_t   rozofs_safe;
    uint8_t   rozofs_forward;
    uint8_t   layout;
    storcli_write_arg_no_data_t *storcli_write_rq_p = (storcli_write_arg_no_data_t*)&working_ctx_p->storcli_write_arg;
    int error=0;
    int storage_idx;

    rozofs_storcli_projection_ctx_t *prj_cxt_p   = working_ctx_p->prj_ctx;   
    rozofs_storcli_lbg_prj_assoc_t  *lbg_assoc_p = working_ctx_p->lbg_assoc_tb;

    layout         = storcli_write_rq_p->layout;
    rozofs_safe    = rozofs_get_rozofs_safe(layout);
    rozofs_forward = rozofs_get_rozofs_forward(layout);
    /*
    ** Now update the state of each load balancing group since it might be possible
    ** that some experience a state change
    */
    for (storage_idx = 0; storage_idx < rozofs_safe; storage_idx++) 
    {
      /*
      ** Check the state of the load Balancing group
      */
      rozofs_storcli_lbg_prj_insert_lbg_state(lbg_assoc_p,
                                              storage_idx,
                                              NORTH_LBG_GET_STATE(lbg_assoc_p[storage_idx].lbg_id));      
    }    
    /**
    * attempt to select a new storage
    */
    if (rozofs_storcli_select_storage_idx_for_write (working_ctx_p,rozofs_forward,rozofs_safe,projection_id) < 0)
    {
      /*
      ** Cannot select a new storage: OK so now double check if the retry on the same storage is
      ** acceptable.When it is the case, check if the max retry has not been yet reached
      ** Otherwise, we are in deep shit-> reject the read request
      */
      if (same_storage_retry_acceptable == 0) 
      {
        error = EIO;
        prj_cxt_p[projection_id].errcode = error;
        goto reject;      
      }
      if (++prj_cxt_p[projection_id].retry_cpt >= ROZOFS_STORCLI_MAX_RETRY)
      {
        error = EIO;
        prj_cxt_p[projection_id].errcode = error;
        goto reject;          
      }
    } 
    /*
    ** we are lucky since either a get a new storage or the retry counter is not exhausted
    */
     sp_write_arg_no_bins_t *request; 
     sp_write_arg_no_bins_t  write_prj_args;
     void  *xmit_buf;  
     int ret;  
      
     xmit_buf = prj_cxt_p[projection_id].prj_buf;
     if (xmit_buf == NULL)
     {
       /*
       ** fatal error since the ressource control already took place
       */
       error = EFAULT;
       prj_cxt_p[projection_id].errcode = error;
       goto fatal;     
     }
     /*
     ** fill partially the common header
     */
retry:
     request   = &write_prj_args;
     request->cid = storcli_write_rq_p->cid;
     request->sid = (uint8_t) rozofs_storcli_lbg_prj_get_sid(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);
     request->layout        = storcli_write_rq_p->layout;
     if (prj_cxt_p[projection_id].stor_idx >= rozofs_forward) request->spare = 1;
     else request->spare = 0;
     memcpy(request->dist_set, storcli_write_rq_p->dist_set, ROZOFS_SAFE_MAX*sizeof (uint8_t));
     memcpy(request->fid, storcli_write_rq_p->fid, sizeof (sp_uuid_t));
     request->proj_id = projection_id;
     request->bid     = working_ctx_p->wr_bid;
     request->nb_proj = working_ctx_p->wr_nb_blocks;     
     /*
     ** set the length of the bins part.
     */
     int bins_len = ((rozofs_get_max_psize(layout)+(sizeof(rozofs_stor_bins_hdr_t)/sizeof(bin_t)))* request->nb_proj)*sizeof(bin_t);
     request->len = bins_len; /**< bins length MUST be in bytes !!! */
     uint32_t  lbg_id = rozofs_storcli_lbg_prj_get_lbg(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);
     /*
     **  increment the lock since it might be possible that this procedure is called after a synchronous transaction failu failure
     ** while the system is still in the initial procedure that triggers the writing of the projection. So it might be possible that
     ** the lock is already asserted
     ** as for the initial case, we need to anticipate the xmit state of the projection since the ERROR status might be set 
     ** on a synchronous transaction failure. If that state is set after a positive submission towards the lbg, we might
     ** overwrite the ERROR state with the IN_PRG state.
     */
     working_ctx_p->write_ctx_lock++;
     prj_cxt_p[projection_id].prj_state = ROZOFS_PRJ_WR_IN_PRG;
     
     STORCLI_START_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),write_prj,bins_len);
     ret =  rozofs_sorcli_send_rq_common(lbg_id,ROZOFS_TMR_GET(TMR_STORAGE_PROGRAM),STORAGE_PROGRAM,STORAGE_VERSION,SP_WRITE,
                                         (xdrproc_t) xdr_sp_write_arg_no_bins_t, (caddr_t) request,
                                          xmit_buf,
                                          working_ctx_p->read_seqnum,
                                          (uint32_t) projection_id,
                                          bins_len,
                                          rozofs_storcli_write_req_processing_cbk,
                                         (void*)working_ctx_p);
     working_ctx_p->write_ctx_lock--;
     if (ret < 0)
     {
       /*
       ** the communication with the storage seems to be wrong (more than TCP connection temporary down
       ** attempt to select a new storage
       **
       */
       STORCLI_STOP_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),write_prj,0);
       if (rozofs_storcli_select_storage_idx_for_write (working_ctx_p,rozofs_forward,rozofs_safe,projection_id) < 0)
       {
         /*
         ** Out of storage !!-> too many storages are down
         */
         goto fatal;
       } 
       /*
       ** retry for that projection with a new storage index: WARNING: we assume that xmit buffer has not been released !!!
       */
//#warning: it is assumed that xmit buffer has not been release, need to double check!!        
       goto retry;
     }
     /*
     ** OK, the buffer has been accepted by the load balancing group, check if there was a direct failure for
     ** that transaction
     */
     if ( prj_cxt_p[projection_id].prj_state == ROZOFS_PRJ_WR_ERROR)
     {
        error = prj_cxt_p[projection_id].errcode;
        goto fatal;     
     }    
    return;
    /*
    **_____________________________________________
    **  Exception cases
    **_____________________________________________
    */      
    
reject:  
     if (working_ctx_p->write_ctx_lock != 0) return;
     /*
     ** we fall in that case when we run out of  storage
     */
     rozofs_storcli_write_reply_error(working_ctx_p,error);
     /*
     ** release the root transaction context
     */
    STORCLI_STOP_NORTH_PROF(working_ctx_p,write,0);
    rozofs_storcli_release_context(working_ctx_p);  
     return; 
      
fatal:
     /*
     ** caution -> reply error is only generated if the ctx_lock is 0
     */
     if (working_ctx_p->write_ctx_lock != 0) return;
     /*
     ** we fall in that case when we run out of  resource-> that case is a BUG !!
     */
     rozofs_storcli_write_reply_error(working_ctx_p,error);
     /*
     ** release the root transaction context
     */
     STORCLI_STOP_NORTH_PROF(working_ctx_p,write,0);
     rozofs_storcli_release_context(working_ctx_p);  
     return; 

}

/*
**__________________________________________________________________________
*/
/**
*  Call back function call upon a success rpc, timeout or any other rpc failure on a projection write request
*
 @param this : pointer to the transaction context
 @param param: pointer to the associated rozofs_fuse_context
 
 @return none
 */

void rozofs_storcli_write_req_processing_cbk(void *this,void *param) 
{
   uint32_t   seqnum;
   uint32_t   projection_id;
   rozofs_storcli_projection_ctx_t  *write_prj_work_p = NULL;   
   rozofs_storcli_ctx_t *working_ctx_p = (rozofs_storcli_ctx_t*) param ;
   XDR       xdrs;       
   uint8_t  *payload;
   int      bufsize;
   sp_status_ret_t   rozofs_status;
   struct rpc_msg  rpc_reply;
   storcli_write_arg_no_data_t *storcli_write_rq_p = NULL;
   rpc_reply.acpted_rply.ar_results.proc = NULL;
   int lbg_id;

   
   int status;
   void     *recv_buf = NULL;   
   int      ret;
   int error = 0;
   int      same_storage_retry_acceptable = 0;

    storcli_write_rq_p = (storcli_write_arg_no_data_t*)&working_ctx_p->storcli_write_arg;
    /*
    ** get the sequence number and the reference of the projection id form the opaque user array
    ** of the transaction context
    */
    rozofs_tx_read_opaque_data(this,0,&seqnum);
    rozofs_tx_read_opaque_data(this,1,&projection_id);
    rozofs_tx_read_opaque_data(this,2,(uint32_t*)&lbg_id);
    /*
    ** check if the sequence number of the transaction matches with the one saved in the tranaaction
    ** that control is required because we can receive a response from a late transaction that
    ** it now out of sequence since the system is waiting for transaction response on a next
    ** set of distribution
    ** In that case, we just drop silently the received message
    */
    if (seqnum != working_ctx_p->read_seqnum)
    {
      /*
      ** not the right sequence number, so drop the received message
      */
      goto drop_msg;    
    }
    /*
    ** check if the write is already doen: this might happen in the case when the same projection
    ** is sent twoards 2 different LBG
    */    
    if (working_ctx_p->prj_ctx[projection_id].prj_state == ROZOFS_PRJ_WR_DONE)
    {
      /*
      ** The reponse has already been received for that projection so we don't care about that
      ** extra reponse
      */
      goto drop_msg;       
    }
    /*    
    ** get the status of the transaction -> 0 OK, -1 error (need to get errno for source cause
    */
    status = rozofs_tx_get_status(this);
    if (status < 0)
    {

       /*
       ** something wrong happened: assert the status in the associated projection id sub-context
       ** now, double check if it is possible to retry on a new storage
       */
       working_ctx_p->prj_ctx[projection_id].prj_state = ROZOFS_PRJ_WR_ERROR;
       working_ctx_p->prj_ctx[projection_id].errcode   = rozofs_tx_get_errno(this);
       errno = rozofs_tx_get_errno(this);  
       if (errno == ETIME)
       {
         storcli_lbg_cnx_sup_increment_tmo(lbg_id);
         STORCLI_ERR_PROF(write_prj_tmo);
       }
       else
       {
         STORCLI_ERR_PROF(write_prj_err);
       }       
       same_storage_retry_acceptable = 1;
       goto retry_attempt; 
    }
    storcli_lbg_cnx_sup_clear_tmo(lbg_id);
    /*
    ** get the pointer to the receive buffer payload
    */
    recv_buf = rozofs_tx_get_recvBuf(this);
    if (recv_buf == NULL)
    {
       /*
       ** something wrong happened
       */
       error = EFAULT;  
       working_ctx_p->prj_ctx[projection_id].prj_state = ROZOFS_PRJ_WR_ERROR;
       working_ctx_p->prj_ctx[projection_id].errcode = error;
       goto fatal;         
    }
    /*
    ** set the useful pointer on the received message
    */
    payload  = (uint8_t*) ruc_buf_getPayload(recv_buf);
    payload += sizeof(uint32_t); /* skip length*/
    /*
    ** OK now decode the received message
    */
    bufsize = ruc_buf_getPayloadLen(recv_buf);
    bufsize -= sizeof(uint32_t); /* skip length*/
    xdrmem_create(&xdrs,(char*)payload,bufsize,XDR_DECODE);
    while (1)
    {
      /*
      ** decode the rpc part
      */
      if (rozofs_xdr_replymsg(&xdrs,&rpc_reply) != TRUE)
      {
       TX_STATS(ROZOFS_TX_DECODING_ERROR);
       errno = EPROTO;
        error = 1;
        break;
      }
      /*
      ** decode the status of the operation
      */
      if (xdr_sp_status_ret_t(&xdrs,&rozofs_status)!= TRUE)
      {
        errno = EPROTO;
        error = 1;
        break;    
      }
      /*
      ** check th estatus of the operation
      */
      if ( rozofs_status.status != SP_SUCCESS )
      {
         errno = rozofs_status.sp_status_ret_t_u.error;
         error = 1;
        break;    
      }
      break;
    }
    /*
    ** check the status of the operation
    */
    if (error)
    {
       /*
       ** there was an error on the remote storage while attempt to write the file
       ** try to write the projection on another storaged
       */
       working_ctx_p->prj_ctx[projection_id].prj_state = ROZOFS_PRJ_WR_ERROR;
       working_ctx_p->prj_ctx[projection_id].errcode   = errno;

       /**
       * The error has been reported by the remote, we cannot retry on the same storage
       ** we imperatively need to select a different one. So if cannot select a different storage
       ** we report a reading error.
       */
       same_storage_retry_acceptable = 0;
       goto retry_attempt;    
    }
    STORCLI_STOP_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),write_prj,0);

    /*
    ** set the pointer to the read context associated with the projection for which a response has
    ** been received
    */
    write_prj_work_p = &working_ctx_p->prj_ctx[projection_id];
    /*
    ** set the status of the transaction to done for that projection
    */
    write_prj_work_p->prj_state = ROZOFS_PRJ_WR_DONE;
    write_prj_work_p->errcode   = errno;
    /*
    ** OK now check if we have send enough projection
    ** if it is the case, the distribution will be valid
    */
    ret = rozofs_storcli_all_prj_write_check(storcli_write_rq_p->layout,
                                             working_ctx_p->prj_ctx,
                                             &working_ctx_p->wr_distribution);
    if (ret == 0)
    {
       /*
       ** no enough projection 
       */
       goto wait_more_projection;
    }
    /*
    ** write is finished, send back the response to the client (rozofsmount)
    */       
    rozofs_storcli_write_reply_success(working_ctx_p);
    /*
    ** release the root context and the transaction context
    */
    if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);       
    rozofs_storcli_release_context(working_ctx_p);    
    rozofs_tx_free_from_ptr(this);
    return;
    
    /*
    **_____________________________________________
    **  Exception cases
    **_____________________________________________
    */    
drop_msg:
    /*
    ** the message has not the right sequence number,so just drop the received message
    ** and release the transaction context
    */  
     if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);       
     rozofs_tx_free_from_ptr(this);
     return;

fatal:
    /*
    ** caution lock can be asserted either by a write retry attempt or an initial attempt
    */
    if (working_ctx_p->write_ctx_lock != 0) return;
    /*
    ** unrecoverable error : mostly a bug!!
    */  
    STORCLI_STOP_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),write_prj,0);

    if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);       
    rozofs_tx_free_from_ptr(this);
    severe("Cannot get the pointer to the receive buffer");

    rozofs_storcli_write_reply_error(working_ctx_p,error);
    /*
    ** release the root transaction context
    */
    STORCLI_STOP_NORTH_PROF(working_ctx_p,write,0);
    rozofs_storcli_release_context(working_ctx_p);  
    return;
    
retry_attempt:    
    /*
    ** There was a read errr for that projection so attempt to find out another storage
    ** but first of all release the ressources related to the current transaction
    */
    STORCLI_STOP_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),write_prj,0);

    if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);       
    rozofs_tx_free_from_ptr(this);
    /**
    * attempt to select a new storage
    */
    return rozofs_storcli_write_projection_retry(working_ctx_p,projection_id,same_storage_retry_acceptable);

        
wait_more_projection:    
    /*
    ** need to wait for some other write transaction responses
    ** 
    */
    if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);           
    rozofs_tx_free_from_ptr(this);
    return;


}



/*
**__________________________________________________________________________
*/

/**
* callback for sending a response to a read to a remote entity

 potential failure case:
  - socket_ref is out of range
  - connection is down
  
 @param buffer : pointer to the ruc_buffer that cointains the response
 @param socket_ref : non significant
 @param user_param_p : pointer to the root context
 
 
 @retval 0 : successfully submitted to the transport layer
 @retval < 0 error, the caller is intended to release the buffer
 */
 
typedef struct _rozofs_rpc_common_t
{
    uint32_t msg_sz;  /**< size of the rpc message */
    uint32_t xid;     /**< transaction identifier */
} rozofs_rpc_common_t;
 
int rozofs_storcli_internal_read_rsp_cbk(void *buffer,uint32_t socket_ref,void *user_param)
{

   int errcode = 0; 
   int ret;
   int match_idx;
   rozofs_storcli_ctx_t                *working_ctx_p = (rozofs_storcli_ctx_t*)user_param;
   rozofs_storcli_ingress_write_buf_t  *wr_proj_buf_p = working_ctx_p->wr_proj_buf;
   storcli_write_arg_no_data_t      *storcli_write_rq_p = NULL;
   
   storcli_write_rq_p = (storcli_write_arg_no_data_t*)&working_ctx_p->storcli_write_arg;
   uint8_t  layout   = storcli_write_rq_p->layout;

   XDR       xdrs;       
   uint8_t  *payload;
   int      bufsize;   
   struct rpc_msg  rpc_reply;
   storcli_status_ret_t rozofs_status;
   int  data_len; 
   int error;  
   rpc_reply.acpted_rply.ar_results.proc = NULL;

   storcli_write_rq_p = &working_ctx_p->storcli_write_arg;

   /*
   ** decode the read internal read reply
   */
   payload  = (uint8_t*) ruc_buf_getPayload(buffer);
   payload += sizeof(uint32_t); /* skip length*/  
   
   uint32_t *p32 = (uint32_t*)payload;
   uint32_t recv_xid = ntohl(*p32);
   /*
   ** OK now decode the received message
   */
   bufsize = ruc_buf_getPayloadLen(buffer);
   bufsize -= sizeof(uint32_t); /* skip length*/
   xdrmem_create(&xdrs,(char*)payload,bufsize,XDR_DECODE);   
   error = 0;
   while (1)
   {
     /*
     ** decode the rpc part
     */
     if (rozofs_xdr_replymsg(&xdrs,&rpc_reply) != TRUE)
     {
       errno = EPROTO;
       error = 1;
       break;
     }
     /*
     ** decode the status of the operation
     */
     if (xdr_storcli_status_ret_t(&xdrs,&rozofs_status)!= TRUE)
     {
       errno = EPROTO;
       error = 1;
       break;    
     }
     /*
     ** check th estatus of the operation
     */
     if ( rozofs_status.status != STORCLI_SUCCESS )
     {
       error = 0;
       break;    
     }
     {
       int alignment;
       /*
       ** skip the alignment
       */
       if (xdr_int(&xdrs, &alignment) != TRUE)
       {
         errno = EPROTO;
         STORCLI_ERR_PROF(read_prj_err);       
         error = 1;
         break;          
       }
      }
     /*
     ** Now get the length of the part that has been read
     */
     if (xdr_int(&xdrs, &data_len) != TRUE)
     {
       errno = EPROTO;
       error = 1;
       break;          
     }
     break;
   }
   if (error)
   {
     severe("error while decoding rpc reply");  
     return -1;  
   }   
   /*
   ** Extract the transaction id and find out the block that is concerned by
   ** the response, either the first or last block:
   ** check it if matches one of the 2 buffers
   */
   match_idx = -1;
   if (wr_proj_buf_p[ROZOFS_WR_FIRST].transaction_id == recv_xid) match_idx = ROZOFS_WR_FIRST;
   if (wr_proj_buf_p[ROZOFS_WR_LAST].transaction_id == recv_xid)  match_idx = ROZOFS_WR_LAST;
   if (match_idx < 0)
   {
     /*
     ** does not match with our transaction, just return, the caller will release the buffer
     */
     return -1;   
   }
   /*
   ** We have the matching index, so check the status and perform the forward transform
   ** in case of success
   */
   /*
   ** save the received buffer that must be released at the end of the write transaction
   */
   wr_proj_buf_p[match_idx].read_buf = buffer;
   /*
   ** check the status of the read operation
   */
   if (rozofs_status.status != STORCLI_SUCCESS)
   {
     errcode = rozofs_status.storcli_status_ret_t_u.error;
   
     if (errcode != ENOENT) {
       wr_proj_buf_p[match_idx].state = ROZOFS_WR_ST_ERROR;
       wr_proj_buf_p[match_idx].errcode = errcode;
       goto write_procedure_failure;
     }
     
     /*
     ** Case of the file that has never been written on disk yet.
     ** There is no data on disk to complement the partial blocks
     ** at the beginning and end of the write request.
     */
     { 
       uint32_t relative_offset ;
       uint32_t len;
       
       wr_proj_buf_p[match_idx].data = (char*)(payload);     

       /*
       ** First block
       */
       if (match_idx == ROZOFS_WR_FIRST) {
       
         /* Pad with zero the begining of the 1rst buffer */
         relative_offset =   storcli_write_rq_p->off - wr_proj_buf_p[match_idx].off;
         memset(wr_proj_buf_p[match_idx].data,0,relative_offset);

         /* Complement the buffer with the data to write... */
         if (relative_offset+storcli_write_rq_p->len >= ROZOFS_BSIZE ) 
         {
	   /*... on the whole block when enough data to write ... */
           len = ROZOFS_BSIZE - relative_offset;
           wr_proj_buf_p[match_idx].last_block_size = ROZOFS_BSIZE;
         }
         else
         {
	   /*... or on a partial block size when too few data is given. */
           len = storcli_write_rq_p->len ;  
           wr_proj_buf_p[match_idx].last_block_size = len+relative_offset;    
         }
         memcpy(wr_proj_buf_p[match_idx].data+relative_offset,working_ctx_p->data_write_p,len);
         wr_proj_buf_p[match_idx].state = ROZOFS_WR_ST_TRANSFORM_REQ;
         goto transform;     
       } 
       /*
       ** case of the last block
       ** copy the data to write in the buffer
       */
       relative_offset =  wr_proj_buf_p[match_idx].off - storcli_write_rq_p->off;
       len = storcli_write_rq_p->len - relative_offset;
       memcpy(wr_proj_buf_p[match_idx].data,working_ctx_p->data_write_p+relative_offset,len);
       wr_proj_buf_p[match_idx].state = ROZOFS_WR_ST_TRANSFORM_REQ;
       wr_proj_buf_p[match_idx].last_block_size = len;                   
       goto transform;              
     }
   }
   /*
   ** OK update the data length of the block since it might be possible that the requested data length gives
   ** a pointer that is after EOF. So in that case the reader returns the data until reaching EOF
   */

   /*
   ** successfull read, save the pointer to the data part and copy the part to the initial buffer at the 
   ** right place
   */
   int position = XDR_GETPOS(&xdrs);
   wr_proj_buf_p[match_idx].data    = (char*)(payload+position); 
   for(;;)
   {
     uint32_t relative_offset ;
     uint32_t len;
     if (match_idx == ROZOFS_WR_FIRST)
     {
 
       relative_offset =   storcli_write_rq_p->off - wr_proj_buf_p[match_idx].off;
      /*
       ** check if the length is 0, that might happen when creating hole in afile
       */
       if (data_len == 0)
       {
          /*
          ** clear the beginning of the buffer
          */
          memset(wr_proj_buf_p[match_idx].data,0,relative_offset);
       
       }
       if (relative_offset+storcli_write_rq_p->len >= ROZOFS_BSIZE ) 
       {
         len = ROZOFS_BSIZE - relative_offset;
         wr_proj_buf_p[match_idx].last_block_size = ROZOFS_BSIZE;
       }
       else
       {
         len = storcli_write_rq_p->len ;  
         if ((len+relative_offset) > data_len) wr_proj_buf_p[match_idx].last_block_size = len+relative_offset;    
         else                                  wr_proj_buf_p[match_idx].last_block_size = data_len;               
       }
       memcpy(wr_proj_buf_p[match_idx].data+relative_offset,working_ctx_p->data_write_p,len);
       wr_proj_buf_p[match_idx].state = ROZOFS_WR_ST_TRANSFORM_REQ;
       break;     
     } 
     /*
     ** case of the last block
     */

     relative_offset =  wr_proj_buf_p[match_idx].off - storcli_write_rq_p->off;
     len = storcli_write_rq_p->len - relative_offset;

     memcpy(wr_proj_buf_p[match_idx].data,working_ctx_p->data_write_p+relative_offset,len);
     wr_proj_buf_p[match_idx].state = ROZOFS_WR_ST_TRANSFORM_REQ;
     if (len > data_len) wr_proj_buf_p[match_idx].last_block_size = len;    
     else                wr_proj_buf_p[match_idx].last_block_size = data_len;                
     break;          
   }

transform:

   /*
   ** Perform the transformation on the received block
   */
   {
      rozofs_storcli_transform_forward(working_ctx_p->prj_ctx,  
                                              layout,
                                              wr_proj_buf_p[match_idx].first_block_idx, 
                                              wr_proj_buf_p[match_idx].number_of_blocks, 
                                              working_ctx_p->timestamp,
                                              wr_proj_buf_p[match_idx].last_block_size,
                                              wr_proj_buf_p[match_idx].data);  
      wr_proj_buf_p[match_idx].state =  ROZOFS_WR_ST_TRANSFORM_DONE; 
   }

   /*
   ** Check check if all the direct transformation is done
   */
   ret = rozofs_storcli_check_all_forward_transform_done(wr_proj_buf_p);
   if (ret == 0)
   {
      /*
      ** No, we have to wait for one or 2 read responses
      */
      return 0;   
   } 
   if (ret == 1)
   {
      /*
      ** all the transformation are done, proceeding with the sending of the projections
      */
      rozofs_storcli_write_req_processing(working_ctx_p);  
      return 0; 
   } 
    /*
    **_____________________________________________
    **  Exception cases
    **_____________________________________________
    */  

write_procedure_failure:
   /*
   ** check if the lock is asserted for the case of the write
   */
   if (working_ctx_p->write_ctx_lock == 1) return 0;
   /*
   ** write failure
   */
   rozofs_storcli_write_reply_error(working_ctx_p,errcode);

   /*
   ** release the transaction root context
   */
   working_ctx_p->xmitBuf = NULL;
   STORCLI_STOP_NORTH_PROF(working_ctx_p,write,0);  
   rozofs_storcli_release_context(working_ctx_p);
   return 0 ;

}


/*
**__________________________________________________________________________
*/
/**
*  Internal Read procedure
   That procedure is used when it is required to read the fisrt or/and the last block before
   perform the forward transformation on a write buffer
   
   @param working_ctx_p: pointer to the root transaction
   @param wr_proj_buf_p : pointer to the structure that describes the read request
   
   @retval 0 on success
   retval < 0 on error (see errno for error details)
   
*/
int rozofs_storcli_internal_read_req(rozofs_storcli_ctx_t *working_ctx_p,rozofs_storcli_ingress_write_buf_t  *wr_proj_buf_p)
{
   storcli_write_arg_no_data_t *storcli_write_rq_p;
   void *xmit_buf;
   storcli_read_arg_t storcli_read_args;
   storcli_read_arg_t *request   = &storcli_read_args;
   struct rpc_msg   call_msg;
   int               bufsize;
   uint32_t          *header_size_p;
   XDR               xdrs;    
   uint8_t           *arg_p;
      
   storcli_write_rq_p = (storcli_write_arg_no_data_t*)&working_ctx_p->storcli_write_arg;
   
   /*
   ** allocated a buffer from sending the request
   */   
   xmit_buf = ruc_buf_getBuffer(ROZOFS_STORCLI_NORTH_SMALL_POOL);
   if (xmit_buf == NULL)
   {
     severe(" out of small buffer on north interface ");
     wr_proj_buf_p->state = ROZOFS_WR_ST_ERROR;
     errno = ENOMEM;
     return -1;   
   }
   /*
   ** build the RPC message
   */
   request->sid = 0;  /* not significant */
   request->layout = storcli_write_rq_p->layout;
   request->cid    = storcli_write_rq_p->cid;
   request->spare = 0;  /* not significant */
   memcpy(request->dist_set, storcli_write_rq_p->dist_set, ROZOFS_SAFE_MAX*sizeof (uint8_t));
   memcpy(request->fid, storcli_write_rq_p->fid, sizeof (sp_uuid_t));
   request->proj_id = 0;  /* not significant */
   request->bid     = wr_proj_buf_p->off/ROZOFS_BSIZE;  
   request->nb_proj = wr_proj_buf_p->number_of_blocks;  
   /*
   ** save the tranaction sequence number, it will be needed to correlate with the context upon
   ** receiving the read response : it is maily used to find out either FIRST or LAST index
   ** since it might be possible to trigger 2 simultaneous read (top and botton of buffer)
   */
   wr_proj_buf_p->transaction_id = rozofs_tx_get_transaction_id();    
   /*
   ** get the pointer to the payload of the buffer
   */
   header_size_p  = (uint32_t*) ruc_buf_getPayload(xmit_buf);
   arg_p = (uint8_t*)(header_size_p+1);  
   /*
   ** create the xdr_mem structure for encoding the message
   */
   bufsize = (int)ruc_buf_getMaxPayloadLen(xmit_buf);
   xdrmem_create(&xdrs,(char*)arg_p,bufsize,XDR_ENCODE);
   /*
   ** fill in the rpc header
   */
   call_msg.rm_direction = CALL;
   /*
   ** allocate a xid for the transaction 
   */
   call_msg.rm_xid             = wr_proj_buf_p->transaction_id; 
   call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
   /* XXX: prog and vers have been long historically :-( */
   call_msg.rm_call.cb_prog = (uint32_t)STORCLI_PROGRAM;
   call_msg.rm_call.cb_vers = (uint32_t)STORCLI_VERSION;
   if (! xdr_callhdr(&xdrs, &call_msg))
   {
      /*
      ** THIS MUST NOT HAPPEN
      */
     ruc_buf_freeBuffer(xmit_buf); 
     errno = EFAULT;
     severe(" rpc header encode error ");
     wr_proj_buf_p->state = ROZOFS_WR_ST_ERROR;
     return -1; 
   }
   /*
   ** insert the procedure number, NULL credential and verifier
   */
   uint32_t opcode = STORCLI_READ;
   uint32_t null_val = 0;
   XDR_PUTINT32(&xdrs, (int32_t *)&opcode);
   XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
   XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
   XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
   XDR_PUTINT32(&xdrs, (int32_t *)&null_val);        
   /*
   ** ok now call the procedure to encode the message
   */
   if (xdr_storcli_read_arg_t(&xdrs,request) == FALSE)
   {
     ruc_buf_freeBuffer(xmit_buf); 
     severe(" internal read request encoding error ");
     errno = EFAULT;
     wr_proj_buf_p->state = ROZOFS_WR_ST_ERROR;
     return -1;
   }
   /*
   ** Now get the current length and fill the header of the message
   */
   int position = XDR_GETPOS(&xdrs);
   /*
   ** update the length of the message : must be in network order
   */
   *header_size_p = htonl(0x80000000 | position);
   /*
   ** set the payload length in the xmit buffer
   */
   int total_len = sizeof(*header_size_p)+ position;
   ruc_buf_setPayloadLen(xmit_buf,total_len);
   /*
   ** Submit the pseudo request
   */
   rozofs_storcli_read_req_init(0,xmit_buf,rozofs_storcli_internal_read_rsp_cbk,(void*)working_ctx_p,STORCLI_DO_NOT_QUEUE);
   return 0;
}
