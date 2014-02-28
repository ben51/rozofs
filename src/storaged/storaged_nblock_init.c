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
#include <rozofs/rpc/eproto.h>
#include <rozofs/rpc/epproto.h>

#include "storaged_nblock_init.h"
#include "storaged_north_intf.h"

uint32_t storio_nb = 0;

DECLARE_PROFILING(spp_profiler_t);

extern uint8_t rbs_start_process;
/*
 **_________________________________________________________________________
 *      PUBLIC FUNCTIONS
 **_________________________________________________________________________
 */

#define sp_display_rbs_progress_probe(the_cid, the_sid,\
                                        the_probe_1, the_probe_2, the_probe_3)\
    {\
        if ( the_probe_3 == 0 ) {\
            pChar += sprintf(pChar,\
                               "%-12"PRIu16" | %-12"PRIu8" | %-16s | "\
                               "%"PRIu64"/%-12"PRIu64" |\n",\
                               the_cid, the_sid, "not started",\
                               the_probe_2, the_probe_1);\
        }\
        if ( the_probe_3 == 1 ) {\
            pChar += sprintf(pChar,\
                               "%-12"PRIu16" | %-12"PRIu8" | %-16s | "\
                               "%"PRIu64"/%-12"PRIu64" |\n",\
                               the_cid, the_sid, "getting cluster list",\
                               the_probe_2, the_probe_1);\
        }\
        if ( the_probe_3 == 2 ) {\
            pChar += sprintf(pChar,\
                               "%-12"PRIu16" | %-12"PRIu8" | %-16s | "\
                               "%"PRIu64"/%-12"PRIu64" |\n",\
                               the_cid, the_sid, "init connections",\
                               the_probe_2, the_probe_1);\
        }\
        if ( the_probe_3 == 3 ) {\
            pChar += sprintf(pChar,\
                               "%-12"PRIu16" | %-12"PRIu8" | %-16s | "\
                               "%"PRIu64"/%-12"PRIu64" |\n",\
                               the_cid, the_sid, "getting list of files",\
                               the_probe_2, the_probe_1);\
        }\
        if ( the_probe_3 == 4 ) {\
            pChar += sprintf(pChar,\
                               "%-12"PRIu16" | %-12"PRIu8" | %-16s | "\
                               "%"PRIu64"/%-12"PRIu64" |\n",\
                               the_cid, the_sid, "rebuild in progress",\
                               the_probe_2, the_probe_1);\
        }\
        if ( the_probe_3 == 5 ) {\
            pChar += sprintf(pChar,\
                               "%-12"PRIu16" | %-12"PRIu8" | %-16s | "\
                               "%"PRIu64"/%-12"PRIu64" |\n",\
                               the_cid, the_sid, "completed",\
                               the_probe_2, the_probe_1);\
        }\
    }

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

#define sp_clear_probe(the_profiler, the_probe)\
    {\
      the_profiler.the_probe[P_COUNT] = 0;\
      the_profiler.the_probe[P_ELAPSE] = 0;\
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

static char * show_profile_storaged_master_display_help(char * pChar) {
  pChar += sprintf(pChar,"usage:\n");
  pChar += sprintf(pChar,"profiler reset       : reset statistics\n");
  pChar += sprintf(pChar,"profiler             : display statistics\n");  
  return pChar; 
}

static void show_profile_storaged_master_display(char * argv[], uint32_t tcpRef, void *bufRef) {
    char *pChar = uma_dbg_get_buffer();

    time_t elapse;
    int days, hours, mins, secs;

    if (argv[1] != NULL) {

        if (strcmp(argv[1], "reset") == 0) {

            sp_clear_probe(gprofiler, stat);
            sp_clear_probe(gprofiler, ports);
            sp_clear_probe(gprofiler, remove);
            sp_clear_probe(gprofiler, list_bins_files);
            uma_dbg_send(tcpRef, bufRef, TRUE, "Reset Done");

            return;
        }

        pChar = show_profile_storaged_master_display_help(pChar);
        uma_dbg_send(tcpRef, bufRef, TRUE, uma_dbg_get_buffer());

        return;
    }

    // Compute uptime for storaged process
    elapse = (int) (time(0) - gprofiler.uptime);
    days = (int) (elapse / 86400);
    hours = (int) ((elapse / 3600) - (days * 24));
    mins = (int) ((elapse / 60) - (days * 1440) - (hours * 60));
    secs = (int) (elapse % 60);

    // Print general profiling values for storaged
    pChar += sprintf(pChar, "storaged: %s - %"PRIu16" IO process(es),"
            " uptime: %d days, %d:%d:%d\n\n",
            gprofiler.vers, gprofiler.nb_io_processes, days, hours, mins, secs);

    // Print header for operations profiling values for storaged
    pChar += sprintf(pChar, " %-16s | %-12s | %-12s | %-12s |\n", "OP",
            "CALL", "RATE(msg/s)", "CPU(us)");
    pChar += sprintf(pChar, "------------------+--------------+--------------+--------------+\n");

    // Print master storaged process profiling values
    sp_display_probe(gprofiler, stat);
    sp_display_probe(gprofiler, ports);
    sp_display_probe(gprofiler, remove);
    sp_display_probe(gprofiler, list_bins_files);

    uma_dbg_send(tcpRef, bufRef, TRUE, uma_dbg_get_buffer());
}

static void show_rebuild_storage_display(char * argv[], uint32_t tcpRef,
        void *bufRef) {

    char *pChar = uma_dbg_get_buffer();
    int i = 0;

    // Print RBS storaged process profiling values
    if (gprofiler.nb_rb_processes != 0) {

        pChar += sprintf(pChar,
                "Nb. of storage(s) to rebuild at startup: %"PRIu16"\n",
                gprofiler.nb_rb_processes);

        pChar += sprintf(pChar, "%-12s | %-12s | %-16s | %-16s |\n",
                "CID", "SID", "STATUS", "FILES REBUILD");

        pChar += sprintf(pChar, "-------------+--------------+-----------------"
                "-+------------------+\n");

        for (i = 0; i < gprofiler.nb_rb_processes; i++) {

            sp_display_rbs_progress_probe(
                    gprofiler.rbs_cids[i], gprofiler.rbs_sids[i],
                    gprofiler.rb_files_total[i], gprofiler.rb_files_current[i],
                    gprofiler.rb_status[i]);

        }

    }

    uma_dbg_send(tcpRef, bufRef, TRUE, uma_dbg_get_buffer());
}
static void show_storio_nb(char * argv[], uint32_t tcpRef, void *bufRef) {
    char *pChar = uma_dbg_get_buffer();
    
    sprintf(pChar,"storio_nb : %d\n", storio_nb);
    uma_dbg_send(tcpRef, bufRef, TRUE, uma_dbg_get_buffer());
}


// For trace purpose
struct timeval Global_timeDay;
unsigned long long Global_timeBefore, Global_timeAfter;


/*
 **______________________________________________________________________________
 */

/**
 *  Convert a hostname into an IP v4 address in host format 

@param host : hostname
@param ipaddr_p : return IP V4 address arreay

@retval 0 on success
@retval -1 on error (see errno faor details
 */
static int host2ip(char *host, uint32_t *ipaddr_p) {
    struct hostent *hp;
    /*
     ** get the IP address of the storage node
     */
    if ((hp = gethostbyname(host)) == 0) {
        severe("gethostbyname failed for host : %s, %s", host,
                strerror(errno));
        return -1;
    }
    bcopy((char *) hp->h_addr, (char *) ipaddr_p, hp->h_length);
    *ipaddr_p = ntohl(*ipaddr_p);
    return 0;

}

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
        host2ip(arg_p->hostname, &local_ip);
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
    //#warning set the number of contexts for socketCtrl to 1024
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
            sprintf(name, "storaged %s", arg_p->hostname);
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
int storaged_start_nb_blocking_th(void *args) {
  int ret;
  storaged_start_conf_param_t *args_p = (storaged_start_conf_param_t*) args;

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
  ** Init of the north interface (read/write request processing)
  */ 
  ret = storaged_north_interface_buffer_init(STORAGED_BUF_RECV_CNT, STORAGED_BUF_RECV_SZ);
  if (ret < 0) {
    fatal("Fatal error on storage_north_interface_buffer_init()\n");
    return -1;
  }
  ret = storaged_north_interface_init(args_p->hostname);
  if (ret < 0) {
    fatal("Fatal error on storage_north_interface_init()\n");
    return -1;
  }
   
  uma_dbg_addTopic_option("profiler", show_profile_storaged_master_display,UMA_DBG_OPTION_RESET);

    info("storaged non-blocking thread started "
            "(instance: %d, host: %s, port: %d).",
            args_p->instance_id, args_p->hostname, args_p->debug_port);

    /*
     ** main loop
     */
    while (1) {
        ruc_sockCtrl_selectWait();
    }
    fatal("Exit from ruc_sockCtrl_selectWait()");
    fdl_debug_loop(__LINE__);
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
int storaged_start_nb_th(void *args) {
    int ret;
    storaged_start_conf_param_t *args_p = (storaged_start_conf_param_t*) args;

    ret = ruc_init(FALSE, args_p);
    if (ret != RUC_OK) {
        /*
         ** fatal error
         */
        fdl_debug_loop(__LINE__);
        fatal("ruc_init() can't initialize storaged non blocking thread");
        return -1;
    }


    /*
     ** Init of the north interface (read/write request processing)
     */
    ret = storaged_north_interface_buffer_init(STORAGED_BUF_RECV_CNT, STORAGED_BUF_RECV_SZ);
    if (ret < 0) {
        fatal("Fatal error on storaged_north_interface_buffer_init()\n");
        return -1;
    }
    ret = storaged_north_interface_init(args_p->hostname);
    if (ret < 0) {
        fatal("Fatal error on storaged_north_interface_init()\n");
        return -1;
    }

    /*
     ** add profiler subject 
     */
    uma_dbg_addTopic_option("profiler", show_profile_storaged_master_display,UMA_DBG_OPTION_RESET);
    
    storio_nb = args_p->io_port;
    uma_dbg_addTopic("storio_nb", show_storio_nb);


    /*
     ** add rebuild storage subject
     */
    if (rbs_start_process == 1) {
        uma_dbg_addTopic("rebuild", show_rebuild_storage_display);
    }

    if ((args_p->hostname[0] != 0)) {
        info("storaged non-blocking thread started (host: %s, dbg port: %d).",
                args_p->hostname, args_p->debug_port);
    } else {
        info("storaged non-blocking thread started (dbg port: %d).", 
                args_p->debug_port);
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
