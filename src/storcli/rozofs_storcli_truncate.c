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


int rozofs_storcli_get_position_of_first_byte2write_in_truncate();

DECLARE_PROFILING(stcpp_profiler_t);

/*
**__________________________________________________________________________
*/
/**
* PROTOTYPES
*/


/**
*  END PROTOTYPES
*/
/*
**__________________________________________________________________________
*/

/**
* Local prototypes
*/
void rozofs_storcli_truncate_req_processing_cbk(void *this,void *param) ;
void rozofs_storcli_truncate_req_processing(rozofs_storcli_ctx_t *working_ctx_p);

//int rozofs_storcli_remote_rsp_cbk(void *buffer,uint32_t socket_ref,void *user_param);

void rozofs_storcli_truncate_req_processing_exec(rozofs_storcli_ctx_t *working_ctx_p, char * data);
/*
**_________________________________________________________________________
*      LOCAL FUNCTIONS
**_________________________________________________________________________
*/


/*
**__________________________________________________________________________
*/
/**
* The purpose of that function is to return TRUE if there are enough truncate response received for
  rebuilding a projection for future reading 
  
  @param layout : layout association with the file
  @param prj_cxt_p: pointer to the projection context (working array)
  @param *distribution: pointer to the resulting distribution--> obsolete
  
  @retval 1 if there are enough received projection
  @retval 0 when there is enough projection
*/
static inline int rozofs_storcli_all_prj_truncate_check(uint8_t layout,rozofs_storcli_projection_ctx_t *prj_cxt_p,dist_t *distribution)
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
/*
**__________________________________________________________________________
*/
/**
  Initial truncate request
    
  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)
  @param rozofs_storcli_remote_rsp_cbk: callback for sending out the response
 
   @retval : TRUE-> xmit ready event expected
  @retval : FALSE-> xmit  ready event not expected
*/
void rozofs_storcli_truncate_req_init(uint32_t  socket_ctx_idx, void *recv_buf,rozofs_storcli_resp_pf_t rozofs_storcli_remote_rsp_cbk)
{
   rozofs_rpc_call_hdr_with_sz_t    *com_hdr_p;
   rozofs_storcli_ctx_t *working_ctx_p = NULL;
   int i;
   uint32_t  msg_len;  /* length of the rpc messsage including the header length */
   storcli_truncate_arg_t *storcli_truncate_rq_p = NULL;
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
   storcli_truncate_rq_p = &working_ctx_p->storcli_truncate_arg;
   STORCLI_START_NORTH_PROF(working_ctx_p,truncate,0);

   
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
   ** decode the RPC message of the truncate request
   */
   if (xdr_storcli_truncate_arg_t(&xdrs,storcli_truncate_rq_p) == FALSE)
   {
      /*
      ** decoding error
      */
      errcode = EFAULT;
      severe("rpc trucnate request decoding error");
      goto failure;
      
   }   
   /*
   ** init of the load balancing group/ projection association table:
   ** That table is ordered: the first corresponds to the storage associated with projection 0, second with 1, etc..
   ** When build that table, we MUST consider the value of the base which is associated with the distribution
   */

   
   uint8_t   rozofs_safe = rozofs_get_rozofs_safe(storcli_truncate_rq_p->layout);
   int lbg_in_distribution = 0;
   for (i = 0; i  <rozofs_safe ; i ++)
   {
    /*
    ** Get the load balancing group associated with the sid
    */
    int lbg_id = rozofs_storcli_get_lbg_for_sid(storcli_truncate_rq_p->cid,storcli_truncate_rq_p->dist_set[i]);
    if (lbg_id < 0)
    {
      /*
      ** there is no associated between the sid and the lbg. It is typically the case
      ** when a new cluster has been added to the configuration and the client does not
      ** know yet the configuration change
      */
      severe("sid is unknown !! %d\n",storcli_truncate_rq_p->dist_set[i]);
      continue;    
    }
     rozofs_storcli_lbg_prj_insert_lbg_and_sid(working_ctx_p->lbg_assoc_tb,lbg_in_distribution,
                                                lbg_id,
                                                storcli_truncate_rq_p->dist_set[i]);  

     rozofs_storcli_lbg_prj_insert_lbg_state(working_ctx_p->lbg_assoc_tb,
                                             lbg_in_distribution,
                                             NORTH_LBG_GET_STATE(working_ctx_p->lbg_assoc_tb[lbg_in_distribution].lbg_id));    
     lbg_in_distribution++;
     if (lbg_in_distribution == rozofs_safe) break;

   }
   /*
   ** allocate a small buffer that will be used for sending the response to the truncate request
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
   ** set now the working variable specific for handling the truncate
   ** we re-use the structure used for writing even if nothing is written
   */
   uint8_t forward_projection = rozofs_get_rozofs_forward(storcli_truncate_rq_p->layout);
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
     int position = rozofs_storcli_get_position_of_first_byte2write_in_truncate();
     uint8_t *pbuf = (uint8_t*)ruc_buf_getPayload(working_ctx_p->prj_ctx[i].prj_buf); 

     working_ctx_p->prj_ctx[i].bins       = (bin_t*)(pbuf+position); 
   }
   		
   /*
   ** Prepare for request serialization
   */
   memcpy(working_ctx_p->fid_key, storcli_truncate_rq_p->fid, sizeof (sp_uuid_t));
   working_ctx_p->opcode_key = STORCLI_TRUNCATE;
   {
       /**
        * lock all the file for a truncate
        */
       uint64_t nb_blocks = 0;
       nb_blocks--;
       int ret;
       ret = stc_rng_insert((void*)working_ctx_p,
               STORCLI_READ,working_ctx_p->fid_key,
               0,nb_blocks,
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
       return rozofs_storcli_truncate_req_processing(working_ctx_p);
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
       STORCLI_STOP_NORTH_PROF(working_ctx_p,truncate,0);
       working_ctx_p->recv_buf   = NULL;
       rozofs_storcli_release_context(working_ctx_p);
     }
     return;
}

/*
**__________________________________________________________________________
*/

/**
* callback for the internal read request triggered by a truncate

 potential failure case:
  - socket_ref is out of range
  - connection is down
  
 @param buffer : pointer to the ruc_buffer that cointains the response
 @param socket_ref : non significant
 @param user_param_p : pointer to the root context
 
 
 @retval 0 : successfully submitted to the transport layer
 @retval < 0 error, the caller is intended to release the buffer
 */
int rozofs_storcli_internal_read_before_truncate_rsp_cbk(void *buffer,uint32_t socket_ref,void *user_param)
{

   int errcode = 0; 
   rozofs_storcli_ctx_t                *working_ctx_p = (rozofs_storcli_ctx_t*)user_param;
   storcli_truncate_arg_t * storcli_truncate_rq_p = (storcli_truncate_arg_t*)&working_ctx_p->storcli_truncate_arg;

   XDR       xdrs;       
   uint8_t  *payload;
   char     *data;
   int       position;
   int      bufsize;   
   struct rpc_msg  rpc_reply;
   storcli_status_ret_t rozofs_status;
   int  data_len; 
   int error;  
   rpc_reply.acpted_rply.ar_results.proc = NULL;

   /*
   ** decode the read internal read reply
   */
   payload  = (uint8_t*) ruc_buf_getPayload(buffer);
   payload += sizeof(uint32_t); /* skip length*/  
   
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
     goto failure;  
   }   

   position = XDR_GETPOS(&xdrs);
   data     = (char*)(payload+position);

   /*
   ** check the status of the read operation
   */
   if (rozofs_status.status != STORCLI_SUCCESS)
   {
     data = NULL;
   }
   else {
     /*, 
     ** No data returned
     */
     if (data_len == 0) {
       data = NULL;
     }
     else if (storcli_truncate_rq_p->last_seg <= data_len) {
       memset(data+storcli_truncate_rq_p->last_seg, 0, ROZOFS_BSIZE-storcli_truncate_rq_p->last_seg);       
     }
     else {
       memset(data+data_len, 0, ROZOFS_BSIZE-data_len);     
     }
   }
   rozofs_storcli_truncate_req_processing_exec(working_ctx_p, data);
   ruc_buf_freeBuffer(buffer);
   return 0 ;   


failure:
   ruc_buf_freeBuffer(buffer);
   /*
   ** check if the lock is asserted to prevent direct call to callback
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
   STORCLI_STOP_NORTH_PROF(working_ctx_p,truncate,0);  
   rozofs_storcli_release_context(working_ctx_p);
   return 0 ;

}
/*
**__________________________________________________________________________
*/
/**
*  Internal Read procedure
   That procedure is used when it is required to read the last block before
   performing the truncate
   
   @param working_ctx_p: pointer to the root transaction
   
   @retval 0 on success
   retval < 0 on error (see errno for error details)
   
*/
int rozofs_storcli_internal_read_before_truncate_req(rozofs_storcli_ctx_t *working_ctx_p)
{
   storcli_truncate_arg_t *storcli_truncate_rq_p;
   void *xmit_buf = NULL;
   storcli_read_arg_t storcli_read_args;
   storcli_read_arg_t *request   = &storcli_read_args;
   struct rpc_msg   call_msg;
   int               bufsize;
   uint32_t          *header_size_p;
   XDR               xdrs;    
   uint8_t           *arg_p;
      
   storcli_truncate_rq_p = (storcli_truncate_arg_t*)&working_ctx_p->storcli_truncate_arg;
   
   /*
   ** allocated a buffer from sending the request
   */   
   xmit_buf = ruc_buf_getBuffer(ROZOFS_STORCLI_NORTH_SMALL_POOL);
   if (xmit_buf == NULL)
   {
     severe(" out of small buffer on north interface ");
     errno = ENOMEM;
     goto failure;
   }
   /*
   ** build the RPC message
   */
   request->sid = 0;  /* not significant */
   request->layout = storcli_truncate_rq_p->layout;
   request->cid    = storcli_truncate_rq_p->cid;
   request->spare = 0;  /* not significant */
   memcpy(request->dist_set, storcli_truncate_rq_p->dist_set, ROZOFS_SAFE_MAX*sizeof (uint8_t));
   memcpy(request->fid, storcli_truncate_rq_p->fid, sizeof (sp_uuid_t));
   request->proj_id = 0;  /* not significant */
   request->bid     = storcli_truncate_rq_p->bid;  
   request->nb_proj = 1;  
   
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
   call_msg.rm_xid             = rozofs_tx_get_transaction_id(); 
   call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
   /* XXX: prog and vers have been long historically :-( */
   call_msg.rm_call.cb_prog = (uint32_t)STORCLI_PROGRAM;
   call_msg.rm_call.cb_vers = (uint32_t)STORCLI_VERSION;
   if (! xdr_callhdr(&xdrs, &call_msg))
   {
      /*
      ** THIS MUST NOT HAPPEN
      */
     errno = EFAULT;
     severe(" rpc header encode error ");
     goto failure;
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
     severe(" internal read request encoding error ");
     errno = EFAULT;
     goto failure;
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
   rozofs_storcli_read_req_init(0,xmit_buf,rozofs_storcli_internal_read_before_truncate_rsp_cbk,(void*)working_ctx_p,STORCLI_DO_NOT_QUEUE);
   return 0;
   
failure:
  if (xmit_buf != NULL) ruc_buf_freeBuffer(xmit_buf); 
  return -1; 
   
}
/*
**__________________________________________________________________________
*/
/*
** That function is called when all the projection are ready to be sent

 @param working_ctx_p: pointer to the root context associated with the top level write request
 @param data         : pointer to the data of the last block to truncate

*/
void rozofs_storcli_truncate_req_processing_exec(rozofs_storcli_ctx_t *working_ctx_p, char * data)
{

  storcli_truncate_arg_t *storcli_truncate_rq_p = (storcli_truncate_arg_t*)&working_ctx_p->storcli_truncate_arg;
  uint8_t layout = storcli_truncate_rq_p->layout;
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
  ** Let's transform the data to write
  */
  working_ctx_p->truncate_bins_len = 0;
  if (data != NULL) {
    STORCLI_START_KPI(storcli_kpi_transform_forward);

    rozofs_storcli_transform_forward(working_ctx_p->prj_ctx,  
                                     layout,
                                     0, 
                                     1, 
                                     working_ctx_p->timestamp,
                                     storcli_truncate_rq_p->last_seg,
                                     data);  
    STORCLI_STOP_KPI(storcli_kpi_transform_forward,0);
    working_ctx_p->truncate_bins_len = rozofs_get_max_psize(layout)*sizeof(bin_t) + sizeof(rozofs_stor_bins_hdr_t);
  } 
  
  /*
  ** We have enough storage, so initiate the transaction towards the storage for each
  ** projection
  */
  for (projection_id = 0; projection_id < rozofs_forward; projection_id++)
  {
     sp_truncate_arg_no_bins_t *request; 
     sp_truncate_arg_no_bins_t  truncate_prj_args;
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
     request   = &truncate_prj_args;
     request->cid = storcli_truncate_rq_p->cid;
     request->sid = (uint8_t) rozofs_storcli_lbg_prj_get_sid(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);
     request->layout        = layout;
     if (prj_cxt_p[projection_id].stor_idx >= rozofs_forward) request->spare = 1;
     else request->spare = 0;
     memcpy(request->dist_set, storcli_truncate_rq_p->dist_set, ROZOFS_SAFE_MAX*sizeof (uint8_t));
     memcpy(request->fid, storcli_truncate_rq_p->fid, sizeof (sp_uuid_t));
     request->proj_id        = projection_id;
     request->bid            = storcli_truncate_rq_p->bid;
     request->last_seg       = storcli_truncate_rq_p->last_seg;
     request->last_timestamp = working_ctx_p->timestamp;

     request->len = working_ctx_p->truncate_bins_len;

     uint32_t  lbg_id = rozofs_storcli_lbg_prj_get_lbg(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);
     STORCLI_START_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),truncate_prj,0);
     /*
     ** caution we might have a direct reply if there is a direct error at load balancing group while
     ** ateempting to send the RPC message-> typically a disconnection of the TCP connection 
     ** As a consequence the response fct 'rozofs_storcli_truncate_req_processing_cbk) can be called
     ** prior returning from rozofs_sorcli_send_rq_common')
     ** anticipate the status of the xmit state of the projection and lock the section to
     ** avoid a reply error before returning from rozofs_sorcli_send_rq_common() 
     ** --> need to take care because the write context is released after the reply error sent to rozofsmount
     */
     working_ctx_p->write_ctx_lock = 1;
     prj_cxt_p[projection_id].prj_state = ROZOFS_PRJ_WR_IN_PRG;
     
     ret =  rozofs_sorcli_send_rq_common(lbg_id,ROZOFS_TMR_GET(TMR_STORAGE_PROGRAM),STORAGE_PROGRAM,STORAGE_VERSION,SP_TRUNCATE,
                                         (xdrproc_t) xdr_sp_truncate_arg_no_bins_t, (caddr_t) request,
                                          xmit_buf,
                                          working_ctx_p->read_seqnum,
                                          (uint32_t) projection_id,
                                          working_ctx_p->truncate_bins_len,
                                          rozofs_storcli_truncate_req_processing_cbk,
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
     STORCLI_STOP_NORTH_PROF(working_ctx_p,truncate,0);
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
     STORCLI_STOP_NORTH_PROF(working_ctx_p,truncate,0);
     rozofs_storcli_release_context(working_ctx_p);  

  return;

}
/*
**__________________________________________________________________________
*/
/*
** That function is called when all the projection are ready to be sent

 @param working_ctx_p: pointer to the root context associated with the top level write request

*/
void rozofs_storcli_truncate_req_processing(rozofs_storcli_ctx_t *working_ctx_p)
{

  storcli_truncate_arg_t *storcli_truncate_rq_p = (storcli_truncate_arg_t*)&working_ctx_p->storcli_truncate_arg;
  int                     ret;
  int                     errcode;
    
  /*
  ** Check whether the truncate operates on a ROZOFS block size bondary. If it is not
  ** the case, we must read the block from the disk to then remove the extra data at
  ** the end of the block.
  */
  if (storcli_truncate_rq_p->last_seg != 0) {

    working_ctx_p->write_ctx_lock = 1;  /* Avoid direct response on internal read error */
    ret = rozofs_storcli_internal_read_before_truncate_req(working_ctx_p);
    working_ctx_p->write_ctx_lock = 0;

    if (ret < 0)
    {
      errcode = errno;
      severe("fatal error on internal read");
      goto fail;        
    } 
    
    /* Wait for the internal response */
    return;   
  } 

  rozofs_storcli_truncate_req_processing_exec(working_ctx_p,NULL);
  return;
  
fail:
  /*
  ** we fall in that case when we run out of  resource-> that case is a BUG !!
  */
  rozofs_storcli_write_reply_error(working_ctx_p,errcode);
  /*
  ** release the root transaction context
  */
  STORCLI_STOP_NORTH_PROF(working_ctx_p,truncate,0);
  rozofs_storcli_release_context(working_ctx_p); 
  errno =  errcode;
  return;
}


/*
**__________________________________________________________________________
*/
/**
* Projection truncate retry: that procedure is called upon the truncate failure
  of one projection. The system attempts to truncate in sequence the next available
  projection if any. 
  The index of the next projection to read is given by redundancyStorageIdxCur
  
  @param  working_ctx_p : pointer to the root transaction context
  @param  projection_id : index of the projection
  @param same_storage_retry_acceptable : assert to 1 if retry on the same storage is acceptable
  
  @retval >= 0 : success, it indicates the reference of the projection id
  @retval< < 0 error
*/

void rozofs_storcli_truncate_projection_retry(rozofs_storcli_ctx_t *working_ctx_p,uint8_t projection_id,int same_storage_retry_acceptable)
{
    uint8_t   rozofs_safe;
    uint8_t   rozofs_forward;
    uint8_t   layout;
    storcli_truncate_arg_t *storcli_truncate_rq_p = (storcli_truncate_arg_t*)&working_ctx_p->storcli_truncate_arg;
    int error=0;
    int storage_idx;

    rozofs_storcli_projection_ctx_t *prj_cxt_p   = working_ctx_p->prj_ctx;   
    rozofs_storcli_lbg_prj_assoc_t  *lbg_assoc_p = working_ctx_p->lbg_assoc_tb;

    layout         = storcli_truncate_rq_p->layout;
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
     sp_truncate_arg_no_bins_t *request; 
     sp_truncate_arg_no_bins_t  truncate_prj_args;
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
     request   = &truncate_prj_args;
     request->cid = storcli_truncate_rq_p->cid;
     request->sid = (uint8_t) rozofs_storcli_lbg_prj_get_sid(working_ctx_p->lbg_assoc_tb,prj_cxt_p[projection_id].stor_idx);
     request->layout        = layout;
     if (prj_cxt_p[projection_id].stor_idx >= rozofs_forward) request->spare = 1;
     else request->spare = 0;
     memcpy(request->dist_set, storcli_truncate_rq_p->dist_set, ROZOFS_SAFE_MAX*sizeof (uint8_t));
     memcpy(request->fid, storcli_truncate_rq_p->fid, sizeof (sp_uuid_t));
     request->proj_id        = projection_id;
     request->bid            = storcli_truncate_rq_p->bid;
     request->last_seg       = storcli_truncate_rq_p->last_seg;
     request->last_timestamp = working_ctx_p->timestamp;


     /*
     ** Bins len has been saved in the working context
     */
     request->len = working_ctx_p->truncate_bins_len;

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
     
     STORCLI_START_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),truncate_prj,0);
     ret =  rozofs_sorcli_send_rq_common(lbg_id,ROZOFS_TMR_GET(TMR_STORAGE_PROGRAM),STORAGE_PROGRAM,STORAGE_VERSION,SP_TRUNCATE,
                                         (xdrproc_t) xdr_sp_truncate_arg_no_bins_t, (caddr_t) request,
                                          xmit_buf,
                                          working_ctx_p->read_seqnum,
                                          (uint32_t) projection_id,
                                          working_ctx_p->truncate_bins_len,
                                          rozofs_storcli_truncate_req_processing_cbk,
                                         (void*)working_ctx_p);
     working_ctx_p->write_ctx_lock--;
     if (ret < 0)
     {
       /*
       ** the communication with the storage seems to be wrong (more than TCP connection temporary down
       ** attempt to select a new storage
       **
       */
       STORCLI_STOP_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),truncate_prj,0);
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
    STORCLI_STOP_NORTH_PROF(working_ctx_p,truncate,0);
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
     STORCLI_STOP_NORTH_PROF(working_ctx_p,truncate,0);
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

void rozofs_storcli_truncate_req_processing_cbk(void *this,void *param) 
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
   storcli_truncate_arg_t *storcli_truncate_rq_p = NULL;
   rpc_reply.acpted_rply.ar_results.proc = NULL;
   int lbg_id;

   
   int status;
   void     *recv_buf = NULL;   
   int      ret;
   int error = 0;
   int      same_storage_retry_acceptable = 0;


    storcli_truncate_rq_p = (storcli_truncate_arg_t*)&working_ctx_p->storcli_truncate_arg;
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
    ** check if the truncate is already done: this might happen in the case when the same projection
    ** is sent towards 2 different LBGs
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
         STORCLI_ERR_PROF(truncate_prj_tmo);
       }
       else
       {
         STORCLI_ERR_PROF(truncate_prj_err);
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
       ** there was an error on the remote storage while attempt to truncate the file
       ** try to send the truncate of  the projection on another storaged
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
    STORCLI_STOP_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),truncate_prj,0);
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
    ret = rozofs_storcli_all_prj_truncate_check(storcli_truncate_rq_p->layout,
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
    STORCLI_STOP_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),truncate_prj,0);

    if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);       
    rozofs_tx_free_from_ptr(this);
    severe("Cannot get the pointer to the receive buffer");

    rozofs_storcli_write_reply_error(working_ctx_p,error);
    /*
    ** release the root transaction context
    */
    STORCLI_STOP_NORTH_PROF(working_ctx_p,truncate,0);
    rozofs_storcli_release_context(working_ctx_p);  
    return;
    
retry_attempt:    
    /*
    ** There was a read errr for that projection so attempt to find out another storage
    ** but first of all release the ressources related to the current transaction
    */
    STORCLI_STOP_NORTH_PROF((&working_ctx_p->prj_ctx[projection_id]),truncate_prj,0);

    if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);       
    rozofs_tx_free_from_ptr(this);
    /**
    * attempt to select a new storage
    */
    return rozofs_storcli_truncate_projection_retry(working_ctx_p,projection_id,same_storage_retry_acceptable);

        
wait_more_projection:    
    /*
    ** need to wait for some other write transaction responses
    ** 
    */
    if(recv_buf!= NULL) ruc_buf_freeBuffer(recv_buf);           
    rozofs_tx_free_from_ptr(this);
    return;


}


