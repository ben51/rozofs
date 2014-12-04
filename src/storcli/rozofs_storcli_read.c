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

#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_opt.h>
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
#include <rozofs/core/rozofs_tx_common.h>
#include <rozofs/core/rozofs_tx_api.h>
#include "rozofs_storcli.h"
#include "storage_proto.h"
#include <rozofs/core/af_unix_socket_generic_api.h>
#include <rozofs/core/ruc_timer_api.h>
#include <rozofs/core/ruc_traffic_shaping.h>
#include <rozofs/rozofs_srv.h>
#include "rozofs_storcli_rpc.h"
#include <rozofs/rpc/sproto.h>
#include "storcli_main.h"
#include <rozofs/rozofs_timer_conf.h>
#include "rozofs_storcli_mojette_thread_intf.h"

DECLARE_PROFILING(stcpp_profiler_t);


/*
**__________________________________________________________________________
*/
/**
* PROTOTYPES
*/


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
void rozofs_storcli_read_req_processing_cbk(void *this,void *param) ;
void rozofs_storcli_read_req_processing(rozofs_storcli_ctx_t *working_ctx_p);



/*
**_________________________________________________________________________
*      LOCAL FUNCTIONS
**_________________________________________________________________________
*/
/*
**__________________________________________________________________________
*/
/**
* The purpose of that function is to return the number of projection received
  rebuilding the associated initial message
  
  @param layout : layout association with the file
  @param prj_cxt_p: pointer to the projection context (working array)
  
  @retval number of received projection
*/
static inline int rozofs_storcli_rebuild_check(uint8_t layout,rozofs_storcli_projection_ctx_t *prj_cxt_p)
{
  /*
  ** Get the rozofs_inverse and rozofs_forward value for the layout
  */
  uint8_t   rozofs_inverse = rozofs_get_rozofs_inverse(layout);
  uint8_t   rozofs_safe = rozofs_get_rozofs_safe(layout);
  int i;
  int received = 0;
  
  for (i = 0; i <rozofs_safe; i++,prj_cxt_p++)
  {
    if (prj_cxt_p->prj_state == ROZOFS_PRJ_READ_DONE) received++;
    if (received == rozofs_inverse) return received;   
  }
  return received;
}

/*
**__________________________________________________________________________
*/
/**
* The purpose of that function is to return TRUE if there is at least one projection
   for which we expect a response from a storage
  
  @param layout : layout association with the file
  @param prj_cxt_p: pointer to the projection context (working array)
  
  @retval number of received projection
*/
static inline int rozofs_storcli_check_read_in_progress_projections(uint8_t layout,rozofs_storcli_projection_ctx_t *prj_cxt_p)
{
  /*
  ** Get the rozofs_inverse and rozofs_forward value for the layout
  */
  uint8_t   rozofs_safe = rozofs_get_rozofs_safe(layout);
  int i;
  
  for (i = 0; i <rozofs_safe; i++,prj_cxt_p++)
  {
    if (prj_cxt_p->prj_state == ROZOFS_PRJ_READ_IN_PRG) return 1;
  }
  return 0;
}



/*
**_________________________________________________________________________
*      PUBLIC FUNCTIONS
**_________________________________________________________________________
*/



/*
**__________________________________________________________________________
*/

/**
* callback for sending a response to a read ta remote entity

 potential failure case:
  - socket_ref is out of range
  - connection is down
  
 @param buffer : pointer to the ruc_buffer that cointains the response
 @param socket_ref : index of the scoket context with the caller is remode, non significant for local caller
 @param user_param_p : pointer to a user opaque parameter (non significant for a remote access)
 
 @retval 0 : successfully submitted to the transport layer
 @retval < 0 error, the caller is intended to release the buffer
 */
int rozofs_storcli_remote_rsp_cbk(void *buffer,uint32_t socket_ref,void *user_param_p)
{
#ifndef TEST_STORCLI_TEST
    return af_unix_generic_send_stream_with_idx((int)socket_ref,buffer);  
#else
    return test_af_unix_generic_send_stream_with_idx((int)socket_ref,buffer);  
#endif
}
/*
**__________________________________________________________________________
*/
/**
  Initial read request
    
  @param socket_ctx_idx: index of the TCP connection
  @param recv_buf: pointer to the ruc_buffer that contains the message
  @param rozofs_storcli_remote_rsp_cbk: callback for sending out the response
  @param user_param : pointer to a user opaque parameter (non significant for a remote access)
  @param do_not_queue: when asserted, the request in not inserted in the serialization hash table
 
   @retval : TRUE-> xmit ready event expected
  @retval : FALSE-> xmit  ready event not expected
*/
void rozofs_storcli_read_req_init(uint32_t  socket_ctx_idx, 
                                  void *recv_buf,
                                  rozofs_storcli_resp_pf_t rozofs_storcli_remote_rsp_cbk,
                                  void *user_param,
                                  uint32_t do_not_queue)
{
   rozofs_rpc_call_hdr_with_sz_t    *com_hdr_p;
   rozofs_storcli_ctx_t *working_ctx_p = NULL;
   uint32_t  msg_len;  /* length of the rpc messsage including the header length */
   int      len;       /* effective length of application message               */
   uint8_t  *pmsg;     /* pointer to the first available byte in the application message */
   rozofs_rpc_call_hdr_t   hdr;   /* structure that contains the rpc header in host format */
   storcli_read_arg_t     *storcli_read_rq_p = NULL ;
   
   uint32_t header_len;
   XDR xdrs;
   int errcode = EINVAL;
   /*
   ** allocate a context for the duration of the read
   */
   working_ctx_p = rozofs_storcli_alloc_context();
   if (working_ctx_p == NULL)
   {
     /*
     ** that situation MUST not occur since there the same number of receive buffer and working context!!
     */
     severe("out of working read/write saved context");
     errcode = ENOMEM;
     goto failure;

   }
   /*
   ** no bytes since lenngth is unknown
   */
   STORCLI_START_NORTH_PROF(working_ctx_p,read,0);
   
   storcli_read_rq_p = &working_ctx_p->storcli_read_arg;
   memset(storcli_read_rq_p,0,sizeof(storcli_read_arg_t));  /* FDL do we really need it ???? */
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
   working_ctx_p->recv_buf     = recv_buf;
   working_ctx_p->response_cbk = rozofs_storcli_remote_rsp_cbk;
   working_ctx_p->user_param   = user_param;
   /*
   ** Get the payload of the receive buffer and set the pointer to array that describes the read request
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
   working_ctx_p->src_transaction_id = hdr.hdr.xid;
   /*
   ** decode the RPC message of the read request
   */
   if (xdr_storcli_read_arg_t(&xdrs,storcli_read_rq_p) == FALSE)
   {
      /*
      ** decoding error
      */
      errcode = EFAULT;
      severe("rpc read request decoding error");
      goto failure;
      
   }
   /*
   ** allocate a large buffer request where read data will be copied for inverse transform
   */
   working_ctx_p->xmitBuf = ruc_buf_getBuffer(ROZOFS_STORCLI_NORTH_LARGE_POOL);
   if (working_ctx_p->xmitBuf == NULL)
   {
     /*
     ** that situation MUST not occur since there the same number of receive buffer and working context!!
     */
     errcode = ENOMEM;
     severe("out of read buffer");
     goto failure;
   }
   /*
   ** Now set the pointer to the data payload
   */
   /*
   ** generate a fake RPC reply
   */
   {
     char *pbuf = ruc_buf_getPayload(working_ctx_p->xmitBuf);
     int position;
      /*
      ** now get the current position in the buffer for loading the first byte of the bins 
      */  
      position =  sizeof(uint32_t); /* length header of the rpc message */
      position += rozofs_storcli_get_min_rpc_reply_hdr_len();
      position += sizeof(uint32_t);   /* length of the storage status field */
      position += sizeof(uint32_t);   /* length of the alignment field (FDL) */
      position += sizeof(uint32_t);   /* length of the bins len field */
      pbuf +=position;      
      working_ctx_p->data_read_p        = pbuf;
   }
   /**
   *  check the presence of the shared memory buffer
   *  the storcli detects that the rozofsmount has provided a shared memory when
   *  the "spare" field contains a 'S'. However the storcli might decide to ignore it
   *   if it fails to setup the shared memory
   */
   if (storcli_read_rq_p->spare =='S')
   {
     /*
     ** check the presence of the shared memory on storcli
     */
     if (storcli_rozofsmount_shared_mem.active == 1)
     {
       /*
       ** set data_read_p to point to the array where data will be returned
       */
       uint8_t *pbase = (uint8_t*)storcli_rozofsmount_shared_mem.data_p;
       uint32_t buf_offset = storcli_read_rq_p->proj_id*storcli_rozofsmount_shared_mem.buf_sz;
       uint32_t *pbuffer = (uint32_t*) (pbase + buf_offset);
       pbuffer[1] = 0; /** bin_len */
       working_ctx_p->data_read_p  = (char*)&pbuffer[2];
       working_ctx_p->shared_mem_p = pbuffer;           
     }   
   }
   /*
   ** Allocate a sequence for the context. The goal of the seqnum is to detect late
   ** rpc response. In fact when the system trigger parallel RPC requests, all the rpc requests
   ** are tight to the working context allocated for the read. Upon receiving a rpc resposne, first
   ** the xid of the rpc response MUST match with the current transaction context allocated to handle
   ** the rpc transaction, and second point, in order to retrieve the working context that from
   ** which the transaction has be been triggered, the system stores the reference of the 
   ** working context in the transaction context. However saving the context address in the 
   ** transaction context is not enough since we might received a late rpc reply while the
   ** working context has been release and re-allocate for a new read or write request. So
   ** the system might process a wrong rpc reply that is not related to the current read
   ** associated with the working context.
   ** To avoid such issue, the system associated a sequence number (seqnum) that is stored in
   ** working context as well as any transction contexts associated with that working context
   ** (store as an opaque parameter in the transaction context). By this way, the system 
   ** can correlate the RPC reply with the working context by checking the seqnum of the
   ** working context and the seqnum of the transaction context.
   */
   working_ctx_p->read_seqnum        = rozofs_storcli_allocate_read_seqnum();
   /*
   ** set now the working variable specific for handling the read
   */
   int i;
   for (i = 0; i < ROZOFS_SAFE_MAX; i++)
   {
     working_ctx_p->prj_ctx[i].prj_state = ROZOFS_PRJ_READ_IDLE;
     working_ctx_p->prj_ctx[i].prj_buf   = NULL;   
     working_ctx_p->prj_ctx[i].bins       = NULL;   
   }
   working_ctx_p->cur_nmbs2read = 0;  /**< relative index of the starting nmbs */
   working_ctx_p->cur_nmbs = 0;
   working_ctx_p->redundancyStorageIdxCur = 0;
   /*
   ** clear the table that keep tracks of the blocks that have been transformed
   */
   for (i = 0; i < ROZOFS_DISTRIBUTION_MAX_SIZE; i++)
   {
     working_ctx_p->block_ctx_table[i].state = ROZOFS_BLK_TRANSFORM_REQ;
   }

   /*
   ** check the case of an internal request
   */
   if (do_not_queue == STORCLI_DO_NOT_QUEUE )
   {
      rozofs_storcli_read_req_processing(working_ctx_p);
      return;

   }
   /*
   ** Prepare for request serialization
   */
   memcpy(working_ctx_p->fid_key, storcli_read_rq_p->fid, sizeof (sp_uuid_t));
   working_ctx_p->opcode_key = STORCLI_READ;
   {
       int ret;
       ret = stc_rng_insert((void*)working_ctx_p,
               STORCLI_READ,working_ctx_p->fid_key,
               storcli_read_rq_p->bid,storcli_read_rq_p->nb_proj,
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
      rozofs_storcli_read_req_processing(working_ctx_p);
      return;
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
     rozofs_storcli_reply_error_with_recv_buf(socket_ctx_idx,recv_buf,user_param,rozofs_storcli_remote_rsp_cbk,errcode);
     /*
     ** check if the root context was allocated. Free it if is exist
     */
     if (working_ctx_p != NULL) 
     {  
         STORCLI_STOP_NORTH_PROF(working_ctx_p,read,0);
        /*
        ** remove the reference to the recvbuf to avoid releasing it twice
        */
        working_ctx_p->recv_buf   = NULL;
        rozofs_storcli_release_context(working_ctx_p);
     }
     return;
}

/*
**__________________________________________________________________________
*/

void rozofs_storcli_read_req_processing(rozofs_storcli_ctx_t *working_ctx_p)
{

  storcli_read_arg_t *storcli_read_rq_p;
  uint32_t  cur_nmbs;
  uint32_t  nmbs;
  bid_t     bid;
  uint32_t  nb_projections2read = 0;
  uint8_t   rozofs_inverse;
  uint8_t   rozofs_forward;
  uint8_t   rozofs_safe;
  uint8_t   projection_id;
  int       error;
  int i;
  int       rotate=0;
  rozofs_storcli_lbg_prj_assoc_t  *lbg_assoc_p = working_ctx_p->lbg_assoc_tb;
  rozofs_storcli_projection_ctx_t *prj_cxt_p   = working_ctx_p->prj_ctx;   
  uint8_t used_dist_set[ROZOFS_SAFE_MAX];
  uint8_t rotate_modulo;
  uint8_t local_storage_idx;

     
  storcli_read_rq_p = (storcli_read_arg_t*)&working_ctx_p->storcli_read_arg;
  /*
  ** compute the number of blocks with the same distribution starting
  ** from the current block index
  */
  cur_nmbs       = working_ctx_p->cur_nmbs;
  rozofs_inverse = rozofs_get_rozofs_inverse(storcli_read_rq_p->layout);
  rozofs_forward = rozofs_get_rozofs_forward(storcli_read_rq_p->layout);
  rozofs_safe    = rozofs_get_rozofs_safe(storcli_read_rq_p->layout);
  bid            = storcli_read_rq_p->bid;
  nmbs           = storcli_read_rq_p->nb_proj;

  nb_projections2read = nmbs;
  /*
  ** OK, now we known the number of blocks that must read (nb_projections2read) starting at
  ** bid+cur_nmbs block id.
  ** dist_iterator points to the last distribution that matches
  */
  if (nb_projections2read == 0) 
  {
    /*
    * That's the end since there is no block to read
    */
    goto end ;
  }  
  
  /*
  ** Rotate the distribution set for using all the first forward storages
  */ 
#if 1
  rotate = storcli_read_rq_p->sid; 
  i = 0;
  while (1) {
    /*
    ** Check whether one storage is local in the 1rst foward storages of the distribution
    */
    local_storage_idx = 0xFF;
    for (i = 0; i  <rozofs_forward ; i ++)
    {
      int lbg_id = rozofs_storcli_get_lbg_for_sid(storcli_read_rq_p->cid,storcli_read_rq_p->dist_set[i]);
      if (north_lbg_is_local(lbg_id)) {
	local_storage_idx = i;
	break;
      }
    } 
    /*
    ** One local storage is found => always use this guy 1rst
    */ 
    if (local_storage_idx != 0xFF) {
      used_dist_set[0] = storcli_read_rq_p->dist_set[local_storage_idx];    
      for (i = 1; i  <rozofs_forward ; i ++)
      {
	rotate_modulo = (rotate+i) % (rozofs_forward-1);
	used_dist_set[rotate_modulo+1] = storcli_read_rq_p->dist_set[(local_storage_idx+i) % rozofs_forward]; 
      } 
      break;       
    }
    /*
    ** No local storage is found => use every possible storage 
    */
    for (i = 0; i  <rozofs_forward ; i ++)
    {
      rotate_modulo = (i + rotate) % rozofs_forward;
      used_dist_set[i] = storcli_read_rq_p->dist_set[rotate_modulo];     
    } 
    break;
  }  
  /*
  ** Fullfill the distribution with the spare storages
  */
  for (; i  <rozofs_safe ; i ++) {
    used_dist_set[i] = storcli_read_rq_p->dist_set[i];     
  } 
#endif  
  /*
  ** init of the load balancing group/ projection association table with the state of each lbg
  ** search in the current distribution the relative reference of the storage
  ** the first "rozofs_forward" bits correspnd to the optimal distribution and the 
  ** bits between "rozofs_forward" and "rozofs_safe" correpond to the spare storage(s).
  ** The number of spare storages depends on rozofs layout.
  */
  int lbg_in_distribution = 0;
  for (i = 0; i  <rozofs_safe ; i ++)
  {
    /*
    ** FDL do we need to check the value of the sid ????
    */
    /*
    ** Get the load balancing group associated with the sid
    */
    int lbg_id = rozofs_storcli_get_lbg_for_sid(storcli_read_rq_p->cid,used_dist_set[i]);
    if (lbg_id < 0)
    {
      /*
      ** there is no associated between the sid and the lbg. It is typically the case
      ** when a new cluster has been added to the configuration and the client does not
      ** know yet the configuration change
      */
      severe("sid is unknown !! %d\n",used_dist_set[i]);
      continue;    
    }
    rozofs_storcli_lbg_prj_insert_lbg_and_sid(working_ctx_p->lbg_assoc_tb,lbg_in_distribution,
                                              lbg_id,
                                              used_dist_set[i]);   
    rozofs_storcli_lbg_prj_insert_lbg_state(lbg_assoc_p,
                                            lbg_in_distribution,
                                            NORTH_LBG_GET_STATE(lbg_assoc_p[lbg_in_distribution].lbg_id));    
    lbg_in_distribution++;

  }
  if (lbg_in_distribution < rozofs_safe)
  {
    /*
    ** we must have  rozofs_safe storages in the distribution vector !!
    */
    error = EINVAL;
    goto fatal;  
  }  
  /*
  ** Now we have to select the storages that will be used for reading the projection. There are
  ** nb_projections2read to read
  ** The system search for a set of rozofs_forward storage, but it will trigger the read
  ** on a rozofs_inverse set for reading. In case of failure it will use the storages that
  ** are included between rozofs_inverse and rozofs_forward. (we might more than one
  ** spare storages depending of the selected rozofs layout).
  */
  for (projection_id = 0; projection_id < rozofs_inverse; projection_id++) 
  {
    if (rozofs_storcli_select_storage_idx (working_ctx_p,rozofs_safe,projection_id) < 0)
    {
      /*
      ** Out of storage !!-> too many storage down
      */
      error = EIO;
      goto fatal;
    }  
  }
  /*
  ** All the storages on which we can get the projection have been identified, so we can start the
  ** transactions towards the storages. Only rozofs_inverse transactions are intiated. the other
  ** storage(s) might be used if one storage of the rozofs_inverse interval fails.
  */
  working_ctx_p->redundancyStorageIdxCur = 0;
  working_ctx_p->nb_projections2read     = nb_projections2read;
  /*
  ** to be able to rebuild the initial data, the system must read rozofs_inverse different projections
  ** A) Optimal case
  ** in the optimal case the system gets it by read the projection on the "rozofs_inverse" fisrt storages on its allocated distribution
  ** B) first level failure
  ** This corresponds to the failure of one storage that belongs to the first "rozofs_forward" storages
  ** C) second level failure
  ** This corresponds to the situation where there is no enough valid projection in the first "rozofs_forward" storages.
  ** In that case the system will attempt to read up the "rozofs_safe"
  **
  ** Notice that one storage can return on out of date projection. In order to be able to rebuild the initial message
  ** all the projection MUST have the same timestamp
  
  */


  int sent = 0;
  for (projection_id = 0;projection_id < rozofs_inverse; projection_id++) 
  {
     void  *xmit_buf;  
     int ret;  
     sp_read_arg_t read_prj_args;
     sp_read_arg_t *request;    
      
     xmit_buf = ruc_buf_getBuffer(ROZOFS_STORCLI_SOUTH_LARGE_POOL);
     if (xmit_buf == NULL)
     {
       /*
       ** fatal error since the ressource control already took place
       */
       error = ENOMEM;
       goto fatal;     
     }

retry:
     /*
     ** fills the header of the request
     */
     
     request   = &read_prj_args;
     request->cid = storcli_read_rq_p->cid;
     request->sid = (uint8_t) rozofs_storcli_lbg_prj_get_sid(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);;
     request->layout        = storcli_read_rq_p->layout;
     if (prj_cxt_p[projection_id].stor_idx >= rozofs_forward) request->spare = 1;
//     if (projection_id >= rozofs_forward) request->spare = 1;
     else request->spare = 0;
     memcpy(request->dist_set, storcli_read_rq_p->dist_set, ROZOFS_SAFE_MAX*sizeof (uint8_t));
     memcpy(request->fid, storcli_read_rq_p->fid, sizeof (sp_uuid_t));
     request->bid = bid+cur_nmbs;
     request->nb_proj  = nb_projections2read;
     uint32_t  lbg_id = rozofs_storcli_lbg_prj_get_lbg(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);
     STORCLI_START_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),read_prj,0);
     /*
     ** assert by anticipation the expected state for the projection. It might be possible
     ** that the state would be changed at the time rozofs_sorcli_send_rq_common() by 
     ** rozofs_storcli_read_req_processing_cbk() ,if we get a tcp disconnection without 
     ** any other TCP connection up in the lbg.
     ** In that case the state of the projection is changed to ROZOFS_PRJ_READ_ERROR
     **
     ** We also increment the inuse counter of the buffer to avoid a release of the buffer
     ** while releasing the transaction context if there is an error either  while submitting the 
     ** buffer to the lbg or if there is a direct reply to the transaction due to a transmission failure
     ** that usage of the inuse is mandatory since in case of failure the function re-use the same
     ** xmit buffer to attempt a transmission on another lbg
     */
     working_ctx_p->read_ctx_lock++;
     prj_cxt_p[projection_id].prj_state = ROZOFS_PRJ_READ_IN_PRG;
     ruc_buf_inuse_increment(xmit_buf);
     
     ret =  rozofs_sorcli_send_rq_common(lbg_id,ROZOFS_TMR_GET(TMR_STORAGE_PROGRAM),STORAGE_PROGRAM,STORAGE_VERSION,SP_READ,
                                         (xdrproc_t) xdr_sp_read_arg_t, (caddr_t) request,
                                          xmit_buf,
                                          working_ctx_p->read_seqnum,
                                         (uint32_t)projection_id,
                                         0,
                                         rozofs_storcli_read_req_processing_cbk,
                                         (void*)working_ctx_p);
     working_ctx_p->read_ctx_lock--;
     ruc_buf_inuse_decrement(xmit_buf);

     if (ret < 0)
     {
       /*
       ** the communication with the storage seems to be wrong (more than TCP connection temporary down
       ** attempt to select a new storage
       **
       */
       STORCLI_ERR_PROF(read_prj_err);       
       STORCLI_STOP_NORTH_PROF(&working_ctx_p->prj_ctx[projection_id],read_prj,0);
       prj_cxt_p[projection_id].prj_state = ROZOFS_PRJ_READ_ERROR;
       prj_cxt_p[projection_id].errcode = errno;
       if (rozofs_storcli_select_storage_idx (working_ctx_p,rozofs_safe,projection_id) < 0)
       {
         /*
         ** Out of storage !!-> too many storages are down
         ** release the allocated xmit buffer and then reply with appropriated error code
         */
         error = EIO;
         ruc_buf_freeBuffer(xmit_buf);
         goto fatal;
       } 
       /*
       ** retry for that projection with a new storage index: WARNING: we assume that xmit buffer has not been released !!!
       */
       goto retry;
     } 
     /*
     ** check if the  state of the read buffer has not been changed. That change is possible when
     ** all the connection of the load balancing group are down. In that case we attempt to select
     ** another available load balancing group associated with a spare storage for reading the projection
     */
     if ( prj_cxt_p[projection_id].prj_state == ROZOFS_PRJ_READ_ERROR)
     {
         /*
         ** retry to send the request to another storage (spare) 
         */
       if (rozofs_storcli_select_storage_idx (working_ctx_p,rozofs_safe,projection_id) < 0)
       {
         /*
         ** Out of storage !!-> too many storages are down
         ** release the allocated xmit buffer and then reply with appropriated error code
         */
         error = EIO;
         ruc_buf_freeBuffer(xmit_buf);
         goto fatal;
       }
       /*
       ** there is some spare storage available (their associated load group is UP)
       ** so try to read the projection on that spare storage
       */
       goto retry; 
     }
     else
     {
       /*
       ** the projection has been submitted to the load balancing group without error, just need to wait
       ** for the response. Send read request of the next projection to read (until rozofs_inverse)
       */
       sent +=1;
     }
   }
    /*
    ** All projection read request have been sent, just wait for the answers
    */
    return; 
    
    /*
    **_____________________________________________
    **  Exception cases
    **_____________________________________________
    */      
    
fatal:  
     /*
     ** we fall in that case when we run out of  resource
     */
     rozofs_storcli_read_reply_error(working_ctx_p,error);
     /*
     ** release the root transaction context
     */
     STORCLI_STOP_NORTH_PROF(working_ctx_p,read,0);
     rozofs_storcli_release_context(working_ctx_p);  
     return;  

end:
     /*
     ** That case should not append here since the contol is done of the projection reception after rebuilding
     ** the rozofs block. That situation might after a read of 0 block is requested!!
     */
     rozofs_storcli_read_reply_success(working_ctx_p);
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
  
  @retval  0 : show must go on!!
  @retval < 0 : context has been released
*/

int rozofs_storcli_read_projection_retry(rozofs_storcli_ctx_t *working_ctx_p,uint8_t projection_id,int same_storage_retry_acceptable)
{
    uint8_t   rozofs_safe;
    uint8_t   layout;
    uint8_t   rozofs_forward;
    uint8_t   rozofs_inverse;
    storcli_read_arg_t *storcli_read_rq_p;
    int error;

    storcli_read_rq_p = (storcli_read_arg_t*)&working_ctx_p->storcli_read_arg;
    rozofs_storcli_projection_ctx_t *prj_cxt_p   = working_ctx_p->prj_ctx;   

    layout         = storcli_read_rq_p->layout;
    rozofs_safe    = rozofs_get_rozofs_safe(layout);
    rozofs_forward = rozofs_get_rozofs_forward(layout);
    rozofs_inverse = rozofs_get_rozofs_inverse(layout);
    /*
    ** Now update the state of each load balancing group since it might be possible
    ** that some experience a state change
    */
    rozofs_storcli_update_lbg_for_safe_range(working_ctx_p,rozofs_safe);
    /**
    * attempt to select a new storage
    */
    if (rozofs_storcli_select_storage_idx (working_ctx_p,rozofs_safe,projection_id) < 0)
    {
    
      /*
      ** In case of a rozofsmount write request, when the data flushed are not a 
      ** multiple of the rozofs block size, an internal read request is done in 
      ** order to complement the partial blocks. 
      ** In the case it is the very 1rst write of a file on disk, the file does
      ** not yet exist and one must receive back from every storage the error
      */
      {
        int i;
        for (i=0; i< rozofs_inverse; i++) {
	  if (prj_cxt_p[i].prj_state != ROZOFS_PRJ_READ_ERROR) break;
	  if (prj_cxt_p[i].errcode   != ENOENT) break;
	}
	if (i == rozofs_inverse) {
	  error = ENOENT;
	  goto reject;
	}
      }	
      
      /*
      ** Cannot select a new storage: OK so now double check if the retry on the same storage is
      ** acceptable.When it is the case, check if the max retry has not been yet reached
      ** Otherwise, we are in deep shit-> reject the read request
      */
      if (same_storage_retry_acceptable == 0) 
      {
        error = EIO;
        goto reject;      
      }
      if (++prj_cxt_p[projection_id].retry_cpt >= ROZOFS_STORCLI_MAX_RETRY)
      {
        error = EIO;
        goto reject;          
      }
    } 
    /*
    ** we are lucky since either a get a new storage or the retry counter is not exhausted
    */
    sp_read_arg_t *request; 
    sp_read_arg_t  read_prj_args;   
    void  *xmit_buf;  
    int ret;  

    xmit_buf = ruc_buf_getBuffer(ROZOFS_STORCLI_SOUTH_LARGE_POOL);
    if (xmit_buf == NULL)
    {
      /*
      ** fatal error since the ressource control already took place
      */
      severe("Out of small buffer");
      error = ENOMEM;
      goto fatal;
    }
    /*
    ** fill partially the common header
    */

  retry:
    /*
    ** fills the header of the request
    */
     request   = &read_prj_args;
     request->cid = storcli_read_rq_p->cid;
     request->sid = (uint8_t) rozofs_storcli_lbg_prj_get_sid(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);;
     request->layout        = storcli_read_rq_p->layout;
     if (prj_cxt_p[projection_id].stor_idx >= rozofs_forward) request->spare = 1;
     else request->spare = 0;
     memcpy(request->dist_set, storcli_read_rq_p->dist_set, ROZOFS_SAFE_MAX*sizeof (uint8_t));
     memcpy(request->fid, storcli_read_rq_p->fid, sizeof (sp_uuid_t));
     request->bid = storcli_read_rq_p->bid+working_ctx_p->cur_nmbs;
     request->nb_proj  = working_ctx_p->nb_projections2read;
     uint32_t  lbg_id = rozofs_storcli_lbg_prj_get_lbg(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);

     STORCLI_START_NORTH_PROF(&working_ctx_p->prj_ctx[projection_id],read_prj,0);
     /*
     ** assert by anticipation the expected state for the projection. It might be possible
     ** that the state would be changed at the time rozofs_sorcli_send_rq_common() by 
     ** rozofs_storcli_read_req_processing_cbk() ,if we get a tcp disconnection without 
     ** any other TCP connection up in the lbg.
     ** In that case the state of the projection is changed to ROZOFS_PRJ_READ_ERROR
     */
     working_ctx_p->read_ctx_lock++;
     ruc_buf_inuse_increment(xmit_buf);
     prj_cxt_p[projection_id].prj_state = ROZOFS_PRJ_READ_IN_PRG;
     
     ret =  rozofs_sorcli_send_rq_common(lbg_id,ROZOFS_TMR_GET(TMR_STORAGE_PROGRAM),STORAGE_PROGRAM,STORAGE_VERSION,SP_READ,
                                         (xdrproc_t) xdr_sp_read_arg_t, (caddr_t) request,
                                          xmit_buf,
                                          working_ctx_p->read_seqnum,
                                         (uint32_t)projection_id,
                                         0,
                                         rozofs_storcli_read_req_processing_cbk,
                                         (void*)working_ctx_p);

    working_ctx_p->read_ctx_lock--;
     ruc_buf_inuse_decrement(xmit_buf);
    if (ret < 0)
    {
      /*
      ** the communication with the storage seems to be wrong (more than TCP connection temporary down
      ** attempt to select a new storage
      **
      */
      STORCLI_ERR_PROF(read_prj_err);       
      STORCLI_STOP_NORTH_PROF(&working_ctx_p->prj_ctx[projection_id],read_prj,0);
      prj_cxt_p[projection_id].prj_state = ROZOFS_PRJ_READ_ERROR;
      prj_cxt_p[projection_id].errcode = errno;
      /*
      ** retry to send the request to another storage (spare) 
      */       
      if (rozofs_storcli_select_storage_idx (working_ctx_p,rozofs_safe,projection_id) < 0)
      {
        /*
        ** Out of storage !!-> too many storages are down
        */
        ruc_buf_freeBuffer(xmit_buf);
        error = EIO;
        goto reject;
      } 
      goto retry;
    } 
    /*
    ** check if the  state of the read buffer has not been changed. That change is possible when
    ** all the connection of the load balancing group are down. In that case we attempt to select
    ** another available load balancing group associated with a spare storage for reading the projection
    */
    if ( prj_cxt_p[projection_id].prj_state == ROZOFS_PRJ_READ_ERROR)
    {
      /*
      ** retry to send the request to another storage (spare) 
      */
      if (rozofs_storcli_select_storage_idx (working_ctx_p,rozofs_safe,projection_id) < 0)
      {
        /*
        ** Out of storage !!-> too many storages are down
        ** release the allocated xmit buffer and then reply with appropriated error code
        */
        error = EIO;
        ruc_buf_freeBuffer(xmit_buf);
        goto reject;
      }
      /*
      ** there is some spare storage available (their associated load group is UP)
      ** so try to read the projection on that spare storage
      */
      goto retry; 
    }
    /*
    ** All's is fine, just wait for the response
    */
    prj_cxt_p[projection_id].prj_state = ROZOFS_PRJ_READ_IN_PRG;

    return 0;
    /*
    **_____________________________________________
    **  Exception cases
    **_____________________________________________
    */      
    
reject:  
     /*
     ** Check it there is projection for which we expect a response from storage
     ** that situation can occur because of some anticipation introduced by the read
     ** guard timer mechanism
     */
     if (rozofs_storcli_check_read_in_progress_projections(layout,working_ctx_p->prj_ctx) == 0)
     {
       /*
       ** we fall in that case when we run out of  storage
       */
       rozofs_storcli_read_reply_error(working_ctx_p,error);
       /*
       ** release the root transaction context
       */
        STORCLI_STOP_NORTH_PROF(working_ctx_p,read,0);
       rozofs_storcli_release_context(working_ctx_p);  
       return -1;
     }
     return 0; 
      
fatal:
     /*
     ** we fall in that case when we run out of  resource-> that case is a BUG !!
     */
     rozofs_storcli_read_reply_error(working_ctx_p,error);
     /*
     ** release the root transaction context
     */
      STORCLI_STOP_NORTH_PROF(working_ctx_p,read,0);
     rozofs_storcli_release_context(working_ctx_p);  
     return -1; 

}
/*
**__________________________________________________________________________
*/
/**
*  Call back function call upon a success rpc, timeout or any other rpc failure
*
 @param this : pointer to the transaction context
 @param param: pointer to the associated rozofs_fuse_context
 
 @return none
 */

void rozofs_storcli_read_req_processing_cbk(void *this,void *param) 
{
   uint32_t   seqnum;
   uint32_t   projection_id;
   rozofs_storcli_projection_ctx_t  *read_prj_work_p = NULL;
   rozofs_storcli_ctx_t *working_ctx_p = (rozofs_storcli_ctx_t*) param ;
   storcli_read_arg_t *storcli_read_rq_p;
   int status;
   XDR       xdrs;       
   uint8_t  *payload;
   int      bufsize;
   void     *recv_buf = NULL;   
   int      ret;
   int      same_storage_retry_acceptable = 0;
   sp_status_ret_t   rozofs_status;
   int error = 0;
   struct rpc_msg  rpc_reply;
   uint16_t rozofs_max_psize = 0;
   uint32_t nb_projection_blocks_returned = 0;
   bin_t   *bins_p;
   uint64_t raw_file_size;
   int bins_len = 0;
   int lbg_id;
   /*
   ** take care of the rescheduling of the pending frames
   */
   trshape_schedule_on_response();

   rpc_reply.acpted_rply.ar_results.proc = NULL;

   storcli_read_rq_p = (storcli_read_arg_t*)&working_ctx_p->storcli_read_arg;
   
   uint8_t layout         = storcli_read_rq_p->layout;
   uint8_t rozofs_safe    = rozofs_get_rozofs_safe(layout);
   uint8_t rozofs_inverse = rozofs_get_rozofs_inverse(layout);
   rozofs_max_psize       = rozofs_get_max_psize(layout);
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
      ** not the right sequence number, so drop the received message but before check the status of the
      ** operation since we might decide to put the LBG in quarantine
      */
      status = rozofs_tx_get_status(this);
      if (status < 0)
      {
         /*
         ** something wrong happened: assert the status in the associated projection id sub-context
         ** now, double check if it is possible to retry on a new storage
         */
         errno = rozofs_tx_get_errno(this);  
         if (errno == ETIME)
         {
           storcli_lbg_cnx_sup_increment_tmo(lbg_id);
         }
      }
      else
      {
        storcli_lbg_cnx_sup_clear_tmo(lbg_id);
      }
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
       
       working_ctx_p->prj_ctx[projection_id].prj_state = ROZOFS_PRJ_READ_ERROR;
       errno = rozofs_tx_get_errno(this);  
       working_ctx_p->prj_ctx[projection_id].errcode = errno;
       if (errno == ETIME)
       {
         storcli_lbg_cnx_sup_increment_tmo(lbg_id);
         STORCLI_ERR_PROF(read_prj_tmo);
       }
       else
       {
         STORCLI_ERR_PROF(read_prj_err);
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
       STORCLI_ERR_PROF(read_prj_err);       
       errno = EFAULT;  
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
       STORCLI_ERR_PROF(read_prj_err);       
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
       STORCLI_ERR_PROF(read_prj_err);       
        error = 1;
        break;    
      }
      /*
      ** check th estatus of the operation
      */
      if ( rozofs_status.status != SP_SUCCESS )
      {
        errno = rozofs_status.sp_status_ret_t_u.error;
//        printf("FDL storage error %s\n",strerror(errno));
        if (errno == ENOENT) {
          STORCLI_ERR_PROF(read_prj_enoent); 	
	}
	else {
          STORCLI_ERR_PROF(read_prj_err); 
	}      
        error = 1;
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
      if (xdr_int(&xdrs, &bins_len) != TRUE)
      {
        errno = EPROTO;
        STORCLI_ERR_PROF(read_prj_err);       
        error = 1;
        break;          
      }
      int position = xdr_getpos(&xdrs);
      /*
      ** get the pointer to the first byte available in the bins array
      */
      bins_p = (bin_t*)(payload+position);
      /*
      ** Get the file size
      */
      position += ((bins_len+(sizeof(uint32_t)-1))/sizeof(uint32_t))*sizeof(uint32_t);
      xdr_setpos(&xdrs,position);      
      xdr_uint64_t(&xdrs,&raw_file_size);
      /*
      ** The system MUST always returns a length that is a multiple of a projection block size
      */
      nb_projection_blocks_returned = bins_len / (rozofs_max_psize * sizeof (bin_t) + sizeof (rozofs_stor_bins_hdr_t));
      if ((bins_len % (rozofs_max_psize * sizeof (bin_t) + sizeof (rozofs_stor_bins_hdr_t))) != 0) 
      {
          errno = EPROTO;
          STORCLI_ERR_PROF(read_prj_err);       
          error = 1;
          severe("bad bins len %d  projection sz :%d\n",bins_len,rozofs_max_psize);
          break;          
      }
      if (nb_projection_blocks_returned > working_ctx_p->nb_projections2read)
      {
         severe("More blocks than expected %d %d",nb_projection_blocks_returned, working_ctx_p->nb_projections2read);
          STORCLI_ERR_PROF(read_prj_err);       
          errno = EPROTO;
          error = 1;      
      }
      break;
    }
    /*
    ** check the status of the operation
    */
    if (error)
    {
       /*
       ** there was an error on the remote storage while attempt to read the file
       ** try to read the projection on another storaged
       */
       working_ctx_p->prj_ctx[projection_id].prj_state = ROZOFS_PRJ_READ_ERROR;
       working_ctx_p->prj_ctx[projection_id].errcode = errno;
       /**
       * The error has been reported by the remote, we cannot retry on the same storage
       ** we imperatively need to select a different one. So if cannot select a different storage
       ** we report a reading error.
       */
       same_storage_retry_acceptable = 0;
       /*
       ** assert the projection rebuild flag if the selected storage index is the same as the
       ** index of the projection
       */
       if (working_ctx_p->prj_ctx[projection_id].stor_idx == projection_id)
       {
         working_ctx_p->prj_ctx[projection_id].rebuild_req = 1;       
       }
       goto retry_attempt;    
    }


    /*
    ** set the pointer to the read context associated with the projection for which a response has
    ** been received
    */
    STORCLI_STOP_NORTH_PROF(&working_ctx_p->prj_ctx[projection_id],read_prj,bins_len);
    read_prj_work_p = &working_ctx_p->prj_ctx[projection_id];
    /*
    ** save the reference of the receive buffer that contains the projection data in the root transaction context
    */
    read_prj_work_p->prj_buf = recv_buf;
    read_prj_work_p->prj_state = ROZOFS_PRJ_READ_DONE;
    read_prj_work_p->bins = bins_p;
    /*
    ** Go through the bins and copy the each block header in the projection context
    */
    rozofs_storcli_transform_update_headers(read_prj_work_p,layout,nb_projection_blocks_returned,working_ctx_p->nb_projections2read,raw_file_size);
    /*
    ** OK now check if we have enough projection to rebuild the initial message
    */
    ret = rozofs_storcli_rebuild_check(layout,working_ctx_p->prj_ctx);
    if (ret <rozofs_inverse)
    {
      /*
      ** start the timer on the first received projection
      */
      if (ret == 1) 
      {
        rozofs_storcli_start_read_guard_timer(working_ctx_p);
      }
       /*
       ** no enough projection 
       */
       goto wait_more_projection;
    }
    /*
    ** stop the guard timer since enough projection have been received
    */
    rozofs_storcli_stop_read_guard_timer(working_ctx_p);
    
    /*
    ** That's fine, all the projections have been received start rebuild the initial message
    ** for the case of the shared memory, we must check if the rozofsmount has not aborted the request
    */
    if (working_ctx_p->shared_mem_p != NULL)
    {
      uint32_t *xid_p = (uint32_t*)working_ctx_p->shared_mem_p;
      if (*xid_p !=  working_ctx_p->src_transaction_id)
      {
        /*
        ** the source has aborted the request
        */
        error = EPROTO;
        goto io_error;
      }    
    }    
    /*
    ** check if we can proceed with transform inverse
    */
    ret = rozofs_storcli_transform_inverse_check_for_thread(working_ctx_p->prj_ctx,
                                     layout,
                                     working_ctx_p->cur_nmbs2read,
                                     working_ctx_p->nb_projections2read,
                                     working_ctx_p->block_ctx_table,
                                     &working_ctx_p->effective_number_of_blocks,
				     &working_ctx_p->rozofs_storcli_prj_idx_table[0]);				     

    if (ret < 0)
    {
      /*
      ** check if we have some read request pending. If it is the case wait for the 
      ** response of the request pending, otherwise attempt to read one more
      */
      if (rozofs_storcli_check_read_in_progress_projections(layout,
         working_ctx_p->prj_ctx) != 0) {
	 rozofs_tx_free_from_ptr(this);
	 return;
      }
       /*
       ** There is no enough projection to rebuild the initial message
       ** check if we still have storage on which we can read some more projection
       */
       if( working_ctx_p->redundancyStorageIdxCur + rozofs_inverse >= rozofs_safe)
       {
         /*
         ** there are no enough valid storages to be able to rebuild the initial message
         */
         STORCLI_ERR_PROF(read_prj_err);       
         error = EIO;
         goto io_error;
       }         
       /*
       ** we can take a new entry for a projection on a another storage
       */   
       projection_id = rozofs_inverse+ working_ctx_p->redundancyStorageIdxCur;
       working_ctx_p->redundancyStorageIdxCur++;    
       /*
        * do not forget to release the context of the transaction
       */
       rozofs_tx_free_from_ptr(this);
       rozofs_storcli_read_projection_retry(working_ctx_p,projection_id,0);   
       return;     
    }

    /*
    ** we have all the projections with the good timestamp so proceed with
    ** the inverse transform
    */


    if (working_ctx_p->effective_number_of_blocks != 0)
    {
      /*
      **  check if Mojette threads are enable , if the length is greater than the threshold
      **  and there is no read request pending then use them
      */
      int blocklen = working_ctx_p->effective_number_of_blocks*ROZOFS_BSIZE;
      if ((rozofs_stcmoj_thread_read_enable) && (blocklen >rozofs_stcmoj_thread_len_threshold)&&
          (rozofs_storcli_check_read_in_progress_projections(layout,working_ctx_p->prj_ctx) == 0)) 
      {
	ret = rozofs_stcmoj_thread_intf_send(STORCLI_MOJETTE_THREAD_INV,working_ctx_p,0);
	if (ret < 0) 
	{
           errno = EPROTO;
	   goto io_error;
	}
	/*
	** release the transaction context
	*/
        rozofs_tx_free_from_ptr(this);
	return;   
      }
      STORCLI_START_KPI(storcli_kpi_transform_inverse);
      rozofs_storcli_transform_inverse(working_ctx_p->prj_ctx,
                                       layout,
                                       working_ctx_p->cur_nmbs2read,
                                       working_ctx_p->effective_number_of_blocks,
                                       working_ctx_p->block_ctx_table,
                                       working_ctx_p->data_read_p,
                                       &working_ctx_p->effective_number_of_blocks,
				       &working_ctx_p->rozofs_storcli_prj_idx_table[0]);
    }
    else
    {
      STORCLI_START_KPI(storcli_kpi_transform_inverse);    
    }
    STORCLI_STOP_KPI(storcli_kpi_transform_inverse,0);

    /*
    ** now the inverse transform is finished, release the allocated ressources used for
    ** rebuild
    */
    read_prj_work_p = working_ctx_p->prj_ctx;
    for (projection_id = 0; projection_id < rozofs_safe; projection_id++)
    {
      if  (read_prj_work_p[projection_id].prj_buf != NULL) {
        ruc_buf_freeBuffer(read_prj_work_p[projection_id].prj_buf);
      }	
      read_prj_work_p[projection_id].prj_buf = NULL;
      read_prj_work_p[projection_id].prj_state = ROZOFS_PRJ_READ_IDLE;
    }

    /*
    ** update the index of the next block to read
    */
    working_ctx_p->cur_nmbs2read += working_ctx_p->nb_projections2read;
    /*
    ** check if it was the last read
    */
    if (working_ctx_p->cur_nmbs2read < storcli_read_rq_p->nb_proj)
    {
      /*
      ** attempt to read block with the next distribution
      */
      return rozofs_storcli_read_req_processing(working_ctx_p);        
    }    
    /*
    ** read is finished, send back the buffer to the client (rozofsmount)
    */       
    rozofs_storcli_read_reply_success(working_ctx_p);
    /*
    ** release the root context and the transaction context
    */
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
    ** unrecoverable error : mostly a bug!!
    */  
    STORCLI_STOP_NORTH_PROF(&working_ctx_p->prj_ctx[projection_id],read_prj,0);
    if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);       
    rozofs_tx_free_from_ptr(this);
    if (working_ctx_p->read_ctx_lock != 0) return;
    fatal("Cannot get the pointer to the receive buffer");
    return;
    
retry_attempt:    
    /*
    ** There was a read errr for that projection so attempt to find out another storage
    ** but first of all release the ressources related to the current transaction
    */
    STORCLI_STOP_NORTH_PROF(&working_ctx_p->prj_ctx[projection_id],read_prj,0);

    if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);       
    rozofs_tx_free_from_ptr(this);
    /**
    * attempt to select a new storage: a call to a read retry is done only if the
    ** error is not direct. If the system is in the sending process we do not call
    ** the retry attempt.    
    */
    if (working_ctx_p->read_ctx_lock != 0) return;

    rozofs_storcli_read_projection_retry(working_ctx_p,projection_id,same_storage_retry_acceptable);
    return;

io_error:
    /*
    ** issue with connection towards storages and the projections are not consistent
    */
    if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);       
    rozofs_tx_free_from_ptr(this);
    
     rozofs_storcli_read_reply_error(working_ctx_p,error);
     /*
     ** release the root transaction context
     */
      STORCLI_STOP_NORTH_PROF(working_ctx_p,read,0);
     rozofs_storcli_release_context(working_ctx_p);      
    return;
        
wait_more_projection:

    /*
     ** Check it there is projection for which we expect a response from storage
     ** that situation can occur because of some anticipation introduced by the read
     ** guard timer mechanism
     */
    if (rozofs_storcli_check_read_in_progress_projections(layout,
            working_ctx_p->prj_ctx) == 0) {
        /*
         ** we fall in that case when we run out of storage
         */
        error = EIO;
        rozofs_storcli_read_reply_error(working_ctx_p, error);
        /*
         ** release the root transaction context
         */
        STORCLI_STOP_NORTH_PROF(working_ctx_p, read, 0);
        rozofs_storcli_release_context(working_ctx_p);
    }

    /*
    ** there is no enough projection to rebuild the block, release the transaction
    ** and waiting for more projection read replies
    */
    rozofs_tx_free_from_ptr(this);
    return;


}



/*
**__________________________________________________________________________
*/
/**
*  Call back function call upon a success rpc, timeout or any other rpc failure
*
 @param this : pointer to the transaction context
 @param param: pointer to the associated rozofs_fuse_context
 
 @return none
 */

void rozofs_storcli_read_timeout(rozofs_storcli_ctx_t *working_ctx_p) 
{
    uint8_t   rozofs_safe;
    uint8_t   layout;
    uint8_t   rozofs_inverse;
    storcli_read_arg_t *storcli_read_rq_p;
    uint32_t   projection_id;
    int missing;
    int ret;
    int i;
    int nb_received;

    storcli_read_rq_p = (storcli_read_arg_t*)&working_ctx_p->storcli_read_arg;

    layout         = storcli_read_rq_p->layout;
    rozofs_safe    = rozofs_get_rozofs_safe(layout);
    rozofs_inverse = rozofs_get_rozofs_inverse(layout);

    nb_received = rozofs_storcli_rebuild_check(layout,working_ctx_p->prj_ctx);
    
    missing = rozofs_inverse - nb_received;
    
    for (i = 0; i < missing; i++)
    {

      /*
      ** Check if it is possible to read from another storage
      ** if we cannot, we just leave without raising an error since the system may already
      ** ask to spare and is waiting for its response
      */
      if( working_ctx_p->redundancyStorageIdxCur + rozofs_inverse >= rozofs_safe)
      {
        return;
      }         
      /*
      ** we can take a new entry for a projection on a another storage
      */   
      projection_id = rozofs_inverse+ working_ctx_p->redundancyStorageIdxCur;
      working_ctx_p->redundancyStorageIdxCur++;    

      ret = rozofs_storcli_read_projection_retry(working_ctx_p,projection_id,0);
      if (ret < 0)
      {
        /*
        ** the read context has been release, so give up
        */
        break;
      }
    }    
    return;    
}        


#define ROZOFS_STORCLI_TIMER_BUCKET 2
typedef struct _rozofs_storcli_read_clk_t
{
  uint32_t        bucket_cur;
  ruc_obj_desc_t  bucket[ROZOFS_STORCLI_TIMER_BUCKET];  /**< link list of the context waiting on timer */
} rozofs_storcli_read_clk_t;


rozofs_storcli_read_clk_t  rozofs_storcli_read_clk;

/*
**____________________________________________________
*/
/**
* start the read guard timer: must be called upon the reception of the first projection

  @param p: read main context
  
 @retval none
*/
void rozofs_storcli_start_read_guard_timer(rozofs_storcli_ctx_t  *p)
{
   rozofs_storcli_stop_read_guard_timer(p);
   ruc_objInsertTail((ruc_obj_desc_t*)&rozofs_storcli_read_clk.bucket[rozofs_storcli_read_clk.bucket_cur],
                    &p->timer_list);
   

}
/*
**____________________________________________________
*/
/**
* stop the read guard timer

  @param p: read main context
  
 @retval none
*/
void rozofs_storcli_stop_read_guard_timer(rozofs_storcli_ctx_t  *p)
{
   ruc_objRemove(&p->timer_list);
}

/*
**____________________________________________________
*/
/*
  Periodic timer expiration
  
   @param param: Not significant
*/
static uint64_t ticker_count = 0;
void rozofs_storcli_periodic_ticker(void * param) 
{
   ruc_obj_desc_t   *bucket_head_p;
   rozofs_storcli_ctx_t   *read_ctx_p;
   ruc_obj_desc_t  *timer;
   int bucket_idx;
   
   ticker_count += 100;
   if (ticker_count < ROZOFS_TMR_GET(TMR_PRJ_READ_SPARE)) return;
   
   ticker_count = 0;
   
   bucket_idx = rozofs_storcli_read_clk.bucket_cur;
   bucket_idx = (bucket_idx+1)%ROZOFS_STORCLI_TIMER_BUCKET;
   bucket_head_p = &rozofs_storcli_read_clk.bucket[bucket_idx];
   rozofs_storcli_read_clk.bucket_cur = bucket_idx;


    while  ((timer = ruc_objGetFirst(bucket_head_p)) !=NULL) 
    {
       read_ctx_p = (rozofs_storcli_ctx_t * )ruc_listGetAssoc(timer);
       rozofs_storcli_stop_read_guard_timer(read_ctx_p);       
       rozofs_storcli_read_timeout(read_ctx_p);    
    }          
}
/*
**____________________________________________________
*/
/*
  start a periodic timer to chech wether the export LBG is down
  When the export is restarted its port may change, and so
  the previous configuration of the LBG is not valid any more
*/
void rozofs_storcli_read_init_timer_module() {
  struct timer_cell * periodic_timer;
  int i;
  
  for (i = 0; i < ROZOFS_STORCLI_TIMER_BUCKET; i++)
  {
    ruc_listHdrInit(&rozofs_storcli_read_clk.bucket[i]);   
  }
  rozofs_storcli_read_clk.bucket_cur = 0;
  
  periodic_timer = ruc_timer_alloc(0,0);
  if (periodic_timer == NULL) {
    severe("no timer");
    return;
  }
  ruc_periodic_timer_start (periodic_timer, 
                            100,
 	                        rozofs_storcli_periodic_ticker,
 			                0);

}

