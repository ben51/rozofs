/*
 * Please do not edit this file.
 * It was generated using rpcgen.
 */

#ifndef _MPPROTO_H_RPCGEN
#define _MPPROTO_H_RPCGEN

#include <rpc/rpc.h>


#ifdef __cplusplus
extern "C" {
#endif

#include <rozofs/rozofs.h>

enum mpp_status_t {
	MPP_SUCCESS = 0,
	MPP_FAILURE = 1,
};
typedef enum mpp_status_t mpp_status_t;

struct mpp_status_ret_t {
	mpp_status_t status;
	union {
		int error;
	} mpp_status_ret_t_u;
};
typedef struct mpp_status_ret_t mpp_status_ret_t;

struct mpp_profiler_t {
	uint64_t uptime;
	uint64_t now;
	uint8_t vers[20];
	uint64_t rozofs_ll_lookup[2];
	uint64_t rozofs_ll_forget[2];
	uint64_t rozofs_ll_getattr[2];
	uint64_t rozofs_ll_setattr[2];
	uint64_t rozofs_ll_readlink[2];
	uint64_t rozofs_ll_mknod[2];
	uint64_t rozofs_ll_mkdir[2];
	uint64_t rozofs_ll_unlink[2];
	uint64_t rozofs_ll_rmdir[2];
	uint64_t rozofs_ll_symlink[2];
	uint64_t rozofs_ll_rename[2];
	uint64_t rozofs_ll_open[2];
	uint64_t rozofs_ll_link[2];
	uint64_t rozofs_ll_read[3];
	uint64_t rozofs_ll_write[3];
	uint64_t rozofs_ll_flush[2];
	uint64_t rozofs_ll_release[2];
	uint64_t rozofs_ll_opendir[2];
	uint64_t rozofs_ll_readdir[2];
	uint64_t rozofs_ll_releasedir[2];
	uint64_t rozofs_ll_fsyncdir[2];
	uint64_t rozofs_ll_statfs[2];
	uint64_t rozofs_ll_setxattr[2];
	uint64_t rozofs_ll_getxattr[2];
	uint64_t rozofs_ll_listxattr[2];
	uint64_t rozofs_ll_removexattr[2];
	uint64_t rozofs_ll_access[2];
	uint64_t rozofs_ll_create[2];
	uint64_t rozofs_ll_getlk[2];
	uint64_t rozofs_ll_setlk[2];
	uint64_t rozofs_ll_ioctl[2];
};
typedef struct mpp_profiler_t mpp_profiler_t;

struct mpp_profiler_ret_t {
	mpp_status_t status;
	union {
		mpp_profiler_t profiler;
		int error;
	} mpp_profiler_ret_t_u;
};
typedef struct mpp_profiler_ret_t mpp_profiler_ret_t;

#define ROZOFSMOUNT_PROFILE_PROGRAM 0x20000006
#define ROZOFSMOUNT_PROFILE_VERSION 1

#if defined(__STDC__) || defined(__cplusplus)
#define MPP_NULL 0
extern  void * mpp_null_1(void *, CLIENT *);
extern  void * mpp_null_1_svc(void *, struct svc_req *);
#define MPP_GET_PROFILER 1
extern  mpp_profiler_ret_t * mpp_get_profiler_1(void *, CLIENT *);
extern  mpp_profiler_ret_t * mpp_get_profiler_1_svc(void *, struct svc_req *);
#define MPP_CLEAR 2
extern  mpp_status_ret_t * mpp_clear_1(void *, CLIENT *);
extern  mpp_status_ret_t * mpp_clear_1_svc(void *, struct svc_req *);
extern int rozofsmount_profile_program_1_freeresult (SVCXPRT *, xdrproc_t, caddr_t);

#else /* K&R C */
#define MPP_NULL 0
extern  void * mpp_null_1();
extern  void * mpp_null_1_svc();
#define MPP_GET_PROFILER 1
extern  mpp_profiler_ret_t * mpp_get_profiler_1();
extern  mpp_profiler_ret_t * mpp_get_profiler_1_svc();
#define MPP_CLEAR 2
extern  mpp_status_ret_t * mpp_clear_1();
extern  mpp_status_ret_t * mpp_clear_1_svc();
extern int rozofsmount_profile_program_1_freeresult ();
#endif /* K&R C */

/* the xdr functions */

#if defined(__STDC__) || defined(__cplusplus)
extern  bool_t xdr_mpp_status_t (XDR *, mpp_status_t*);
extern  bool_t xdr_mpp_status_ret_t (XDR *, mpp_status_ret_t*);
extern  bool_t xdr_mpp_profiler_t (XDR *, mpp_profiler_t*);
extern  bool_t xdr_mpp_profiler_ret_t (XDR *, mpp_profiler_ret_t*);

#else /* K&R C */
extern bool_t xdr_mpp_status_t ();
extern bool_t xdr_mpp_status_ret_t ();
extern bool_t xdr_mpp_profiler_t ();
extern bool_t xdr_mpp_profiler_ret_t ();

#endif /* K&R C */

#ifdef __cplusplus
}
#endif

#endif /* !_MPPROTO_H_RPCGEN */
