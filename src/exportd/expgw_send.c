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

#include <config.h>
#include <rozofs/rozofs.h>
#include <rozofs/common/list.h>
#include <rozofs/common/log.h>
#include <rozofs/common/xmalloc.h>
#include <rozofs/rpc/export_profiler.h>
#include <rozofs/rpc/eproto.h>
#include <rozofs/rpc/epproto.h>

#include <rozofs/core/rozofs_tx_common.h>
#include <rozofs/core/rozofs_tx_api.h>
#include <rozofs/rpc/rozofs_rpc_util.h>
#include "expgw_export.h"

/**
* API to forward a request that has been received on from rozofsmount towards exportd
  The buffer used is the buffer stored in the recv_buf field of the context.
  
  Prior to forward the message to the exportd, the following fields of the rpc message are modified
    - rpc transaction id (xid)
    - number of export gateways (user part)
    - index of the current export gateway (user part of the rpc message)

  In order to avoid a decoding of the message, the context has been updated with the offset on
  the "number of export gateways" of the user message. The index of the current export gateway
  must then be the next field after "number of export gateways"


 @param lbg_id     : reference of the load balancing group
 @param seqnum     : unused
 @param opaque_value_idx1 : unused
 @param recv_cbk   : receive callback function

 @param user_ctx_p : pointer to the working context
 
 @retval 0 on success;
 @retval -1 on error,, errno contains the cause
 */

int expgw_forward_rq_common(expgw_ctx_t *working_ctx_p,
                                 uint32_t lbg_id,
                                 uint32_t seqnum,
                                 uint32_t opaque_value_idx1,  
                                 sys_recv_pf_t recv_cbk,void *user_ctx_p) 
{
   
    rozofs_tx_ctx_t   *rozofs_tx_ctx_p = NULL;
    int               ret;
    XDR               xdrs; 
    void              *xmit_buf= NULL;
    
    /*
    ** remove the reference of the recived buffer from  the context since we use
    ** it for the transaction and it is up to the transaction module to release it
    */
    xmit_buf = working_ctx_p->recv_buf;
    working_ctx_p->recv_buf = NULL;
    
    working_ctx_p->response_cbk = recv_cbk;   
    /*
    ** allocate a transaction context
    */
    rozofs_tx_ctx_p = rozofs_tx_alloc();  
    if (rozofs_tx_ctx_p == NULL) 
    {
       /*
       ** out of context
       ** --> put a pending list for the future to avoid repluing ENOMEM
       */
       
       ruc_buf_freeBuffer(xmit_buf);
       xmit_buf = NULL;
       TX_STATS(ROZOFS_TX_NO_CTX_ERROR);
       errno = ENOMEM;
       goto error;
    }    
    /*
    ** store the reference of the xmit buffer in the transaction context: might be useful
    ** in case we want to remove it from a transmit list of the underlying network stacks
    */
    rozofs_tx_save_xmitBuf(rozofs_tx_ctx_p,xmit_buf);
    /*
    ** get the pointer to the payload of the buffer
    */
    rozofs_rpc_call_hdr_with_sz_t *com_hdr_p  = (rozofs_rpc_call_hdr_with_sz_t*) ruc_buf_getPayload(xmit_buf);  
    com_hdr_p->hdr.xid = ntohl(rozofs_tx_alloc_xid(rozofs_tx_ctx_p)); ; 
    /*
    ** push the consistency data
    */
    char *pchar = (char *) com_hdr_p;
    pchar += working_ctx_p->xdr_offset_in_req;
    xdrmem_create(&xdrs,(char*)pchar,sizeof(uint64_t),XDR_ENCODE);  
#if 0
    if (!xdr_uint64_t (&xdrs,(uint64_t*) &working_ctx_p->request_timestamp.u64))
    {
       errno = EINVAL;
       goto error;		
    }
#endif
    /*
    ** store the receive call back and its associated parameter
    */
    rozofs_tx_ctx_p->recv_cbk   = expgw_generic_export_reply_cbk;
    rozofs_tx_ctx_p->user_param = user_ctx_p;    
    /*
    ** store the sequence number in one of the opaque user data array of the transaction
    */
    rozofs_tx_write_opaque_data( rozofs_tx_ctx_p,0,seqnum);  
    rozofs_tx_write_opaque_data( rozofs_tx_ctx_p,1,opaque_value_idx1);  
    /*
    ** now send the message
    */
#ifndef TEST_MSTORCLI_TEST
    ret = north_lbg_send(lbg_id,xmit_buf);
#else
    ret = test_north_lbg_send(lbg_id,xmit_buf);
#endif
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);

    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p,1);  
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
    return -1;    
}


/*
**__________________________________________________________________________
*/
/**
* API to forward a request that has been received on from rozofsmount towards exportd
  The buffer used is the buffer stored in the recv_buf field of the context.
  
  Prior to forward the message to the exportd, the following fields of the rpc message are modified
    - rpc transaction id (xid)
    - number of export gateways (user part)
    - index of the current export gateway (user part of the rpc message)

  In order to avoid a decoding of the message, the context has been updated with the offset on
  the "number of export gateways" of the user message. The index of the current export gateway
  must then be the next field after "number of export gateways"


 @param eid     : reference of the export
 @param fid     : unique reference of a filesystem object (file, directory, etc...)
 @param seqnum     : unused
 @param opaque_value_idx1 : unused
 @param recv_cbk   : receive callback function

 @param user_ctx_p : pointer to the working context
 
 @retval 0 on success;
 @retval -1 on error,, errno contains the cause
 */

int expgw_routing_rq_common(expgw_ctx_t *working_ctx_p,
                                 uint32_t eid,fid_t fid,
                                 uint32_t seqnum,
                                 uint32_t opaque_value_idx1,  
                                 sys_recv_pf_t recv_cbk,void *user_ctx_p) 
{
   
    rozofs_tx_ctx_t   *rozofs_tx_ctx_p = NULL;
    int               ret;
    XDR               xdrs; 
    void              *xmit_buf= NULL;
    int lbg_id;
    expgw_tx_routing_ctx_t  *routing_ctx_p = &working_ctx_p->expgw_routing_ctx ;
 
 
    
    /*
    ** remove the reference of the recived buffer from  the context since we use
    ** it for the transaction and it is up to the transaction module to release it
    */
    xmit_buf = working_ctx_p->recv_buf;
    working_ctx_p->recv_buf = NULL;

    /*
    ** get the available load balancing group(s) for routing the request 
    */    
    ret  = expgw_get_export_routing_lbg_info(eid,fid,routing_ctx_p);
    if (ret < 0)
    {
      /*
      ** no load balancing group available
      */
      errno = EPROTO;
      goto error;    
    }
    
    working_ctx_p->response_cbk = recv_cbk;   
    /*
    ** allocate a transaction context
    */
    rozofs_tx_ctx_p = rozofs_tx_alloc();  
    if (rozofs_tx_ctx_p == NULL) 
    {
       /*
       ** out of context
       ** --> put a pending list for the future to avoid repluing ENOMEM
       */
       
       ruc_buf_freeBuffer(xmit_buf);
       xmit_buf = NULL;
       TX_STATS(ROZOFS_TX_NO_CTX_ERROR);
       errno = ENOMEM;
       goto error;
    }    
    /*
    ** The system attempts first to forward the message toward load balancing group
    ** of an export gateway and then to the master export if the load balancing group
    ** of the export gateway is not available
    */
    lbg_id = expgw_routing_get_next(routing_ctx_p,xmit_buf);
    /*
    ** store the reference of the xmit buffer in the transaction context: might be useful
    ** in case we want to remove it from a transmit list of the underlying network stacks
    */
    rozofs_tx_save_xmitBuf(rozofs_tx_ctx_p,xmit_buf);
    /*
    ** get the pointer to the payload of the buffer
    */
    rozofs_rpc_call_hdr_with_sz_t *com_hdr_p  = (rozofs_rpc_call_hdr_with_sz_t*) ruc_buf_getPayload(xmit_buf);  
    com_hdr_p->hdr.xid = ntohl(rozofs_tx_alloc_xid(rozofs_tx_ctx_p)); ; 
    /*
    ** push the consistency data
    */
    char *pchar = (char *) com_hdr_p;
    pchar += working_ctx_p->xdr_offset_in_req;
    xdrmem_create(&xdrs,(char*)pchar,sizeof(uint64_t),XDR_ENCODE);  
#if 0
    if (!xdr_uint64_t (&xdrs,(uint64_t*) &working_ctx_p->request_timestamp.u64))
    {
       errno = EINVAL;
       goto error;		
    }
#endif
    /*
    ** store the receive call back and its associated parameter
    */
    rozofs_tx_ctx_p->recv_cbk   = expgw_generic_export_reply_cbk;
    rozofs_tx_ctx_p->user_param = user_ctx_p;    
    /*
    ** store the sequence number in one of the opaque user data array of the transaction
    */
    rozofs_tx_write_opaque_data( rozofs_tx_ctx_p,0,seqnum);  
    rozofs_tx_write_opaque_data( rozofs_tx_ctx_p,1,opaque_value_idx1);  
    /*
    ** now send the message
    */
#ifndef TEST_MSTORCLI_TEST
    ret = north_lbg_send(lbg_id,xmit_buf);
#else
    ret = test_north_lbg_send(lbg_id,xmit_buf);
#endif
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);

    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p,1);  
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
    return -1;    
}


/*
**__________________________________________________________________________
*/
/**
* send a read pr write success reply
  insert the transaction_id associated with the inittial request transaction id
  
  In case of a success it is up to the called function to release the xmit buffer
  
  @param p : pointer to the root transaction context used for the read
  
  @retval none

*/
void expgw_common_reply_forward(expgw_ctx_t *p)
{
   int ret;
   int len;
   rozofs_rpc_call_hdr_with_sz_t *com_hdr_p;

    /*
    ** create xdr structure on top of the buffer that will be used for sending the response
    */
    len = ruc_buf_getPayloadLen(p->xmitBuf);
    len -=sizeof(uint32_t);   
     

    com_hdr_p  = (rozofs_rpc_call_hdr_with_sz_t*) ruc_buf_getPayload(p->xmitBuf);  
    /*
    ** restore the initial transaction id
    */
    com_hdr_p->hdr.xid  = htonl(p->src_transaction_id);   
    /*
    ** Get the callback for sending back the response:
    ** A callback is needed since the request for read might be local or remote
    */
    ret =  af_unix_generic_send_stream_with_idx((int)p->socketRef,p->xmitBuf);  
    if (ret == 0)
    {
      /**
      * success so remove the reference of the xmit buffer since it is up to the called
      * function to release it
      */
      p->xmitBuf = NULL;
    }    
    return;
} 

/*
**__________________________________________________________________________
*/
/**
* send a error read reply
  That API fill up the common header with the SP_READ_RSP opcode
  insert the transaction_id associated with the inittial request transaction id
  insert a status OK
  insert the length of the data payload
  
  In case of a success it is up to the called function to release the xmit buffer
  
  @param p : pointer to the root transaction context used for the read
  @param error : error code
  
  @retval none

*/
void expgw_reply_error(expgw_ctx_t *p,int error)
{
    int ret;
    uint8_t *pbuf;           /* pointer to the part that follows the header length */
    uint32_t *header_len_p;  /* pointer to the array that contains the length of the rpc message*/
    XDR xdrs;
    int len;
    epgw_mattr_ret_t status;

    status.status_gw.status = EP_FAILURE;
    
    status.status_gw.ep_mattr_ret_t_u.error = error;
    status.parent_attr.status = EP_EMPTY;

    /*
    ** need to allocated an xmit buffer if none is available in the context
    */
    if (p->xmitBuf == NULL)
    {
      p->xmitBuf = ruc_buf_getBuffer(ROZOFS_TX_LARGE_RX_POOL);
      if (p->xmitBuf == NULL) 
      {
        fatal("out of received buffer on exportd interface");    
      }
    }   
    /*
    ** create xdr structure on top of the buffer that will be used for sending the response
    */
    header_len_p = (uint32_t*)ruc_buf_getPayload(p->xmitBuf); 
    pbuf = (uint8_t*) (header_len_p+1);            
    len = (int)ruc_buf_getMaxPayloadLen(p->xmitBuf);
    len -= sizeof(uint32_t);
    xdrmem_create(&xdrs,(char*)pbuf,len,XDR_ENCODE); 
    if (rozofs_encode_rpc_reply(&xdrs,(xdrproc_t)xdr_epgw_mattr_ret_t,(caddr_t)&status,p->src_transaction_id) != TRUE)
    {
      severe("rpc reply encoding error");
      goto error;     
    }       
    /*
    ** compute the total length of the message for the rpc header and add 4 bytes more bytes for
    ** the ruc buffer to take care of the header length of the rpc message.
    */
    int total_len = xdr_getpos(&xdrs) ;
    *header_len_p = htonl(0x80000000 | total_len);
    total_len +=sizeof(uint32_t);
    ruc_buf_setPayloadLen(p->xmitBuf,total_len);

    /*
    ** Get the callback for sending back the response:
    ** A callback is needed since the request for read might be local or remote
    */
    ret =  af_unix_generic_send_stream_with_idx((int)p->socketRef,p->xmitBuf);  
    if (ret == 0)
    {
      /**
      * success so remove the reference of the xmit buffer since it is up to the called
      * function to release it
      */
      p->xmitBuf = NULL;
    }
    
error:
    return;
}


/*
**__________________________________________________________________________
*/
/**
* send a error read reply
  That API fill up the common header with the SP_READ_RSP opcode
  insert the transaction_id associated with the inittial request transaction id
  insert a status OK
  insert the length of the data payload
  
  In case of a success it is up to the called function to release the xmit buffer
  
  @param p : pointer to the root transaction context used for the read
  @param eid : reference of the eid
  
  @retval none

*/
void expgw_reply_error_no_such_eid(expgw_ctx_t *p,int eid)
{
    int ret;
    uint8_t *pbuf;           /* pointer to the part that follows the header length */
    uint32_t *header_len_p;  /* pointer to the array that contains the length of the rpc message*/
    XDR xdrs;
    int len;
    epgw_mattr_ret_t status;

    status.status_gw.status = EP_FAILURE_EID_NOT_SUPPORTED;
    status.hdr.eid = eid;
    
    status.parent_attr.status = EP_EMPTY;

    /*
    ** need to allocated an xmit buffer if none is available in the context
    */
    if (p->xmitBuf == NULL)
    {
      p->xmitBuf = ruc_buf_getBuffer(ROZOFS_TX_LARGE_RX_POOL);
      if (p->xmitBuf == NULL) 
      {
        fatal("out of received buffer on exportd interface");    
      }
    }   
    /*
    ** create xdr structure on top of the buffer that will be used for sending the response
    */
    header_len_p = (uint32_t*)ruc_buf_getPayload(p->xmitBuf); 
    pbuf = (uint8_t*) (header_len_p+1);            
    len = (int)ruc_buf_getMaxPayloadLen(p->xmitBuf);
    len -= sizeof(uint32_t);
    xdrmem_create(&xdrs,(char*)pbuf,len,XDR_ENCODE); 
    if (rozofs_encode_rpc_reply(&xdrs,(xdrproc_t)xdr_epgw_mattr_ret_t,(caddr_t)&status,p->src_transaction_id) != TRUE)
    {
      severe("rpc reply encoding error");
      goto error;     
    }       
    /*
    ** compute the total length of the message for the rpc header and add 4 bytes more bytes for
    ** the ruc buffer to take care of the header length of the rpc message.
    */
    int total_len = xdr_getpos(&xdrs) ;
    *header_len_p = htonl(0x80000000 | total_len);
    total_len +=sizeof(uint32_t);
    ruc_buf_setPayloadLen(p->xmitBuf,total_len);

    /*
    ** Get the callback for sending back the response:
    ** A callback is needed since the request for read might be local or remote
    */
    ret =  af_unix_generic_send_stream_with_idx((int)p->socketRef,p->xmitBuf);  
    if (ret == 0)
    {
      /**
      * success so remove the reference of the xmit buffer since it is up to the called
      * function to release it
      */
      p->xmitBuf = NULL;
    }
    
error:
    return;
}



/*
**__________________________________________________________________________
*/
/**
* send a rpc reply: the encoding function MUST be found in xdr_result 
 of the gateway context

  It is assumed that the xmitBuf MUST be found in xmitBuf field
  
  In case of a success it is up to the called function to release the xmit buffer
  
  @param p : pointer to the root transaction context used for the read
  @param arg_ret : returned argument to encode 
  
  @retval none

*/
void expgw_forward_reply (expgw_ctx_t *p,char * arg_ret)
{
   int ret;
   uint8_t *pbuf;           /* pointer to the part that follows the header length */
   uint32_t *header_len_p;  /* pointer to the array that contains the length of the rpc message*/
   XDR xdrs;
   int len;

   if (p->xmitBuf == NULL)
   {
      severe("no xmit buffer");
      goto error;
   } 
    /*
    ** create xdr structure on top of the buffer that will be used for sending the response
    */
    header_len_p = (uint32_t*)ruc_buf_getPayload(p->xmitBuf); 
    pbuf = (uint8_t*) (header_len_p+1);            
    len = (int)ruc_buf_getMaxPayloadLen(p->xmitBuf);
    len -= sizeof(uint32_t);
    xdrmem_create(&xdrs,(char*)pbuf,len,XDR_ENCODE); 
    if (rozofs_encode_rpc_reply(&xdrs,(xdrproc_t)p->xdr_result,(caddr_t)arg_ret,p->src_transaction_id) != TRUE)
    {
      severe("rpc reply encoding error");
      goto error;     
    }       
    /*
    ** compute the total length of the message for the rpc header and add 4 bytes more bytes for
    ** the ruc buffer to take care of the header length of the rpc message.
    */
    int total_len = xdr_getpos(&xdrs) ;
    *header_len_p = htonl(0x80000000 | total_len);
    total_len +=sizeof(uint32_t);
    ruc_buf_setPayloadLen(p->xmitBuf,total_len);

    /*
    ** Get the callback for sending back the response:
    ** A callback is needed since the request for read might be local or remote
    */
    ret =  af_unix_generic_send_stream_with_idx((int)p->socketRef,p->xmitBuf);  
    if (ret == 0)
    {
      /**
      * success so remove the reference of the xmit buffer since it is up to the called
      * function to release it
      */
      p->xmitBuf = NULL;
    }
    
error:
    return;
}


/**
* API for creation a transaction towards an exportd

 The reference of the north load balancing is extracted for the client structure
 fuse_ctx_p:
 That API needs the pointer to the current fuse context. That nformation will be
 saved in the transaction context as userParam. It is intended to be used later when
 the client gets the response from the server
 encoding function;
 For making that API generic, the caller is intended to provide the function that
 will encode the message in XDR format. The source message that is encoded is 
 supposed to be pointed by msg2encode_p.
 Since the service is non-blocking, the caller MUST provide the callback function 
 that will be used for decoding the message
 

 @param lbg_id     : reference of the load balancing group of the exportd
 @param prog       : program
 @param vers       : program version
 @param opcode     : metadata opcode
 @param encode_fct : encoding function
 @msg2encode_p     : pointer to the message to encode
 @param recv_cbk   : receive callback function
 @param ctx_p      : pointer to the user context
 
 @retval 0 on success;
 @retval -1 on error,, errno contains the cause
 */

int expgw_export_build_and_send_common(int lbg_id,uint32_t prog,uint32_t vers,
                                       int opcode,xdrproc_t encode_fct,void *msg2encode_p,
                                       sys_recv_pf_t recv_cbk,void *ctx_p) 
{
    DEBUG_FUNCTION;
   
    uint8_t           *arg_p;
    uint32_t          *header_size_p;
    rozofs_tx_ctx_t   *rozofs_tx_ctx_p = NULL;
    void              *xmit_buf = NULL;
    int               bufsize;
    int               ret;
    int               position;
    XDR               xdrs;    
	struct rpc_msg   call_msg;
    uint32_t         null_val = 0;

    /*
    ** allocate a transaction context
    */
    rozofs_tx_ctx_p = rozofs_tx_alloc();  
    if (rozofs_tx_ctx_p == NULL) 
    {
       /*
       ** out of context
       ** --> put a pending list for the future to avoid repluing ENOMEM
       */
       TX_STATS(ROZOFS_TX_NO_CTX_ERROR);
       errno = ENOMEM;
       goto error;
    }    
    /*
    ** allocate an xmit buffer
    */  
    xmit_buf = ruc_buf_getBuffer(ROZOFS_TX_LARGE_TX_POOL);
    if (xmit_buf == NULL)
    {
      /*
      ** something rotten here, we exit we an error
      ** without activating the FSM
      */
      TX_STATS(ROZOFS_TX_NO_BUFFER_ERROR);
      errno = ENOMEM;
      goto error;
    } 
    /*
    ** store the reference of the xmit buffer in the transaction context: might be useful
    ** in case we want to remove it from a transmit list of the underlying network stacks
    */
    rozofs_tx_save_xmitBuf(rozofs_tx_ctx_p,xmit_buf);
    /*
    ** get the pointer to the payload of the buffer
    */
    header_size_p  = (uint32_t*) ruc_buf_getPayload(xmit_buf);
    arg_p = (uint8_t*)(header_size_p+1);  
    /*
    ** create the xdr_mem structure for encoding the message
    */
    bufsize = ruc_buf_getMaxPayloadLen(xmit_buf);
    xdrmem_create(&xdrs,(char*)arg_p,bufsize,XDR_ENCODE);
    /*
    ** fill in the rpc header
    */
    call_msg.rm_direction = CALL;
    /*
    ** allocate a xid for the transaction 
    */
	call_msg.rm_xid             = rozofs_tx_alloc_xid(rozofs_tx_ctx_p); 
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	/* XXX: prog and vers have been long historically :-( */
	call_msg.rm_call.cb_prog = (uint32_t)prog;
	call_msg.rm_call.cb_vers = (uint32_t)vers;
	if (! xdr_callhdr(&xdrs, &call_msg))
    {
       /*
       ** THIS MUST NOT HAPPEN
       */
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;	
    }
    /*
    ** insert the procedure number, NULL credential and verifier
    */
    XDR_PUTINT32(&xdrs, (int32_t *)&opcode);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
        
    /*
    ** ok now call the procedure to encode the message
    */
    if ((*encode_fct)(&xdrs,msg2encode_p) == FALSE)
    {
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;
    }
    /*
    ** Now get the current length and fill the header of the message
    */
    position = XDR_GETPOS(&xdrs);
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
    ** store the receive call back and its associated parameter
    */
    rozofs_tx_ctx_p->recv_cbk   = recv_cbk;
    rozofs_tx_ctx_p->user_param = ctx_p;    
    /*
    ** now send the message
    */
    ret = north_lbg_send(lbg_id,xmit_buf);
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);

    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p, 25);
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
    return -1;    
}


/*
**__________________________________________________________________
*/
/**
* API for creation a transaction towards an exportd


 @param eid     : reference of the export
 @param fid     : unique reference of a filesystem object (file, directory, etc...)
 @param prog       : program
 @param vers       : program version
 @param opcode     : metadata opcode
 @param encode_fct : encoding function
 @msg2encode_p     : pointer to the message to encode
 @param recv_cbk   : receive callback function
 @param ctx_p      : pointer to the user context
 
 @retval 0 on success;
 @retval -1 on error,, errno contains the cause
 */
int expgw_export_build_and_route_common(uint32_t eid,fid_t fid,uint32_t prog,uint32_t vers,
                                       int opcode,xdrproc_t encode_fct,void *msg2encode_p,
                                       sys_recv_pf_t recv_cbk,void *ctx_p) 
{
    DEBUG_FUNCTION;
   
    uint8_t           *arg_p;
    uint32_t          *header_size_p;
    rozofs_tx_ctx_t   *rozofs_tx_ctx_p = NULL;
    void              *xmit_buf = NULL;
    int               bufsize;
    int               ret;
    int               position;
    XDR               xdrs;    
	struct rpc_msg   call_msg;
    uint32_t         null_val = 0;
    int lbg_id;

    expgw_ctx_t *req_ctx_p = (expgw_ctx_t*) ctx_p;        
    expgw_tx_routing_ctx_t  *routing_ctx_p = &req_ctx_p->expgw_routing_ctx ;
    /*
    ** get the available load balancing group(s) for routing the request 
    */    
    ret  = expgw_get_export_routing_lbg_info(eid,fid,routing_ctx_p);
    if (ret < 0)
    {
      /*
      ** no load balancing group available
      */
      errno = EPROTO;
      goto error;    
    }
    /*
    ** allocate a transaction context
    */
    rozofs_tx_ctx_p = rozofs_tx_alloc();  
    if (rozofs_tx_ctx_p == NULL) 
    {
       /*
       ** out of context
       ** --> put a pending list for the future to avoid repluing ENOMEM
       */
       TX_STATS(ROZOFS_TX_NO_CTX_ERROR);
       errno = ENOMEM;
       goto error;
    }    
    /*
    ** allocate an xmit buffer
    */  
    xmit_buf = ruc_buf_getBuffer(ROZOFS_TX_LARGE_TX_POOL);
    if (xmit_buf == NULL)
    {
      /*
      ** something rotten here, we exit we an error
      ** without activating the FSM
      */
      TX_STATS(ROZOFS_TX_NO_BUFFER_ERROR);
      errno = ENOMEM;
      goto error;
    } 
    /*
    ** The system attempts first to forward the message toward load balancing group
    ** of an export gateway and then to the master export if the load balancing group
    ** of the export gateway is not available
    */
    lbg_id = expgw_routing_get_next(routing_ctx_p,xmit_buf);
    /*
    ** store the reference of the xmit buffer in the transaction context: might be useful
    ** in case we want to remove it from a transmit list of the underlying network stacks
    */
    rozofs_tx_save_xmitBuf(rozofs_tx_ctx_p,xmit_buf);
    /*
    ** get the pointer to the payload of the buffer
    */
    header_size_p  = (uint32_t*) ruc_buf_getPayload(xmit_buf);
    arg_p = (uint8_t*)(header_size_p+1);  
    /*
    ** create the xdr_mem structure for encoding the message
    */
    bufsize = ruc_buf_getMaxPayloadLen(xmit_buf);
    xdrmem_create(&xdrs,(char*)arg_p,bufsize,XDR_ENCODE);
    /*
    ** fill in the rpc header
    */
    call_msg.rm_direction = CALL;
    /*
    ** allocate a xid for the transaction 
    */
	call_msg.rm_xid             = rozofs_tx_alloc_xid(rozofs_tx_ctx_p); 
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	/* XXX: prog and vers have been long historically :-( */
	call_msg.rm_call.cb_prog = (uint32_t)prog;
	call_msg.rm_call.cb_vers = (uint32_t)vers;
	if (! xdr_callhdr(&xdrs, &call_msg))
    {
       /*
       ** THIS MUST NOT HAPPEN
       */
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;	
    }
    /*
    ** insert the procedure number, NULL credential and verifier
    */
    XDR_PUTINT32(&xdrs, (int32_t *)&opcode);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
        
    /*
    ** ok now call the procedure to encode the message
    */
    if ((*encode_fct)(&xdrs,msg2encode_p) == FALSE)
    {
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;
    }
    /*
    ** Now get the current length and fill the header of the message
    */
    position = XDR_GETPOS(&xdrs);
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
    ** store the receive call back and its associated parameter
    */
    rozofs_tx_ctx_p->recv_cbk   = recv_cbk;
    rozofs_tx_ctx_p->user_param = ctx_p;    
    /*
    ** now send the message
    */
    ret = north_lbg_send(lbg_id,xmit_buf);
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);

    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p, 25);
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
    return -1;    
}


/*
**__________________________________________________________________
*/
/**
* API for re-sending a rpc message towards an exportd
 Here it is assumed that the transaction context is already allocated and ready to use
 The routing context has a buffer available and is already encoded (xdr)
 
 Only the transaction xid of the rpc message will be changed.
 
 note : that function is intended to be called by expgw_generic_export_reply_cbk()

 @param rozofs_tx_ctx_p        : transaction context
 @param recv_cbk        : callback function (may be NULL)
 @param req_ctx_p       : exportd gateway context (associated with a request comming from either an export gateway or rozofsmount)
 @param vers       : program version
 
 @retval 0 on success;
 @retval -1 on error,, errno contains the cause
 */
int expgw_resend_routing_common(rozofs_tx_ctx_t *rozofs_tx_ctx_p, sys_recv_pf_t recv_cbk,expgw_ctx_t *req_ctx_p) 
{
    DEBUG_FUNCTION;
   
    void              *xmit_buf = NULL;
    int               ret;
    int lbg_id;    
    
    expgw_tx_routing_ctx_t  *routing_ctx_p = &req_ctx_p->expgw_routing_ctx ;


    /*
    ** get the xmit buffer from the current routing context
    */  
    xmit_buf = routing_ctx_p->xmit_buf;
    if (xmit_buf == NULL)
    {
      /*
      ** something rotten here, we exit we an error
      ** without activating the FSM
      */
      severe("expgw_resend_routing_common : not xmit_buf in routing context");
      TX_STATS(ROZOFS_TX_NO_BUFFER_ERROR);
      errno = ENOMEM;
      goto error;
    } 
    /*
    ** The system attempts first to forward the message toward load balancing group
    ** of an export gateway and then to the master export if the load balancing group
    ** of the export gateway is not available
    */
    lbg_id = expgw_routing_get_next(routing_ctx_p,xmit_buf);
    if (lbg_id == -1)
    {
      errno = EPROTO;
      goto error;              
    }
    /*
    ** store the reference of the xmit buffer in the transaction context: might be useful
    ** in case we want to remove it from a transmit list of the underlying network stacks
    */
    rozofs_tx_save_xmitBuf(rozofs_tx_ctx_p,xmit_buf);
    /*
    ** get the pointer to the payload of the buffer
    */
    rozofs_rpc_call_hdr_with_sz_t *com_hdr_p  = (rozofs_rpc_call_hdr_with_sz_t*) ruc_buf_getPayload(xmit_buf);  
    com_hdr_p->hdr.xid = ntohl(rozofs_tx_alloc_xid(rozofs_tx_ctx_p)); ; 
    /*
    ** store the receive call back and its associated parameter
    */
    if (recv_cbk != NULL) {
      rozofs_tx_ctx_p->recv_cbk   = recv_cbk;
    }
    rozofs_tx_ctx_p->user_param = req_ctx_p; 
    /*
    ** now send the message
    */
reloop:
    ret = north_lbg_send(lbg_id,xmit_buf);
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       /*
       ** attempt to get the next available load balancing group
       */
       lbg_id = expgw_routing_get_next(routing_ctx_p,xmit_buf);
       if (lbg_id >= 0) goto reloop;
       
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);
    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p, 25);
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
    return -1;    
}

/*
**__________________________________________________________________
*/
/**
*  Call back function call upon a success rpc, timeout or any other rpc failure
*
 @param this : pointer to the transaction context
 @param param: pointer to the associated gateway context
 
 @return none
 */

void expgw_generic_export_reply_cbk(void *this,void *param) 
{
   struct rpc_msg  rpc_reply;
   char argument[EXPGW_RPC_MAX_DECODE_BUFFER];
   expgw_ctx_t *req_ctx_p = (expgw_ctx_t*) param;
   epgw_mattr_ret_t *ret;  
   int status;
   uint8_t  *payload;
   void     *recv_buf = NULL;   
   XDR       xdrs;    
   int      bufsize;
   xdrproc_t decode_proc = req_ctx_p->xdr_result;
   int xdr_free_done = 0;
   
   
   ret = (epgw_mattr_ret_t*)argument;
   
   memset(ret,0,EXPGW_RPC_MAX_DECODE_BUFFER);

   rpc_reply.acpted_rply.ar_results.proc = NULL;
    /*
    ** get the pointer to the transaction context:
    ** it is required to get the information related to the receive buffer
    */
    rozofs_tx_ctx_t      *rozofs_tx_ctx_p = (rozofs_tx_ctx_t*)this;     
    /*    
    ** get the status of the transaction -> 0 OK, -1 error (need to get errno for source cause
    */
    status = rozofs_tx_get_status(this);
    if (status < 0)
    {
       /*
       ** something wrong happened
       */
       errno = rozofs_tx_get_errno(this);  
       goto error; 
    }
    /*
    ** get the pointer to the receive buffer payload
    */
    recv_buf = rozofs_tx_get_recvBuf(this);
    if (recv_buf == NULL)
    {
       /*
       ** something wrong happened
       */
       errno = EFAULT;  
       goto error;         
    }
    payload  = (uint8_t*) ruc_buf_getPayload(recv_buf);
    payload += sizeof(uint32_t); /* skip length*/
    /*
    ** OK now decode the received message
    */
    bufsize = (int) ruc_buf_getPayloadLen(recv_buf);
    xdrmem_create(&xdrs,(char*)payload,bufsize,XDR_DECODE);
    /*
    ** decode the rpc part
    */
    if (rozofs_xdr_replymsg(&xdrs,&rpc_reply) != TRUE)
    {
     TX_STATS(ROZOFS_TX_DECODING_ERROR);
     errno = EPROTO;
     goto error;
    }
    /*
    ** ok now call the procedure to encode the message
    */
    if (decode_proc(&xdrs,ret) == FALSE)
    {
       TX_STATS(ROZOFS_TX_DECODING_ERROR);
       errno = EPROTO;
       goto error;
    } 
    /*
    ** check if the remote supports the eid
    ** if the eid is not supported by the remote forward the request to the exportd
    */  
    if (ret->status_gw.status == EP_FAILURE_EID_NOT_SUPPORTED) 
    {  
 
        /*
        ** Do not try to select this server again for the eid
        ** but directly send to the exportd
        */
        expgw_routing_expgw_for_eid(&req_ctx_p->expgw_routing_ctx, ret->hdr.eid, EXPGW_DOES_NOT_SUPPORT_EID);       

        xdr_free((xdrproc_t) decode_proc, (char *) &ret); 
        xdr_free_done = 1;   

        /* 
        ** Attempt to re-send the request to the exportd and wait being
        ** called back again. One will use the same buffer, just changing
        ** the xid.
        */
        status = expgw_resend_routing_common(rozofs_tx_ctx_p, NULL,req_ctx_p); 
        if (status == 0) 
        {
          /*
          ** do not forget to release the received buffer
          */
          ruc_buf_freeBuffer(recv_buf);
          recv_buf = NULL;
          return;
        }        
        /*
        ** Not able to resend the request
        */
        errno = EPROTO; /* What else ? */
        goto error;

    }  
    goto out;
error:
    /*
    ** release the received buffer if one was present
    */
    if (recv_buf != NULL)
    {
       ruc_buf_freeBuffer(recv_buf);
       recv_buf = NULL;
    }

    ret->status_gw.status = EP_FAILURE;    
    ret->status_gw.ep_mattr_ret_t_u.error = errno;
    ret->parent_attr.status = EP_EMPTY;    
out:
    /*
    ** release the transaction context and the gateway context
    */
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);    
    /*
    ** call the user callback for returned parameter interpretation: caution recv_buf might be NULL!!
    */
    req_ctx_p->decoded_arg = ret;
    (*req_ctx_p->response_cbk)(req_ctx_p,recv_buf);  
    /*
    ** do not forget to release data allocated for xdr decoding
    */
    if (xdr_free_done == 0) xdr_free((xdrproc_t) decode_proc, (char *) ret);
    return;
}

