// Entspricht der von Xcode generierten <Kext>_info.c
#include <mach/mach_types.h>
#include <mach/kmod.h>

#ifndef __APPLE_CC__
#define __APPLE_CC__ 6000
#endif

extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
extern kern_return_t BootBeacon_real_start(kmod_info_t *, void *);
extern kern_return_t BootBeacon_kern_stop(kmod_info_t *, void *);

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.pigcraft.BootBeacon, "1.1.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = BootBeacon_real_start;
__private_extern__ kmod_stop_func_t *_antimain = BootBeacon_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
