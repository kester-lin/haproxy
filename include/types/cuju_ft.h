#ifndef _TYPES_CUKU_FT_H
#define _TYPES_CUKU_FT_H

#include <proto/pipe.h>

#if ENABLE_CUJU_FT
extern int fd_list_migration;

unsigned long ft_get_flushcnt();
int ft_dup_pipe(struct pipe *source, struct pipe *dest);
#endif

#endif