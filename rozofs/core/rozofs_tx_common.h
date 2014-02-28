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

#ifndef ROZOFS_TX_COMMON_H
#define ROZOFS_TX_COMMON_H

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include "ruc_list.h"
#include "uma_fsm_framework.h"
#include "com_tx_timer_api.h"
#include <errno.h>
#include <rozofs/rozofs.h>

#ifndef ETIME
#define ETIME 62
#endif

/**
 *  Common packet header for the exchange between the blocks
 */
typedef struct _rozofs_com_hdr_t
{
   uint32_t msg_len; /**< message length including the common header  */
   uint16_t family;  /**< nothbound family                            */
   uint16_t opcode;  /**< operating opcode                            */
   uint64_t transaction_id; /**< transaction reference                */
   uint64_t sender_reference;  /**< opaque valeur, owner is sender    */
   uint64_t receiver_reference;

} rozofs_com_hdr_t;

typedef struct _rozofs_tx_rw_lbg_t
{
   ruc_obj_desc_t    link;          /* link to the r/w load balancing hash table */
   fid_t             fid;                  /**< key for load balancing                 */
   int               storcli_idx;          /**< index of the storcli associated with the entry */   
} rozofs_tx_rw_lbg_t;

 
 #define ROZOFS_TX_OPAQUE_MAX 4 /**< size of the user opaque array   */
 
 typedef void (*sys_recv_pf_t)(void *tx_ctx,void *usr_param);
 /**
 * Context associated with procedure with a single transaction
 */
typedef struct _rozofs_tx_ctx_p_t
{
  ruc_obj_desc_t    link;          /* To be able to chain the MS context on any list */
  uint32_t            index;         /* Index of the MS */
  uint32_t            free;          /* Is the context free or allocated TRUE/FALSE */
  uint32_t            integrity;     /* the value of this field is incremented at  each MS ctx allocation */
/*
**   DO NOT MOVE THE EVENT/FLAG ARRAY: integrity field is used for giving
**   the address of the beginning of the bitfields
*/
  /*
    _______Event flags
  */
  uint32_t       rpc_guard_timer_flg:1;  /**< assert to 1 when there is a time-out */
  uint32_t       rpc_recv_flg:1;         /**< assert to 1 when a message has been received for the context */

  uint32_t         flag0_02:1;
  uint32_t         flag0_03:1;
  uint32_t         flag0_04:1;
  uint32_t         flag0_05:1;
  uint32_t         flag0_06:1;
  uint32_t         flag0_07:1;
  uint32_t         flag0_08:1;		      
  uint32_t         flag0_09:1;
  uint32_t         flag0_10:1;
  uint32_t         flag0_11:1;
  uint32_t         flag0_12:1;
  uint32_t         flag0_13:1;
  uint32_t         flag0_14:1;
  uint32_t         flag0_15:1;
  uint32_t         flag0_16:1;
  uint32_t         flag0_17:1;
  uint32_t         flag0_18:1;
  uint32_t         flag0_19:1;
  uint32_t         flag0_20:1;
  uint32_t         flag0_21:1;
  uint32_t         flag0_22:1;
  uint32_t         flag0_23:1;
  uint32_t         flag0_24:1;
  uint32_t         flag0_25:1;
  uint32_t         flag0_26:1;
  uint32_t         flag0_27:1;
  uint32_t         flag0_28:1;
  uint32_t         flag0_29:1;
  uint32_t         flag0_30:1;
  uint32_t         flag0_31:1;
  uint32_t         flag0_32:1;

  uint32_t         flag1_01:1;/* FLAG 1 01-08 ARE RESERVED FOR FSM STATE */
  uint32_t         flag1_02:1;/* FLAG 1 01-08 ARE RESERVED FOR FSM STATE */
  uint32_t         flag1_03:1;/* FLAG 1 01-08 ARE RESERVED FOR FSM STATE */
  uint32_t         flag1_04:1;/* FLAG 1 01-08 ARE RESERVED FOR FSM STATE */
  uint32_t         flag1_05:1;/* FLAG 1 01-08 ARE RESERVED FOR FSM STATE */
  uint32_t         flag1_06:1;/* FLAG 1 01-08 ARE RESERVED FOR FSM STATE */
  uint32_t         flag1_07:1;/* FLAG 1 01-08 ARE RESERVED FOR FSM STATE */
  uint32_t         flag1_08:1;/* FLAG 1 01-08 ARE RESERVED FOR FSM STATE */				      
  uint32_t         flag1_09:1;
  uint32_t         flag1_10:1;
  uint32_t         flag1_11:1;
  uint32_t         flag1_12:1;
  uint32_t         flag1_13:1;
  uint32_t         flag1_14:1;
  uint32_t         flag1_15:1;
  uint32_t         flag1_16:1;
  uint32_t         flag1_17:1;
  uint32_t         flag1_18:1;
  uint32_t         flag1_19:1;
  uint32_t         flag1_20:1;
  uint32_t         flag1_21:1;
  uint32_t         flag1_22:1;
  uint32_t         flag1_23:1;
  uint32_t         flag1_24:1;
  uint32_t         flag1_25:1;
  uint32_t         flag1_26:1;
  uint32_t         flag1_27:1;
  uint32_t         flag1_28:1; 				      
  uint32_t         flag1_29:1;
  uint32_t         flag1_30:1;
  uint32_t         flag1_31:1;
  uint32_t         flag1_32:1;
    /**
    * opcode and transaction_id of the current on-going request
    */
//    uint32_t     opcode;        /**< current opcode associated with the context */ 
//    uint64_t     transaction_id; /**< current transaction Id */
    uint32_t       xid;           /**< current transaction ID, 0 when the context is free  */
    uint32_t       xid_low;       /**< lower 16 bits part of the XID, the 16 MSB context the reference of the transaction context */ 
    /*
    ** xmit buffer: release by the transmitter or upon a TMO
    */
    void   *xmit_buf;      
    /*
    ** response part
    */
    void *recv_buf;    /**< allocated by the receiver -> no to be release by the application */
    int status;        /**< status of the operation */
    int tx_errno;        /**< status of the operation */
//    int timeout;       /**< assert to 1 if there was a timeout on the transaction */
    sys_recv_pf_t  recv_cbk;   /**< receive callback */
    void *user_param;         /**< user param to provide upon reception */
    com_tx_tmr_cell_t  rpc_guard_timer;   /**< guard timer associated with the transaction */
      /* FSM */
//    uma_fsm_t    sys_tx_fsm;         /**< active fsm for the current transaction  */
    
//    int moduleId; 
    uint64_t timeStamp;
    uint32_t opaque_usr[ROZOFS_TX_OPAQUE_MAX];   /**< opaque usr data array   */
    
   rozofs_tx_rw_lbg_t  rw_lbg;      /**< context used for r/w load balancing   */
} rozofs_tx_ctx_t;


#endif

