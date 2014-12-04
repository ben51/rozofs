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
#include <rozofs/core/rozofs_ip_utilities.h>
#include <rozofs/common/log.h>


#define MAX_IP_LIST 256
static uint32_t nb_local_ip_addresses = 0;
static uint32_t local_ip_addresses_list[MAX_IP_LIST];

/*__________________________________________________________________________
*/
/**
*  Get the list of local IP addresses
*/
void get_local_ip_addresse_list() {
  int s;
  struct ifconf ifconf;
  struct ifreq ifr[MAX_IP_LIST];
  int i;

  s = socket(PF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    severe("socket %s",strerror(errno));
    return;
  }

  ifconf.ifc_buf = (char *) ifr;
  ifconf.ifc_len = sizeof(ifr);

  if (ioctl(s, SIOCGIFCONF, &ifconf) == -1) {
    severe("ioctl(SIOCGIFCONF) %s",strerror(errno));
    close(s);
    return;
  }

  nb_local_ip_addresses = ifconf.ifc_len / sizeof(ifr[0]);
  if (nb_local_ip_addresses > MAX_IP_LIST) {
    severe("get_local_ip_addresse_list %d addresses while table is %d size",
            nb_local_ip_addresses, MAX_IP_LIST);
    nb_local_ip_addresses = MAX_IP_LIST;
  } 
  
  for (i = 0; i < nb_local_ip_addresses; i++) {
    struct sockaddr_in *s_in = (struct sockaddr_in *) &ifr[i].ifr_addr;
    local_ip_addresses_list[i] = ntohl(s_in->sin_addr.s_addr);
  }
  close(s);
}
/*__________________________________________________________________________
*/
/**
* Check whether an IP address is local
*
* @param ipV4 IP address to test
*
* @retval 1 when local 0 else
*/
int is_this_ipV4_local(uint32_t ipv4) {
  int i;

  if (nb_local_ip_addresses == 0) {
    get_local_ip_addresse_list();
  }
  
  for (i=0; i < nb_local_ip_addresses; i++) {
    if (ipv4 == local_ip_addresses_list[i]) return 1;
  }
  return 0;
}
/*__________________________________________________________________________
*/
/**
* Check whether an IP address is configured localy
*
* @param ipV4 IP address to test
*
* @retval 1 when local 0 else
*/
int is_this_ipV4_configured(uint32_t ipv4) {
  int i;

  /*
  ** Read list of local IP addresses
  */
  get_local_ip_addresse_list();
  
  for (i=0; i < nb_local_ip_addresses; i++) {
    if (ipv4 == local_ip_addresses_list[i]) return 1;
  }
  return 0;
}

