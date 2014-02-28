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


#ifndef DIRENT_JOURNAL_H
#define DIRENT_JOURNAL_H

#include <string.h>
#include <fcntl.h>

#include <rozofs/rozofs.h>
#include <rozofs/common/log.h>
#include <rozofs/common/list.h>
#include <rozofs/common/htable.h>

#include "mdir.h"
#include "mdirent.h"

typedef enum _dirent_journal_type_e
{
  DIRENT_E = 0,
  SECT0_E,
  NAME_BITMAP_E,
  HASH_TBL_E,
  HASH_ENTRY_E,
  NAME_ENTRY_E,
  JOURNAL_MAX_TYPE_E
} dirent_journal_type_e;

#define DIRENT_JOURNAL_USR_DESC_SZ 10
typedef struct _dirent_journal_tlv_t
{
  uint8_t  type;    /**< type of the entry (see above) */
  uint8_t  filler;  /**< for future use */
  uint16_t length;  /**< length of the data part  */
  uint8_t  usr_descriptor[DIRENT_JOURNAL_USR_DESC_SZ];

} dirent_journal_tlv_t;

/**
*  The following structure applied to :
*  DIRENT_E: file information
   NAME_BITMAP_E
   HASH_TBL_E
   NAME_ENTRY_E
   HASH_ENTRY_E
*/
#define DIRENT_JOURNAL_ACTION_DELETE 0
#define DIRENT_JOURNAL_ACTION_UPDATE 1
typedef struct _dirent_journal_desc_type_0
{
   uint16_t  index;  /**< dirent file index : 0 -> root other collision */
   uint16_t  action; /**< 0: delete, 1 : update */
} dirent_journal_desc_type_0;


/**
* dirent journal header
*/
#define DIRENT_JOUNAL_INVALID 0xdeadbeef
#define DIRENT_JOUNAL_VALID   0x12345678

typedef struct _dirent_journal_header_t
{
  uint32_t  validity;  /**< see above */
  uint32_t  length;    /**< length of the payload excluding that header */
} dirent_journal_header_t;



#define DIRENT_JOURNAL_MAXSIZE (1024*256)
/**
* structure used to handle the dirent journal
*/
typedef struct _dirent_journal_buffer_t
{
   void *buf_start;        /**< pointer to the beginning of the buffer */
   uint32_t  file_offset;  /**< offset in file of the first byte of buf_start */
   uint32_t  lenmax;       /**< max size                              */
   uint32_t  lencur;       /**< current length of the buffer          */
} dirent_journal_buffer_t;

/**
*  public API:
   Build a dirent file name.
   the filename has the following structure
   level0 (root): dirent_"dirent_idx[0]"
   level1 (coll1): dirent_"dirent_idx[0]"_"dirent_idx[1]"
   level2 (coll2): dirent_"dirent_idx[0]"_"dirent_idx[1]"_"dirent_idx[2]"

  @param root_idx: index of the root file
  @param *buf: pointer to the output buffer

  @retval NULL if header has wrong information
  @retval <>NULL pointer to the beginning of the dirent filename
*/
static inline char *dirent_journal_build_filename(int root_idx ,char *buf)
{
   char *pdata;
   pdata = buf;
   pdata+=sprintf(pdata,"j");
   pdata+=sprintf(pdata,"_%d",root_idx);
   return buf;
}
/*
**______________________________________________________________________________
*/
/**
*  API to initialize the journal buffer
  @param journal_p : pointer to the buffer journal structure

  @retval 0 on success
  @retval -1 on error (out of memory)
*/
static inline int dirent_journal_buf_init(dirent_journal_buffer_t *buf_p)
{
  buf_p->buf_start = malloc(DIRENT_JOURNAL_MAXSIZE);
  if (buf_p->buf_start == NULL) return -1;
  buf_p->lencur = 0;
  buf_p->file_offset = 0;
  buf_p->lenmax = DIRENT_JOURNAL_MAXSIZE;
  return 0;
}

/*
**______________________________________________________________________________
*/
/**
*  API to initialize the journal buffer
  @param journal_p : pointer to the buffer journal structure

  @retval none
*/
static inline void dirent_journal_buf_release(dirent_journal_buffer_t *buf_p)
{
  if (buf_p->buf_start != NULL) return free(buf_p->buf_start);
  buf_p->buf_start = NULL;
  buf_p->file_offset = 0;
  buf_p->lenmax = 0;
}

/*
**______________________________________________________________________________
*/
/**
*  API to create the journal file
*
*  The journal is created in the invalid state

  @param root_idx : index of the root file
  @param dirfd : file descriptor of the parent directory
  @param buf_p: pointer to the journal current buffer

  @retval 0 on success
  @retval -1 on error
*/
static inline int dirent_journal_create(int root_idx, int dirfd,dirent_journal_buffer_t *buf_p)
{
    int fd = -1;
    int flag = O_WRONLY | O_CREAT;
    char      pathname[64];
    char     *path_p;
    dirent_journal_header_t  *header_p;

    /*
    ** build the filename of the dirent file to read
    */
    path_p = dirent_journal_build_filename(root_idx,pathname);
    if (path_p == NULL)
    {
      /*
      ** something wrong that must not happen
      */
      DIRENT_SEVERE("Cannot build filename( line %d\n)",__LINE__);
      goto error;
    }

    if ((fd = openat(dirfd, path_p, flag, S_IRWXU)) == -1)
    {
        DIRENT_SEVERE("Cannot open file( line %d\n)",__LINE__);
        goto error;
    }
    /*
    ** create the header
    */
    buf_p->lencur = 0;
    header_p = (dirent_journal_header_t*)(&((uint8_t*)buf_p->buf_start)[buf_p->lencur]);
    header_p->validity = DIRENT_JOUNAL_INVALID;
    header_p->length   = 0;
    if (DIRENT_PWRITE(fd, buf_p->buf_start, sizeof (dirent_journal_header_t), 0) != (sizeof (dirent_journal_header_t))) {
        DIRENT_SEVERE("pwrite failed for file %s: %s at line %d", pathname, strerror(errno),__LINE__);
        goto error;
    }
    /*
    ** release the resources
    */
    buf_p->file_offset += sizeof (dirent_journal_header_t);
    close(fd);
    return 0;

error:
    if (fd != -1)
        close(fd);
    return -1;
}

/*
**______________________________________________________________________________
*/

/**
*  API to append an item to the journal
*
*  It is assumed that the journal buffer has already been allocated

  @param root_idx : index of the root file
  @param dirfd : file descriptor of the parent directory
  @param buf_p: pointer to the journal current buffer
  @param item_type : see dirent_journal_type_e
  @param descriptor_p : pointer to a descriptor associated with the item type or NULL
  @param payload_p : pointer to the payload or NULL if no payload
  @param payload_len :length of the payload of payload_p is not NULL

  @retval 0 on success
  @retval -1 on error
*/

static inline int dirent_journal_append_item(int root_idx, int dirfd,dirent_journal_buffer_t *buf_p,
                                             dirent_journal_type_e item_type,
                                             void *descriptor_p,
                                             void *payload_p,int payload_len)

{

    int fd = -1;
    int flag = O_WRONLY;
#ifdef __linux__
		flag |= O_NOATIME;
#endif
    char      pathname[64];
    char     *path_p;
    uint32_t appended_size;
    dirent_journal_tlv_t tlv;
    dirent_journal_tlv_t *tlv_p = &tlv;
   /*
   ** First of all check if the current buffer need to be flushed on disk
   */
   appended_size = sizeof(dirent_journal_tlv_t);
   if (payload_p != NULL) appended_size+= payload_len;
   if ((buf_p->lencur+appended_size) >= buf_p->lenmax)
   {
     /*
     ** need to write on disk
     */
     path_p = dirent_journal_build_filename(root_idx,pathname);
     if (path_p == NULL)
     {
       /*
       ** something wrong that must not happen
       */
       DIRENT_SEVERE("Cannot build filename( line %d\n)",__LINE__);
       goto error;
     }

     if ((fd = openat(dirfd, path_p, flag, S_IRWXU)) == -1)
     {
         DIRENT_SEVERE("Cannot open file( line %d\n)",__LINE__);
         goto error;
     }
     /*
     ** flush current buffer on disk
     */
     if (DIRENT_PWRITE(fd, buf_p->buf_start, buf_p->lencur,(off_t) buf_p->file_offset) != buf_p->lencur) {
         DIRENT_SEVERE("pwrite failed for file %s: %s at line %d", pathname, strerror(errno),__LINE__);
         goto error;
     }
     /*
     ** release the resources
     */
     buf_p->file_offset += buf_p->lencur;
     buf_p->lencur = 0;
     close(fd);
   }

   switch (item_type)
   {

     case DIRENT_E:
        tlv_p->type   = item_type;
        tlv_p->length = 0;
        memcpy(tlv_p->usr_descriptor,descriptor_p,DIRENT_JOURNAL_USR_DESC_SZ);
        memcpy(&((uint8_t*)buf_p->buf_start)[buf_p->lencur],tlv_p,sizeof(dirent_journal_tlv_t));
        buf_p->lencur += sizeof(dirent_journal_tlv_t);
        break;

     case SECT0_E:
        tlv_p->type   = item_type;
        tlv_p->length = 0;
        memcpy(tlv_p->usr_descriptor,descriptor_p,DIRENT_JOURNAL_USR_DESC_SZ);
        memcpy(&((uint8_t*)buf_p->buf_start)[buf_p->lencur],tlv_p,sizeof(dirent_journal_tlv_t));
        buf_p->lencur += sizeof(dirent_journal_tlv_t);
        break;

     case NAME_BITMAP_E:
     case HASH_TBL_E:
     case HASH_ENTRY_E:
     case NAME_ENTRY_E:
        tlv_p->type   = item_type;
        tlv_p->length = 0;
        memcpy(tlv_p->usr_descriptor,descriptor_p,DIRENT_JOURNAL_USR_DESC_SZ);
        memcpy(&((uint8_t*)buf_p->buf_start)[buf_p->lencur],tlv_p,sizeof(dirent_journal_tlv_t));
        buf_p->lencur += sizeof(dirent_journal_tlv_t);
        if (payload_p != NULL)
        {
           memcpy(&((uint8_t*)buf_p->buf_start)[buf_p->lencur],payload_p,payload_len);
        }
        buf_p->lencur += payload_len;
        break;

    default :
      DIRENT_SEVERE(" Unknown item type %d at lien %d\n", item_type,__LINE__);
      goto error;

   }
   /*
   ** OK, wait for the next item
   */
   return 0;

 error:
     if (fd != -1)
         close(fd);
     return -1;
}




/**
*  API for flushing the journal stored in memory to disk
*
*  It is assumed that the journal buffer has already been allocated

  @param root_idx : index of the root file
  @param dirfd : file descriptor of the parent directory
  @param buf_p: pointer to the journal current buffer

  @retval 0 on success
  @retval -1 on error
*/

static inline int dirent_journal_flush_disk(int root_idx, int dirfd,dirent_journal_buffer_t *buf_p)

{

    int fd = -1;
    int flag = O_WRONLY;
#ifdef __linux__
		flag |= O_NOATIME;
#endif
    char      pathname[64];
    char     *path_p;


   /*
   ** need to write on disk
   */
   path_p = dirent_journal_build_filename(root_idx,pathname);
   if (path_p == NULL)
   {
     /*
     ** something wrong that must not happen
     */
     DIRENT_SEVERE("Cannot build filename( line %d\n)",__LINE__);
     goto error;
   }

   if ((fd = openat(dirfd, path_p, flag, S_IRWXU)) == -1)
   {
       DIRENT_SEVERE("Cannot open file( line %d\n)",__LINE__);
       goto error;
   }
   /*
   ** flush current buffer on disk
   */
   if (DIRENT_PWRITE(fd, buf_p->buf_start, buf_p->lencur,(off_t) buf_p->file_offset) != buf_p->lencur) {
       DIRENT_SEVERE("pwrite failed for file %s: %s at line %d", pathname, strerror(errno),__LINE__);
       goto error;
   }
   /*
   ** release the resources
   */
   buf_p->file_offset += buf_p->lencur;
   buf_p->lencur = 0;
   close(fd);

   return 0;

 error:
     if (fd != -1)
         close(fd);
     return -1;
}


/*
**______________________________________________________________________________
*/
/**
*  API to validate the journal file
*
*  The journal is created in the invalid state

  @param root_idx : index of the root file
  @param dirfd : file descriptor of the parent directory
  @param buf_p: pointer to the journal current buffer

  @retval 0 on success
  @retval -1 on error
*/
static inline int dirent_journal_validate(int root_idx, int dirfd,dirent_journal_buffer_t *buf_p)
{
    int fd = -1;
    int flag = O_WRONLY | O_CREAT;
    char      pathname[64];
    char     *path_p;
    dirent_journal_header_t  *header_p;

    /*
    ** build the filename of the dirent file to read
    */
    path_p = dirent_journal_build_filename(root_idx,pathname);
    if (path_p == NULL)
    {
      /*
      ** something wrong that must not happen
      */
      DIRENT_SEVERE("Cannot build filename( line %d\n)",__LINE__);
      goto error;
    }

    if ((fd = openat(dirfd, path_p, flag, S_IRWXU)) == -1)
    {
        DIRENT_SEVERE("Cannot open file( line %d\n)",__LINE__);
        goto error;
    }
    /*
    ** create the header
    */
    buf_p->lencur = 0;
    header_p = (dirent_journal_header_t*)(&((uint8_t*)buf_p->buf_start)[buf_p->lencur]);
    header_p->validity = DIRENT_JOUNAL_VALID;
    header_p->length   = buf_p->file_offset;
    if (DIRENT_PWRITE(fd, buf_p->buf_start, sizeof (dirent_journal_header_t), 0) != (sizeof (dirent_journal_header_t))) {
        DIRENT_SEVERE("pwrite failed for file %s: %s at line %d", pathname, strerror(errno),__LINE__);
        goto error;
    }
    /*
    ** release the resources
    */
    close(fd);
    return 0;

error:
    if (fd != -1)
        close(fd);
    return -1;
}



#endif
