#define _GNU_SOURCE

#include <common/splice.h>
#include <errno.h>
#include <types/cuju_ft.h>
#include <types/global.h>
#include <types/fd.h>

#include <proto/fd.h>

#ifdef DEBUG_FULL
#include <assert.h>
#endif

#if ENABLE_CUJU_FT
int fd_list_migration = 0;

unsigned long ft_get_flushcnt() 
{
    static unsigned long flush_count = 0;

    return flush_count++;
}

int ft_dup_pipe(struct pipe *source, struct pipe *dest, int clean)
{
    int ret = 0;
    static unsigned long retry_cnt = 0;

    if (!source->data) {
        return 0;
    }
    if (dest->data) {
#ifdef DEBUG_FULL        
        assert(1);
#else
        return 0;
#endif        
    }

    while (1) {
        if (clean) {
		    ret = splice(source->cons, NULL, dest->prod, NULL, 
					     source->data, SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
        }
        else {
            ret = tee(source->cons, dest->prod, source->data, SPLICE_F_NONBLOCK);
        }

        if (ret < 0) {
            if (errno == EAGAIN) {
                retry_cnt++;
                ret = 0;
                continue;
            }
            
            return ret;
        }

        break;
    }

    dest->data = source->data;
    dest->in_fd = source->in_fd;
    dest->out_fd = source->out_fd;

    return ret;
}



/* Cuju IPC handler callback */
void cuju_fd_handler(int fd)
{
	struct connection *conn = fdtab[fd].owner;

	if (unlikely(!conn))
		return;

    printf("cuju_fd_handler fd is %d\n", fd);

	return;
}
#endif
