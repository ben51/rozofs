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
#include <stdio.h>
#include <inttypes.h>
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
#include <config.h>

#include <rozofs/rozofs.h>
#include <rozofs/rozofs_debug_ports.h>
#include <rozofs/common/log.h>
#include <rozofs/common/profile.h>
#include <rozofs/core/ruc_common.h>
#include <rozofs/core/ruc_sockCtl_api.h>
#include <rozofs/core/ruc_timer_api.h>
#include <rozofs/core/uma_tcp_main_api.h>
#include <rozofs/core/uma_dbg_api.h>
#include <rozofs/core/ruc_tcpServer_api.h>
#include <rozofs/core/ruc_tcp_client_api.h>
#include <rozofs/core/uma_well_known_ports_api.h>
#include <rozofs/core/af_unix_socket_generic_api.h>
#include <rozofs/core/north_lbg_api.h>
#include <rozofs/core/ruc_list.h>
#include <rozofs/core/af_unix_socket_generic_api.h>
#include <rozofs/core/rozofs_rpc_non_blocking_generic_srv.h>
#include <rozofs/core/ruc_buffer_debug.h>
#include <rozofs/core/rozofs_host2ip.h>
#include <rozofs/rpc/eproto.h>
#include <rozofs/rpc/epproto.h>
#include <rozofs/rpc/sproto.h>

#include "storio_nblock_init.h"
#include "storio_north_intf.h"
#include "storage_fd_cache.h"
#include "storio_disk_thread_intf.h"
#include "sproto_nb.h"
#include "config.h"
#include "sconfig.h"
#include "storage.h"

extern sconfig_t storaged_config;

void * decoded_rpc_buffer_pool = NULL;

DECLARE_PROFILING(spp_profiler_t);

/*
 **_________________________________________________________________________
 *      PUBLIC FUNCTIONS
 **_________________________________________________________________________
 */



#define sp_display_probe(the_profiler, the_probe)\
    {\
        uint64_t rate;\
        uint64_t cpu;\
        if ((the_profiler.the_probe[P_COUNT] == 0) || (the_profiler.the_probe[P_ELAPSE] == 0) ){\
            cpu = rate = 0;\
        } else {\
            rate = (the_profiler.the_probe[P_COUNT] * 1000000 / the_profiler.the_probe[P_ELAPSE]);\
            cpu = the_profiler.the_probe[P_ELAPSE] / the_profiler.the_probe[P_COUNT];\
        }\
        pChar += sprintf(pChar, " %-16s | %-12"PRIu64" | %-12"PRIu64" | %-12"PRIu64" |\n",\
                #the_probe, the_profiler.the_probe[P_COUNT], \
                rate, cpu);\
    }

#define sp_display_io_probe(the_profiler, the_probe)\
    {\
        uint64_t rate;\
        uint64_t cpu;\
        uint64_t throughput;\
        if ((the_profiler.the_probe[P_COUNT] == 0) || (the_profiler.the_probe[P_ELAPSE] == 0) ){\
            cpu = rate = throughput = 0;\
        } else {\
            rate = (the_profiler.the_probe[P_COUNT] * 1000000 / the_profiler.the_probe[P_ELAPSE]);\
            cpu = the_profiler.the_probe[P_ELAPSE] / the_profiler.the_probe[P_COUNT];\
            throughput = (the_profiler.the_probe[P_BYTES] / 1024 /1024 * 1000000 / the_profiler.the_probe[P_ELAPSE]);\
        }\
        pChar += sprintf(pChar, " %-16s | %-12"PRIu64" | %-12"PRIu64" | %-12"PRIu64" | %-12"PRIu64" | %-12"PRIu64"     |\n",\
                 #the_probe, the_profiler.the_probe[P_COUNT], \
                rate, cpu, the_profiler.the_probe[P_BYTES], throughput);\
    }

#define sp_clear_io_probe(the_profiler, the_probe)\
    {\
       the_profiler.the_probe[P_COUNT] = 0;\
       the_profiler.the_probe[P_ELAPSE] = 0;\
       the_profiler.the_probe[P_BYTES] = 0;\
    }
static char * show_profile_storaged_io_display_help(char * pChar) {
  pChar += sprintf(pChar,"usage:\n");
  pChar += sprintf(pChar,"profiler reset       : reset statistics\n");
  pChar += sprintf(pChar,"profiler             : display statistics\n");  
  return pChar; 
}
static void show_profile_storaged_io_display(char * argv[], uint32_t tcpRef, void *bufRef) {

    char *pChar = uma_dbg_get_buffer();

    time_t elapse;
    int days, hours, mins, secs;

    if (argv[1] != NULL)
    {
      if (strcmp(argv[1],"reset")==0) {
	sp_clear_io_probe(gprofiler, read);
	sp_clear_io_probe(gprofiler, write);
	sp_clear_io_probe(gprofiler, truncate);
	uma_dbg_send(tcpRef, bufRef, TRUE, "Reset Done");
	return;      
      }
      pChar = show_profile_storaged_io_display_help(pChar);
      uma_dbg_send(tcpRef, bufRef, TRUE, uma_dbg_get_buffer());
      return;          
    }

    // Compute uptime for storaged process
    elapse = (int) (time(0) - gprofiler.uptime);
    days = (int) (elapse / 86400);
    hours = (int) ((elapse / 3600) - (days * 24));
    mins = (int) ((elapse / 60) - (days * 1440) - (hours * 60));
    secs = (int) (elapse % 60);


    pChar += sprintf(pChar, "GPROFILER version %s uptime =  %d days, %d:%d:%d\n\n", gprofiler.vers, days, hours, mins, secs);


    // Print header for operations profiling values for storaged
    pChar += sprintf(pChar, " %-16s | %-12s | %-12s | %-12s | %-12s | %-16s |\n", "OP",
            "CALL", "RATE(msg/s)", "CPU(us)", "COUNT(B)", "THROUGHPUT(MB/s)");
    pChar += sprintf(pChar, "------------------+--------------+--------------+--------------+--------------+------------------+\n");


    // Print master storaged process profiling values
    sp_display_io_probe(gprofiler, read);
    sp_display_io_probe(gprofiler, write);
    sp_display_io_probe(gprofiler, truncate);
    uma_dbg_send(tcpRef, bufRef, TRUE, uma_dbg_get_buffer());
}

// For trace purpose
struct timeval Global_timeDay;
unsigned long long Global_timeBefore, Global_timeAfter;



/*
 **
 */

void fdl_debug_loop(int line) {
    int loop = 1;

    return;
    while (loop) {
        sleep(5);
        info("Fatal error on nb thread create (line %d) !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ", line);

    }


}

uint32_t ruc_init(uint32_t test, storaged_start_conf_param_t *arg_p) {
    int ret = RUC_OK;


    uint32_t mx_tcp_client = 2;
    uint32_t mx_tcp_server = 2;
    uint32_t mx_tcp_server_cnx = 10;
    uint32_t local_ip = INADDR_ANY;
    uint32_t        mx_af_unix_ctx = 1024;
    


    if (arg_p->hostname[0] != 0) {
      rozofs_host2ip(arg_p->hostname, &local_ip);
    }

    //#warning TCP configuration ressources is hardcoded!!
    /*
     ** init of the system ticker
     */
    rozofs_init_ticker();
    /*
     ** trace buffer initialization
     */
    ruc_traceBufInit();

    /*
     ** initialize the socket controller:
     **   for: NPS, Timer, Debug, etc...
     */
    // warning set the number of contexts for socketCtrl to 1024
    ret = ruc_sockctl_init(1024);
    if (ret != RUC_OK) {
        fdl_debug_loop(__LINE__);
        fatal( " socket controller init failed" );
    }

    /*
     **  Timer management init
     */
    ruc_timer_moduleInit(FALSE);

    while (1) {
        /*
         **--------------------------------------
         **  configure the number of TCP connection
         **  supported
         **--------------------------------------   
         **  
         */
        ret = uma_tcp_init(mx_tcp_client + mx_tcp_server + mx_tcp_server_cnx);
        if (ret != RUC_OK) {
            fdl_debug_loop(__LINE__);
            break;
        }

        /*
         **--------------------------------------
         **  configure the number of TCP server
         **  context supported
         **--------------------------------------   
         **  
         */
        ret = ruc_tcp_server_init(mx_tcp_server);
        if (ret != RUC_OK) {
            fdl_debug_loop(__LINE__);
            break;
        }


        /*
        **--------------------------------------
        **  configure the number of AF_UNIX/AF_INET
        **  context supported
        **--------------------------------------   
        **  
        */    
        ret = af_unix_module_init(mx_af_unix_ctx,
                                  2,1024*1, // xmit(count,size)
                                  2,1024*1 // recv(count,size)
                                  );
        if (ret != RUC_OK) break;   
        /*
         **--------------------------------------
         **   D E B U G   M O D U L E
         **--------------------------------------
         */

        uma_dbg_init(10, local_ip, arg_p->debug_port);

        {
            char name[256];
	    sprintf(name, "storio%d %s", arg_p->instance_id, arg_p->hostname);
            uma_dbg_set_name(name);
        }
        /*
        ** RPC SERVER MODULE INIT
        */
        ret = rozorpc_srv_module_init();
        if (ret != RUC_OK) break; 


        break;
    }

    //#warning Start of specific application initialization code


    return ret;
}


/*
 *_______________________________________________________________________
 */

/**
 *  This function is the entry point for setting rozofs in non-blocking mode

   @param args->ch: reference of the fuse channnel
   @param args->se: reference of the fuse session
   @param args->max_transactions: max number of transactions that can be handled in parallel
   
   @retval -1 on error
   @retval : no retval -> only on fatal error

 */
int storio_start_nb_th(void *args) {
  int ret;
  storaged_start_conf_param_t *args_p = (storaged_start_conf_param_t*) args;
  int size = 0;

  ret = ruc_init(FALSE, args_p);
  if (ret != RUC_OK) {
    /*
     ** fatal error
     */
    fdl_debug_loop(__LINE__);
    fatal("can't initialize non blocking thread");
    return -1;
  }

  /*
  ** Create a buffer pool to decode spproto RPC requests
  */
  size = sizeof(sp_write_arg_no_bins_t);
  if (size < sizeof(sp_read_arg_t)) size = sizeof(sp_read_arg_t);
  if (size < sizeof(sp_truncate_arg_no_bins_t)) size = sizeof(sp_truncate_arg_no_bins_t);
  decoded_rpc_buffer_pool = ruc_buf_poolCreate(ROZORPC_SRV_CTX_CNT,size);
  if (decoded_rpc_buffer_pool == NULL) {
    fatal("Can not allocate decoded_rpc_buffer_pool");
    return -1;
  }
  ruc_buffer_debug_register_pool("rpcDecodedRequest",decoded_rpc_buffer_pool);

  /*
  ** Initialize the disk thread interface and start the disk threads
  */	
  ret = storio_disk_thread_intf_create(args_p->hostname,args_p->instance_id, storaged_config.nb_disk_threads, ROZORPC_SRV_CTX_CNT) ;
  if (ret < 0) {
    fatal("storio_disk_thread_intf_create");
    return -1;
  }

  /*
  ** Init of the north interface (read/write request processing)
  */ 
  ret = storio_north_interface_buffer_init(ROZORPC_SRV_CTX_CNT, STORIO_BUF_RECV_SZ);
  if (ret < 0) {
    fatal("Fatal error on storio_north_interface_buffer_init()\n");
    return -1;
  }
  ret = storio_north_interface_init(args_p->hostname,args_p->instance_id);
  if (ret < 0) {
    fatal("Fatal error on storio_north_interface_init()\n");
    return -1;
  }
  
  /*
  ** A timing counter service
  */ 
  detailed_counters_init();
   
  /*
  ** init of the fd cache
  */
  storage_fd_cache_init(48,16);

  /*
  ** add profiler subject 
  */
  uma_dbg_addTopic_option("profiler", show_profile_storaged_io_display,UMA_DBG_OPTION_RESET);

    if ((args_p->hostname[0] != 0)) {
        info("storio started (instance: %d, host: %s, dbg port: %d).",
                args_p->instance_id, args_p->hostname, args_p->debug_port);
    } else {
        info("storio started (instance: %d, dbg port: %d).",
                args_p->instance_id, args_p->debug_port);
    }

  /*
   ** main loop
   */
  while (1) {
      ruc_sockCtrl_selectWait();
  }
  fatal("Exit from ruc_sockCtrl_selectWait()");
  fdl_debug_loop(__LINE__);
}
