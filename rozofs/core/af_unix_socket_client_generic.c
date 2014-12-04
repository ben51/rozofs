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
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/un.h>
#include <errno.h>
 #include <poll.h>

#include <rozofs/common/types.h>
#include <rozofs/common/log.h>

#include "ruc_common.h"
#include "ruc_list.h"
#include "af_unix_socket_generic.h"
#include "af_unix_socket_generic_api.h"
#include "af_inet_stream_api.h"

/**
* Callbacks prototypes : not yet connected
*/
uint32_t af_unix_generic_cli_rcvReadysock(void * socket_ctx_p,int socketId);
uint32_t af_unix_generic_cli_fake_Cbk(void * socket_ctx_p,int socketId);
uint32_t af_unix_generic_cli_xmitReadysock(void * socket_ctx_p,int socketId);
uint32_t af_unix_generic_cli_connectReply_CBK(void * socket_ctx_p,int socketId);
/**
* Callbacks prototypes :  connected
*/

uint32_t af_unix_generic_cli_connected_rcvReady_cbk(void * socket_ctx_p,int socketId);
uint32_t af_unix_generic_cli_connected_recv_cbk(void * socket_ctx_p,int socketId);
uint32_t af_unix_generic_cli_connected_xmitReady_cbk(void * socket_ctx_p,int socketId);
uint32_t af_unix_generic_cli_connected_xmitEvt_cbk(void * socket_ctx_p,int socketId);
/*
**  Call back function for socket controller
*/
ruc_sockCallBack_t af_unix_generic_client_callBack_sock=
  {
     af_unix_generic_cli_rcvReadysock,
     af_unix_generic_cli_fake_Cbk,
     af_unix_generic_cli_xmitReadysock,
     af_unix_generic_cli_connectReply_CBK
  };


 extern ruc_sockCallBack_t af_unix_generic_client_connected_callBack_sock;

 /*
**__________________________________________________________________________
**
**    N O T   Y E T   C O N N E C T E D    C A L L B A C K S
**
**__________________________________________________________________________
*/

/*
**__________________________________________________________________________
*/
/**
  Application callBack:

   receiver ready function: called from socket controller.
   Always false for a client socket


  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)

  @retval : TRUE-> receiver ready
  @retval : FALSE-> receiver not ready
*/

uint32_t af_unix_generic_cli_rcvReadysock(void * socket_ctx_p,int socketId)
{

    return FALSE;
}

/*
**__________________________________________________________________________
*/
/**
  Application callBack:

   Called from the socket controller when there is a message pending on the
   socket associated with the context provide in input arguments.
   always true for client socket


  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)

   @retval : TRUE-> xmit ready event expected
  @retval : FALSE-> xmit  ready event not expected
*/

uint32_t af_unix_generic_cli_fake_Cbk(void * socket_ctx_p,int socketId)
{


   return TRUE;
}


/*
**__________________________________________________________________________
*/
/**
  Application callBack:

  Called from the socket controller. The application is intended to
  indicate if it wants to be warn when the transmitter is ready
  Typically after facing a congestion on xmit, the application will
  generally request from xmit ready event from the socket layer

   --> always TRUE for client socket

  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)

  @retval : always TRUE
*/

uint32_t af_unix_generic_cli_xmitReadysock(void * socket_ctx_p,int socketId)
{

    return TRUE;
}




/*
**__________________________________________________________________________
*/
/**
  Application callBack:

   Called from the socket controller upon receiving a xmit ready event
   for the associated socket. That callback is activeted only if the application
   has replied TRUE in af_unix_generic_cli_xmitReadysock().

   -> process the response of a connect()

  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)

   @retval :always TRUE
*/

uint32_t af_unix_generic_cli_connectReply_CBK(void * socket_ctx_p,int socketId)
{
   uint32_t ret;
   int error = -1;
   af_unix_ctx_generic_t  *sock_p;
   sock_p = (af_unix_ctx_generic_t*)socket_ctx_p;
   int len =sizeof(int);

  ret = RUC_OK;
  while (1)
  {
    if (getsockopt (socketId,SOL_SOCKET,
                    SO_ERROR,&error,(socklen_t*)&len) == -1)
    {
      severe( " ruc_tcp_client_connectReply_CBK: getsockopt error for context %d errno:%d ",(int)sock_p->index,errno );
      if (sock_p->connectionId !=  NULL)
      {
	    ruc_sockctl_disconnect(sock_p->connectionId);
	    sock_p->connectionId = NULL;
      }
      close(socketId);
      sock_p->socketRef = -1;

      ret = RUC_NOK;
      break;
    }
    if (error == 0)
    {
      /*
      ** all is fine, perform the connection with the socket
      ** controller
      */
      ruc_sockctl_disconnect(sock_p->connectionId);
      sock_p->connectionId = NULL;

      /*
      ** reconnect with the application callback
      */
      sock_p->connectionId = ruc_sockctl_connect(sock_p->socketRef,  // Reference of the socket
                                                sock_p->nickname,   // name of the socket
                                                16,                  // Priority within the socket controller
                                                (void*)sock_p,      // user param for socketcontroller callback
                                                &af_unix_generic_client_connected_callBack_sock);  // Default callbacks
      if (sock_p->connectionId == NULL)
      {

        /*
        ** Delete the client connection or attempt to re-connect
        */
        warning("socket cannot re-connect with socket controller %s",sock_p->nickname);
        ret =  RUC_NOK;
        break;
      }
      /*
      ** Make sure the transmitter and receiver will be ready again;
      */
      sock_p->xmit.state = XMIT_READY;
      sock_p->recv.state = RECV_IDLE;
      if (sock_p->userPollingCallBack)
      {
	    sock_p->cnx_availability_state = AF_UNIX_CNX_UNAVAILABLE;
	    af_inet_set_cnx_tmo(sock_p,20);
      }
      else
      {
        sock_p->cnx_availability_state = AF_UNIX_CNX_AVAILABLE;      
      }
      if (sock_p->userAvailabilityCallBack!= NULL)
      {
        /**
        * application has a polling callback for the connection supervision
        */
        (sock_p->userAvailabilityCallBack)(sock_p->availability_param);   
      }
      ret =  RUC_OK;
      break;
    }
    /*
    **______________________
    ** there is an error
    **______________________
    */

    switch (error)
    {
      case EADDRINUSE:
      case EBADF:
      case ENOTSOCK:
      case EISCONN:

      case ECONNREFUSED:
      case ETIMEDOUT:
      /*
      ** no listening on port
      */
      default:
#if 0
        perror("tcp client error on connect");
#endif
        break;
     }
     /*
     **  disconnect from the socket controller
     **  and close the socket
     */
     ruc_sockctl_disconnect(sock_p->connectionId);
     sock_p->connectionId =  NULL;
     close(socketId);
     sock_p->socketRef = -1;
     ret =  RUC_NOK;
     break;
   }
   /*
   ** call the user callback on connect
   */   
   (sock_p->userConnectCallBack)(sock_p->userRef,sock_p->index,ret,error);
   return RUC_OK;

}


 /*
**__________________________________________________________________________
**
**     C O N N E C T E D    C A L L B A C K S
**
**__________________________________________________________________________
*/


/*
**__________________________________________________________________________
*/
/**
*  check if the supervision timer of the connexion has expired
  
   @retval 1 if expired
   @retval 0 if not expired or inactive
*/
static  int af_inet_cnx_check_expiration (af_unix_ctx_generic_t  *sock_p)
{

   af_inet_check_cnx_t *p = &sock_p->cnx_supevision;
   uint64_t cur_ts;
   uint64_t cnx_ts;        

     
   if (p->s.check_cnx_enabled == 0) return 0;
   if (p->s.check_cnx_rq == 0) return 0;
   
   /*
   ** check the timestamp
   */
   cur_ts = timer_get_ticker();
   cnx_ts = p->s.timestamp;
   if (cur_ts < cnx_ts) return 0;

   /*
   ** timer has expired: check the reception
   */
   p->s.check_cnx_rq = 0;
   if (sock_p->cnx_availability_state  != AF_UNIX_CNX_UNAVAILABLE)
   {
     sock_p->cnx_availability_state = AF_UNIX_CNX_UNAVAILABLE;
     if (sock_p->userAvailabilityCallBack!= NULL)
     {
       /**
       * application has a polling callback for the connection supervision
       */
       (sock_p->userAvailabilityCallBack)(sock_p->availability_param);   
     }
   }
   
   if (sock_p->userPollingCallBack!= NULL)
   {
     /**
     * application has a polling callback for the connection supervision
     */
     (sock_p->userPollingCallBack)(sock_p);   
   }

#if 0   
   int len;
   int status;
   uint8_t fake_buf[16];
   
   status = af_unix_recv_stream_sock_recv(sock_p,fake_buf,2,MSG_PEEK,&len);
   switch(status)
   {
     case RUC_OK:   
//     case RUC_WOULDBLOCK:
     case RUC_PARTIAL:
      /*
      ** the connexion seems to be still up
      */
      return  0;

     case RUC_WOULDBLOCK:
      /*
      ** the other end does not answer
      */
//      af_unix_sock_stream_disconnect_internal(sock_p);
//      (sock_p->userDiscCallBack)(sock_p->userRef,sock_p->index,NULL,errno);
      return  0;
      
     case RUC_DISC:
     default:
      af_unix_sock_stream_disconnect_internal(sock_p);
      (sock_p->userDiscCallBack)(sock_p->userRef,sock_p->index,NULL,errno);
      return 1;
   }
#endif
   return 0;
}


/*
**  Call back function for socket controller
*/
ruc_sockCallBack_t af_unix_generic_client_connected_callBack_sock=
  {
     af_unix_generic_cli_connected_rcvReady_cbk,
     af_unix_generic_cli_connected_recv_cbk,
     af_unix_generic_cli_connected_xmitReady_cbk,
     af_unix_generic_cli_connected_xmitEvt_cbk
  };


/*
**__________________________________________________________________________
*/
/**
  Application callBack:

   receiver ready function: called from socket controller.
   The module is intended to return if the receiver is ready to receive a new message
   and FALSE otherwise


  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)

  @retval : TRUE-> receiver ready
  @retval : FALSE-> receiver not ready
*/

uint32_t af_unix_generic_cli_connected_rcvReady_cbk(void * socket_ctx_p,int socketId)
{
    af_unix_ctx_generic_t  *sock_p;
    sock_p = (af_unix_ctx_generic_t*)socket_ctx_p;
    /*
    ** check if there is a potential connection expiration 
    */
    if (af_inet_cnx_check_expiration(sock_p) == 1)
    {
      /*
      ** expiration of the connection : no response for a request that is still in the connection
      */
      return FALSE;
    
    }
    /*
    ** check if there is a user callback associated with that socket
    ** otherwise use the default function of the generic socket
    */
    if (sock_p->userRcvReadyCallBack != NULL)
    {

      return (sock_p->userRcvReadyCallBack)(sock_p->userRef,sock_p->index);
    }
    else
    {
      /*
      ** check the amount of buffer on the receiver side
      */

    }
    return TRUE;
}

/*
**__________________________________________________________________________
*/
/**
  Application callBack:

   Called from the socket controller when there is a message pending on the
   socket associated with the context provide in input arguments.



  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)

   @retval : TRUE-> xmit ready event expected
  @retval : FALSE-> xmit  ready event not expected
*/

uint32_t af_unix_generic_cli_connected_recv_cbk(void * socket_ctx_p,int socketId)
{
  af_unix_ctx_generic_t  *sock_p = (af_unix_ctx_generic_t*)socket_ctx_p;
  com_recv_template_t    *recv_p;
  recv_p = &sock_p->recv;
  /*
  ** deassert the xmit/resp supervision bit 
  */
  af_inet_cnx_ok(sock_p);
  
  if (recv_p->rpc.receiver_active)
  {
    af_unix_recv_rpc_stream_generic_cbk(socket_ctx_p,socketId);
  }
  else
  {
    af_unix_recv_stream_generic_cbk(socket_ctx_p,socketId);
  }
  return TRUE;
}


/*
**__________________________________________________________________________
*/
/**
  Application callBack:

  Called from the socket controller. The application is intended to
  indicate if it wants to be warn when the transmitter is ready
  Typically after facing a congestion on xmit, the application will
  generally request from xmit ready event from the socket layer


  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)

  @retval : always TRUE
*/

uint32_t af_unix_generic_cli_connected_xmitReady_cbk(void * socket_ctx_p,int socketId)
{
    af_unix_ctx_generic_t  *sock_p;
    com_xmit_template_t *xmit_p;
    uint32_t ret = FALSE;

    sock_p = (af_unix_ctx_generic_t*)socket_ctx_p;
    xmit_p = (com_xmit_template_t*)&sock_p->xmit;

    if (xmit_p->xmit_req_flag == 1)
    {
      ret = TRUE;
    }
    if (xmit_p->congested_flag == 1)
    {
      ret = TRUE;
    }
    return ret;
}

/*
**__________________________________________________________________________
*/
/**
  Application callBack:

   Called from the socket controller upon receiving a xmit ready event
   for the associated socket. That callback is activeted only if the application
   has replied TRUE in af_unix_generic_xmitReadysock().

   It typically the processing of a end of congestion on the socket


  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)

   @retval :always TRUE
*/

uint32_t af_unix_generic_cli_connected_xmitEvt_cbk(void * socket_ctx_p,int socketId)
{
    af_unix_ctx_generic_t  *sock_p;
    com_xmit_template_t *xmit_p;



    sock_p = (af_unix_ctx_generic_t*)socket_ctx_p;
    xmit_p = (com_xmit_template_t*)&sock_p->xmit;
    /*
    ** check if credit reload is required
    */
    if (xmit_p->xmit_req_flag == 1)
    {
      xmit_p->xmit_req_flag = 0;
      xmit_p->xmit_credit = 0;
      af_unix_send_stream_fsm(sock_p,xmit_p);
    }
    /*
    ** active the fsm for end of congestion (xmit ready and credit reload
    */
    if (xmit_p->congested_flag == 1)
    {
      af_unix_send_stream_fsm(sock_p,xmit_p);
    }



    return TRUE;
}

/**
*  Stream common disconnect procedure
  That function is called upon the detection of a remote disconnection or
  any other error that implies the closing of the current stream

  The purpose of that function is to release buffer resources when needed
  and to inform the application about the disconnedt, (if it has provided
  the disconnect callback
*/
void af_unix_sock_stream_disconnect_internal(af_unix_ctx_generic_t *socket_p)
{

   com_xmit_template_t *xmit_p = &socket_p->xmit;
   int inuse;
   /*
   ** set the path as unavailable and call any associated callback
   */
   if (socket_p->cnx_availability_state  != AF_UNIX_CNX_UNAVAILABLE)
   {
     socket_p->cnx_availability_state = AF_UNIX_CNX_UNAVAILABLE;
     if (socket_p->userAvailabilityCallBack!= NULL)
     {
       /**
       * application has a polling callback for the connection supervision
       */
       (socket_p->userAvailabilityCallBack)(socket_p->availability_param);   
     }
   }   
   /*
   ** check if there is a pending buffer
   */
   if (xmit_p->bufRefCurrent != NULL)
   {
     inuse = ruc_buf_inuse_decrement(xmit_p->bufRefCurrent);
     if (inuse < 0)
     {
      fatal("Inuse is negative %d",inuse);
     }
     /*
     ** the opaque_ref value needs to be tested on the cyurrent xmit buffer only, since
     ** it is the only situation where we can have a race condition between an application that
     ** attempt to remove the wmit buffer and the socket transmitter: An xmit buffer cannot be
     ** released while it is in the XMIT in progress state.
     */
     if (socket_p->userXmitDoneCallBack != NULL)
     {
       if (ruc_buf_get_opaque_ref(xmit_p->bufRefCurrent) == socket_p) 
       {
        /*
        ** caution: in that case it is up to the application that provides the callback to release
        ** the xmit buffer
        */
        (socket_p->userXmitDoneCallBack)(socket_p->userRef,socket_p->index,xmit_p->bufRefCurrent);
       }
       else
       {
         if (inuse == 1) ruc_buf_freeBuffer(xmit_p->bufRefCurrent);       
       }
     }
     else
     {
       if (inuse == 1) ruc_buf_freeBuffer(xmit_p->bufRefCurrent);
     }
   }
   xmit_p->bufRefCurrent = NULL;
   xmit_p->state = XMIT_DEAD;
   socket_p->xmit.xmit_req_flag  = 0;
   socket_p->xmit.xmit_credit    = 0;
   socket_p->xmit.congested_flag = 0;
   /*
   ** now get all the buffers that were queued in the xmit queue
   */
   ruc_obj_desc_t *bufRef;
   uint8_t   opcode = 0;
   while ((bufRef = (ruc_obj_desc_t*)ruc_objReadQueue(&xmit_p->xmitList[0],&opcode))
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
  /*
  ** Go now to the receiver side
  */
  com_recv_template_t    *recv_p= &socket_p->recv;

  if ( recv_p->bufRefCurrent != NULL)
  {

     ruc_buf_freeBuffer(recv_p->bufRefCurrent);
     recv_p->bufRefCurrent = NULL;
  }
  recv_p->state = RECV_DEAD;
}
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
int af_unix_sock_stream_client_create_internal(char *nameOfSocket,int size)
{
  int ret;
  int fd=-1;
  struct sockaddr_un addr;
  int fdsize;
  int optionsize=sizeof(fdsize);
  int fileflags;

  /*
  ** create a datagram socket
  */
  fd=socket(PF_UNIX,SOCK_STREAM,0);
  if(fd<0)
  {
    warning("socket creation failure  :%s",strerror(errno));
   return -1;
  }
  /*
  ** remove fd if it already exists
  */
  if (nameOfSocket != NULL)
  {
    ret=unlink(nameOfSocket);
    /*
    ** named the socket reception side
    */
    addr.sun_family= AF_UNIX;
    strcpy(addr.sun_path,nameOfSocket);
    ret=bind(fd,(struct sockaddr*)&addr,sizeof(addr));
    if(ret<0)
    {
     warning("socket binding failure  :%s",strerror(errno));
     return -1;
    }
  }
  /*
  ** change the length for the send buffer, nothing to do for receive buf
  ** since it is out of the scope of the AF_SOCKET
  */
  ret= getsockopt(fd,SOL_SOCKET,SO_SNDBUF,(char*)&fdsize,(socklen_t*)&optionsize);
  if(ret<0)
  {
    warning("socket getsockopt failure (SO_SNDBUF)  :%s",strerror(errno));
   return -1;
  }
  /*
  ** update the size, always the double of the input
  */
  fdsize=2*size;

  /*
  ** set a new size for emission and
  ** reception socket's buffer
  */
  ret=setsockopt(fd,SOL_SOCKET,SO_SNDBUF,(char*)&fdsize,sizeof(int));
  if(ret<0)
  {
    warning("socket setsockopt failure (SO_SNDBUF)  :%s",strerror(errno));
   return -1;
  }
  ret=setsockopt(fd,SOL_SOCKET,SO_RCVBUF,(char*)&fdsize,sizeof(int));
  if(ret<0)
  {
    warning("socket setsockopt failure (SO_SNDBUF)  :%s",strerror(errno));
   return -1;
  }
  /*
  ** Change the mode of the socket to non blocking
  */
  if((fileflags=fcntl(fd,F_GETFL,0))==-1)
  {
    warning("socket cannot switch to non-blocking :%s",strerror(errno));
    return -1;
  }
//  #warning socket is operating blocking mode
#if 1

  if((fcntl(fd,F_SETFL,fileflags|O_NDELAY))==-1)
  {
    warning("socket cannot switch to non-blocking :%s",strerror(errno));
    return -1;
  }
#endif
  return(fd);
}


/*
**__________________________________________________________________________
*/
/**
   Attempt to reconnect to the server

   In case of failure, it is up to the application to call the service
   for releasing the socket context and the associated resources
   Cleaning the socket context will automatically close the socket and
   remove the binding with the socket controller.
   It also release any recv/xmit buffer attached with the socket context.


   @param af_unix_ctx_id : reference tf the socket context

    retval: !=0 reference of the created socket context
    retval < 0 : error on reconnect

*/
int af_unix_sock_client_reconnect(uint32_t af_unix_ctx_id)
{

   af_unix_ctx_generic_t *sock_p ;
  com_xmit_template_t    *xmit_p;
  com_recv_template_t    *recv_p;
  struct sockaddr_un addr;
  int ret = -1;


   /*
   ** get the reference from idx
   */
   sock_p = af_unix_getObjCtx_p(af_unix_ctx_id);
   if (sock_p == NULL)
   {
     /*
     ** socket reference is out of range
     */
     return -1;
   }


   xmit_p = &sock_p->xmit;
   recv_p = &sock_p->recv;

   while(1)
   {

   /*
   ** first of all, check if the socket is still connected with the socket controller
   ** because in that case we must first disconnect from the socket controller and then reconnect
   */
   if (sock_p->connectionId != NULL)
   {
     ruc_sockctl_disconnect(sock_p->connectionId);
     sock_p->connectionId = NULL;
   }
   if (sock_p->socketRef != -1)
   {
     close(sock_p->socketRef);
   }
   if (sock_p->af_family == AF_UNIX)
   {
     sock_p->socketRef = af_unix_sock_stream_client_create_internal(NULL,sock_p->so_sendbufsize);
   }
   else
   {
     sock_p->socketRef = af_inet_sock_stream_client_create_internal(sock_p,sock_p->so_sendbufsize);
#ifdef __linux__
     /*
     ** Set a max SYN retry number to 2
     */
     int sync_max_retry = 2;
     if (setsockopt (sock_p->socketRef,IPPROTO_TCP,
                     TCP_SYNCNT,&sync_max_retry,sizeof(int)) == -1)
     {
	   severe("setsockopt TCP_SYNCNT %d %s",sync_max_retry, strerror(errno));
     }     
#endif
   }
   if (sock_p->socketRef == -1)
   {
     break;
   }
   /*
   ** clear  the xmit credit and the congestion flag to avoid a loop in xmit_ready
   */
   sock_p->xmit.xmit_req_flag  = 0;
   sock_p->xmit.xmit_credit    = 0;
   sock_p->xmit.congested_flag = 0;

   /*
   ** OK, we are almost done, install the default callback before getting the connect confirrm
   */
   sock_p->connectionId = ruc_sockctl_connect(sock_p->socketRef,  // Reference of the socket
                                              sock_p->nickname,   // name of the socket
                                              3,                  // Priority within the socket controller
                                              (void*)sock_p,      // user param for socketcontroller callback
                                              &af_unix_generic_client_callBack_sock);  // Default callbacks
    if (sock_p->connectionId == NULL)
    {
       /*
       ** Fail to connect with the socket controller
       */
       warning("socket cannot connect with socket controller %s",sock_p->nickname);
       break;
    }
    /*
    ** do the connect
    */
    if (sock_p->af_family == AF_UNIX)
    {
        /* AF_UNIX connection case */
      addr.sun_family= AF_UNIX;
      strcpy(addr.sun_path,sock_p->remote_sun_path);
      ret = connect(sock_p->socketRef,(const struct sockaddr *)&addr,(socklen_t)sizeof(addr));
    }
    else
    {
      /* TCP connection case */
      uint32_t 	IpAddr;
      struct sockaddr_in vSckAddr;

      vSckAddr.sin_family = AF_INET;
      vSckAddr.sin_port   = htons(sock_p->remote_port_host);
      IpAddr = htonl(sock_p->remote_ipaddr_host);
      memcpy(&vSckAddr.sin_addr.s_addr, &IpAddr, sizeof(uint32_t));
    ret =connect(sock_p->socketRef,(const struct sockaddr *)&vSckAddr,sizeof(struct sockaddr_in));
    }
    if (ret == -1)
    {
      if (errno == EINPROGRESS) return 0;

//      printf("FDL error on %s errno %s\n",addr.sun_path,strerror(errno));
//      RUC_WARNING(errno);
      break;
      /*
      ** for the other case we disconnect from the socket controller to avoid
      ** burning CPU for the case of the AF_UNIX since for that case EINPROGRESS
      ** does not exist
      */
      ruc_sockctl_disconnect(sock_p->connectionId);
      sock_p->connectionId = NULL;
      break;

    }
    /*
    ** All is fine
    */
    xmit_p->state = XMIT_READY;
    recv_p->state = RECV_IDLE;
    return 0;
    break;

   }
   /**
   * we come here for the error case
   */

   return -1;
}



/*
**__________________________________________________________________________
*/
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

   @param nickname : name of socket for socket controller display name
   @param remote_sun_path : name of remote  the AF_UNIX socket
   @param size: size in byte of the xmit buffer (the service double that value

    retval: !=0 reference of the created socket context
    retval < 0 : error on socket context creation

*/
int af_unix_sock_client_create(char *nickname,char *remote_sun_path,af_unix_socket_conf_t *conf_p)
{
   af_unix_ctx_generic_t *sock_p;
  com_xmit_template_t    *xmit_p;
  com_recv_template_t    *recv_p;
  struct sockaddr_un addr;
  int ret = -1;
  int len;
  int release_req = 0;


   /*
   ** the client must provide a callback to get the status of the connect operation
   */
   if (conf_p->userConnectCallBack == NULL)
   {
      fatal("af_unix_sock_client_create: userConnectCallBack missing");

   }

   len = strlen(remote_sun_path);
   if (len >= AF_UNIX_SOCKET_NAME_SIZE)
   {
      /*
      ** name is too big!!
      */
      warning("af_unix_sock_client_create: remote_sun_path too big:%d max: %d ",len,AF_UNIX_SOCKET_NAME_SIZE);
      return -1;
   }

   len = strlen(nickname);
   if (len >= AF_UNIX_SOCKET_NAME_SIZE)
   {
      /*
      ** name is too big!!
      */
      warning("af_unix_sock_client_create: nickname too big:%d max: %d ",len,AF_UNIX_SOCKET_NAME_SIZE);
      return -1;
   }
   /*
   ** Allocate a socket context
   */
   sock_p = af_unix_alloc();
   if (sock_p == NULL)
   {
     /*
     ** Out of socket context
     */
      warning("af_unix_sock_client_create: out of socket context");
      return -1;
   }
   xmit_p = &sock_p->xmit;
   recv_p = &sock_p->recv;

   while(1)
   {
   sock_p->af_family = AF_UNIX;
   /*
   ** Copy the name of the remote socket
   */
   strcpy(sock_p->remote_sun_path,remote_sun_path);
   sock_p->nickname[0]='C';
   sock_p->nickname[1]=':';
   strcpy(&sock_p->nickname[2],nickname);
   /*
   ** create the socket and save the sending buffer size to address the case of the reconnection
   */
   sock_p->so_sendbufsize = conf_p->so_sendbufsize;
   sock_p->socketRef = af_unix_sock_stream_client_create_internal(NULL,sock_p->so_sendbufsize);
   if (sock_p->socketRef == -1)
   {
     ret = -1;
     release_req = 1;
     break;
   }

   /*
   ** copy the family id and instance id
   */
   sock_p->family      = conf_p->family;
   sock_p->instance_id = conf_p->instance_id;

   /*
   ** install the receiver part
   */
   recv_p->headerSize = conf_p->headerSize;
   recv_p->msgLenOffset = conf_p->msgLenOffset;
   recv_p->msgLenSize = conf_p->msgLenSize;
   recv_p->bufSize = conf_p->bufSize;
   /*
   ** set- the service type for the stream receiver
   */
   if (conf_p->recv_srv_type == ROZOFS_RPC_SRV) 
   {
     recv_p->rpc.receiver_active = 1;
     recv_p->rpc.max_receive_sz  = conf_p->rpc_recv_max_sz;
   }
   else
   {
     recv_p->rpc.receiver_active = 0;   
   }   

   /*
   ** install the mandatory user callbacks
   */
   sock_p->userRcvCallBack = conf_p->userRcvCallBack;
   /*
   ** Install the optional user callbacks
   */
   if (conf_p->userRcvAllocBufCallBack) sock_p->userRcvAllocBufCallBack = conf_p->userRcvAllocBufCallBack;
   if (conf_p->userDiscCallBack) sock_p->userDiscCallBack = conf_p->userDiscCallBack;
   if (conf_p->userRcvReadyCallBack) sock_p->userRcvReadyCallBack = conf_p->userRcvReadyCallBack;
   if (conf_p->userXmitReadyCallBack) sock_p->userXmitReadyCallBack = conf_p->userXmitReadyCallBack;
   if (conf_p->userXmitEventCallBack) sock_p->userXmitEventCallBack = conf_p->userXmitEventCallBack;
   if (conf_p->userXmitDoneCallBack) sock_p->userXmitDoneCallBack = conf_p->userXmitDoneCallBack;
   if (conf_p->userHdrAnalyzerCallBack) sock_p->userHdrAnalyzerCallBack = conf_p->userHdrAnalyzerCallBack;
   sock_p->userConnectCallBack = conf_p->userConnectCallBack;

   /*
   ** install the user reference
   */
   sock_p->userRef = conf_p->userRef;
   /*
   ** Check if the user has provided its own buffer pools (notice that the user might not provide
   ** any pool and use the common AF_UNIX socket pool or can use its own one by providing
   ** a userRcvAllocBufCallBack callback.
   */
   if (conf_p->xmitPool != 0) xmit_p->xmitPoolRef = conf_p->xmitPool;
   if (conf_p->xmitPool != 0) recv_p->rcvPoolRef  = conf_p->recvPool;

   /*
   ** OK, we are almost done, install the default callback before getting the connect confirrm
   */
   sock_p->connectionId = ruc_sockctl_connect(sock_p->socketRef,  // Reference of the socket
                                              sock_p->nickname,   // name of the socket
                                              3,                  // Priority within the socket controller
                                              (void*)sock_p,      // user param for socketcontroller callback
                                              &af_unix_generic_client_callBack_sock);  // Default callbacks
    if (sock_p->connectionId == NULL)
    {
       /*
       ** Fail to connect with the socket controller
       */
       warning("socket cannot connect with socket controller %s",sock_p->nickname);
       release_req = 1;
       break;
    }
    /*
    ** do the connect
    */
    addr.sun_family= AF_UNIX;
    strcpy(addr.sun_path,sock_p->remote_sun_path);
    ret = connect(sock_p->socketRef,(const struct sockaddr *)&addr,(socklen_t)sizeof(addr));
    if (ret == -1)
    {
//      printf("FDL error on %s errno %s\n",addr.sun_path,strerror(errno));
//      RUC_WARNING(errno);
      
      /*
      ** error, but we rely on periodic timer for retrying
      */
      xmit_p->state = XMIT_READY;
      recv_p->state = RECV_IDLE;
      ret = sock_p->index;
      return ret;
      break;
    }
    /*
    ** All is fine
    */
    xmit_p->state = XMIT_READY;
    recv_p->state = RECV_IDLE;
    ret = sock_p->index;
    return ret;
    break;

   }
   /*
   ** Check the operation status: in case of error, release the socket and the context
   */
   if (release_req)
   {
      if (sock_p->socketRef != -1) close(sock_p->socketRef);
//#warning unlink(sock_p->sockname) because of the connected mode --> need to double check for datagram or define an other service for it
      /*
      ** release the context
      */
      af_unix_free_from_ptr(sock_p);
   }
   return -1;

}






/**
*  Create a bunch of AF_UNIX socket associated with a Family

 @param  nicknamebase_p : Base name of the family
 @param  basename_p : Base name of the remote sunpath
 @param  base_instance: index of the first instance
 @param  nb_instances: number of instance in the family
 @param  socket_tb_p : pointer to an array were the socket references will be stored
 @param  xmit_size : size for the sending buffer (SO_SNDBUF parameter)

 @retval: 0 success, all the socket have been created
 @retval < 0 error on at least one socket creation
*/
int af_unix_socket_client_family_create (char *nicknamebase_p, char *basename_p, int base_instance,int nb_instances,
                                         int *socket_ctx_tb_p,af_unix_socket_conf_t *conf_p)
{
  int *local_socket_tb_p = socket_ctx_tb_p;
  char nickname[128];
  char sun_path[128];
  int i;
  int error = 0;


  /*
  ** Clear the socket table
  */
  memset(local_socket_tb_p,-1,sizeof(int)*nb_instances);
  /*
  ** Loop creation
  */
  for (i = 0; i < nb_instances; i++)
  {
     sprintf(sun_path,"%s_inst_%d",basename_p,base_instance+i);
     sprintf(nickname,"%s_%d",nicknamebase_p,base_instance+i);
     conf_p->instance_id = base_instance+i;
     local_socket_tb_p[i] = af_unix_sock_client_create(nickname,sun_path,conf_p);
     if (local_socket_tb_p[i] == -1)
     {
       error = 1;
       break;
     }
  }
  if (error)
  {
    /*
    ** clean up the sockets that have been already created
    */
    for (i = 0; i < nb_instances; i++)
    {
      if (local_socket_tb_p[i] != -1) af_unix_delete_socket(local_socket_tb_p[i]);
      return -1;
    }
  }
  return 0;
}

