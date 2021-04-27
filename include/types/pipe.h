/*
  include/types/pipe.h
  Pipe management.

  Copyright (C) 2000-2009 Willy Tarreau - w@1wt.eu

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, version 2.1
  exclusively.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef _TYPES_PIPE_H
#define _TYPES_PIPE_H

#include <common/config.h>
#include <types/global.h>
#include <types/cuju_ft.h>

/* A pipe is described by its read and write FDs, and the data remaining in it.
 * The FDs are valid if there are data pending. The user is not allowed to
 * change the FDs.
 */
struct pipe {
	int data;	/* number of bytes present in the pipe  */
	int prod;	/* FD the producer must write to ; -1 if none */
	int cons;	/* FD the consumer must read from ; -1 if none */
	struct pipe *next;

#if ENABLE_CUJU_FT
  struct pipe *pipe_nxt;
  struct pipe *pipe_dup;
  struct pipe *pipe_ted; /* Only for Pipe Head */

  //struct pipe *pipe_nxt_last;
  //struct pipe *pipe_dup_last;
  //struct pipe *pipe_ted_last; /* Only for Pipe Head */

  unsigned long flush_id;
  u_int32_t flush_idx;
  unsigned long epoch_id;
  u_int32_t epoch_idx;
  int in_fd;
  int out_fd;
  int trans_suspend;
  unsigned long transfer_cnt;
  int transfered; 

  u_int32_t offset;
#endif
};

#endif /* _TYPES_PIPE_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
