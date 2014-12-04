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


/*
**  Call back function for socket controller
*/
ruc_sockCallBack_t af_unix_generic_callBack_sock=
  {
     af_unix_generic_rcvReadysock,
     af_unix_generic_rcvMsgsock,
     af_unix_generic_xmitReadysock,
     af_unix_generic_xmitEvtsock
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

uint32_t af_unix_generic_rcvReadysock(void * socket_ctx_p,int socketId)
{
    af_unix_ctx_generic_t  *sock_p;
    sock_p = (af_unix_ctx_generic_t*)socket_ctx_p;
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

uint32_t af_unix_generic_rcvMsgsock(void * socket_ctx_p,int socketId)
{
    af_unix_recv_generic_cbk(socket_ctx_p,socketId);
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

uint32_t af_unix_generic_xmitReadysock(void * socket_ctx_p,int socketId)
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


#if 0
/**
** Poll check
*/
#include "rozofs_socket_family.h"
void test_poll(int socket)
{
  char bufall[128];
  struct pollfd *fds;
  int fdcount,rv;
  struct  sockaddr_un     dest;
  dest.sun_family= AF_UNIX;

  fdcount = 1;
  fds = calloc(sizeof(struct pollfd),fdcount);
  fds[0].fd = socket;
  fds[0].events |= 0xffff;

  rv = poll(fds,fdcount,-1);
  if (rv < 0)
  {
    perror("test_poll:");
  }
  printf("Recv events %x (%d)\n",fds[0].revents,POLLOUT);



  bcopy(bufall,dest.sun_path,(strlen(bufall)+1));
  /*
  ** attempt to connect with the destination
  */
  rv = connect(socket,(struct sockaddr*)&dest,sizeof(dest));

  rv = poll(fds,fdcount,-1);
  if (rv < 0)
  {
    perror("test_poll:");
  }
  printf("connect Recv events %x (%d)\n",fds[0].revents,POLLOUT);

  free(fds);





}
#endif
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

uint32_t af_unix_generic_xmitEvtsock(void * socket_ctx_p,int socketId)
{
    af_unix_ctx_generic_t  *sock_p;
    com_xmit_template_t *xmit_p;


//    test_poll(socketId);

    sock_p = (af_unix_ctx_generic_t*)socket_ctx_p;
    xmit_p = (com_xmit_template_t*)&sock_p->xmit;
    /*
    ** check if credit reload is required
    */
    if (xmit_p->xmit_req_flag == 1)
    {
      xmit_p->xmit_req_flag = 0;
      xmit_p->xmit_credit = 0;
      af_unix_send_fsm(sock_p,xmit_p);
    }
    /*
    ** active the fsm for end of congestion (xmit ready and credit reload
    */
    if (xmit_p->congested_flag == 1)
    {
      af_unix_send_fsm(sock_p,xmit_p);
    }



    return TRUE;
}

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
int af_unix_sock_set_non_blocking(int fd,int size)
{
  int ret;
  int fdsize;
  int optionsize=sizeof(fdsize);
  int fileflags;


  /*
  ** change the length for the send buffer, nothing to do for receive buf
  ** since it is out of the scope of the AF_SOCKET
  */
  ret= getsockopt(fd,SOL_SOCKET,SO_SNDBUF,(char*)&fdsize,(socklen_t*)&optionsize);
  if(ret<0)
  {
    RUC_WARNING(errno);
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
    RUC_WARNING(errno);
   return -1;
  }

  /*
  ** set a new size for 
  ** reception socket's buffer
  */
  ret=setsockopt(fd,SOL_SOCKET,SO_RCVBUF,(char*)&fdsize,sizeof(int));
  if(ret<0)
  {
    RUC_WARNING(errno);
   return -1;
  }
  /*
  ** Change the mode of the socket to non blocking
  */
  if((fileflags=fcntl(fd,F_GETFL,0))==-1)
  {
    RUC_WARNING(errno);
    return -1;
  }
//  #warning socket is operating blocking mode
#if 1

  if((fcntl(fd,F_SETFL,fileflags|O_NDELAY))==-1)
  {
    RUC_WARNING(errno);
    return -1;
  }
#endif
  return(fd);
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

   @param name0fSocket : name of the AF_UNIX socket
   @param size: size in byte of the xmit buffer (the service double that value

    retval: >0 reference of the created AF_UNIX socket
    retval < 0 : error on socket creation

*/
int af_unix_sock_create_internal(char *nameOfSocket,int size)
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
  fd=socket(PF_UNIX,SOCK_DGRAM,0);
  if(fd<0)
  {
    warning("af_unix_sock_create_internal socket(%s) %s", nameOfSocket, strerror(errno));
    return -1;
  }
  /*
  ** remove fd if it already exists
  */
  ret=unlink(nameOfSocket);
  /*
  ** named the socket reception side
  */
  addr.sun_family= AF_UNIX;
  strcpy(addr.sun_path,nameOfSocket);
  ret=bind(fd,(struct sockaddr*)&addr,sizeof(addr));
  if(ret<0)
  {
    warning("af_unix_sock_create_internal bind(%s) %s", nameOfSocket, strerror(errno));
    return -1;
  }
  /*
  ** change the length for the send buffer, nothing to do for receive buf
  ** since it is out of the scope of the AF_SOCKET
  */
  ret= getsockopt(fd,SOL_SOCKET,SO_SNDBUF,(char*)&fdsize,(socklen_t*)&optionsize);
  if(ret<0)
  {
    warning("af_unix_sock_create_internal getsockopt(%s) %s", nameOfSocket, strerror(errno));
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
    warning("af_unix_sock_create_internal setsockopt(%s,%d) %s", nameOfSocket, fdsize, strerror(errno));
    return -1;
  }
  /*
  ** Change the mode of the socket to non blocking
  */
  if((fileflags=fcntl(fd,F_GETFL,0))==-1)
  {
    warning("af_unix_sock_create_internal fcntl(F_GETFL %s) %s", nameOfSocket, strerror(errno));
    return -1;
  }
//  #warning socket is operating blocking mode
#if 1

  if((fcntl(fd,F_SETFL,fileflags|O_NDELAY))==-1)
  {
    warning("af_unix_sock_create_internal fcntl(F_SETFL O_NDELAY %s) %s", nameOfSocket, strerror(errno));
    return -1;
  }
#endif
  return(fd);
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

   @param src_sun_path : name of the AF_UNIX socket
   @param size: size in byte of the xmit buffer (the service double that value

    retval: !=0 reference of the created socket context
    retval < 0 : error on socket context creation

*/
int af_unix_sock_create(char *src_sun_path,af_unix_socket_conf_t *conf_p)
{
   af_unix_ctx_generic_t *sock_p;
  com_xmit_template_t    *xmit_p;
  com_recv_template_t    *recv_p;
  int ret = -1;
  int len;


   len = strlen(src_sun_path);
   if (len >= AF_UNIX_SOCKET_NAME_SIZE)
   {
      /*
      ** name is too big!!
      */
      RUC_WARNING(len);
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
     RUC_WARNING(-1);
     return -1;
   }
   sock_p->af_family = AF_UNIX;
   xmit_p = &sock_p->xmit;
   recv_p = &sock_p->recv;

   while(1)
   {
   /*
   ** Copy the name of the socket
   */
   strcpy(sock_p->src_sun_path,src_sun_path);
   strcpy(sock_p->nickname,src_sun_path);
   /*
   ** create the socket
   */
   sock_p->socketRef = af_unix_sock_create_internal(sock_p->src_sun_path,conf_p->so_sendbufsize);
   if (sock_p->socketRef == -1)
   {
     ret = -1;
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
   if (conf_p->userHdrAnalyzerCallBack) sock_p->userHdrAnalyzerCallBack = conf_p->userHdrAnalyzerCallBack;
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
   ** OK, we are almost done, just need to connect with the socket controller
   */
   sock_p->connectionId = ruc_sockctl_connect(sock_p->socketRef,  // Reference of the socket
                                              sock_p->nickname,   // name of the socket
                                              16,                  // Priority within the socket controller
                                              (void*)sock_p,      // user param for socketcontroller callback
                                              &af_unix_generic_callBack_sock);  // Default callbacks
    if (sock_p->connectionId == NULL)
    {
       /*
       ** Fail to connect with the socket controller
       */
       RUC_WARNING(-1);
       break;
    }
    /*
    ** All is fine
    */
    xmit_p->state = XMIT_READY;
    ret = sock_p->index;
    return ret;
    break;

   }
   /*
   ** Check the operation status: in case of error, release the socket and the context
   */
   if (sock_p->socketRef != -1) close(sock_p->socketRef);
   unlink(sock_p->src_sun_path);
   /*
   ** release the context
   */
   af_unix_free_from_ptr(sock_p);
   return -1;
}



/*
**__________________________________________________________________________
*/
/**
* delete a socket context allocated for a server context

 @param socket_ctx_id : reference of the socket context

 @retval 0 : success
 @retval < 0 : error
 */
 int af_unix_delete_socket(uint32_t socket_ctx_id)
 {
   int i;
   int inuse;
   /*
   ** get the pointer to the context
   */
   af_unix_ctx_generic_t *sock_p = af_unix_getObjCtx_p(socket_ctx_id);
   if (sock_p == NULL) return -1;
   if (sock_p->socketRef != -1) close(sock_p->socketRef);

   if (sock_p->af_family == AF_UNIX)
   {
     if (sock_p->src_sun_path[0] != 0)  unlink(sock_p->src_sun_path);

   }
   /*
   ** Disconnect from socket controller
   */
   if (sock_p->connectionId != NULL)
   {
      ruc_sockctl_disconnect(sock_p->connectionId);
      sock_p->connectionId = NULL;
   }
   /*
   ** release of the buffers that were queued in the transmit queue
   */
   for (i = 0; i < UMA_MAX_TCP_XMIT_PRIO ; i ++)
       com_xmit_pendingQueue_purge(sock_p,(uint8_t) i);

   if (sock_p->xmit.bufRefCurrent != NULL)
   {
     inuse = ruc_buf_inuse_decrement(sock_p->xmit.bufRefCurrent);
     if (inuse < 0)
     {
       severe("inuse counter is negative %d",inuse);
     }
     if (sock_p->userXmitDoneCallBack != NULL)
     {
        /*
        ** caution: in that case it is up to the application that provides the callback to release
        ** the xmit buffer
        */
        (sock_p->userXmitDoneCallBack)(sock_p->userRef,sock_p->index,sock_p->xmit.bufRefCurrent);
     }
     else
     {
       if (inuse > 1)
       {
         severe("inuse counter is greater than 1 with no CBK %d",inuse);
       }
       if (inuse == 1) ruc_buf_freeBuffer(sock_p->xmit.bufRefCurrent);
     }
     sock_p->xmit.bufRefCurrent = NULL;
   }

   if (sock_p->recv.bufRefCurrent != NULL)
   {
     ruc_buf_freeBuffer(sock_p->recv.bufRefCurrent);
     sock_p->recv.bufRefCurrent = NULL;
   }
   /*
   ** release the context
   */
   af_unix_free_from_ptr(sock_p);

   return 0;
 }

/*
**__________________________________________________________________________
*/
/**
* Perform a disconnection from the socket controller:

 the socket is also closed if that one is still reference in
 the socket context

 @param socket_ctx_id : reference of the socket context

 @retval 0 : success
 @retval < 0 : error
 */
 int af_unix_disconnect_from_socketCtrl(uint32_t socket_ctx_id)
 {
   /*
   ** get the pointer to the context
   */
   af_unix_ctx_generic_t *sock_p = af_unix_getObjCtx_p(socket_ctx_id);
   if (sock_p == NULL) return -1;
   if (sock_p->socketRef != -1)
   {
     close(sock_p->socketRef);
     sock_p->socketRef = -1;
   }
   if (sock_p->af_family == AF_UNIX)
   {
    if (sock_p->src_sun_path[0] != 0)  unlink(sock_p->src_sun_path);
   }
   /*
   ** Disconnect from socket controller
   */
   if (sock_p->connectionId != NULL)
   {
      ruc_sockctl_disconnect(sock_p->connectionId);
      sock_p->connectionId = NULL;
   }
   return 0;
 }



/**
*  Create a bunch of AF_UNIX socket associated with a Family

 @param  basename_p : Base name of the family
 @param  base_instance: index of the first instance
 @param  nb_instances: number of instance in the family
 @param  socket_tb_p : pointer to an array were the socket references will be stored
 @param  xmit_size : size for the sending buffer (SO_SNDBUF parameter)

 @retval: 0 success, all the socket have been created
 @retval < 0 error on at least one socket creation
*/
int af_unix_socket_family_create (char *basename_p, int base_instance,int nb_instances, int *socket_ctx_tb_p,af_unix_socket_conf_t *conf_p)
{
  int *local_socket_tb_p = socket_ctx_tb_p;
  char bufall[128];
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
     sprintf(bufall,"%s_inst_%d",basename_p,base_instance+i);
     conf_p->instance_id = base_instance+i;
     local_socket_tb_p[i] = af_unix_sock_create(bufall,conf_p);
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
/**
*  Set the maximum qlen of a AF_UNIX datagram socket

 @param  len : The length of the AF_UNIX datagram socket

 @retval: 0 success, all the socket have been created
 @retval < 0 error on at least one socket creation
*/
#define QLEN_FILE "/proc/sys/net/unix/max_dgram_qlen"
int af_unix_socket_set_datagram_socket_len(int len) {
#ifndef __linux__
    return 0;
#endif
  char new_value[32];
  int fd;
  int ret = 0;
  
  if ((fd=open(QLEN_FILE, O_RDWR)) < 0) {
    severe("set_datagram_socket_len(%d) open(%s) %s", len, QLEN_FILE,strerror(errno));
    return 1;
  } 
  
  sprintf(new_value,"%d",len);
  
  if (write (fd, &new_value, strlen(new_value)) < 0) {
    severe("set_datagram_socket_len(%d) write(%s) %s", len,QLEN_FILE, strerror(errno));
    ret = 1;
  } 
  
  close(fd);
  return ret;
}
