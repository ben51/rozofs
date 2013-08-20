 General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see
 <http://www.gnu.org/licenses/>.
 */
 
 
#ifndef SPROTOSVC_NB_H
#define SPROTOSVC_NB_H

#include <stdint.h>

#include <config.h>
#include <rozofs/rozofs.h>
#include <rozofs/core/rozofs_rpc_non_blocking_generic_srv.h>
#include <rozofs/core/ruc_buffer_api.h>
#include <rozofs/rpc/rozofs_rpc_util.h>
#include <rozofs/common/profile.h>
#include <rozofs/rpc/sproto.h>
#include "sproto_nb.h"
/*
**__________________________________________________________________________
*/
/**
  Server callback  for GW_PROGRAM protocol:
    
     GW_INVALIDATE_SECTIONS
     GW_INVALIDATE_ALL
     GW_CONFIGURATION
     GW_POLL
     
  That callback is called upon receiving a GW_PROGRAM message
  from the master exportd

    
  @param socket_ctx_p: pointer to the af unix socket
  @param socketId: reference of the socket (not used)
 
   @retval : TRUE-> xmit ready event expected
  @retval : FALSE-> xmit  ready event not expected
*/
void storage_req_rcv_cbk(void *userRef,uint32_t  socket_ctx_idx, void *recv_buf);

#endif