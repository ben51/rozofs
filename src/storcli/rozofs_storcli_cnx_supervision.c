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

#include <config.h>
#include <rozofs/rozofs.h>
#include <rozofs/common/list.h>
#include <rozofs/common/log.h>
#include <rozofs/common/profile.h>
#include <rozofs/rpc/sproto.h>
#include <rozofs/core/rozofs_tx_common.h>
#include <rozofs/core/rozofs_tx_api.h>
#include <rozofs/core/north_lbg_api.h>
#include "rozofs_storcli.h"
#include <rozofs/rozofs_timer_conf.h>

/*
**____________________________________________________
*/
/**
* API for creation a transaction towards an storaged


 For making that API generic, the caller is intended to provide the function that
 will encode the message in XDR format. The source message that is encoded is 
 supposed to be pointed by msg2encode_p.
 Since the service is non-blocking, the caller MUST provide the callback function 
 that will be used for decoding the message
 

 @param sock_p     : pointer to the connection context (AF_INET or AF_UNIX)
 @param prog       : program
 @param vers       : program version
 @param opcode     : metadata opcode
 @param encode_fct : encoding function
 @msg2encode_p     : pointer to the message to encode
 @param xmit_buf : pointer to the buffer to send, in case of error that function release the buffer
 @param extra_len  : extra length to add after encoding RPC (must be 4 bytes aligned !!!)
 @param recv_cbk   : receive callback function
 @param applicative_tmo_sec : guard timer for the transaction

 @param user_ctx_p : pointer to the working context
 
 @retval 0 on success;
 @retval -1 on error,, errno contains the cause
 */

int rozofs_sorcli_poll_tx(af_unix_ctx_generic_t  *sock_p,
                          uint32_t prog,uint32_t vers,
                          int opcode,xdrproc_t encode_fct,void *msg2encode_p,
                          void *xmit_buf,
                          int      extra_len,  
                          int applicative_tmo_sec,                          
                          sys_recv_pf_t recv_cbk,void *user_ctx_p) 
{
    DEBUG_FUNCTION;
   
    uint8_t           *arg_p;
    uint32_t          *header_size_p;
    rozofs_tx_ctx_t   *rozofs_tx_ctx_p = NULL;
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
    ** add the extra_len if any
    */
    position +=extra_len;
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
    rozofs_tx_ctx_p->user_param = user_ctx_p;    
    rozofs_tx_write_opaque_data(rozofs_tx_ctx_p,1,1);  /* lock */
    /*
    ** now send the message
    */
    ret = af_unix_generic_stream_send(sock_p,xmit_buf); 
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);
    /*
    ** just iunlock the context and don't care about the end of transaction
    ** the transaction might end because of a direct error sending (tcp 
    ** disconnection)
    **
    ** By not releasing the tx context the end of the transaction ends upon receiving
    ** the tx timer expiration
    */
    rozofs_tx_write_opaque_data(rozofs_tx_ctx_p,1,0);  /* unlock */    
    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p,applicative_tmo_sec);  
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
    return -1;    
}
/*
**____________________________________________________
*/
/**
*  
  Applicative Polling of a TCP connection towards Storage
  @param sock_p : pointer to the connection context
  
  @retval none
 */
  
extern void rozofs_storcli_storage_poll_cbk(void *this,void *param); 

void storcli_lbg_cnx_polling(af_unix_ctx_generic_t  *sock_p)
{
  void *xmit_buf = NULL;
  int ret;
  int timeout = (int)ROZOFS_TMR_GET(TMR_RPC_NULL_PROC_TCP);

  af_inet_set_cnx_tmo(sock_p,timeout*10);
  /*
  ** attempt to poll
  */
   xmit_buf = ruc_buf_getBuffer(ROZOFS_STORCLI_SOUTH_LARGE_POOL);
   if (xmit_buf == NULL)
   {
      return ; 
   }
   ret =  rozofs_sorcli_poll_tx(sock_p,STORAGE_PROGRAM,STORAGE_VERSION,SP_NULL,
                                       (xdrproc_t) xdr_void, (caddr_t) NULL,
                                        xmit_buf,
                                        0,
                                        timeout,   /** TMO in secs */
                                        rozofs_storcli_storage_poll_cbk,
                                        (void*)NULL);
  if (ret < 0)
  {
   /*
   ** direct need to free the xmit buffer
   */
   ruc_buf_freeBuffer(xmit_buf);    
  }
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

void rozofs_storcli_storage_poll_cbk(void *this,void *param) 
{
    uint32_t lock;
   rozofs_tx_read_opaque_data(this,1,&lock);  
   if ( lock == 0) rozofs_tx_free_from_ptr(this);
   return;
}

