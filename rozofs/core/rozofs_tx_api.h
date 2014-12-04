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

 #ifndef ROZOFS_TX_API_H
 #define ROZOFS_TX_API_H
 
#include <stdlib.h>
#include <stddef.h>
#include "rozofs_tx_common.h"
#include "ruc_buffer_api.h"
/**
* transaction statistics
*/
typedef enum 
{
  ROZOFS_TX_SEND =0 ,
  ROZOFS_TX_SEND_ERROR,
  ROZOFS_TX_RECV_OK, 
  ROZOFS_TX_RECV_OUT_SEQ,
  ROZOFS_TX_TIMEOUT,
  ROZOFS_TX_ENCODING_ERROR,
  ROZOFS_TX_DECODING_ERROR,
  ROZOFS_TX_NO_CTX_ERROR,
  ROZOFS_TX_CTX_MISMATCH,
  ROZOFS_TX_NO_BUFFER_ERROR,
  ROZOFS_TX_COUNTER_MAX
}rozofs_tx_stats_e;

extern uint64_t rozofs_tx_stats[];

#define TX_STATS(counter) rozofs_tx_stats[counter]++;

/**
* Buffers information
*/
extern int rozofs_small_tx_xmit_count;
extern int rozofs_small_tx_xmit_size;
extern int rozofs_large_tx_xmit_count;
extern int rozofs_large_tx_xmit_size;
extern int rozofs_small_tx_recv_count;
extern int rozofs_small_tx_recv_size;
extern int rozofs_large_tx_recv_count;
extern int rozofs_large_tx_recv_size;

/**
* Buffer Pools
*/
typedef enum 
{
  _ROZOFS_TX_SMALL_TX_POOL =0 ,
  _ROZOFS_TX_LARGE_TX_POOL, 
  _ROZOFS_TX_SMALL_RX_POOL,
  _ROZOFS_TX_LARGE_RX_POOL,
  _ROZOFS_TX_MAX_POOL
}rozofs_tx_buffer_pool_e;

extern void *rozofs_tx_pool[];


#define ROZOFS_TX_SMALL_TX_POOL rozofs_tx_pool[_ROZOFS_TX_SMALL_TX_POOL]
#define ROZOFS_TX_LARGE_TX_POOL rozofs_tx_pool[_ROZOFS_TX_LARGE_TX_POOL]
#define ROZOFS_TX_SMALL_RX_POOL rozofs_tx_pool[_ROZOFS_TX_SMALL_RX_POOL]
#define ROZOFS_TX_LARGE_RX_POOL rozofs_tx_pool[_ROZOFS_TX_LARGE_RX_POOL]
/**
**____________________________________________________
*  Get the number of free context in the transaction context distributor

  @param none
  @retval <>NULL, success->pointer to the allocated context
  @retval NULL, error ->out of context
*/
int rozofs_tx_get_free_ctx_number(void);

/*
**____________________________________________________
*/
/**
  Get the receive buffer associated with the current transaction
  
   @param this : pointer to the transaction context
   
   @retval spointer to the receive buffer or null
*/   
static inline void *rozofs_tx_get_recvBuf(rozofs_tx_ctx_t *this)
{
  void *buf;
  buf = this->recv_buf ;
  this->recv_buf = NULL;
  return buf;

}

/*
**____________________________________________________
*/
/**
  Clear the receive buffer reference
  
   @param this : pointer to the transaction context
   
   @retval none
*/   
static inline void *rozofs_tx_clear_recvBuf_ref(rozofs_tx_ctx_t *this)
{
  return this->recv_buf = NULL;
}

/*-----------------------------------------------
**   rozofs_tx_alloc

**  create a Transaction context
**   That function tries to allocate a free PDP
**   context. In case of success, it returns the
**   index of the Transaction context.
**
@param     : recli index
@param       relayCref : RELAY-C ref of the context
@retval   : MS controller reference (if OK)
@retval    NULL if out of context.
*/
rozofs_tx_ctx_t *rozofs_tx_alloc();

/*
**____________________________________________________
*/
/**
   delete a Transaction context
   
   That function is intended to be called when
   a Transaction context is deleted. It returns the
   Transaction context to the free list. The delete
   procedure of the MS automaton and
   controller are called by that service.

   If the Transaction context is out of limit, and 
   error is returned.

@param     : MS Index
@retval   : RUC_OK : context has been deleted
@retval     RUC_NOK : out of limit index.
*/
uint32_t rozofs_tx_free_from_idx(uint32_t transaction_id);

/*
**____________________________________________________
*/
/**
   rozofs_tx_free_from_ptr

   delete a Transaction context
   That function is intended to be called when
   a Transaction context is deleted. It returns the
   Transaction context to the free list. The delete
   procedure of the MS automaton and
   controller are called by that service.

   If the Transaction context is out of limit, and 
   error is returned.

@param     : pointer to the transaction context
@retval   : RUC_OK : context has been deleted
@retval     RUC_NOK : out of limit index.

*/
uint32_t rozofs_tx_free_from_ptr(rozofs_tx_ctx_t *p);

/*
**____________________________________________________
*/
/**
  Get the status of transaction
  
   @param this : pointer to the transaction context
   
   @retval status of the transaction
*/   
static inline int rozofs_tx_get_status(rozofs_tx_ctx_t *this)
{
  return this->status;

}
/*
**____________________________________________________
*/
/**
  Get the status of transaction
  
   @param this : pointer to the transaction context
   
   @retval status of the transaction
*/   
static inline int rozofs_tx_get_errno(rozofs_tx_ctx_t *this)
{
  return this->tx_errno;

}

/*
**____________________________________________________
*/
/*
  stop the guard timer associated with the transaction

@param     :  tx_p : pointer to the transaction context
@retval   : none
*/

void rozofs_tx_stop_timer(rozofs_tx_ctx_t *pObj);

/*
**____________________________________________________
*/
/*
  start the guard timer associated with the transaction

@param     : tx_p : pointer to the transaction context
@param     : uint32_t  : delay in seconds (??)
@retval   : none
*/
void rozofs_tx_start_timer(rozofs_tx_ctx_t *tx_p,uint32_t time_ms) ;

/*
   rozofs_tx_module_init

  create the Transaction context pool

@param     : transaction_count : number of Transaction context
@retval   : RUC_OK : done
@retval          RUC_NOK : out of memory
*/
uint32_t rozofs_tx_module_init(uint32_t transaction_count,
                             int max_small_tx_xmit_count,int max_small_tx_xmit_size,
                             int max_large_tx_xmit_count,int max_large_tx_xmit_size,
                             int max_small_tx_recv_count,int max_small_recv_xmit_size,
                             int max_large_tx_recv_count,int max_large_recv_xmit_size
                             );


/*
*________________________________________________________
*/
/**  allocate an xid for a transaction
*  The system uses the index of the transaction context combined with a
*  16 bits counter that belongs to the transaction context
* the resulting xid is stored in the transaction context for later usage upon
  RPC reply reception

  @param call_p : pointer to the transaction context
  retval  value of the xid
*/
static inline uint32_t rozofs_tx_alloc_xid(rozofs_tx_ctx_t *tx_p)
{
  uint32_t  xid = (tx_p->xid_low++) & (0xffff);
  if (xid == 0) 
  {
    xid = 1;
    tx_p->xid_low = 1;
  }
  tx_p->xid = ((tx_p->index<<16) | xid);
  return  tx_p->xid;

}

/*
*________________________________________________________
*/
/**  Get the  context reference from the xid

  @param xid :xid value in host order
  retval  index of the  context
*/
static inline uint32_t rozofs_tx_get_tx_idx_from_xid(uint32_t xid )
{

  return ( xid >> 16)& 0xffff;

}

/**
*  API to get the size of the small xmit buffer
 @param none
 
 @retval sise of the small buffer
*/
static inline int rozofs_tx_get_small_buffer_size()
{
  return rozofs_small_tx_xmit_size;
}


/**
*  API to write an opaque user data

 @param p : pointer to the transaction context
 @param idx : index of the data
 @param data : data to write
 
 @retval 0 on success
 @retval -1 on error
*/
static inline int rozofs_tx_write_opaque_data(rozofs_tx_ctx_t *p,uint8_t idx,uint32_t data)
{
  if (idx >= ROZOFS_TX_OPAQUE_MAX) return -1;
  p->opaque_usr[idx] = data;
  return 0;
}


/**
*  API to read an opaque user data

 @param p : pointer to the transaction context
 @param idx : index of the data
 @param data_p : pointer to the array where data is returned
 
 @retval 0 on success
 @retval -1 on error
*/
static inline int rozofs_tx_read_opaque_data(rozofs_tx_ctx_t *p,uint8_t idx,uint32_t *data_p)
{
  if (idx >= ROZOFS_TX_OPAQUE_MAX) return -1;
  *data_p = p->opaque_usr[idx] ;
  return 0;

}

/**
*  API to store the reference of the xmit buffer
  Application are intended to use that service if they want to attempt
  releasing the xmit buffer while it is queued on the xmit queue
  of a load balancing group or any other transport stack.

 @param p : pointer to the transaction context
 @param xmit_buf :pointer to the xmit buffer
 
 @retval none
*/
static inline void rozofs_tx_save_xmitBuf(rozofs_tx_ctx_t *p,void *xmit_buf)
{
    ruc_buf_inuse_increment(xmit_buf);
    p->xmit_buf = xmit_buf;

}

/*
**____________________________________________________
*/
/**
* Transaction abort callback:

  That callback is called when the load balancing group fails to send a rpc request
  Thaemajor cause of the failure is that all the TCP connections are down and retry counter of the request
  is exhauted.
 
  
  @param userRef: not used (not significant, always NULL) 
  @param lbg_id: reference of the load balancing group
  @param xmit_buf: pointer to the xmit buffer that contains the RPC request
  @param errcode: error encountered (errno value)
*/
void rozofs_tx_xmit_abort_rpc_cbk(void *userRef,uint32_t  lbg_id, void *xmit_buf,int errcode);

/*
**____________________________________________________
*/
/**
*  transaction receive callback associated with the RPC protocol
  This corresponds to the callback that is call upon the
  reception of the transaction reply from the remote end
  
  The input parameter is a receive buffer belonging to
  the transaction egine module
  
  @param socket_ctx_p: pointer to the af unix socket: not used 
  @param lbg_id: reference of the load balancing group
  @param recv_buf: pointer to the receive buffer
*/
void rozofs_tx_recv_rpc_cbk(void *userRef,uint32_t  lbg_id, void *recv_buf);

/*
**____________________________________________________
*/
/**
*  
  Analyze the rpc header to extract the length of the rpc message
  The RPC header is a partial header and contains only the length of the rpc message
  That header is in network order format
  
  @param hdr_p: pointer to the beginning of a rpc message (length field)
  
  @retval length of the rpc payload
*/
uint32_t rozofs_tx_get_rpc_msg_len_cbk(char *hdr_p);


/*
**____________________________________________________
*/
/**
*  
  Callback to allocate a buffer for receiving a rpc message (mainly a RPC response
 
 
 The service might reject the buffer allocation because the pool runs
 out of buffer or because there is no pool with a buffer that is large enough
 for receiving the message because of a out of range size.

 @param userRef : pointer to a user reference: not used here
 @param socket_context_ref: socket context reference
 @param len : length of the incoming message
 
 @retval <>NULL pointer to a receive buffer
 @retval == NULL no buffer 
*/
void * rozofs_tx_userRcvAllocBufCallBack(void *userRef,uint32_t socket_context_ref,uint32_t len);

/*
**____________________________________________________
*/
/*  
 Get a global transaction identifier
  CAUTION: that API must not be used when working with a transaction
  context since the xid associated with a transaction context is
  a compound value that contains the reference of the transaction context
  and a 16 bits counter used as a xid.

@retval          transaction identifier
*/
extern uint32_t  rozofs_tx_global_transaction_id;

static inline uint32_t  rozofs_tx_get_transaction_id( )
{
  rozofs_tx_global_transaction_id++;
  if (rozofs_tx_global_transaction_id == 0) rozofs_tx_global_transaction_id++;
  return rozofs_tx_global_transaction_id;
}

#endif

