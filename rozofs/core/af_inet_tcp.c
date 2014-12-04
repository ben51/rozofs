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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include <rozofs/common/types.h>
#include <rozofs/common/log.h>

#include "ruc_common.h"
#include "ruc_list.h"
#include "af_unix_socket_generic.h"
#include "af_unix_socket_generic_api.h"
#include "af_inet_stream_api.h"

/*
**__________________________________________________________________________
*/
/**
*
** Tune the configuration of the socket with:
**   - TCP KeepAlive,
**   - asynchrounous xmit/receive,
**   -  new sizeof  buffer for xmit/receive
**
**  IN : socketId
**
**  OUT: RUC_OK : success
**       RUC_NOK : error
*/
uint32_t af_inet_tcp_tuneTcpSocket(int socketId,int size)
{

  int sockSndSize = size*4;
  int sockRcvdSize = size*4;
  int fileflags;

#if 1
  int YES = 1;
  int IDLE = 2;
  int INTVL = 2;
  int COUNT = 3;
 /*
  ** active keepalive on the new connection
  */
  if (setsockopt (socketId,SOL_SOCKET,
                  SO_KEEPALIVE,&YES,sizeof(int)) == -1)
  {
    return RUC_NOK;
  }

  if (setsockopt (socketId,IPPROTO_TCP,
                  TCP_KEEPIDLE,&IDLE,sizeof(int)) == -1)
  {
    return RUC_NOK;
  }
  if (setsockopt (socketId,IPPROTO_TCP,
                  TCP_KEEPINTVL,&INTVL,sizeof(int)) == -1)
  {
    return RUC_NOK;
  }
  if (setsockopt (socketId,IPPROTO_TCP,
                  TCP_KEEPCNT,&COUNT,sizeof(int)) == -1)
  {
    return RUC_NOK;
  }
#endif

#if 1
  int UMA_TCP_NODELAY = 1;
  if (setsockopt (socketId,IPPROTO_TCP,
                  TCP_NODELAY,&UMA_TCP_NODELAY,sizeof(int)) == -1)
  {
    perror("TCP_NODELAY");
    return RUC_NOK;
  }
#endif


  /*
  ** change sizeof the buffer of socket for sending
  */
  if (setsockopt (socketId,SOL_SOCKET,
                  SO_SNDBUF,&sockSndSize,sizeof(int)) == -1)
  {
    return RUC_NOK;
  }
  /*
  ** change sizeof the buffer of socket for receiving
  */
  if (setsockopt (socketId,SOL_SOCKET,
                  SO_RCVBUF,&sockRcvdSize,sizeof(int)) == -1)
  {
    return RUC_NOK;
  }



  /*
  ** change socket mode to asynchronous
  */
  if((fileflags=fcntl(socketId,F_GETFL,0))==-1)
  {
    return RUC_NOK;
  }
  if((fcntl(socketId,F_SETFL,fileflags|O_NDELAY))==-1)
  {
    return RUC_NOK;
  }

  return RUC_OK;

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
int af_inet_sock_stream_client_create_internal(af_unix_ctx_generic_t *sock_p,int size)
{
  int ret;
  int fd=-1;
  int fdsize;
  int optionsize=sizeof(fdsize);
  struct sockaddr_in vSckAddr;
  int fileflags;

  /*
  ** create a datagram socket
  */
  fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd<0)
  {
    RUC_WARNING(errno);
   return -1;
  }
  /*
  **  It the source IP address is not IP_ANYADDR then bind
  **  the socket to the IP and TCP port
  */
  if (sock_p->src_ipaddr_host != INADDR_ANY)
  {
    memset(&vSckAddr, 0, sizeof(struct sockaddr_in));
    vSckAddr.sin_family = AF_INET;
    vSckAddr.sin_port   = htons(sock_p->src_port_host);
    vSckAddr.sin_addr.s_addr = htonl(sock_p->src_ipaddr_host);
    if((bind(fd,
            (struct sockaddr *)&vSckAddr,
             sizeof(struct sockaddr_in)))< 0)
    {
      /*
      **  error on socket binding
      */
      severe( "fail to bind socket with IP %x and TCP %x, errno = %u",sock_p->src_ipaddr_host,sock_p->src_ipaddr_host,errno );
      close(fd);
      return ((uint32_t)-1);
    }
  }

  /*
  ** change the length for the send buffer, nothing to do for receive buf
  ** since it is out of the scope of the AF_SOCKET
  */
  ret= getsockopt(fd,SOL_SOCKET,SO_SNDBUF,(char*)&fdsize,(socklen_t *)&optionsize);
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
  ** set a new size for emission and
  ** reception socket's buffer
  */
  ret=setsockopt(fd,SOL_SOCKET,SO_RCVBUF,(char*)&fdsize,sizeof(int));
  if(ret<0)
  {
    RUC_WARNING(errno);
   return -1;
  }

#if 1
  int UMA_TCP_NODELAY = 1;
  if (setsockopt (fd,IPPROTO_TCP,
                  TCP_NODELAY,&UMA_TCP_NODELAY,sizeof(int)) == -1)
  {
    perror("TCP_NODELAY");
    return RUC_NOK;
  }
#endif

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
  int YES = 1;
  int IDLE = 10;
  int INTVL = 2;
  int COUNT = 3;
 /*
  ** active keepalive on the new connection
  */
#if 1
  if (setsockopt (fd,SOL_SOCKET,
                  SO_KEEPALIVE,&YES,sizeof(int)) == -1)
  {
    RUC_WARNING(errno);
    return -1;
  }

  if (setsockopt (fd,IPPROTO_TCP,
                  TCP_KEEPIDLE,&IDLE,sizeof(int)) == -1)
  {
    RUC_WARNING(errno);
    return -1;
  }
  if (setsockopt (fd,IPPROTO_TCP,
                  TCP_KEEPINTVL,&INTVL,sizeof(int)) == -1)
  {
    RUC_WARNING(errno);
    return -1;
  }
  if (setsockopt (fd,IPPROTO_TCP,
                  TCP_KEEPCNT,&COUNT,sizeof(int)) == -1)
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
*
    Create the socket and associate it with
    the TCP port provided as input parameter
    
    
    WARNING: This function re-attempts several time the bind() call in case 
    it gets an error, and sleeps between each attempts. Such a behavior is 
    obviously only acceptable during an application startup !
    
    This behavior tries to handle the case of a fast stop restart of a 
    process, when the dying process is not actually dead while the new one
    already starts up. In this case the bind will fail even with option REUSEADDR.
    The starting process has to wait a little to let the previous process die
    before re-attempting a bind. 

   @param tcpPort : tcp well-known port
   @param ipAddr :  source IP address (could be ANY)

  @retval :-1 : error. The socket is not created
  @retval :!=-1: socket id

*/

int af_inet_sock_stream_listen_create_internal(uint32_t ipAddr,uint16_t tcpPort)
{
  int                 socketId;
  struct  sockaddr_in vSckAddr;
  int		      sock_opt;
  int                 retry=9;
  int                 ret;

  if((socketId = socket(AF_INET, SOCK_STREAM, 0)) < 0)
  {
    /*
    ** unable to create the socket
    */
    severe( "af_inet_sock_stream_listen_create socket error %u.%u.%u.%u:%u . Errno %d - %s",
      (ipAddr>>24)&0xFF,  (ipAddr>>16)&0xFF,(ipAddr>>8)&0xFF ,ipAddr&0xFF,
      tcpPort,
      errno, strerror(errno)
    );;
    return -1;
  }

  /* Tell the system to allow local addresses to be reused. */
  sock_opt = 1;
  if (setsockopt(socketId, SOL_SOCKET, SO_REUSEADDR, (void *)&sock_opt,sizeof (sock_opt)) == -1)
  {
     severe( "af_inet_sock_stream_listen_create setsockopt error %u.%u.%u.%u:%u . Errno %d - %s",
      (ipAddr>>24)&0xFF,  (ipAddr>>16)&0xFF,(ipAddr>>8)&0xFF ,ipAddr&0xFF,
      tcpPort,
      errno, strerror(errno)
     );
  }


  /*
  ** bind it to the well-known port
  */
  memset(&vSckAddr, 0, sizeof(struct sockaddr_in));
  vSckAddr.sin_family = AF_INET;
  vSckAddr.sin_port   = htons(tcpPort);
  vSckAddr.sin_addr.s_addr = htonl(ipAddr);
  ret = bind(socketId,(struct sockaddr *)&vSckAddr,sizeof(struct sockaddr_in));
  while (ret<0) {
  
    if (retry <= 0) {
      /*
      **  error on socket binding
      */
      severe( "af_inet_sock_stream_listen_create BIND error %u.%u.%u.%u:%u . Errno %d - %s",
	(ipAddr>>24)&0xFF,  (ipAddr>>16)&0xFF,(ipAddr>>8)&0xFF ,ipAddr&0xFF,
	tcpPort,
	errno, strerror(errno)
      );;
      close(socketId);
      return -1;
    }
    
    retry--;
    usleep(300000);
    ret = bind(socketId,(struct sockaddr *)&vSckAddr,sizeof(struct sockaddr_in));
  }   

  return socketId;
}


/*
**__________________________________________________________________________
*/
/**
   Creation of a AF_UNIX stream socket in non blocking mode

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
   @param src_ipaddr_host : IP address in host format
   @param src_port_host : port in host format
   @param size: size in byte of the xmit buffer (the service double that value

    retval: !=0 reference of the created socket context
    retval < 0 : error on socket context creation

*/
int af_inet_sock_listening_create(char *nickname,
                                  uint32_t src_ipaddr_host,uint16_t src_port_host,
                                  af_unix_socket_conf_t *conf_p)
{
   af_unix_ctx_generic_t *sock_p;
  com_xmit_template_t    *xmit_p;
  com_recv_template_t    *recv_p;
  char buf_nickname[ROZOFS_SOCK_EXTNAME_SIZE];
  char *buf_nickname_p;
  buf_nickname_p = buf_nickname;

  int ret = -1;


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
   xmit_p = &sock_p->xmit;
   recv_p = &sock_p->recv;
   sock_p->af_family = AF_INET;

   while(1)
   {

   sock_p->src_ipaddr_host    = src_ipaddr_host;
   sock_p->src_port_host      = src_port_host;
   /*
   ** Copy the name of the socket
   */
   strcpy(sock_p->nickname,nickname);
   buf_nickname_p+= sprintf(buf_nickname_p,"L:%s/",nickname);
   /*
   ** put the IP address and port
   */
   if (sock_p->src_ipaddr_host == INADDR_ANY)
   {
     buf_nickname_p+= sprintf(buf_nickname_p,"*:%u",sock_p->src_port_host);
   }
   else
   {
     buf_nickname_p+= sprintf(buf_nickname_p,"%u.%u.%u.%u:%u",
                        (sock_p->src_ipaddr_host>>24)&0xFF,
                        (sock_p->src_ipaddr_host>>16)&0xFF,
                        (sock_p->src_ipaddr_host>>8)&0xFF ,
                         sock_p->src_ipaddr_host&0xFF,sock_p->src_port_host);

   }

   /*
   ** create the socket
   */
   sock_p->socketRef = af_inet_sock_stream_listen_create_internal(sock_p->src_ipaddr_host,sock_p->src_port_host);
   if (sock_p->socketRef == -1)
   {
     ret = -1;
     break;
   }
   /*
   ** store the use configuration
   */
   sock_p->conf_p = conf_p;

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
   ** install the user reference
   */
   sock_p->userRef = conf_p->userRef;
   /*
   ** Call the listen
   */
   if((listen(sock_p->socketRef,5))==-1)
   {
     severe( " ruc_tcp_server_connect: listen fails for %s ,",sock_p->nickname );
     break;
   }

   /*
   ** OK, we are almost done, just need to connect with the socket controller
   */
   sock_p->connectionId = ruc_sockctl_connect(sock_p->socketRef,  // Reference of the socket
                                              buf_nickname,   // name of the socket
                                              16,                  // Priority within the socket controller
                                              (void*)sock_p,      // user param for socketcontroller callback
                                              &af_unix_generic_listening_callBack_sock);  // Default callbacks
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
//   unlink(sock_p->sockname);
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
   Creation of a AF_INET socket stream  in non blocking mode

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

     The bind on the source is only done if the source IP address address is different from INADDR_ANY

   @param nickname : name of socket for socket controller display name
   @param src_ipaddr_host : IP address in host format
   @param src_port_host : port in host format
   @param remote_ipaddr_host : IP address in host format
   @param remote_port_host : port in host format
   @param size: size in byte of the xmit buffer (the service double that value

    retval: !=0 reference of the created socket context
    retval < 0 : error on socket context creation

*/
int af_inet_sock_client_create(char *nickname,
                                uint32_t src_ipaddr_host,uint16_t src_port_host,
                                uint32_t remote_ipaddr_host,uint16_t remote_port_host,
                                af_unix_socket_conf_t *conf_p)
{
   af_unix_ctx_generic_t *sock_p;
  com_xmit_template_t    *xmit_p;
  com_recv_template_t    *recv_p;
  int ret = -1;
  int len;
  int release_req = 0;
  char *buf_nickname_p;

   /*
   ** the client must provide a callback to get the status of the connect operation
   */
   if (conf_p->userConnectCallBack == NULL)
   {
     RUC_WARNING(-1);

   }
   len = strlen(nickname);
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
   xmit_p = &sock_p->xmit;
   recv_p = &sock_p->recv;

   while(1)
   {
   sock_p->af_family = AF_INET;
   sock_p->src_ipaddr_host    = src_ipaddr_host;
   sock_p->src_port_host      = src_port_host;
   sock_p->remote_ipaddr_host = remote_ipaddr_host;
   sock_p->remote_port_host   = remote_port_host;

   /*
   ** Copy the name of the remote socket
   */
   buf_nickname_p = sock_p->nickname;
   buf_nickname_p+= sprintf(buf_nickname_p,"C:%s/",nickname);
   /*
   ** put the IP address and port
   */
   {
     buf_nickname_p+= sprintf(buf_nickname_p,"%u.%u.%u.%u:%u",
                        (sock_p->remote_ipaddr_host>>24)&0xFF,
                        (sock_p->remote_ipaddr_host>>16)&0xFF,
                        (sock_p->remote_ipaddr_host>>8)&0xFF ,
                         sock_p->remote_ipaddr_host&0xFF,sock_p->remote_port_host);

   }


   /*
   ** create the socket and save the sending buffer size to address the case of the reconnection
   */
   sock_p->so_sendbufsize = conf_p->so_sendbufsize;
   sock_p->socketRef = af_inet_sock_stream_client_create_internal(sock_p,sock_p->so_sendbufsize);
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
                                              buf_nickname_p,   // name of the socket
                                              3,                  // Priority within the socket controller
                                              (void*)sock_p,      // user param for socketcontroller callback
                                              &af_unix_generic_client_callBack_sock);  // Default callbacks
    if (sock_p->connectionId == NULL)
    {
       /*
       ** Fail to connect with the socket controller
       */
       RUC_WARNING(-1);
       release_req = 1;
       break;
    }
    
    
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
    
    /*
    ** do the connect
    */
    /* TCP connection case */
    uint32_t 	IpAddr;
    struct sockaddr_in vSckAddr;

    vSckAddr.sin_family = AF_INET;
    vSckAddr.sin_port   = htons(sock_p->remote_port_host);
    IpAddr = htonl(sock_p->remote_ipaddr_host);
    memcpy(&vSckAddr.sin_addr.s_addr, &IpAddr, sizeof(uint32_t));

    connect(sock_p->socketRef,(const struct sockaddr *)&vSckAddr,sizeof(struct sockaddr_in));
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
/*
**__________________________________________________________________________
*/
/**
   Modify the destination port of a client AF_INET socket
   
   @param sockRef : socket reference 
   @param remote_port_host : port in host format

    retval: 0 when done
    retval < 0 :if socket do not exist

*/
int af_inet_sock_client_modify_destination_port(int sockRef, uint16_t remote_port_host)
{
  af_unix_ctx_generic_t *sock_p;
  char *buf_nickname_p;

   /*
   ** Retrieve the socket context from its reference
   */
   sock_p = af_unix_getObjCtx_p(sockRef);
   if (sock_p == NULL)
   {
     return -1;
   }

   sock_p->remote_port_host = remote_port_host;

   /*
   ** Copy the name of the remote socket
   */
   buf_nickname_p = sock_p->nickname;
   while ((*buf_nickname_p != 0)&&(*buf_nickname_p != '/')) buf_nickname_p++;
   if (*buf_nickname_p == 0) return 0;   
   buf_nickname_p++;
   while ((*buf_nickname_p != 0)&&(*buf_nickname_p != ':')) buf_nickname_p++;
   if (*buf_nickname_p == 0) return 0;  
   buf_nickname_p++;
   
   sprintf(buf_nickname_p,"%u",remote_port_host);
   return 0;

}
