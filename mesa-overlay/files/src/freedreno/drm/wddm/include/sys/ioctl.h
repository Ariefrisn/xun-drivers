/* ioctl() for libsync.h on Windows: there are no fence fds, always fails (XunGpu shim). */
#ifndef _XUNGPU_SYS_IOCTL_H_
#define _XUNGPU_SYS_IOCTL_H_
#include "sys/ioccom.h"
#ifdef __cplusplus
extern "C" {
#endif
int fd_wddm_ioctl(int fd, unsigned long request, ...);
#ifdef __cplusplus
}
#endif
#define ioctl fd_wddm_ioctl
#endif
