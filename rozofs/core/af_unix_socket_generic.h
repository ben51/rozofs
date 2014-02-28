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


#ifndef AF_UNIX_SOCKET_GENERIC_H
#define AF_UNIX_SOCKET_GENERIC_H
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/un.h>
//#include <arpa/inet.h>
//#include <netinet/in.h>
//#include <netdb.h>
//#include <netinet/tcp.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

#include <rozofs/common/types.h>
#include <rozofs/common/log.h>

#include "uma_tcp.h"
#include "ruc_common.h"


/**
* Callback that returns the length of the received header
  @param *hdr_p: pointer to the received header
  
  @retval: length of the payload (Must be different from 0)
  @retval: 0 if the payload is greater than the maximum supported of if there is an error in the interpretation
*/
typedef uint32_t (*generic_hdr_getsz_CBK_t)(char *bufRef);
/**
* Callback definition
*/
typedef void (*generic_recv_CBK_t)(void *userRef,uint32_t socket_context_ref,void *bufRef);
/**
* Call back upon socket disconnection
*/
typedef void (*generic_disc_CBK_t)(void *userRef,uint32_t socket_context_ref,void *bufRef,int errnum);
/**
* call back for application that rely on socket to get an xmit buffer
*/
typedef void (*generic_xmit_CBK_t)(uint32_t userRef,void * buffer,uint8_t status);
/**
* Call back for socket context that rely on application for receive buffer allocation
*/
typedef void* (*generic_recvBufAll_CBK_t)(void *userRef,uint32_t socket_context_ref,uint32_t len);
/**
* Call back upon socket connect status
**  userRef : opaque
** socket_context_ref : index of the socket
** retcode: RUC_OK or RUC_NOK
** errnum : errno 0 for RUC_OK
*/
typedef void (*generic_connect_CBK_t)(void *userRef,uint32_t socket_context_ref,int retcode,int errnum);
/**
* cal used when the application wants to be warned when its buffers has been sent
*
  @param : userRef: opaque valude that depends on the application
  @param socket_context_ref : reference of the socket context
  @param   buffer: pointer to the sent buffer
*/
typedef void (*generic_xmitDone_CBK_t)(void *userRef,uint32_t socket_context_ref,void * buffer);


/**
* Call back for connection supervision from application
  @param *sock_p: pointer to cnnection context
  
  @retval: none
*/
typedef void (*af_stream_poll_CBK_t)(void *sock_p);


/**
* Call back for connection supervision state change 
  @param *user_param: opaque value
  
  @retval: none
*/
typedef void (*af_stream_avail_CBK_t)(uint32_t param);


/**
* service type for stream reception
*/
typedef enum _com_stream_recv_service_e
{
     ROZOFS_GENERIC_SRV = 0,  /**< generic receiver  */
     ROZOFS_RPC_SRV,     /**< RPC stream receiver-> is able to deal with the record concept of rpc */
} com_stream_recv_service_e;


/**
*   Structure used for configuring an AF_UNIX socket
*/
typedef struct _af_unix_socket_conf_t
{
  int           family;        /**< identifier of the socket family    */
  int           instance_id;        /**< instance number within the family   */
  uint32_t        headerSize;       /* size of the header to read                 */
  uint32_t        msgLenOffset;     /* offset where the message length fits       */
  uint32_t        msgLenSize;       /* size of the message length field in bytes  */
  uint32_t        bufSize;         /* length of buffer (xmit and received)        */
  uint32_t        so_sendbufsize;  /* length of buffer (xmit and received)        */
  generic_recvBufAll_CBK_t   userRcvAllocBufCallBack;   /* user callback for buffer allocation */
  generic_recv_CBK_t         userRcvCallBack;   /* callback provided by the connection owner block */
  generic_disc_CBK_t         userDiscCallBack; /* callBack for TCP disconnection detection         */
  generic_connect_CBK_t      userConnectCallBack; /**< callback for client connection only         */
  generic_xmitDone_CBK_t     userXmitDoneCallBack; /**< optional call that must be set when the application when to be warned when packet has been sent */
  ruc_pf_sock_t              userRcvReadyCallBack; /* NULL for default callback                    */
  ruc_pf_sock_t              userXmitReadyCallBack; /* NULL for default callback                    */
  ruc_pf_sock_t              userXmitEventCallBack; /* NULL for default callback                    */
  generic_hdr_getsz_CBK_t    userHdrAnalyzerCallBack; /* NULL by default, function that analyse the received header that returns the payload  length  */
  com_stream_recv_service_e  recv_srv_type;   /**< stream receiver service type: ROZOFS_GENERIC_SRV or ROZOFS_RPC_SRV */
  uint32_t                   rpc_recv_max_sz; /**< rpc stream receiver max message size             */
  void          *userRef;           /* user reference that must be recalled in the callbacks */
  void        *xmitPool; /* user pool reference or -1 */
  void        *recvPool; /* user pool reference or -1 */
} af_unix_socket_conf_t;


/**
* Transmitter FSM states
*/
typedef enum _com_xmit_state_e
{
     XMIT_IDLE = 0,  /**< socket has not yet been created  */
     XMIT_READY,     /**< transmitter is ready and the pending xmit is empty */
     XMIT_IN_PRG,    /**< transmitter is ready and the pending xmitmight not be empty */
     XMIT_CHECK_XMITQ,   /**< unstable state for check the pending transmit queue */
     XMIT_CONGESTED,     /**< the transmitter is congested   */
     XMIT_DEAD           /**< the destination is dead        */
} com_xmit_state_e;



/**
* Receiver FSM states
*/
typedef enum _com_recv_state_e
{
     RECV_IDLE = 0,      /**< no reception in progress  */
     RECV_WAIT_HDR,      /**< wait for the full header message */
     RECV_ALLOC_BUF,     /**< allocate buffer for receiving message: might remain in that state if there is none available */
     RECV_PAYLOAD,       /**< message payload reception */
     RECV_DEAD,         /**< receiver is dead because of a fatal error on socket or buffer allocation   */
} com_recv_state_e;


/**
* That structure contains the statistics of a socket : might be common for all
* sockets
*/
typedef struct rozofs_socket_stats_t
{
   uint64_t totalUpDownTransition; /**< total number of messages submitted for with EWOULDBLOCK is returned  */
   /*
   ** xmit side
   */
   uint64_t totalXmitBytes;         /**< total number of bytes that has been sent */
   uint64_t totalXmitAttempts;  /**< total number of messages submitted       */
   uint64_t totalXmitAttemptsCycles;  /**< total number of messages submitted reset on show      */
   uint64_t totalXmitCycles;    /**< number of cpu cycles       */
   uint64_t totalXmitSuccess;   /**< total number of messages submitted with success  */
   uint64_t totalXmitCongested; /**< total number of messages submitted for with EWOULDBLOCK is returned  */
   uint64_t totalXmitError;     /**< total number of messages submitted with an error  */

   /*
   ** xmit side
   */
   uint64_t totalRecvBytes;         /**< total number of bytes that has been sent */
   uint64_t totalRecv;  /**< total number of messages submitted       */
   uint64_t totalRecvSuccess;   /**< total number of messages submitted with success  */
   uint64_t totalRecvBadHeader;     /**< total number of messages submitted with an error  */
   uint64_t totalRecvBadLength;     /**< total number of messages submitted with an error  */
   uint64_t totalRecvOutoFBuf;     /**< total number of messages submitted with an error  */
   uint64_t totalRecvError;     /**< total number of messages submitted with an error  */
} rozofs_socket_stats_t;

#define AF_UNIX_CONGESTION_DEFAULT_THRESHOLD 2  /**< number of loop before restarting to send after
eoc */
#define AF_UNIX_XMIT_CREDIT_DEFAULT 8
#define AF_UNIX_RECV_CREDIT_DEFAULT 4
/**
* transmitter generic Context
*/
typedef struct _com_xmit_template_t
{
   void         *xmitPoolOrigin;     /**<current pool reference                */
   void         *xmitPoolRef;        /**< head of the current xmit buffer pool */
   uint8_t       state;              /**< xmit fsm state                       */

   uint8_t       eoc_flag:1;         /**< fin de congestion flag               */
   uint8_t       congested_flag:1;   /**< congested:1                          */
   uint8_t       xmit_req_flag:1;    /**< assert to 1 when xmit ready is required */
   uint8_t       filler_flag:5;      /**< congested:1                          */
   int           nbWrite;              /**< number of bytes that has been currently written, */
   int           nb2Write;              /**< total number of bytes that must be written     */
   void         *bufRefCurrent;      /**< reference of the current buffer to send or NULL if no buffer  */
   uint16_t      eoc_threshold;      /**< current EOC threshold                */
   uint16_t      eoc_threshold_conf;      /**< configured EOC threshold        */
   uint16_t      xmit_credit;         /**< current xmit credit                 */
   uint16_t      xmit_credit_conf;    /**< configured xmit credit              */
   ruc_obj_desc_t xmitList[UMA_MAX_TCP_XMIT_PRIO]; /* pending xmit list        */

} com_xmit_template_t;

#define    ROZOFS_MAX_HEADER_SIZE 128 /**< max size of a header */


typedef struct _com_rpc_recv_template_t
{
  uint32_t        receiver_active; /**< asserted to 1 when stream receiver is associated with rpc protocol */				
  uint32_t        in_tot_len;       /**< total length of the current message: sum of all records */
  uint32_t        record_len;       /**< length of the current record                      */
  uint32_t        last_record;      /**< asserted to 1 when last record is encountered     */
  uint32_t        in_wr_offset;     /**< write offset in the receive buffer              */
  uint32_t        max_receive_sz;   /**< max size of the receive buffer: need when first record has not bit 31 asserted */

} com_rpc_recv_template_t;


/**
* Receiver generic Context
*/
typedef struct _com_recv_template_t
{
  void         *bufRefCurrent;      /**< reference of the current buffer to send or NULL if no buffer  */
  void          *rcvPoolOrigin;     /**<current pool reference */
  void          *rcvPoolRef;        /* it could be either the reference of
                                    ** the user bufferv reference pool or
				                    ** the default one used by the TCP
				                    ** connection*/
  uint8_t       state;               /**< state of the receiver fsm                    */
  uint16_t      recv_credit_conf;    /**< configured receive  credit                   */
  int           nbread;              /**< number of bytes that has been currently read */
  int           nb2read;              /**< total number of bytes that must be read     */
  /*
  **  configuration parameters
  */
  uint32_t        headerSize;       /**<size of the header to read                  */
  uint32_t        msgLenOffset;     /**< offset where the message length fits       */
  uint32_t        msgLenSize;       /**< size of the message length field in bytes  */
  uint32_t        bufSize;         /**< length of buffer (xmit and received)        */
  uint8_t       buffer_header[ROZOFS_MAX_HEADER_SIZE]; /**< array used for receiving the header of a message */
  /*
  **  dedicated RPC parameters
  */
  com_rpc_recv_template_t rpc;    /**< just to address the case of the rpc reception with multiple records */

} com_recv_template_t;

#define ROZOFS_SOCK_EXTNAME_SIZE 64
#define AF_UNIX_SOCKET_NAME_SIZE 64

#define AF_UNIX_CNX_AVAILABLE  0
#define AF_UNIX_CNX_UNAVAILABLE  1
/**
* structure used to supervise the activity of a tcp connection
*/
typedef union 
{
   uint64_t u64;
   struct {
   uint64_t check_cnx_enabled:1 ; /**< assert to one if enabled */
   uint64_t check_cnx_rq:1 ;      /**< assert to one when there is a buffer submitted */
   uint64_t check_cnx_rq_per_appli:1 ;/* When set to 1 the check_cnx_rq must be cleared by the application*/
   uint64_t timestamp:61 ;        /**< expiration date */
   } s;
} af_inet_check_cnx_t;
/**
* AF UNIX generic context
*/
typedef struct _af_unix_ctx_generic_t
{
  ruc_obj_desc_t    link;          /**< To be able to chain the MS context on any list */
  uint32_t            index;         /**<Index of the MS */
  uint32_t            free;          /**< Is the context free or allocated TRUE/FALSE */
  /*
  ** specific part
  */
  int           family;        /**< identifier of the socket family    */
  int           instance_id;        /**< instance number within the family   */
  char          nickname[ROZOFS_SOCK_EXTNAME_SIZE]; /**< name of the socket for socket controller display name */
  /*
  ** local socket info
  */
  int           af_family;        /**< either AF_INET or AF_UNIX   */
  /*
  ** AF_UNIX case
  */
  char          src_sun_path[AF_UNIX_SOCKET_NAME_SIZE]; /**< basename+instance id  */
  char          remote_sun_path[AF_UNIX_SOCKET_NAME_SIZE]; /**< basename+instance id  */
  /*
  ** AF_INET case
  */
  uint32_t      src_ipaddr_host;    /**< IP address in host format */
  uint32_t      src_port_host;      /**< source port in host format */
  uint32_t      remote_ipaddr_host;    /**< remote IP address in host format */
  uint32_t      remote_port_host;      /**< remote port in host format */

  int           socketRef;
  uint32_t        so_sendbufsize;  /* length of buffer (xmit and received): extracted from conf_p        */
  void         *connectionId;   /* reference of the socket controller */
  generic_recv_CBK_t         userRcvCallBack;   /* callback provided by the connection owner block */
  generic_recvBufAll_CBK_t   userRcvAllocBufCallBack;   /**< user callback for buffer allocation     */
  generic_disc_CBK_t         userDiscCallBack; /**< callBack for TCP disconnection detection         */
  ruc_pf_sock_t              userRcvReadyCallBack; /**< callBack for receiver ready                  */
  ruc_pf_sock_t              userXmitReadyCallBack; /**< default or application                      */
  ruc_pf_sock_t              userXmitEventCallBack; /**< default or application                      */
  generic_connect_CBK_t      userConnectCallBack; /**< callback for client connection only         */
  generic_xmitDone_CBK_t     userXmitDoneCallBack; /**< optional call that must be set when the application when to be warned when packet has been sent */
  generic_hdr_getsz_CBK_t    userHdrAnalyzerCallBack; /* NULL by default, function that analyse the received header that returns the payload  length  */
  af_stream_poll_CBK_t       userPollingCallBack;    /**< call that permits polling at application level */
  af_stream_avail_CBK_t      userAvailabilityCallBack;
  uint32_t                   availability_param;
  void          *userRef;           /**< user reference that must be recalled in the callbacks */
  af_unix_socket_conf_t *conf_p;  /**< for listening socket only */
  com_xmit_template_t   xmit;
  com_recv_template_t   recv;
  rozofs_socket_stats_t stats;
  af_inet_check_cnx_t   cnx_supevision; /**< supervision context */
  uint8_t               cnx_availability_state;  /**< operational state of the connection */  

} af_unix_ctx_generic_t;


extern ruc_sockCallBack_t af_unix_generic_listening_callBack_sock;
extern ruc_sockCallBack_t af_unix_generic_client_callBack_sock;
/*
**__________________________________________________________________________
*/
/**
*   PRIVATE API
*  Insert either a ruc_buffer or a ruc_buffer request in the pending list of
   a tranmitter. That service is called when the transmitter has already a
   buffer in transmission

 @param this : pointer to the xmit structure
 @param element_p : pointer to the ruc_buffer or ruc_buffer request
 @param elemnt_type : type of the element: UMA_XMIT_TYPE_XMIT_ASSOC or UMA_XMIT_TYPE_BUFFER
 @param priority : priority of the element
*/
 static inline void com_xmit_pendingQueue_insert(com_xmit_template_t *this,
                                                 void *element_p,uint8_t type,uint8_t prio)
 {
    if (prio > 1 ) prio = 1;
    ruc_objPutQueue(&this->xmitList[prio],(ruc_obj_desc_t*)element_p,type);
 }

/*
**__________________________________________________________________________
*/
/**
*  insert either a ruc_buffer or a request for a ruc buffer in the
   xmit queue of a socket

 @param this : pointer to the xmit structure
 @param buf_p : pointer to the ruc_buffer
 @param priority : priority of the element

 */
 static inline void com_xmit_pendingQueue_buffer_insert(com_xmit_template_t *this,void *buf_p,uint8_t prio)
 {

   return com_xmit_pendingQueue_insert(this,buf_p,UMA_XMIT_TYPE_BUFFER,prio);
 }

/*
**__________________________________________________________________________
*/
/**
*  insert a request for a ruc buffer in the xmit queue of a socket
  the request MUST be a "uma_xmit_assoc_t" stucture.
 @param this : pointer to the xmit structure
 @param buf_p :pointer to an association block provided by the calling application that request a buffer
 @param priority : priority of the element

 */
 static inline void com_xmit_pendingQueue_bufferReq_insert(com_xmit_template_t *this,void *buf_p,uint8_t prio)
 {

   return com_xmit_pendingQueue_insert(this,buf_p,UMA_XMIT_TYPE_XMIT_ASSOC,prio);
 }



/*
**__________________________________________________________________________
*/
/**
*  Read a pending Xmit queue: done at end of congestion

 @param this : pointer to the xmit structure
 @param priority : priority of the element

 @retval != NULL pointer to the ruc_buffer to transmit
 @retval NULL: xmit pending queue is empty for that priority
 */
static inline void * com_xmit_pendingQueue_get(com_xmit_template_t *this,uint8_t prio)
{
  ruc_obj_desc_t *bufRef;
  uma_xmit_assoc_t *pXmitReq;
  uint8_t   opcode = 0;
  uint8_t   status = RUC_OK;

  bufRef = (ruc_obj_desc_t*)ruc_objReadQueue(&this->xmitList[prio],&opcode);
  if (bufRef == (ruc_obj_desc_t*) NULL)
  {
    /*
    ** The queue is empty
    */
    return NULL;
  }
  /*
  ** OK, now check if it is a real buffer to transmit or a transmit
  ** request for which the transmitter must allocate an xmit buffer
  */
  switch (opcode)
  {
     case UMA_XMIT_TYPE_BUFFER:
      /*
      ** nothing more to do
      */
     break;
     case UMA_XMIT_TYPE_XMIT_ASSOC:
       pXmitReq = (uma_xmit_assoc_t*)bufRef;
       /*
       ** need to allocate a buffer from the free transmit buffer pool
       */
       bufRef = (ruc_obj_desc_t*)ruc_buf_getBuffer(this->xmitPoolRef);
       status = RUC_OK;
       if (bufRef == (ruc_obj_desc_t*)NULL)
       {
	     /*
	     **  out of xmit buffer, that 's not normal
	     */
	     status = RUC_NOK;
       }

       /*
       ** the caller is intended to fill the buffer
       ** in any it should release that buffer
       */
       (pXmitReq->xmitCall)((uint32_t)pXmitReq->userRef,bufRef,status);
     break;
     default:
       bufRef = NULL;
     break;
  }
  return bufRef;
}



/*
**__________________________________________________________________________
*/
/**
*  Purge the xmit queue of a context (only BUFFER type is supported

 @param socket_p : pointer to socket context
 @param priority : priority of the element

 @retval != NULL pointer to the ruc_buffer to transmit
 @retval NULL: xmit pending queue is empty for that priority
 */
static inline void com_xmit_pendingQueue_purge(af_unix_ctx_generic_t *socket_p,uint8_t prio)
{
  ruc_obj_desc_t *bufRef;
  uint8_t   opcode = 0;
  com_xmit_template_t *this = &socket_p->xmit;
  int inuse;


   while ((bufRef = (ruc_obj_desc_t*)ruc_objReadQueue(&this->xmitList[prio],&opcode))
               !=(ruc_obj_desc_t*)NULL)
  {
    /*
    ** OK, now check if it is a real buffer to transmit or a transmit
    ** request for which the transmitter must allocate an xmit buffer
    */
    switch (opcode)
    {
       case UMA_XMIT_TYPE_BUFFER:
        /*
        ** nothing more to do
        */
         inuse = ruc_buf_inuse_get(bufRef);
         if (socket_p->userXmitDoneCallBack != NULL)
         {
            /*
            ** caution: in that case it is up to the application that provides the callback to release
            ** the xmit buffer
            */
            (socket_p->userXmitDoneCallBack)(socket_p->userRef,socket_p->index,bufRef);
         }
         else
         {
           if (inuse == 1) ruc_buf_freeBuffer(bufRef);
         }
       break;

       default:
       break;
    }
  }
}


/*
**____________________________________________________
*/
/**
   af_unix_createIndex

  create a AF UNIX context given by index
   That function tries to allocate a free PDP
   context. In case of success, it returns the
   index of the Transaction context.

@param     : af_unix_ctx_id is the reference of the context
@retval   : MS controller reference (if OK)
retval     -1 if out of context.
*/
af_unix_ctx_generic_t *af_unix_alloc();

/*
**____________________________________________________
*/
/**
   delete a AF_UNIX context

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
uint32_t af_unix_free_from_idx(uint32_t af_unix_ctx_id);


/*
**____________________________________________________
*/
/**
   af_unix_free_from_ptr

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
uint32_t af_unix_free_from_ptr(af_unix_ctx_generic_t *p);

/*
**__________________________________________________________________________
*/
/**
*  AF_UNIX generic send:
  the ruc_buffer is supposed to contain the reference of the destination socket
  in the usrDestInfo field of the ruc_buffer header (for AF_UNIX it is the
  pathname of the destination socket)
  .
  To obtain the pointer to that array use the ruc_buf_get_usrDestInfo() service API
  Notice that the length of that array is limited to 64 bytes.

  @param this : pointer to the AF UNIX socket context
  @param buf_p: pointer to the buffer to send

  retval 0 : success
  retval -1 : error
*/
int af_unix_generic_send(af_unix_ctx_generic_t *this,void *buf_p);

int af_unix_generic_send_with_idx(int af_unix_socket_idx,void *buf_p);
/*
**__________________________________________________________________________
*/
/**
*  socket controller callbacks
*/
uint32_t af_unix_generic_rcvReadysock(void * socket_ctx_p,int socketId);
uint32_t af_unix_generic_rcvMsgsock(void * socket_ctx_p,int socketId);
uint32_t af_unix_generic_xmitReadysock(void * socket_ctx_p,int socketId);
uint32_t af_unix_generic_xmitEvtsock(void * socket_ctx_p,int socketId);

/*
**_____________________________________________
*/
/**
 based on the object index, that function
 returns the pointer to the object context.

 That function may fails if the index is
 not a Transaction context index type.
**
@param     : af_unix socket context index
@retval   : NULL if error
*/
af_unix_ctx_generic_t *af_unix_getObjCtx_p(uint32_t af_unix_ctx_id);


uint32_t af_unix_generic_rcvMsgsock(void * socket_ctx_p,int socketId);


/*
**__________________________________________________________________________
*/
/**
   Set a socket in the non-blocking mode and adjust xmit buffer size

   see /proc/sys/net/core for socket parameters:
     - wmem_default: default xmit buffer size
     - wmem_max : max allocatable
     Changing the max is still possible with root privilege:
     either edit /etc/sysctl.conf (permanent) :
     (write):
     net.core.wmem_default = xxxx
     net.core.wmem_max = xxxx
     (read):
     net.core.rmem_default = xxxx
     net.core.rmem_max = xxxx

     or temporary with:
     echo <new_value> > /proc/sys/net/core/<your_param>

   @param name0fSocket : name of the AF_UNIX socket
   @param size: size in byte of the xmit buffer (the service double that value

    retval: >0 reference of the created AF_UNIX socket
    retval < 0 : error on socket creation

*/
int af_unix_sock_set_non_blocking(int fd,int size);

/**
   Creation of a AF_UNIX socket Datagram  in non blocking mode

   see /proc/sys/net/core for socket parameters:
     - wmem_default: default xmit buffer size
     - wmem_max : max allocatable
     Changing the max is still possible with root privilege:
     either edit /etc/sysctl.conf (permanent) :
     (write):
     net.core.wmem_default = xxxx
     net.core.wmem_max = xxxx
     (read):
     net.core.rmem_default = xxxx
     net.core.rmem_max = xxxx

     or temporary with:
     echo <new_value> > /proc/sys/net/core/<your_param>

   @param name0fSocket : name of the AF_UNIX socket
   @param size: size in byte of the xmit buffer (the service double that value

    retval: >0 reference of the created AF_UNIX socket
    retval < 0 : error on socket creation

*/
int af_unix_sock_create_internal(char *nameOfSocket,int size);


/*
**__________________________________________________________________________
*/
/**
*  callback associated with the socket controller for receiving a
   message on a AF_UNIX socket operating of datagram mode

  @param socket_pointer: pointer to the socket context
  @param socketId: reference of the socket--> not used

*/
uint32_t af_unix_recv_generic_cbk(void * socket_pointer,int socketId);


/*
**__________________________________________________________________________
*/
void af_unix_send_fsm(af_unix_ctx_generic_t *socket_p,com_xmit_template_t *xmit_p);

/**
* default callback for normal reception
*/
extern ruc_sockCallBack_t af_unix_generic_callBack_sock;


/*
**__________________________________________________________________________
*/
/**
   Creation of a AF_UNIX socket stream in non blocking mode

   see /proc/sys/net/core for socket parameters:
     - wmem_default: default xmit buffer size
     - wmem_max : max allocatable
     Changing the max is still possible with root privilege:
     either edit /etc/sysctl.conf (permanent) :
     (write):
     net.core.wmem_default = xxxx
     net.core.wmem_max = xxxx
     (read):
     net.core.rmem_default = xxxx
     net.core.rmem_max = xxxx

     or temporary with:
     echo <new_value> > /proc/sys/net/core/<your_param>

   @param name0fSocket : name of the AF_UNIX socket
   @param size: size in byte of the xmit buffer (the service double that value

    retval: >0 reference of the created AF_UNIX socket
    retval < 0 : error on socket creation

*/
int af_unix_sock_stream_create_internal(char *nameOfSocket,int size);

/*
**  That procedure returns the current length of message received. This
**  correspond to the remaining length without the header

  @param pbuf: pointer to the received header
  @param offset: offset where the length filed can be found in the header
  @param fieldLen: length of the length attribute

  @retval : length of the message payload
*/
static inline uint32_t com_sock_extract_length_from_header_host_format(char *pbuf,uint32_t offset,uint32_t fieldLen)
{
   uint16_t word16;
   uint32_t word32;
   uint8_t * p;
   uint32_t *p32;

   p = (uint8_t*)(pbuf+offset);
   switch (fieldLen)
   {
     case 1:
	return (uint32_t)*p;
     case 2:
	 word16 = (p[1]<<8)+ p[0];
	 return (uint32_t) word16 /*(ntohs(word16))*/;
     case 4:
        p32 = (uint32_t *)p;
        word32 = *p32;

	 return (uint32_t)word32; /*(uint32)(ntohl(word32));*/
     default:
	severe( "Bad size of header field length %d\n", fieldLen );
	return 0;
   }
   return 0;
}


/**
*  Stream common disconnect procedure
  That function is called upon the detection of a remote disconnection or
  any other error that implies the closing of the current stream

  The purpose of that function is to release buffer resources when needed
  and to inform the application about the disconnedt, (if it has provided
  the disconnect callback
*/
void af_unix_sock_stream_disconnect_internal(af_unix_ctx_generic_t *socket_p);

/**
*  Socket stream receive prototypes
*/
uint32_t af_unix_recv_stream_sock_recv(af_unix_ctx_generic_t  *sock_p, void *buf,int len, int flags,int *len_read);
uint32_t af_unix_recv_stream_generic_cbk(void * socket_pointer,int socketId);
uint32_t af_unix_recv_rpc_stream_generic_cbk(void * socket_pointer,int socketId);

/**
*  Socket stream transmit prototypes
*/
uint32_t af_unix_send_stream_generic(int fd,char *pMsg,int lgth,int *len_sent_p);
void af_unix_send_stream_fsm(af_unix_ctx_generic_t *socket_p,com_xmit_template_t *xmit_p);

/*
**__________________________________________________________________________
*/
/**
*  clear the check_cnx_rq bit for the following condition
   - init
   - successful reception (data or EAGAIN)
*/
static inline void af_inet_cnx_ok (af_unix_ctx_generic_t *sock_p)
{
   af_inet_check_cnx_t *p = &sock_p->cnx_supevision;
   
   if (p->s.check_cnx_rq_per_appli == 0) { 
     p->s.check_cnx_rq = 0;
   }
   
   if (sock_p->cnx_availability_state  != AF_UNIX_CNX_AVAILABLE)
   {
     sock_p->cnx_availability_state = AF_UNIX_CNX_AVAILABLE;
     if (sock_p->userAvailabilityCallBack!= NULL)
     {
       /**
       * application has a polling callback for the connection supervision
       */
       (sock_p->userAvailabilityCallBack)(sock_p->availability_param);   
     }
   }
}

/*
**__________________________________________________________________________
*/
/**
*  Set a guard timer for the connexion to monitor the response
   
   @param tmo: tmo in ticker (base is 100 ms)
*/
static inline void af_inet_set_cnx_tmo (af_unix_ctx_generic_t  *sock_p, int tmo)
{
   uint64_t cur_ts;
   af_inet_check_cnx_t *p = &sock_p->cnx_supevision;
   if (p->s.check_cnx_enabled == 0) return;
   if (p->s.check_cnx_rq == 0)
   {
     cur_ts = timer_get_ticker();
     p->s.timestamp = cur_ts+tmo;
     p->s.check_cnx_rq = 1;
   }   
}

/*
**__________________________________________________________________________
*/
/**
*  Enable the supervision at the level of the stream connection
   
   @param sock_p: reference of the connection
*/
static inline void af_inet_enable_cnx_supervision(af_unix_ctx_generic_t  *sock_p)
{
   af_inet_check_cnx_t *p = &sock_p->cnx_supevision;
   p->s.check_cnx_enabled =1;
}



/*
**__________________________________________________________________________
*/
/**
*  Declare an application callback for connection supervision
   
   @param sock_p: reference of the connection
   @param supervision_callback supervision_callback
*/
static inline void af_inet_attach_application_supervision_callback(af_unix_ctx_generic_t  *sock_p,af_stream_poll_CBK_t supervision_callback, int active_standby_mode)
{
   af_inet_check_cnx_t *p = &sock_p->cnx_supevision;
   p->s.check_cnx_enabled =1;
   if (active_standby_mode == 1) {
     p->s.check_cnx_rq_per_appli = 1;
   }
   else {
     p->s.check_cnx_rq_per_appli = 0;
   }
   sock_p->userPollingCallBack = supervision_callback;
}


/*
**__________________________________________________________________________
*/
/**
*  Clear the check_cnx_rq flag from the application
   
   @param sock_p: reference of the connection
   @param supervision_callback supervision_callback
*/
static inline void af_inet_clear_check_cnx_rq(af_unix_ctx_generic_t  *sock_p)
{
   af_inet_check_cnx_t *p = &sock_p->cnx_supevision;
   p->s.check_cnx_rq_per_appli = 0;
}




/*
**__________________________________________________________________________
*/
/**
*  Declare an application callback for connection supervision
   
   @param sock_p: reference of the connection
   @param available_callback : call back to process availability change 
   @param param : opaque param associated with the callback
*/
static inline void af_inet_attach_application_availability_callback(af_unix_ctx_generic_t  *sock_p,af_stream_avail_CBK_t available_callback,uint32_t param)
{
   sock_p->userAvailabilityCallBack = available_callback;
   sock_p->availability_param = param;
}


/**
*  Set the maximum qlen of a AF_UNIX datagram socket

 @param  len : The length of the AF_UNIX datagram socket

 @retval: 0 success, all the socket have been created
 @retval < 0 error on at least one socket creation
*/
int af_unix_socket_set_datagram_socket_len(int len);
#ifdef __FreeBSD__
#define tcpi_probes __tcpi_probes
#define tcpi_unacked __tcpi_unacked
#define tcpi_lost __tcpi_lost
#define tcpi_retrans __tcpi_retrans
#define tcpi_last_data_sent __tcpi_last_data_sent
#endif

#endif
