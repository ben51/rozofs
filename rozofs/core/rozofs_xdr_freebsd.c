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


#include <rozofs/rozofs.h>

#ifdef __FreeBSD__

/* XDR 8bit integers */
bool_t
xdr_int8_t (XDR *xdrs, int8_t *ip)
{
  int32_t t;

  switch (xdrs->x_op)
    {
    case XDR_ENCODE:
      t = (int32_t) *ip;
      return XDR_PUTINT32 (xdrs, &t);
    case XDR_DECODE:
      if (!XDR_GETINT32 (xdrs, &t))
  return FALSE;
      *ip = (int8_t) t;
      return TRUE;
    case XDR_FREE:
      return TRUE;
    default:
      return FALSE;
    }
}

/* XDR 8bit unsigned integers */
bool_t
xdr_uint8_t (XDR *xdrs, uint8_t *uip)
{
  uint32_t ut;

  switch (xdrs->x_op)
    {
    case XDR_ENCODE:
      ut = (uint32_t) *uip;
      return XDR_PUTINT32 (xdrs, (int32_t *) &ut);
    case XDR_DECODE:
      if (!XDR_GETINT32 (xdrs, (int32_t *) &ut))
  return FALSE;
      *uip = (uint8_t) ut;
      return TRUE;
    case XDR_FREE:
      return TRUE;
    default:
      return FALSE;
    }
}

#endif
