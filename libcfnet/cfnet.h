/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/


#ifndef CFENGINE_CFNET_H
#define CFENGINE_CFNET_H


#include <platform.h>
#include <openssl/ssl.h>


/* ************************************************ */
/* The following were copied from cf3.defs.h and still exist there, TODO */

/* max size of plaintext in one transaction, see
   net.c:SendTransaction(), leave space for encryption padding
   (assuming max 64*8 = 512-bit cipher block size)*/
#define CF_BUFSIZE 4096
#define CF_SMALLBUF 128
#define CF_MAX_IP_LEN 64        /* numerical ip length */
#define CF_DONE 't'
#define CF_MORE 'm'
/* ************************************************ */


#define SOCKET_INVALID -1
#define MAXIP4CHARLEN 16
#define CF_RSA_PROTO_OFFSET 24
#define CF_PROTO_OFFSET 16
#define CF_INBAND_OFFSET 8


/* The only protocol we support inside TLS, for now... */
#define CFNET_PROTOCOL_VERSION 1


/* TODO Shouldn't this be in libutils? */
typedef enum
{
    FILE_TYPE_REGULAR,
    FILE_TYPE_LINK,
    FILE_TYPE_DIR,
    FILE_TYPE_FIFO,
    FILE_TYPE_BLOCK,
    FILE_TYPE_CHAR_, /* Conflict with winbase.h */
    FILE_TYPE_SOCK
} FileType;
typedef struct Stat_ Stat;
struct Stat_
{
    char *cf_filename;          /* What file are we statting? */
    char *cf_server;            /* Which server did this come from? */
    FileType cf_type;           /* enum filetype */
    mode_t cf_lmode;            /* Mode of link, if link */
    mode_t cf_mode;             /* Mode of remote file, not link */
    uid_t cf_uid;               /* User ID of the file's owner */
    gid_t cf_gid;               /* Group ID of the file's group */
    off_t cf_size;              /* File size in bytes */
    time_t cf_atime;            /* Time of last access */
    time_t cf_mtime;            /* Time of last data modification */
    time_t cf_ctime;            /* Time of last file status change */
    char cf_makeholes;          /* what we need to know from blksize and blks */
    char *cf_readlink;          /* link value or NULL */
    int cf_failed;              /* stat returned -1 */
    int cf_nlink;               /* Number of hard links */
    int cf_ino;                 /* inode number on server */
    dev_t cf_dev;               /* device number */
    Stat *next;
};


/*
 * TLS support
 */
#define DEFAULT_TLS_TIMEOUT_SECONDS     5
#define DEFAULT_TLS_TIMEOUT_USECONDS    0
#define SET_DEFAULT_TLS_TIMEOUT(x) \
    x.tv_sec = DEFAULT_TLS_TIMEOUT_SECONDS; \
    x.tv_usec = DEFAULT_TLS_TIMEOUT_USECONDS
#define DEFAULT_TLS_TRIES 5


typedef enum
{
    /* When connection is initialised ProtocolVersion is 0, i.e. undefined. */
    CF_PROTOCOL_UNDEFINED = 0,
    CF_PROTOCOL_CLASSIC,
    CF_PROTOCOL_TLS
} ProtocolVersion;

typedef struct
{
    ProtocolVersion type;
    int sd;                           /* Socket descriptor */
    SSL *ssl;                         /* OpenSSL struct for TLS connections */
    RSA *remote_key;
    char remote_keyhash[EVP_MAX_MD_SIZE];       /* key hash */
    char remote_keyhash_str[EVP_MAX_MD_SIZE*4]; /* key hash as a SHA=... string */
} ConnectionInfo;

typedef struct
{
    int family;                 /* AF_INET or AF_INET6 */
    ConnectionInfo conn_info;
    int trust;                  /* true if key being accepted on trust */
    int authenticated;
    char username[CF_SMALLBUF];
    /* Unused for now... */
    /* char localip[CF_MAX_IP_LEN]; */
    char remoteip[CF_MAX_IP_LEN];
    unsigned char *session_key;
    char encryption_type;
    short error;
    char *this_server;
    Stat *cache;             /* Cache for network connection (SYNCH result) */
} AgentConnection;



/* misc.c */

const char *sockaddr_ntop(const void *src, char *dst, socklen_t size);


#endif
