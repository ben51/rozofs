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

#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <pthread.h>

#include <rozofs/rozofs.h>
#include <rozofs/common/log.h>
#include <rozofs/core/ruc_buffer_api.h>
#include <rozofs/core/ruc_common.h>
#include <rozofs/core/ruc_sockCtl_api.h>
#include <rozofs/core/ruc_timer_api.h>
#include <rozofs/core/ruc_trace_api.h>
#include <rozofs/core/uma_tcp_main_api.h>
#include <rozofs/core/uma_dbg_api.h>
#include <rozofs/core/ruc_tcpServer_api.h>
#include <rozofs/core/ruc_tcp_client_api.h>
#include <rozofs/core/af_inet_stream_api.h>
#include <rozofs/rpc/gwproto.h>
#include <rozofs/rpc/gwproto_cache.h>

#define EXP_MAX_CACHE_SRV  128  /**< max number of servers involved in a distributed cache */
#define EXP_CHILD_BITMAP_BIT_SZ  256
#define EXP_CHILD_BITMAP_BYTE_SZ  (EXP_CHILD_BITMAP_BIT_SZ/8)
#define EXP_PARENT_BITMAP_BIT_SZ  256
#define EXP_PARENT_BITMAP_BYTE_SZ  (EXP_PARENT_BITMAP_BIT_SZ/8)


typedef struct _exp_dirty_dirty_child_t
{
   uint32_t  bitmap;
   uint8_t   child_bitmap[EXP_CHILD_BITMAP_BYTE_SZ];
} exp_dirty_dirty_child_t;
typedef struct _exp_dirty_parent_t
{
   uint32_t parent_update_count;  /**< number of time a bit has been changed in the parent bitmap */
   uint32_t child_update_count;    /**< number of time a bit has been changed in the child bitmaps */
   uint8_t parent_bitmap[EXP_PARENT_BITMAP_BYTE_SZ];
} exp_dirty_dirty_parent_t;



typedef struct _exp_cache_srv_front_end_t
{
  int  active_idx;
  exp_dirty_dirty_parent_t *parent[2]; 
  exp_dirty_dirty_child_t  *child[2];
} exp_cache_srv_front_end_t;


typedef struct _exp_cache_dirty_ctx_t
{
   uint32_t level0_sz;                            /**< size of the level 0 associated remote cache (in power of 2)  */
   uint32_t nb_cache_servers;                      /**< number of cache server        */
   uint32_t total_update_count;                   /**< number of time a change occured */
   exp_cache_srv_front_end_t   *srv_rank[EXP_MAX_CACHE_SRV];
} exp_cache_dirty_ctx_t;


uint32_t exp_cache_xid = 1;


typedef struct _exp_cache_cnf_t {
  uint32_t   export_id;
  uint32_t   eid_nb;
  uint32_t * eid_list;
  uint32_t   export_ipAddr;
  uint16_t   export_port;
} exp_cache_cnf_t;

/*
** Default export identifier is 1
*/
exp_cache_cnf_t exp_cache_cnf = { 
  1,
  0,
  NULL,
  0,
  0,
}; 
 
/*
 **______________________________________________________________________________
 */
/** 
 *  Set the export identifier value


 @param id   : New export identifier
 @param nb   : Number of eid in the export
 @param list : table of eid values;

 @retval none
 */
void set_export_id(uint32_t id, uint32_t ipAddr, uint16_t port, uint32_t nb, uint32_t * list) {
  exp_cache_cnf.export_id     = id; 
  exp_cache_cnf.eid_nb        = nb;
  exp_cache_cnf.eid_list      = list; 
  exp_cache_cnf.export_ipAddr = ipAddr;
  exp_cache_cnf.export_port   = port;
}







	
/*
 **______________________________________________________________________________
 */
/** @ingroup DIRENT_BITMAP
 *  That function go throught the bitmap of free chunk by skipping allocated chunks
 it returns the next chunk to check or -1 if there is no more free chunk



 @param p : pointer to the bitmap of the free chunk (must be aligned on a 8 bytes boundary)
 @param first_chunk : first chunk to test
 @param last_chunk: last allocated chunk
 @param loop_cnt : number of busy chunks that has been skipped (must be cleared by the caller
 @param empty :assert to 1 for searching for free chunk or 0 for allocated chunk

 @retval next_chunk < 0 : out of free chunk (last_chunk has been reached
 @retval next_chunk !=0 next free chunk bit to test
 */
static inline int check_bytes_val(uint8_t *p, int first_chunk, int last_chunk,
        int *loop_cnt, uint8_t empty) {
    int chunk_idx = first_chunk;
    int chunk_u8_idx = 0;
    int align;
    uint64_t val = 0;

    /*
     ** check if the search is for the next free array or the next busy array
     */
    if (empty)
        val = ~val;
//  *loop_cnt = 0;

    while (chunk_idx < last_chunk) {
        chunk_u8_idx = chunk_idx / 8;
        align = chunk_u8_idx & 0x07;
        switch (align) {
        case 0:
            if (((uint64_t*) p)[chunk_idx / 64] == val) {
                chunk_idx += 64;
                break;
            }
            if (((uint32_t*) p)[chunk_idx / 32] == (val & 0xffffffff)) {
                chunk_idx += 32;
                break;
            }
            if (((uint16_t*) p)[chunk_idx / 16] == (val & 0xffff)) {
                chunk_idx += 16;
                break;
            }
            if (((uint8_t*) p)[chunk_idx / 8] == (val & 0xff)) {
                chunk_idx += 8;
                break;
            }
            if ((((uint8_t*) p)[chunk_idx / 8] & 0xf) == (val & 0xf)) {
                chunk_idx += 4;
            }
            return chunk_idx;

        case 2:
        case 6:
            if (((uint16_t*) p)[chunk_idx / 16] == (val & 0xffff)) {
                chunk_idx += 16;
                break;
            }
            if (((uint8_t*) p)[chunk_idx / 8] == (val & 0xff)) {
                chunk_idx += 8;
                break;
            }
            if ((((uint8_t*) p)[chunk_idx / 8] & 0xf) == (val & 0xf)) {
                chunk_idx += 4;
            }
            return chunk_idx;

        case 4:
            if (((uint32_t*) p)[chunk_idx / 32] == (val & 0xffffffff)) {
                chunk_idx += 32;
                break;
            }
            if (((uint16_t*) p)[chunk_idx / 16] == (val & 0xffff)) {
                chunk_idx += 16;
                break;
            }
            if (((uint8_t*) p)[chunk_idx / 8] == (val & 0xff)) {
                chunk_idx += 8;
                break;
            }
            if ((((uint8_t*) p)[chunk_idx / 8] & 0xf) == (val & 0xf)) {
                chunk_idx += 4;
            }
            return chunk_idx;
        case 1:
        case 3:
        case 5:
        case 7:
            if (((uint8_t*) p)[chunk_idx / 8] == (val & 0xff)) {
                chunk_idx += 8;
                break;
            }
            if ((((uint8_t*) p)[chunk_idx / 8] & 0xf) == (val & 0xf)) {
                chunk_idx += 4;
            }
            return chunk_idx;
        }
        *loop_cnt += 1;
    }
    /*
     ** all the bits have been skipped
     */
    return -1;
}

/*
**______________________________________________________________________________
*/
/**
*  release the memory for a front end

  @param front_end_p : data to release
  
  @retval none

*/
void exp_free_srv_front_end(exp_cache_srv_front_end_t *front_end_p)
{

  int i;
  
  if (front_end_p == NULL) return;
  
  for (i = 0; i < 2;i++)
  {
    if (front_end_p->parent[i] != NULL)
    {
      free(front_end_p->parent[i]);
    } 
    if (front_end_p->child[i] != NULL)
    {
      free(front_end_p->child[i]);
    } 
  }
  free(front_end_p);  
}
/*
**______________________________________________________________________________
*/
/**
*  allocate the memory for a front end

  @param level0_sz : size of the remote level 0 cache
  
  @retval <> NULL : the allocated front context
  @retval NULL: out of memory

*/
exp_cache_srv_front_end_t *exp_alloc_srv_front_end(uint32_t level0_sz)
{

 exp_cache_srv_front_end_t *front_end_p;
 uint32_t count = (1 << level0_sz)/EXP_CHILD_BITMAP_BIT_SZ;
 int i;
 
 front_end_p = malloc(sizeof(exp_cache_srv_front_end_t));
 if (front_end_p == NULL)
 {
    severe("out of memory: cannot allocate: %u bytes ",(unsigned  int) sizeof(exp_cache_srv_front_end_t));
    return NULL; 
 }
 
 for (i = 0; i < 2;i++)
 {
   front_end_p->parent[i] = malloc(sizeof(exp_dirty_dirty_parent_t));
   if (front_end_p->parent[i] == NULL)
   {
      severe("out of memory: cannot allocate: %u bytes ",(unsigned  int) sizeof(exp_dirty_dirty_parent_t));
      goto error;
   } 
   memset(front_end_p->parent[i],0,sizeof(exp_dirty_dirty_parent_t)); 
 }


 for (i = 0; i < 2;i++)
 {
   front_end_p->child[i] = malloc(sizeof(exp_dirty_dirty_child_t)*count);
   if (front_end_p->child[i] == NULL)
   {
      severe("out of memory: cannot allocate: %u bytes ",(unsigned  int) sizeof(exp_dirty_dirty_child_t)*count);
      goto error;
   } 
   memset(front_end_p->child[i],0,sizeof(exp_dirty_dirty_child_t)*count); 
 }
 return front_end_p;

error:
 exp_free_srv_front_end(front_end_p);
 
 return NULL; 
}

/**
*  Release a dirty bit management context

 @param exp_cache_dirty_ctx_t: pointer to the context toi release
 
 @retval none
*/
void exp_cache_dirty_release_ctx(exp_cache_dirty_ctx_t *p)
{
  int i;
  
  for (i = 0; i < p->nb_cache_servers; i++)
  {
    if (p->srv_rank[i] == NULL) continue;     
    exp_free_srv_front_end(p->srv_rank[i]);
  }
  free(p);
}

/*
**______________________________________________________________________________
*/
/**
*  Create a context for handling the case of the dirty bits of a remote cache

  @param level0_sz : size of the remote level 0 cache
  @param nb_cache_servers: number of servers participating in the distributed cache
  
  @retval <>NULL: pointer to the dirty management structure 
  @retval  NULL: allocation failure (out of memory)  
*/
exp_cache_dirty_ctx_t *exp_cache_dirty_create_ctx(uint32_t level0_sz,uint32_t nb_cache_servers)
{

   exp_cache_dirty_ctx_t  *ctx_dirty_p = NULL;
   int i;
   
   ctx_dirty_p = malloc(sizeof(exp_cache_dirty_ctx_t));
   if (ctx_dirty_p)
   { 
     severe("out of memory: cannot allocate: %u bytes ",(unsigned  int) sizeof(exp_cache_dirty_ctx_t));
     return NULL;
   }
   memset(ctx_dirty_p,0,sizeof(exp_cache_dirty_ctx_t));
   ctx_dirty_p->level0_sz        = level0_sz;
   ctx_dirty_p->nb_cache_servers = nb_cache_servers;
   
   for (i = 0; i < nb_cache_servers; i++)
   {
     ctx_dirty_p->srv_rank[i]  = exp_alloc_srv_front_end(ctx_dirty_p->level0_sz);
     if (ctx_dirty_p->srv_rank[i] == NULL) goto error;   
   }
   return ctx_dirty_p;
   
error:
   exp_cache_dirty_release_ctx(ctx_dirty_p);
   return NULL;

}

/*
**______________________________________________________________________________
*/
/**
*  set the dirty bit

   @param ctx_p: pointer to the dirty managmenet context
   @param slice : slice associated with the object
   @param hash_value : hash value associated with the object
   
   @retval none
*/
void exp_dirty_set( exp_cache_dirty_ctx_t * ctx_p,uint16_t slice,uint32_t hash_value)
{
  int srv_rank;
  int active_idx;
  int idx_child;
  int relative_idx;
  int absolute_idx;
  uint8_t chunk_u8_idx;
  exp_cache_srv_front_end_t *front_end_p;
  int bit_idx;
  exp_dirty_dirty_parent_t * pParent;
  exp_dirty_dirty_child_t  * pChild;
  
  srv_rank = slice%ctx_p->nb_cache_servers;
  front_end_p = ctx_p->srv_rank[srv_rank];
  if (front_end_p == NULL)
  {
    return;
  }
  
  ctx_p->total_update_count++;
  absolute_idx = hash_value & ((1<<ctx_p->level0_sz)-1);

  active_idx = front_end_p->active_idx;
  pParent = front_end_p->parent[active_idx];
  
  idx_child = absolute_idx / EXP_CHILD_BITMAP_BYTE_SZ;
  chunk_u8_idx = idx_child / 8;
  bit_idx = idx_child % 8;

  if ((pParent->parent_bitmap[chunk_u8_idx] & (1 << bit_idx)) == 0) {
    pParent->parent_update_count++;
    pParent->parent_bitmap[chunk_u8_idx] |= (1 << bit_idx);
  }

  pChild = &(front_end_p->child[active_idx][idx_child]);

  relative_idx = absolute_idx % EXP_CHILD_BITMAP_BYTE_SZ;
  chunk_u8_idx = relative_idx / 8;
  bit_idx = relative_idx % 8;

  if ((pChild->child_bitmap[chunk_u8_idx] & (1 << bit_idx)) == 0) {
  
    pParent->child_update_count++;    
    pChild->child_bitmap[chunk_u8_idx] |= (1 << bit_idx);
    pChild->bitmap                     |= (1 << chunk_u8_idx);
  }  
}


/*
**______________________________________________________________________________
*/
/**
*  Change the active set of the dirty management context for a given fron end 

   @param ctx_p: pointer to the dirty managmenet context
   @param srv_rank : Server to build the message for

   @retval none
*/
void exp_dirty_active_switch(exp_cache_dirty_ctx_t *ctx_p, 
                             int                    srv_rank)
{
  exp_cache_srv_front_end_t * front_end_p;
  int                         inactive_idx;
  uint32_t                    count;  
  
  /*
  ** Retrieve front end context of this server
  */
  if (srv_rank >= EXP_MAX_CACHE_SRV) {
    severe("server rank out of range %d",srv_rank);
    return;
  }
  front_end_p = ctx_p->srv_rank[srv_rank];
  if (front_end_p == NULL) {
    severe("server %d do not exist",srv_rank);
    return ;
  }  
  
  inactive_idx = 1 - front_end_p->active_idx;    
  
  /* 
  ** Clear the data of the inactive set 
  */
  memset(front_end_p->parent[inactive_idx],0,sizeof(exp_dirty_dirty_parent_t)); 
  count = (1 << ctx_p->level0_sz)/EXP_CHILD_BITMAP_BIT_SZ;
  memset(front_end_p->child[inactive_idx],0,sizeof(exp_dirty_dirty_child_t)*count); 

  /*
  ** Switch the active set
  */
  front_end_p->active_idx = inactive_idx;
}   


/*
**______________________________________________________________________________
*/
/**
*   build the dirty message to invalidate some sections for a server

   @param ctx_p     : pointer to the dirty management context
   @param srv_rank  : Server to build the message for
   
   @retval exp_invalidate_nothing   Nothing to invalidate the message is built
   @retval exp_invalidate_ready     The message is ready 
   @retval exp_invalidate_too_big   The message would be too big (should invalidate everything)
   @retval exp_invalidate_error     Some resource is missing !!!

*/
typedef enum _exp_invalidate_type_e {
  exp_invalidate_nothing    = 0,
  exp_invalidate_ready,
  exp_invalidate_too_big,
  exp_invalidate_error
} exp_invalidate_type_e;
  

#define                  EXP_MAX_SECTION_BUFFER_SIZE    2048
char                     gw_dirty_section_buffer[EXP_MAX_SECTION_BUFFER_SIZE];
gw_invalidate_sections_t gw_invalidate_sections_msg;

exp_invalidate_type_e exp_cache_build_invalidate_sections_msg(exp_cache_dirty_ctx_t * ctx_p,
				            int                     srv_rank)
{
  exp_cache_srv_front_end_t * front_end_p;
  exp_dirty_dirty_parent_t  * pParent;
  exp_dirty_dirty_child_t   * pChild;
  int                         inactive_idx;
  int                         idx;
  int                         loop_cnt;
  uint8_t                     chunk_u8_idx;
  int                         bit_idx;
  rozofs_section_header_u   * pSection;
  //int                         section_buffer_size;
  int                         section_size;
  /*
  ** Retrieve front end context of this server
  */
  if (srv_rank >= EXP_MAX_CACHE_SRV) {
    severe("server rank out of range %d",srv_rank);
    return exp_invalidate_error;
  }
  front_end_p = ctx_p->srv_rank[srv_rank];
  if (front_end_p == NULL) {
    severe("server %d do not exist",srv_rank);
    return exp_invalidate_error;
  }
  

  /*
  ** Check whether there has been any modification in the non active bitmaps
  */
  inactive_idx = 1 - front_end_p->active_idx;  
  pParent = front_end_p->parent[inactive_idx]; 
  if (pParent->parent_update_count == 0) {
    return exp_invalidate_nothing;
  }
 
  /*
  ** Initialize the message header
  */
  gw_invalidate_sections_msg.hdr.export_id       = exp_cache_cnf.export_id;
  gw_invalidate_sections_msg.hdr.gateway_rank    = srv_rank;  
  gw_invalidate_sections_msg.hdr.nb_gateways     = ctx_p->nb_cache_servers;   
  
  gw_invalidate_sections_msg.section.section_len = 0;    
  gw_invalidate_sections_msg.section.section_val = gw_dirty_section_buffer;            
  pSection = (rozofs_section_header_u *) gw_dirty_section_buffer;
  
  //section_buffer_size = 0;
  
  /*
  ** Loop on the parent bit map to find out the significant childs
  */
  idx = 0;
  while ((idx < EXP_PARENT_BITMAP_BIT_SZ) && (pParent->parent_update_count != 0)) {
        
    if (idx % 8 == 0) {
      /*
      ** skip the entries that have not been modified
      */
      idx = check_bytes_val(pParent->parent_bitmap, idx, EXP_PARENT_BITMAP_BIT_SZ, &loop_cnt, 0);
      if (idx < 0) break;
    }
    
    chunk_u8_idx = idx / 8;
    bit_idx      = idx % 8;

    /* 
    ** Current child is not dirty
    */
    if ((pParent->parent_bitmap[chunk_u8_idx] & (1 << bit_idx)) == 0) {
      idx++;
      continue;
    }

    /* 
    ** This child is dirty
    */    
    pChild = &(front_end_p->child[inactive_idx][idx]);
             
    
    /* Clear this child in the parent bitmap */	      
    pParent->parent_bitmap[chunk_u8_idx] &= ~(1 << bit_idx);	
    pParent->parent_update_count--;
    
    /*
    ** Let's add this section in the message
    */      
    pSection->u64 = 0;

    pSection->field.absolute_idx  = idx * EXP_CHILD_BITMAP_BYTE_SZ;
    pSection->field.byte_bitmap   = pChild->bitmap;

    /*
    ** Copy the valid bytes 
    */
    int i;
    section_size = 0;
    char * pChar = (char *) (pSection+1);
    for (i=0; i < EXP_CHILD_BITMAP_BYTE_SZ; i++) {
      if (pChild->child_bitmap[i] !=0) {
      
        if ((pChar - gw_dirty_section_buffer) >= EXP_MAX_SECTION_BUFFER_SIZE) {
	  return exp_invalidate_too_big;
	}
        *pChar++ = pChild->child_bitmap[i];
	section_size++;
      }
    }
    pSection->field.section_size = section_size;


    gw_invalidate_sections_msg.section.section_len += (pChar-(char*)pSection);
    pSection = (rozofs_section_header_u *) pChar;
    
    idx++;       
  }
  
  
  /*
  ** Check that the message actualy contains some sections
  */
  if ((char *)pSection == gw_dirty_section_buffer) {
    severe("srv %d parent_update_count %d inconsistent with parent_bitmap",srv_rank,pParent->parent_update_count);
    pParent->parent_update_count = 0;    
    return exp_invalidate_nothing;
  }
  
  return exp_invalidate_ready;
} 
/*
**______________________________________________________________________________
*/
/**
*   build a message to invalidate the whole cache of a server

   @param ctx_p     : pointer to the dirty management context
   @param srv_rank  : Server to build the message for
   
   @retval the message structure ready to encode
*/
gw_header_t gw_invalidate_all_msg;
gw_header_t * exp_cache_build_invalidate_all_msg(exp_cache_dirty_ctx_t * ctx_p,
						 int                     srv_rank)
{
  exp_cache_srv_front_end_t * front_end_p;
    
  /*
  ** Retrieve front end context of this server
  */
  if (srv_rank >= EXP_MAX_CACHE_SRV) {
    severe("server rank out of range %d",srv_rank);
    return NULL;
  }
  front_end_p = ctx_p->srv_rank[srv_rank];
  if (front_end_p == NULL) {
    severe("server %d do not exist",srv_rank);
    return NULL;
  }
  
 
  /*
  ** Initialize the message header
  */
  gw_invalidate_all_msg.export_id       = exp_cache_cnf.export_id;
  gw_invalidate_all_msg.gateway_rank    = srv_rank;  
  gw_invalidate_all_msg.nb_gateways     = ctx_p->nb_cache_servers;   
  
  return &gw_invalidate_all_msg;
}
/*
**______________________________________________________________________________
*/
/**
*   build a configuration message for a server

   @param ctx_p     : pointer to the dirty management context
   @param srv_rank  : Server to build the message for
   
   @retval the message structure ready to encode
*/
gw_configuration_t gw_configuration_msg;
gw_configuration_t * exp_cache_build_configuration_msg(exp_cache_dirty_ctx_t * ctx_p,
						       uint16_t                port,
						       int                     srv_rank)
{
  exp_cache_srv_front_end_t * front_end_p;
    
  /*
  ** Retrieve front end context of this server
  */
  if (srv_rank >= EXP_MAX_CACHE_SRV) {
    severe("server rank out of range %d",srv_rank);
    return NULL;
  }
  front_end_p = ctx_p->srv_rank[srv_rank];
  if (front_end_p == NULL) {
    severe("server %d do not exist",srv_rank);
    return NULL;
  }
  
 
  /*
  ** Initialize the message header
  */
  gw_configuration_msg.hdr.export_id       = exp_cache_cnf.export_id;
  gw_configuration_msg.hdr.gateway_rank    = srv_rank;  
  gw_configuration_msg.hdr.nb_gateways     = ctx_p->nb_cache_servers;   
  
//  gw_configuration_msg.ipAddr              = exp_cache_cnf.export_ipAddr;
//  gw_configuration_msg.port                = exp_cache_cnf.export_port;

//  gw_configuration_msg.eid.eid_len         = exp_cache_cnf.eid_nb;
//  gw_configuration_msg.eid.eid_val         = exp_cache_cnf.eid_list; 
  
  return &gw_configuration_msg;
}
/*
**______________________________________________________________________________
*/
/**
    Encode a cache gateway message in a buffer
    
    @param opcode         code of the message
    @param encode_fct     encoding function
    @param msg   arguments to encode
    @param xmit_buf       buffer where to encode the message in 
 */
int exp_cache_encode_common(int opcode,xdrproc_t encode_fct,void *msg, void * xmit_buf) 			       
{   
    uint8_t           *arg_p;
    uint32_t          *header_size_p;
    int               bufsize;
    int               position;
    XDR               xdrs;    
    struct rpc_msg   call_msg;
    uint32_t         null_val = 0;

    /*
    ** get the pointer to the payload of the buffer
    */
    header_size_p  = (uint32_t*) ruc_buf_getPayload(xmit_buf);
    arg_p = (uint8_t*)(header_size_p+1);  
    /*
    ** create the xdr_mem structure for encoding the message
    */
    bufsize = ruc_buf_getMaxPayloadLen(xmit_buf);
    xdrmem_create(&xdrs,(char*)arg_p,bufsize,XDR_ENCODE);
    /*
    ** fill in the rpc header
    */
    call_msg.rm_direction = CALL;
    /*
    ** allocate a xid for the transaction 
    */
    call_msg.rm_xid             = exp_cache_xid++; 
    call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
    call_msg.rm_call.cb_prog = (uint32_t)GW_PROGRAM;
    call_msg.rm_call.cb_vers = (uint32_t)GW_VERSION;
    if (! xdr_callhdr(&xdrs, &call_msg))
    {
       return -1;	
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
    if ((*encode_fct)(&xdrs,msg) == FALSE)
    {
       return -1;
    }
    /*
    ** Now get the current length and fill the header of the message
    */
    position = XDR_GETPOS(&xdrs);
    /*
    ** update the length of the message : must be in network order
    */
    *header_size_p = htonl(0x80000000 | position);
    /*
    ** set the payload length in the xmit buffer
    */
    int total_len = sizeof(*header_size_p)+ position;
    ruc_buf_setPayloadLen(xmit_buf,total_len);
    return 0;
}
/*
**______________________________________________________________________________
*/
/**
*   Encode the message to invalidate the whole cache of a server

   @param ctx_p     : pointer to the dirty management context
   @param pool_ref  : poll ref to get the buffer from 
   @param srv_rank  : Server to build the message for
   
   @retval the buffer containing the encode message
*/
void * exp_cache_encode_invalidate_all_msg(exp_cache_dirty_ctx_t * ctx_p,
                                           void                  * pool_ref,
				           int                     srv_rank)
{
  gw_header_t              * msg = NULL;
  void                     * xmit_buf = NULL;
  int                        ret;

  /*
  ** allocate an xmit buffer
  */  
  xmit_buf = ruc_buf_getBuffer(pool_ref);
  if (xmit_buf == NULL) {
    severe ("Out of buffer");
    return NULL;
  } 
      
  /*
  ** Build the structure to encode from the cache data
  */    
  msg = exp_cache_build_invalidate_all_msg(ctx_p,srv_rank);
  if (msg == NULL) {
    ruc_buf_freeBuffer(xmit_buf);    
    return NULL;
  }
  
  /*
  ** Encode the message in the buffer
  */
  ret = exp_cache_encode_common(GW_INVALIDATE_ALL,(xdrproc_t) xdr_gw_header_t,msg, xmit_buf);
  if (ret != 0) {
    ruc_buf_freeBuffer(xmit_buf);    
    return NULL;    
  }
  
  /*
  ** Since everything has been invalidated, let's operate a flip flop on the cache
  */
  exp_dirty_active_switch(ctx_p, srv_rank);
  
  return xmit_buf;
}
					   
/*
**______________________________________________________________________________
*/
/**
*   Encode the dirty message to invalidate some sections for a server

   @param ctx_p     : pointer to the dirty management context
   @param pool_ref  : poll ref to get the buffer from 
   @param srv_rank  : Server to build the message for
   
   @retval the buffer containing the encode message
*/
void * exp_cache_encode_invalidate_sections_msg(exp_cache_dirty_ctx_t * ctx_p,
                                                void                  * pool_ref,
						int                     srv_rank)
{
  gw_invalidate_sections_t * msg = NULL;
  void                     * xmit_buf = NULL;
  int                        ret;
  exp_invalidate_type_e      build_msg_ret;

  /*
  ** allocate an xmit buffer
  */  
  xmit_buf = ruc_buf_getBuffer(pool_ref);
  if (xmit_buf == NULL) {
    severe ("Out of buffer");
    return NULL;
  } 
      
  /*
  ** Build the structure to encode from the cache data
  */    
  build_msg_ret = exp_cache_build_invalidate_sections_msg(ctx_p,srv_rank);
  
  /*
  ** Nothing to send 
  */
  if ((build_msg_ret == exp_invalidate_nothing)||(build_msg_ret == exp_invalidate_error)) {
    ruc_buf_freeBuffer(xmit_buf);    
    return NULL;
  }
  
  if (build_msg_ret == exp_invalidate_too_big) {
    /*
    ** Well let's invalidate everything, since we can not encode the invalidate sections
    */
    ruc_buf_freeBuffer(xmit_buf);    
    return exp_cache_encode_invalidate_all_msg(ctx_p,pool_ref,srv_rank);
  }
  
  /*
  ** Encode the message in the buffer
  */
  ret = exp_cache_encode_common(GW_INVALIDATE_SECTIONS,(xdrproc_t) xdr_gw_invalidate_sections_t,msg, xmit_buf);
  if (ret != 0) {
    /*
    ** Well let's invalidate everything, since we can not encode the invalidate sections
    */
    ruc_buf_freeBuffer(xmit_buf);    
    return exp_cache_encode_invalidate_all_msg(ctx_p,pool_ref,srv_rank);
  }
  
  return xmit_buf;
}

