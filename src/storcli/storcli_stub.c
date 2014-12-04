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
#define FUSE_USE_VERSION 26

#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_opt.h>
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
#include "rozofs_storcli_rpc.h"
#include <rozofs/rpc/sproto.h>

int rozofs_storcli_fake_encode(xdrproc_t encode_fct,void *msg2encode_p)
{
    uint8_t *buf= NULL;
    XDR               xdrs;    
    struct rpc_msg   call_msg;
    int              opcode = 0;
    uint32_t         null_val = 0;
    int              position = -1;
    
    buf = malloc(2048);
    if (buf == NULL)
    {
       severe("Out of memory");
       goto out;
    
    }
    xdrmem_create(&xdrs,(char*)buf,2048,XDR_ENCODE);
    /*
    ** fill in the rpc header
    */
    call_msg.rm_direction = CALL;
    /*
    ** allocate a xid for the transaction 
    */
	call_msg.rm_xid             = 1; 
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	/* XXX: prog and vers have been long historically :-( */
	call_msg.rm_call.cb_prog = (uint32_t)0;
	call_msg.rm_call.cb_vers = (uint32_t)0;
	if (! xdr_callhdr(&xdrs, &call_msg))
    {
       /*
       ** THIS MUST NOT HAPPEN
       */
       severe("fake encoding error");
       goto out;	
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
       severe("fake encoding error");
       goto out;
    }
    /*
    ** Now get the current length and fill the header of the message
    */
    position = XDR_GETPOS(&xdrs);
    /*
    ** add the length that corresponds to the field that contains the length part
    ** of the rpc message
    */
    position += sizeof(uint32_t);
out:
    if (buf != NULL) free(buf);
    return position;    
}


int storcli_bin_first_byte = 0;


int rozofs_storcli_get_position_of_first_byte2write()
{
  sp_write_arg_no_bins_t *request; 
  sp_write_arg_no_bins_t  write_prj_args;
  int position;
  
  
  if (storcli_bin_first_byte == 0)
  {
    request = &write_prj_args;
    memset(request,0,sizeof(sp_write_arg_no_bins_t));
    position = rozofs_storcli_fake_encode((xdrproc_t) xdr_sp_write_arg_no_bins_t, (caddr_t) request);
    if (position < 0)
    {
      fatal("Cannot get the size of the rpc header for writing");
      return 0;    
    }
    storcli_bin_first_byte = position;
  }
  return storcli_bin_first_byte;

}

/*
**__________________________________________________________________________
*/
/**
* 
*/
int storcli_bin_truncate_first_byte = 0;
int rozofs_storcli_get_position_of_first_byte2write_in_truncate()
{
  sp_truncate_arg_no_bins_t  truncate_args;
  int position;
  
  
  if (storcli_bin_truncate_first_byte == 0)
  {
    memset(&truncate_args,0,sizeof(truncate_args));
    position = rozofs_storcli_fake_encode((xdrproc_t) xdr_sp_truncate_arg_no_bins_t, (caddr_t) &truncate_args);
    if (position < 0)
    {
      fatal("Cannot get the size of the rpc header for truncate no arg");
      return 0;    
    }
    storcli_bin_truncate_first_byte = position;
  }
  return storcli_bin_truncate_first_byte;

}
