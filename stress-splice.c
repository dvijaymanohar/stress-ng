/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * This code is a complete clean re-write of the stress tool by
 * Colin Ian King <colin.king@canonical.com> and attempts to be
 * backwardly compatible with the stress tool by Amos Waterland
 * <apw@rossby.metr.ou.edu> but has more stress tests and more
 * functionality.
 *
 */
#include "stress-ng.h"

#define SPLICE_BUFFER_LEN (65536)

static const stress_help_t help[] =
{
  { NULL, "splice N",   "start N workers reading/writing using splice" },
  { NULL, "splice-ops N",   "stop after N bogo splice operations" },
  { NULL, "splice-bytes N", "number of bytes to transfer per splice call" },
  { NULL, NULL,     NULL }
};

static int stress_set_splice_bytes(const char *opt)
{
  size_t splice_bytes;
  splice_bytes = (size_t)stress_get_uint64_byte_memory(opt, 1);
  stress_check_range_bytes("splice-bytes", splice_bytes,
                           MIN_SPLICE_BYTES, MAX_MEM_LIMIT);
  return stress_set_setting("splice-bytes", TYPE_ID_SIZE_T, &splice_bytes);
}

static const stress_opt_set_func_t opt_set_funcs[] =
{
  { OPT_splice_bytes, stress_set_splice_bytes },
  { 0,      NULL }
};

#if defined(HAVE_SPLICE) && \
    defined(SPLICE_F_MOVE)

/*
 *  stress_splice_write()
 *  write buffer to fd
 */
static inline int stress_splice_write(
  const int fd,
  const char *buffer,
  const ssize_t buffer_len,
  ssize_t size)
{
  ssize_t ret = 0;
  
  while (size > 0)
  {
    size_t n = (size_t)(size > buffer_len ? buffer_len : size);
    ret = write(fd, buffer, n);
    
    if (ret < 0)
    {
      break;
    }
    
    size -= n;
  }
  
  return (int)ret;
}

/*
 *  stress_splice_non_block_write_4K()
 *  get some data into a pipe to prime it for a read
 *  with stress_splice_looped_pipe()
 */
static bool stress_splice_non_block_write_4K(const int fd)
{
  char buffer[4096];
  int flags;
  flags = fcntl(fd, F_GETFL, 0);
  
  if (flags < 0)
  {
    return false;
  }
  
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
  {
    return false;
  }
  
  (void)memset(buffer, 0xa5, sizeof(buffer));
  
  if (write(fd, buffer, sizeof(buffer)) < 0)
  {
    return false;
  }
  
  if (fcntl(fd, F_SETFL, flags) < 0)
  {
    return false;
  }
  
  return true;
}

/*
 *  stress_splice_looped_pipe()
 *  splice data into one pipe, out of another and back into
 *  the first pipe
 */
static void stress_splice_looped_pipe(
  const int fds3[2],
  const int fds4[2],
  bool *use_splice_loop)
{
  ssize_t ret;
  
  if (!*use_splice_loop)
  {
    return;
  }
  
  ret = splice(fds3[0], 0, fds4[1], 0, 4096, SPLICE_F_MOVE);
  
  if (ret < 0)
  {
    *use_splice_loop = false;
    return;
  }
  
  ret = splice(fds4[0], 0, fds3[1], 0, 4096, SPLICE_F_MOVE);
  
  if (ret < 0)
  {
    *use_splice_loop = false;
    return;
  }
}

/*
 *  stress_splice
 *  stress copying of /dev/zero to /dev/null
 */
static int stress_splice(const stress_args_t *args)
{
  int fd_in, fd_out, fds1[2], fds2[2], fds3[2], fds4[2];
  size_t splice_bytes = DEFAULT_SPLICE_BYTES;
  int rc = EXIT_FAILURE;
  bool use_splice = true;
  bool use_splice_loop;
  char *buffer;
  ssize_t buffer_len;
  
  if (!stress_get_setting("splice-bytes", &splice_bytes))
  {
    if (g_opt_flags & OPT_FLAGS_MAXIMIZE)
    {
      splice_bytes = MAX_SPLICE_BYTES;
    }
    
    if (g_opt_flags & OPT_FLAGS_MINIMIZE)
    {
      splice_bytes = MIN_SPLICE_BYTES;
    }
  }
  
  splice_bytes /= args->num_instances;
  
  if (splice_bytes < MIN_SPLICE_BYTES)
  {
    splice_bytes = MIN_SPLICE_BYTES;
  }
  
  buffer_len = (ssize_t)(splice_bytes > SPLICE_BUFFER_LEN ?
                         SPLICE_BUFFER_LEN : splice_bytes);
  buffer = mmap(NULL, (size_t)buffer_len, PROT_READ | PROT_WRITE,
                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
                
  if (buffer == MAP_FAILED)
  {
    pr_inf("%s: cannot allocate write buffer, errno=%d (%s)\n",
           args->name, errno, strerror(errno));
    goto close_done;
  }
  
  if ((fd_in = open("/dev/zero", O_RDONLY)) < 0)
  {
    pr_fail("%s: open /dev/zero failed, errno=%d (%s)\n",
            args->name, errno, strerror(errno));
    goto close_unmap;
  }
  
  /*
   *  /dev/zero -> pipe splice -> pipe splice -> /dev/null
   */
  if (pipe(fds1) < 0)
  {
    pr_fail("%s: pipe failed, errno=%d (%s)\n",
            args->name, errno, strerror(errno));
    goto close_fd_in;
  }
  
  if (pipe(fds2) < 0)
  {
    pr_fail("%s: pipe failed, errno=%d (%s)\n",
            args->name, errno, strerror(errno));
    goto close_fds1;
  }
  
  if (pipe(fds3) < 0)
  {
    pr_fail("%s: pipe failed, errno=%d (%s)\n",
            args->name, errno, strerror(errno));
    goto close_fds2;
  }
  
  if (pipe(fds4) < 0)
  {
    pr_fail("%s: pipe failed, errno=%d (%s)\n",
            args->name, errno, strerror(errno));
    goto close_fds3;
  }
  
  if ((fd_out = open("/dev/null", O_WRONLY)) < 0)
  {
    pr_fail("%s: open /dev/null failed, errno=%d (%s)\n",
            args->name, errno, strerror(errno));
    goto close_fds4;
  }
  
  /*
   *  place data in fds3 for splice loop pipes
   */
  use_splice_loop = stress_splice_non_block_write_4K(fds3[1]);
  stress_set_proc_state(args->name, STRESS_STATE_RUN);
  
  do
  {
    ssize_t ret;
    loff_t off_in, off_out;
    
    /*
     *  Linux 5.9 dropped the ability to splice from /dev/zero to
     *  a pipe, so fall back to writing to the pipe to at least
     *  get some data into the pipe for subsequent splicing in
     *  the pipeline.
     */
    if (use_splice)
    {
      ret = splice(fd_in, NULL, fds1[1], NULL,
                   splice_bytes, SPLICE_F_MOVE);
                   
      if (ret < 0)
      {
        if (errno == EINVAL)
        {
          if (args->instance == 0)
          {
            pr_inf("%s: using direct write to pipe and not splicing "
                   "from /dev/zero as this is not supported in "
                   "this kernel\n", args->name);
          }
          
          use_splice = false;
          continue;
        }
        
        break;
      }
    }
    else
    {
      ret = stress_splice_write(fds1[1], buffer,
                                buffer_len,
                                (ssize_t)splice_bytes);
                                
      if (ret < 0)
      {
        break;
      }
    }
    
    ret = splice(fds1[0], NULL, fds2[1], NULL,
                 splice_bytes, SPLICE_F_MOVE);
                 
    if (ret < 0)
    {
      break;
    }
    
    ret = splice(fds2[0], NULL, fd_out, NULL,
                 splice_bytes, SPLICE_F_MOVE);
                 
    if (ret < 0)
    {
      break;
    }
    
    /* Exercise -ESPIPE errors */
    off_in = 1;
    off_out = 1;
    ret = splice(fds1[0], &off_in, fds1[1], &off_out,
                 4096, SPLICE_F_MOVE);
    (void)ret;
    off_out = 1;
    ret = splice(fd_in, NULL, fds1[1], &off_out,
                 splice_bytes, SPLICE_F_MOVE);
    (void)ret;
    off_in = 1;
    ret = splice(fds1[0], &off_in, fd_out, NULL,
                 splice_bytes, SPLICE_F_MOVE);
    (void)ret;
    /* Exercise no-op splice of zero size */
    ret = splice(fd_in, NULL, fds1[1], NULL,
                 0, SPLICE_F_MOVE);
    (void)ret;
    /* Exercise invalid splice flags */
    ret = splice(fd_in, NULL, fds1[1], NULL,
                 1, ~0U);
    (void)ret;
    /* Exercise 1 byte splice, zero flags */
    ret = splice(fd_in, NULL, fds1[1], NULL,
                 1, 0);
    (void)ret;
    /* Exercise splicing to oneself */
    off_in = 0;
    off_out = 0;
    ret = splice(fds1[1], &off_in, fds1[1], &off_out,
                 4096, SPLICE_F_MOVE);
    (void)ret;
    /* Exercise splice loop from one pipe to another and back */
    stress_splice_looped_pipe(fds3, fds4, &use_splice_loop);
    stress_splice_looped_pipe(fds3, fds4, &use_splice_loop);
    inc_counter(args);
  }
  while (keep_stressing(args));
  
  rc = EXIT_SUCCESS;
  stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
  (void)close(fd_out);
close_fds4:
  stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
  (void)close(fds4[0]);
  (void)close(fds4[1]);
close_fds3:
  stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
  (void)close(fds3[0]);
  (void)close(fds3[1]);
close_fds2:
  stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
  (void)close(fds2[0]);
  (void)close(fds2[1]);
close_fds1:
  stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
  (void)close(fds1[0]);
  (void)close(fds1[1]);
close_fd_in:
  stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
  (void)close(fd_in);
close_unmap:
  (void)munmap((void *)buffer, (size_t)buffer_len);
close_done:
  stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
  return rc;
}

stressor_info_t stress_splice_info =
{
  .stressor = stress_splice,
  .class = CLASS_PIPE_IO | CLASS_OS,
  .opt_set_funcs = opt_set_funcs,
  .help = help
};
#else
stressor_info_t stress_splice_info =
{
  .stressor = stress_not_implemented,
  .class = CLASS_PIPE_IO | CLASS_OS,
  .opt_set_funcs = opt_set_funcs,
  .help = help
};
#endif
