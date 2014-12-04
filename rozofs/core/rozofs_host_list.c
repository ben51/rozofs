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
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/if.h>
#else
#include <net/if.h>
#endif
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <rozofs/common/log.h>
#include "rozofs_host_list.h"

char   ROZOFS_HOST_LIST_STRING[ROZOFS_HOSTNAME_MAX+1];
char * rozofs_host_pointer[ROZOFS_HOST_LIST_MAX_HOST];
int    rozofs_host_nb = 0;


/*__________________________________________________________________________
*/
/**
*  Builld an array of pointers to hosts from a string containing
*  hosts separated by a specific character.
*
* @param host_list  string containing a list of hosts
* @param separator  the character that separates a host from an other in
*                   the list
*
* @retval the number of host in the host list
*/
int rozofs_host_list_parse(const char * host_list, char separator) {
  char * pChar = ROZOFS_HOST_LIST_STRING;

  rozofs_host_nb = 0;
  
  if (host_list == NULL) return rozofs_host_nb;
  if (host_list[0] == 0) return rozofs_host_nb;

  /*
  ** Recopy the list
  */
  strcpy(pChar, host_list);
  
  while (*pChar != 0) {
  
    /* Skip spaces */
    while (' '==*pChar) pChar++;
    
    if (*pChar == 0) break;
    
    /* Extra separators ?! */
    if (*pChar == separator) {
      pChar++;
      continue;
    }
    
    /* Save pointer on the beginning of the host */
    rozofs_host_pointer[rozofs_host_nb] = pChar;
  
    /* Step until end of string or separator */
    while ((*pChar!= 0)&&(*pChar!=separator)) pChar++;
    
    rozofs_host_nb++;
    if (rozofs_host_nb == ROZOFS_HOST_LIST_MAX_HOST) break;
    
    /* Set end of string where the separator was */
    if (*pChar == 0) break;

    *pChar = 0;
    pChar++;
  }
  return rozofs_host_nb;   
}
