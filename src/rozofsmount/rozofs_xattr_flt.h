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
#ifndef ROZOFS_XATTR_FLT_H
#define ROZOFS_XATTR_FLT_H
#include <inttypes.h>
#include "rozofs_fuse.h"
#include "rozofs_fuse_api.h"

/**
*  xattribute filter entry
*/
typedef struct _rozofs_xattr_flt_t
{
  uint64_t  status;
  char      *buffer;
} rozofs_xattr_flt_t;

#define ROZOFS_XATTR_FILTR_MAX 64
#ifndef ENODATA
#define ENODATA 61
#endif

extern int rozofs_xattr_flt_count; /**< current number of filters */
extern int rozofs_xattr_flt_enable; /**< assert to 1 to enbale the filter */
extern rozofs_xattr_flt_t rozofs_xattr_flt_filter[];


/*
**______________________________________________________________________________
*/
/**
*   search an extended attribute id in the filter;
*
    @param name : extended attribute id
    
    @retval 0 on success
    @retval -1 no found
*/
int rozofs_xattr_flt_search(char *name);

/*
**______________________________________________________________________________
*/
/**
*   insert an extended attribute id in the filter;
*
    @param name : extended attribute id
    
    @retval 0 on success
    @retval -1 on error
*/
int rozofs_xattr_flt_insert(char *name);

/*
**______________________________________________________________________________
*/
/**
*   remove an extended attribute id in the filter;
*
    @param name : extended attribute id
    
    @retval 0 on success
    @retval -1 on error
*/
int rozofs_xattr_flt_remove(char *name);

/*
**______________________________________________________________________________
*/
void show_xattr_flt(char * argv[], uint32_t tcpRef, void *bufRef);

/*
**______________________________________________________________________________
*/
void rozofs_xattr_flt_filter_init();

#endif

