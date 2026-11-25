#ifndef _VPLAYER_COMPAT_ERRNO_H
#define _VPLAYER_COMPAT_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define EAGAIN       11
#define EWOULDBLOCK  11
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define EINVAL       22
#define ENOSPC       28
#define ERANGE       34
#define ENOSYS       38
#define ETIMEDOUT    110

int* __vplayer_errno_location(void);
#define errno (*__vplayer_errno_location())

#ifdef __cplusplus
}
#endif

#endif

