#ifndef COLD_STATIC_HOST_SECTIONS_H
#define COLD_STATIC_HOST_SECTIONS_H

#define L2_RET_BSS_SECT_BEGIN(name)
#define L2_RET_BSS_SECT(name)
#define L2_RET_BSS_SECT_END
#define INIT_COMPONENT_EXPORT(fn) static int (*const host_init_##fn)(void) = fn

#endif
