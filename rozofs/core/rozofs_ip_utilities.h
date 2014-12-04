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
 
#ifndef ROZOFS_IP_UTILITIES_H
#define ROZOFS_IP_UTILITIES_H

#include <string.h>
#include <stdio.h>
#include <netdb.h>
#include <rozofs/common/log.h>
#include <netinet/in.h>


#ifdef __cplusplus
extern "C" {
#endif /*__cplusplus*/

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
static inline int rozofs_host2ip(char *host, uint32_t *ipaddr_p) {
  struct hostent *hp;    
  /*
  ** get the IP address of the storage node
  */
  if ((hp = gethostbyname(host)) == 0) {
    severe("gethostbyname failed for host : %s, %s", host, strerror(errno));
    return -1;
  }
  if (hp->h_length != 4) {
    severe("rozofs_host2ip address length is %d", hp->h_length);
    return -1;
  }  
  bcopy((char *) hp->h_addr, (char *) ipaddr_p, hp->h_length);
  *ipaddr_p = ntohl(*ipaddr_p);
  return 0;
}
/*
**______________________________________________________________________________
*/
/**
*  Convert an IP v4 address in host format to a string in dot notation

@param ipaddr_p : IP V4 address

@retval 0 on success
@retval -1 on error (see errno faor details
*/
static inline char * rozofs_ip2string(char *p, uint32_t ipAddr) {
  p += sprintf(p,"%u.%u.%u.%u",
      (ipAddr>>24)&0xFF,  (ipAddr>>16)&0xFF,(ipAddr>>8)&0xFF ,ipAddr&0xFF);
  return p;    
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
int is_this_ipV4_local(uint32_t ipv4);


/*__________________________________________________________________________
*/
/**
* Check whether an IP address is configured localy
*
* @param ipV4 IP address to test
*
* @retval 1 when local 0 else
*/
int is_this_ipV4_configured(uint32_t ipv4) ;

/*
**__________________________________________________________________________
*/
/**
   Get the port number reserved for a given service name 
   (having a look at /etc/services)
        
   @param serviceName The name of the service
   @param proto       The protocol of the service ("tcp","udp", NULL)
   @param defaultPort The default port value to return when this service
                      has not been found
		      
   retval: the service port

*/
static inline uint16_t get_service_port(char *serviceName, char * proto, uint16_t defaultPort){
  struct servent *servinfo;
  uint16_t        port;
  
  servinfo = getservbyname(serviceName, proto);
  if(!servinfo) {
     DEBUG("Default port %u used for %s:%s",defaultPort,serviceName,proto?proto:"any");
     return defaultPort;
  }
  
  port = servinfo->s_port;// s_port is an int
  port = ntohs(port); 
  DEBUG("Port %u defined in %s for %s:%s",port, _PATH_SERVICES,serviceName,proto?proto:"any");
  return port;
}  
#ifdef __cplusplus
}
#endif /*__cplusplus */

#endif

