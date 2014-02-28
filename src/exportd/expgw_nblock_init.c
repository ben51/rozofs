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

#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <semaphore.h>
#include <pthread.h>

#include <rozofs/rozofs.h>
#include <config.h>
#include <rozofs/core/ruc_list.h>
#include <rozofs/common/log.h>
#include <rozofs/common/htable.h>
#include <rozofs/common/xmalloc.h>
#include <rozofs/rpc/eproto.h>
#include <rozofs/core/rozofs_tx_common.h>
#include <rozofs/core/rozofs_tx_api.h>
#include <rozofs/core/af_unix_socket_generic_api.h>
#include <rozofs/rozofs_srv.h>
#include "expgw_main.h"
#include "expgw_export.h"
#include <rozofs/core/rozofs_rpc_non_blocking_generic_srv.h>


/*
**_________________________________________________________________________
*      PUBLIC FUNCTIONS
**_________________________________________________________________________
*/








#include <rozofs/core/ruc_common.h>
#include <rozofs/core/ruc_sockCtl_api.h>
#include <rozofs/core/ruc_timer_api.h>
#include <rozofs/core/ruc_trace_api.h>
#include <rozofs/core/uma_tcp_main_api.h>
#include <rozofs/core/uma_dbg_api.h>
#include <rozofs/core/ruc_tcpServer_api.h>
#include <rozofs/core/ruc_tcp_client_api.h>
#include <rozofs/core/ppu_trace.h>
#include <rozofs/core/uma_well_known_ports_api.h>
#include <rozofs/core/af_unix_socket_generic_api.h>
#include <rozofs/core/north_lbg_api.h>


// For trace purpose
struct timeval     Global_timeDay;
unsigned long long Global_timeBefore, Global_timeAfter;

/*
**
*/


uint32_t ruc_init(uint32_t test,uint16_t dbg_port,uint32_t local_ip_addr)
{
  int ret;


  uint32_t        mx_tcp_client = 2;
  uint32_t        mx_tcp_server = 2;
  uint32_t        mx_tcp_server_cnx = 10;
  uint32_t        mx_af_unix_ctx = 512;
  uint32_t        mx_lbg_north_ctx = 256;

//#warning TCP configuration ressources is hardcoded!!
  /*
  ** init of the system ticker
  */
  rozofs_init_ticker();
  /*
  ** trace buffer initialization
  */
  ruc_traceBufInit();
#if 1
 /*
 ** Not needed since there is already done
 ** by libUtil
 */

 /* catch the sigpipe signal for socket 
 ** connections with RELC(s) in this way when a RELC
 ** connection breaks an errno is set on a recv or send 
 **  socket primitive 
 */ 
  struct sigaction sigAction;
  
  sigAction.sa_flags=SA_RESTART;
  sigAction.sa_handler = SIG_IGN; /* Mask SIGPIPE */
  if(sigaction (SIGPIPE, &sigAction, NULL) < 0) 
  {
    exit(0);    
  }
#if 0
  sigAction.sa_flags=SA_RESTART;
  sigAction.sa_handler = hand; /*  */
  if(sigaction (SIGUSR1, &sigAction, NULL) < 0) 
  {
    exit(0);    
  }
#endif
#endif

   /*
   ** initialize the socket controller:
   **   4 connections per Relc and 32
   **   for: NPS, Timer, Debug, etc...
   */
//#warning set the number of contexts for socketCtrl to 1024
   ret = ruc_sockctl_init(1024);
   if (ret != RUC_OK)
   {
     ERRFAT " socket controller init failed" ENDERRFAT
   }

   /*
   **  Timer management init
   */
   ruc_timer_moduleInit(FALSE);

   while(1)
   {
     /*
     **--------------------------------------
     **  configure the number of TCP connection
     **  supported
     **--------------------------------------   
     **  
     */ 
     ret = uma_tcp_init(mx_tcp_client+mx_tcp_server+mx_tcp_server_cnx);
     if (ret != RUC_OK) break;

     /*
     **--------------------------------------
     **  configure the number of TCP server
     **  context supported
     **--------------------------------------   
     **  
     */    
     ret = ruc_tcp_server_init(mx_tcp_server);
     if (ret != RUC_OK) break;


     /*
     **--------------------------------------
     **  configure the number of AF_UNIX/AF_INET
     **  context supported
     **--------------------------------------   
     **  
     */    
     ret = af_unix_module_init(mx_af_unix_ctx,
                               32,1024*32, // xmit(count,size)
                               32,1024*32 // recv(count,size)
                               );
     if (ret != RUC_OK) break;   

     /*
     **--------------------------------------
     **  configure the number of Load Balancer
     **  contexts supported
     **--------------------------------------   
     **  
     */    
     ret = north_lbg_module_init(mx_lbg_north_ctx);
     if (ret != RUC_OK) break;   
     /*
     ** init of the storage client structure
     */
     ret = expgw_module_init();
     if (ret != RUC_OK) break; 
     
     /*
     ** RPC SERVER MODULE INIT
     */
     ret = rozorpc_srv_module_init();
     if (ret != RUC_OK) break; 
     
#if 0    
     ret = storcli_sup_moduleInit();
     if (ret != RUC_OK) break; 
#endif     
     ret = rozofs_tx_module_init(EXPGW_SOUTH_TX_CNT,  // transactions count
                                 EXPGW_CNF_NO_BUF_CNT,EXPGW_CNF_NO_BUF_SZ,        // xmit small [count,size]
                                 EXPGW_SOUTH_TX_XMIT_BUF_CNT,EXPGW_SOUTH_TX_XMIT_BUF_SZ,  // xmit large [count,size]
                                 EXPGW_CNF_NO_BUF_CNT,EXPGW_CNF_NO_BUF_SZ,        // recv small [count,size]
                                 EXPGW_SOUTH_TX_RECV_BUF_CNT,EXPGW_SOUTH_TX_RECV_BUF_SZ);  // recv large [count,size];  
     break;
     

   }
   /*
   ** internal debug init
   */
   //ruc_debug_init();


     /*
     **--------------------------------------
     **   D E B U G   M O D U L E
     **--------------------------------------
     */

     uma_dbg_init(10,local_ip_addr,dbg_port);


//#warning Start of specific application initialization code
 

 return ret;
}



/**
*  Init of the data structure used for the non blocking entity
  
  @param dbg_port: debug listening port
  @param local_ip_addr:IP debug address
  
  @retval 0 on success
  @retval -1 on error
*/
int expgw_non_blocking_init(uint16_t dbg_port, uint32_t local_ip_addr)
{
  int   ret;
//  sem_t semForEver;    /* semaphore for blocking the main thread doing nothing */


 ret = ruc_init(FALSE,dbg_port,local_ip_addr);
 
 if (ret != RUC_OK) return -1;
 
 return 0;

}



