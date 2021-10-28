#include <config.h>
#include <stdio.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <selinux/selinux.h>
#if HAVE_HURD_H
# include <hurd.h>
#endif
#if HAVE_PRIV_H
# include <priv.h>
#endif

#include "system.h"
#include "acl.h"
#include "backupfile.h"
#include "buffer-lcm.h"
#include "canonicalize.h"
#include "copy.h"
#include "cp-hash.h"
#include "extent-scan.h"
#include "die.h"
#include "error.h"
#include "fadvise.h"
#include "fcntl--.h"
#include "fiemap.h"
#include "file-set.h"
#include "filemode.h"
#include "filenamecat.h"
#include "force-link.h"
#include "full-write.h"
#include "hash.h"
#include "hash-triple.h"
#include "ignore-value.h"
#include "ioblksize.h"
#include "quote.h"
#include "renameatu.h"
#include "root-uid.h"
#include "same.h"
#include "savedir.h"
#include "stat-size.h"
#include "stat-time.h"
#include "utimecmp.h"
#include "utimens.h"
#include "write-any-file.h"
#include "areadlink.h"
#include "yesno.h"
#include "selinux.h"

#if USE_XATTR
# include <attr/error_context.h>
# include <attr/libattr.h>
# include <stdarg.h>
# include "verror.h"
#endif

#if HAVE_LINUX_FALLOC_H
# include <linux/falloc.h>
#endif
#ifdef HAVE_LINUX_FS_H
# include <linux/fs.h>
#endif

#if !defined FICLONE && defined __linux__
# define FICLONE _IOW (0x94, 9, int)
#endif

#ifndef HAVE_FCHOWN
# define HAVE_FCHOWN false
# define fchown(fd, uid, gid) (-1)
#endif

#ifndef HAVE_LCHOWN
# define HAVE_LCHOWN false
# define lchown(name, uid, gid) chown (name, uid, gid)
#endif

#ifndef HAVE_MKFIFO
static int
rpl_mkfifo (char const *file, mode_t mode)
{
  errno = ENOTSUP;
  return -1;
}
# define mkfifo rpl_mkfifo
#endif

#ifndef USE_ACL
# define USE_ACL 0
#endif

#define SAME_OWNER(A, B) ((A).st_uid == (B).st_uid)
#define SAME_GROUP(A, B) ((A).st_gid == (B).st_gid)
#define SAME_OWNER_AND_GROUP(A, B) (SAME_OWNER (A, B) && SAME_GROUP (A, B))
#if (defined HAVE_LINKAT && ! LINKAT_SYMLINK_NOTSUP) || ! LINK_FOLLOWS_SYMLINKS
# define CAN_HARDLINK_SYMLINKS 1
#else
# define CAN_HARDLINK_SYMLINKS 0
#endif

struct dir_list
{
  struct dir_list *parent;
  ino_t ino;
  dev_t dev;
};

#define DEST_INFO_INITIAL_CAPACITY 61

static bool copy_internal (char const *src_name, char const *dst_name,
                           bool new_dst, struct stat const *parent,
                           struct dir_list *ancestors,
                           const struct cp_options *x,
                           bool command_line_arg,
                           bool *first_dir_created_per_command_line_arg,
                           bool *copy_into_self,
                           bool *rename_succeeded);
static bool owner_failure_ok (struct cp_options const *x);
static char const *top_level_src_name;
static char const *top_level_dst_name;

#ifndef DEV_FD_MIGHT_BE_CHR
# define DEV_FD_MIGHT_BE_CHR false
#endif
static int
follow_fstatat (int dirfd, char const *filename, struct stat *st, int flags)
{
  int result = fstatat (dirfd, filename, st, flags);

  if (DEV_FD_MIGHT_BE_CHR && result == 0 && !(flags & AT_SYMLINK_NOFOLLOW)
      && S_ISCHR (st->st_mode))
    {
      static dev_t stdin_rdev;
      static signed char stdin_rdev_status;
      if (stdin_rdev_status == 0)
        {
          struct stat stdin_st;
          if (stat ("/dev/stdin", &stdin_st) == 0 && S_ISCHR (stdin_st.st_mode)
              && minor (stdin_st.st_rdev) == STDIN_FILENO)
            {
              stdin_rdev = stdin_st.st_rdev;
              stdin_rdev_status = 1;
            }
          else
            stdin_rdev_status = -1;
        }
      if (0 < stdin_rdev_status && major (stdin_rdev) == major (st->st_rdev))
        result = fstat (minor (st->st_rdev), st);
    }

  return result;
}

static inline int
utimens_symlink (char const *file, struct timespec const *timespec)
{
  int err = lutimens (file, timespec);
  if (err && errno == ENOSYS)
    err = 0;
  return err;
}

static int
punch_hole (int fd, off_t offset, off_t length)
{
  int ret = 0;
#if HAVE_FALLOCATE + 0
# if defined FALLOC_FL_PUNCH_HOLE && defined FALLOC_FL_KEEP_SIZE
  ret = fallocate (fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                   offset, length);
  if (ret < 0 && (is_ENOTSUP (errno) || errno == ENOSYS))
    ret = 0;
# endif
#endif
  return ret;
}

static bool
create_hole (int fd, char const *name, bool punch_holes, off_t size)
{
  off_t file_end = lseek (fd, size, SEEK_CUR);

  if (file_end < 0)
    {
      error (0, errno, _("cannot lseek %s"), quoteaf (name));
      return false;
    }
  if (punch_holes && punch_hole (fd, file_end - size, size) < 0)
    {
      error (0, errno, _("error deallocating %s"), quoteaf (name));
      return false;
    }

  return true;
}
static bool
sparse_copy (int src_fd, int dest_fd, char *buf, size_t buf_size,
             size_t hole_size, bool punch_holes,
             char const *src_name, char const *dst_name,
             uintmax_t max_n_read, off_t *total_n_read,
             bool *last_write_made_hole)
{
  *last_write_made_hole = false;
  *total_n_read = 0;
  if (!hole_size)
    while (max_n_read)
      {
        ssize_t ssize_max = TYPE_MAXIMUM (ssize_t);
        ptrdiff_t copy_max = MIN (ssize_max, SIZE_MAX) >> 30 << 30;
        ssize_t n_copied = copy_file_range (src_fd, NULL, dest_fd, NULL,
                                            MIN (max_n_read, copy_max), 0);
        if (n_copied == 0)
          {
            if (*total_n_read == 0)
              break;
            return true;
          }
        if (n_copied < 0)
          {
            if (errno == ENOSYS || errno == EINVAL
                || errno == EBADF || errno == EXDEV)
              break;
            if (errno == EINTR)
              n_copied = 0;
            else
              {
                error (0, errno, _("error copying %s to %s"),
                       quoteaf_n (0, src_name), quoteaf_n (1, dst_name));
                return false;
              }
          }
        max_n_read -= n_copied;
        *total_n_read += n_copied;
      }

  bool make_hole = false;
  off_t psize = 0;

  while (max_n_read)
    {
      ssize_t n_read = read (src_fd, buf, MIN (max_n_read, buf_size));
      if (n_read < 0)
        {
          if (errno == EINTR)
            continue;
          error (0, errno, _("error reading %s"), quoteaf (src_name));
          return false;
        }
      if (n_read == 0)
        break;
      max_n_read -= n_read;
      *total_n_read += n_read;
      size_t csize = hole_size ? hole_size : buf_size;
      char *cbuf = buf;
      char *pbuf = buf;

      while (n_read)
        {
          bool prev_hole = make_hole;
          csize = MIN (csize, n_read);

          if (hole_size && csize)
            make_hole = is_nul (cbuf, csize);

          bool transition = (make_hole != prev_hole) && psize;
          bool last_chunk = (n_read == csize && ! make_hole) || ! csize;

          if (transition || last_chunk)
            {
              if (! transition)
                psize += csize;

              if (! prev_hole)
                {
                  if (full_write (dest_fd, pbuf, psize) != psize)
                    {
                      error (0, errno, _("error writing %s"),
                             quoteaf (dst_name));
                      return false;
                    }
                }
              else
                {
                  if (! create_hole (dest_fd, dst_name, punch_holes, psize))
                    return false;
                }

              pbuf = cbuf;
              psize = csize;

              if (last_chunk)
                {
                  if (! csize)
                    n_read = 0; 

                  if (transition)
                    csize = 0; 
                  else
                    psize = 0;
                }
            }
          else  
            {
              if (INT_ADD_WRAPV (psize, csize, &psize))
                {
                  error (0, 0, _("overflow reading %s"), quoteaf (src_name));
                  return false;
                }
            }

          n_read -= csize;
          cbuf += csize;
        }

      *last_write_made_hole = make_hole;
    }

  if (make_hole && ! create_hole (dest_fd, dst_name, punch_holes, psize))
    return false;
  else
    return true;
}

static inline int
clone_file (int dest_fd, int src_fd)
{
#ifdef FICLONE
  return ioctl (dest_fd, FICLONE, src_fd);
#else
  (void) dest_fd;
  (void) src_fd;
  errno = ENOTSUP;
  return -1;
#endif
}
static bool
write_zeros (int fd, off_t n_bytes)
{
  static char *zeros;
  static size_t nz = IO_BUFSIZE;

  if (zeros == NULL)
    {
      static char fallback[1024];
      zeros = calloc (nz, 1);
      if (zeros == NULL)
        {
          zeros = fallback;
          nz = sizeof fallback;
        }
    }

  while (n_bytes)
    {
      size_t n = MIN (nz, n_bytes);
      if ((full_write (fd, zeros, n)) != n)
        return false;
      n_bytes -= n;
    }

  return true;
}

static bool
extent_copy (int src_fd, int dest_fd, char *buf, size_t buf_size,
             size_t hole_size, off_t src_total_size,
             enum Sparse_type sparse_mode,
             char const *src_name, char const *dst_name,
             struct extent_scan *scan)
{
  off_t last_ext_start = 0;
  off_t last_ext_len = 0;

  off_t dest_pos = 0;

  bool wrote_hole_at_eof = true;
  while (true)
    {
      bool empty_extent = false;
      for (unsigned int i = 0; i < scan->ei_count || empty_extent; i++)
        {
          off_t ext_start;
          off_t ext_len;
          off_t ext_hole_size;

          if (i < scan->ei_count)
            {
              ext_start = scan->ext_info[i].ext_logical;
              ext_len = scan->ext_info[i].ext_length;
            }
          else 
            {
              i--;
              ext_start = last_ext_start + scan->ext_info[i].ext_length;
              ext_len = 0;
            }

          if (src_total_size < ext_start + ext_len)
            {
              if (src_total_size < ext_start)
                ext_start = src_total_size;
              ext_len = src_total_size - ext_start;
            }

          ext_hole_size = ext_start - last_ext_start - last_ext_len;

          wrote_hole_at_eof = false;

          if (ext_hole_size)
            {
              if (lseek (src_fd, ext_start, SEEK_SET) < 0)
                {
                  error (0, errno, _("cannot lseek %s"), quoteaf (src_name));
                fail:
                  extent_scan_free (scan);
                  return false;
                }

              if ((empty_extent && sparse_mode == SPARSE_ALWAYS)
                  || (!empty_extent && sparse_mode != SPARSE_NEVER))
                {
                  if (! create_hole (dest_fd, dst_name,
                                     sparse_mode == SPARSE_ALWAYS,
                                     ext_hole_size))
                    goto fail;
                  wrote_hole_at_eof = true;
                }
              else
                {
                  off_t nzeros = ext_hole_size;
                  if (empty_extent)
                    nzeros = MIN (src_total_size - dest_pos, ext_hole_size);

                  if (! write_zeros (dest_fd, nzeros))
                    {
                      error (0, errno, _("%s: write failed"),
                             quotef (dst_name));
                      goto fail;
                    }

                  dest_pos = MIN (src_total_size, ext_start);
                }
            }

          last_ext_start = ext_start;
          if (0 && (scan->ext_info[i].ext_flags & FIEMAP_EXTENT_UNWRITTEN))
            {
              empty_extent = true;
              last_ext_len = 0;
              if (ext_len == 0) 
                empty_extent = false;
            }
          else
            {
              off_t n_read;
              empty_extent = false;
              last_ext_len = ext_len;
              bool read_hole;

              if ( ! sparse_copy (src_fd, dest_fd, buf, buf_size,
                                  sparse_mode == SPARSE_ALWAYS ? hole_size: 0,
                                  true, src_name, dst_name, ext_len, &n_read,
                                  &read_hole))
                goto fail;

              dest_pos = ext_start + n_read;
              if (n_read)
                wrote_hole_at_eof = read_hole;
            }

          if (dest_pos == src_total_size)
            {
              scan->hit_final_extent = true;
              break;
            }
        }

      extent_scan_free (scan);

      if (scan->hit_final_extent)
        break;
      if (! extent_scan_read (scan) && ! scan->hit_final_extent)
        {
          error (0, errno, _("%s: failed to get extents info"),
                 quotef (src_name));
          return false;
        }
    }

  if ((dest_pos < src_total_size || wrote_hole_at_eof)
      && (sparse_mode != SPARSE_NEVER
          ? ftruncate (dest_fd, src_total_size)
          : ! write_zeros (dest_fd, src_total_size - dest_pos)))
    {
      error (0, errno, _("failed to extend %s"), quoteaf (dst_name));
      return false;
    }

  if (sparse_mode == SPARSE_ALWAYS && dest_pos < src_total_size
      && punch_hole (dest_fd, dest_pos, src_total_size - dest_pos) < 0)
    {
      error (0, errno, _("error deallocating %s"), quoteaf (dst_name));
      return false;
    }

  return true;
}

#ifdef SEEK_HOLE

static bool
lseek_copy (int src_fd, int dest_fd, char *buf, size_t buf_size,
            size_t hole_size, off_t ext_start, off_t src_total_size,
            enum Sparse_type sparse_mode,
            char const *src_name, char const *dst_name)
{
  off_t last_ext_start = 0;
  off_t last_ext_len = 0;
  off_t dest_pos = 0;
  bool wrote_hole_at_eof = true;

  while (0 <= ext_start)
    {
      off_t ext_end = lseek (src_fd, ext_start, SEEK_HOLE);
      if (ext_end < 0)
        {
          if (errno != ENXIO)
            goto cannot_lseek;
          ext_end = src_total_size;
          if (ext_end <= ext_start)
            {
              src_total_size = lseek (src_fd, 0, SEEK_END);
              if (src_total_size < 0)
                goto cannot_lseek;

              if (src_total_size <= ext_start)
                break;

              ext_end = src_total_size;
            }
        }
      if (src_total_size < ext_end)
        src_total_size = ext_end;

      if (lseek (src_fd, ext_start, SEEK_SET) < 0)
        goto cannot_lseek;

      wrote_hole_at_eof = false;
      off_t ext_hole_size = ext_start - last_ext_start - last_ext_len;

      if (ext_hole_size)
        {
          if (sparse_mode != SPARSE_NEVER)
            {
              if (! create_hole (dest_fd, dst_name,
                                 sparse_mode == SPARSE_ALWAYS,
                                 ext_hole_size))
                return false;
              wrote_hole_at_eof = true;
            }
          else
            {
              if (! write_zeros (dest_fd, ext_hole_size))
                {
                  error (0, errno, _("%s: write failed"),
                         quotef (dst_name));
                  return false;
                }
            }
        }

      off_t ext_len = ext_end - ext_start;
      last_ext_start = ext_start;
      last_ext_len = ext_len;
      off_t n_read;
      bool read_hole;
      if ( ! sparse_copy (src_fd, dest_fd, buf, buf_size,
                          sparse_mode == SPARSE_NEVER ? 0 : hole_size,
                          true, src_name, dst_name, ext_len, &n_read,
                          &read_hole))
        return false;

      dest_pos = ext_start + n_read;
      if (n_read)
        wrote_hole_at_eof = read_hole;
      if (n_read < ext_len)
        {
          src_total_size = dest_pos;
          break;
        }

      ext_start = lseek (src_fd, dest_pos, SEEK_DATA);
      if (ext_start < 0)
        {
          if (errno != ENXIO)
            goto cannot_lseek;
          break;
        }
    }
  if ((dest_pos < src_total_size || wrote_hole_at_eof)
      && ! (sparse_mode == SPARSE_NEVER
            ? write_zeros (dest_fd, src_total_size - dest_pos)
            : ftruncate (dest_fd, src_total_size) == 0))
    {
      error (0, errno, _("failed to extend %s"), quoteaf (dst_name));
      return false;
    }

  if (sparse_mode == SPARSE_ALWAYS && dest_pos < src_total_size
      && punch_hole (dest_fd, dest_pos, src_total_size - dest_pos) < 0)
    {
      error (0, errno, _("error deallocating %s"), quoteaf (dst_name));
      return false;
    }

  return true;

 cannot_lseek:
  error (0, errno, _("cannot lseek %s"), quoteaf (src_name));
  return false;
}
#endif

static bool _GL_ATTRIBUTE_PURE
is_ancestor (const struct stat *sb, const struct dir_list *ancestors)
{
  while (ancestors != 0)
    {
      if (ancestors->ino == sb->st_ino && ancestors->dev == sb->st_dev)
        return true;
      ancestors = ancestors->parent;
    }
  return false;
}

static bool
errno_unsupported (int err)
{
  return err == ENOTSUP || err == ENODATA;
}

#if USE_XATTR
static void
copy_attr_error (struct error_context *ctx _GL_UNUSED,
                 char const *fmt, ...)
{
  if (!errno_unsupported (errno))
    {
      int err = errno;
      va_list ap;
      va_start (ap, fmt);
      verror (0, err, fmt, ap);
      va_end (ap);
    }
}

static void
copy_attr_allerror (struct error_context *ctx _GL_UNUSED,
                 char const *fmt, ...)
{
  int err = errno;
  va_list ap;

  va_start (ap, fmt);
  verror (0, err, fmt, ap);
  va_end (ap);
}

static char const *
copy_attr_quote (struct error_context *ctx _GL_UNUSED, char const *str)
{
  return quoteaf (str);
}

static void
copy_attr_free (struct error_context *ctx _GL_UNUSED,
                char const *str _GL_UNUSED)
{
}

static int
check_selinux_attr (char const *name, struct error_context *ctx)
{
  return STRNCMP_LIT (name, "security.selinux")
         && attr_copy_check_permissions (name, ctx);
}

static bool copy_attr (char const *src_path, int src_fd,
           char const *dst_path, int dst_fd, struct cp_options const *x)
{
  int ret;
  bool all_errors = (!x->data_copy_required || x->require_preserve_xattr);
  bool some_errors = (!all_errors && !x->reduce_diagnostics);
  bool selinux_done = (x->preserve_security_context || x->set_security_context);
  struct error_context ctx =
  {
    .error = all_errors ? copy_attr_allerror : copy_attr_error,
    .quote = copy_attr_quote,
    .quote_free = copy_attr_free
  };
  if (0 <= src_fd && 0 <= dst_fd)
    ret = attr_copy_fd (src_path, src_fd, dst_path, dst_fd,
                        selinux_done ? check_selinux_attr : NULL,
                        (all_errors || some_errors ? &ctx : NULL));
  else
    ret = attr_copy_file (src_path, dst_path,
                          selinux_done ? check_selinux_attr : NULL,
                          (all_errors || some_errors ? &ctx : NULL));

  return ret == 0;
}
#else 

static bool
copy_attr (char const *src_path _GL_UNUSED,
           int src_fd _GL_UNUSED,
           char const *dst_path _GL_UNUSED,
           int dst_fd _GL_UNUSED,
           struct cp_options const *x _GL_UNUSED)
{
  return true;
}
#endif 

static bool
copy_dir (char const *src_name_in, char const *dst_name_in, bool new_dst,
          const struct stat *src_sb, struct dir_list *ancestors,
          const struct cp_options *x,
          bool *first_dir_created_per_command_line_arg,
          bool *copy_into_self)
{
  char *name_space;
  char *namep;
  struct cp_options non_command_line_options = *x;
  bool ok = true;

  name_space = savedir (src_name_in, SAVEDIR_SORT_FASTREAD);
  if (name_space == NULL)
    {
      error (0, errno, _("cannot access %s"), quoteaf (src_name_in));
      return false;
    }
  if (x->dereference == DEREF_COMMAND_LINE_ARGUMENTS)
    non_command_line_options.dereference = DEREF_NEVER;

  bool new_first_dir_created = false;
  namep = name_space;
  while (*namep != '\0')
    {
      bool local_copy_into_self;
      char *src_name = file_name_concat (src_name_in, namep, NULL);
      char *dst_name = file_name_concat (dst_name_in, namep, NULL);
      bool first_dir_created = *first_dir_created_per_command_line_arg;

      ok &= copy_internal (src_name, dst_name, new_dst, src_sb,
                           ancestors, &non_command_line_options, false,
                           &first_dir_created,
                           &local_copy_into_self, NULL);
      *copy_into_self |= local_copy_into_self;

      free (dst_name);
      free (src_name);
      if (local_copy_into_self)
        break;

      new_first_dir_created |= first_dir_created;
      namep += strlen (namep) + 1;
    }
  free (name_space);
  *first_dir_created_per_command_line_arg = new_first_dir_created;

  return ok;
}

static int
set_owner (const struct cp_options *x, char const *dst_name, int dest_desc,
           struct stat const *src_sb, bool new_dst,
           struct stat const *dst_sb)
{
  uid_t uid = src_sb->st_uid;
  gid_t gid = src_sb->st_gid;

  if (!new_dst && (x->preserve_mode || x->move_mode || x->set_mode))
    {
      mode_t old_mode = dst_sb->st_mode;
      mode_t new_mode =
        (x->preserve_mode || x->move_mode ? src_sb->st_mode : x->mode);
      mode_t restrictive_temp_mode = old_mode & new_mode & S_IRWXU;

      if ((USE_ACL
           || (old_mode & CHMOD_MODE_BITS
               & (~new_mode | S_ISUID | S_ISGID | S_ISVTX)))
          && qset_acl (dst_name, dest_desc, restrictive_temp_mode) != 0)
        {
          if (! owner_failure_ok (x))
            error (0, errno, _("clearing permissions for %s"),
                   quoteaf (dst_name));
          return -x->require_preserve;
        }
    }

  if (HAVE_FCHOWN && dest_desc != -1)
    {
      if (fchown (dest_desc, uid, gid) == 0)
        return 1;
      if (errno == EPERM || errno == EINVAL)
        {
          int saved_errno = errno;
          ignore_value (fchown (dest_desc, -1, gid));
          errno = saved_errno;
        }
    }
  else
    {
      if (lchown (dst_name, uid, gid) == 0)
        return 1;
      if (errno == EPERM || errno == EINVAL)
        {
          int saved_errno = errno;
          ignore_value (lchown (dst_name, -1, gid));
          errno = saved_errno;
        }
    }

  if (! chown_failure_ok (x))
    {
      error (0, errno, _("failed to preserve ownership for %s"),
             quoteaf (dst_name));
      if (x->require_preserve)
        return -1;
    }

  return 0;
}


static void
set_author (char const *dst_name, int dest_desc, const struct stat *src_sb)
{
#if HAVE_STRUCT_STAT_ST_AUTHOR
  file_t file = (dest_desc < 0
                 ? file_name_lookup (dst_name, 0, 0)
                 : getdport (dest_desc));
  if (file == MACH_PORT_NULL)
    error (0, errno, _("failed to lookup file %s"), quoteaf (dst_name));
  else
    {
      error_t err = file_chauthor (file, src_sb->st_author);
      if (err)
        error (0, err, _("failed to preserve authorship for %s"),
               quoteaf (dst_name));
      mach_port_deallocate (mach_task_self (), file);
    }
#else
  (void) dst_name;
  (void) dest_desc;
  (void) src_sb;
#endif
}

bool
set_process_security_ctx (char const *src_name, char const *dst_name,
                          mode_t mode, bool new_dst, const struct cp_options *x)
{
  if (x->preserve_security_context)
    {
      bool all_errors = !x->data_copy_required || x->require_preserve_context;
      bool some_errors = !all_errors && !x->reduce_diagnostics;
      char *con;

      if (0 <= lgetfilecon (src_name, &con))
        {
          if (setfscreatecon (con) < 0)
            {
              if (all_errors || (some_errors && !errno_unsupported (errno)))
                error (0, errno,
                       _("failed to set default file creation context to %s"),
                       quote (con));
              if (x->require_preserve_context)
                {
                  freecon (con);
                  return false;
                }
            }
          freecon (con);
        }
      else
        {
          if (all_errors || (some_errors && !errno_unsupported (errno)))
            {
              error (0, errno,
                     _("failed to get security context of %s"),
                     quoteaf (src_name));
            }
          if (x->require_preserve_context)
            return false;
        }
    }
  else if (x->set_security_context)
    {
      if (new_dst && defaultcon (x->set_security_context, dst_name, mode) < 0
          && ! ignorable_ctx_err (errno))
        {
          error (0, errno,
                 _("failed to set default file creation context for %s"),
                 quoteaf (dst_name));
        }
    }

  return true;
}

bool
set_file_security_ctx (char const *dst_name,
                       bool recurse, const struct cp_options *x)
{
  bool all_errors = (!x->data_copy_required
                     || x->require_preserve_context);
  bool some_errors = !all_errors && !x->reduce_diagnostics;

  if (! restorecon (x->set_security_context, dst_name, recurse))
    {
      if (all_errors || (some_errors && !errno_unsupported (errno)))
        error (0, errno, _("failed to set the security context of %s"),
               quoteaf_n (0, dst_name));
      return false;
    }

  return true;
}
static int
fchmod_or_lchmod (int desc, char const *name, mode_t mode)
{
#if HAVE_FCHMOD
  if (0 <= desc)
    return fchmod (desc, mode);
#endif
  return lchmod (name, mode);
}

#ifndef HAVE_STRUCT_STAT_ST_BLOCKS
# define HAVE_STRUCT_STAT_ST_BLOCKS 0
#endif
enum scantype
  {
   ERROR_SCANTYPE,
   PLAIN_SCANTYPE,
   ZERO_SCANTYPE,
   LSEEK_SCANTYPE,
   EXTENT_SCANTYPE
  };

union scan_inference
{
  off_t ext_start;
  struct extent_scan extent_scan;
};
static enum scantype
infer_scantype (int fd, struct stat const *sb,
                union scan_inference *scan_inference)
{
  if (! (HAVE_STRUCT_STAT_ST_BLOCKS
         && S_ISREG (sb->st_mode)
         && ST_NBLOCKS (*sb) < sb->st_size / ST_NBLOCKSIZE))
    return PLAIN_SCANTYPE;

#ifdef SEEK_HOLE
  scan_inference->ext_start = lseek (fd, 0, SEEK_DATA);
  if (0 <= scan_inference->ext_start)
    return LSEEK_SCANTYPE;
  else if (errno != EINVAL && errno != ENOTSUP)
    return errno == ENXIO ? LSEEK_SCANTYPE : ERROR_SCANTYPE;
#endif

  struct extent_scan *scan = &scan_inference->extent_scan;
  extent_scan_init (fd, scan);
  extent_scan_read (scan);
  return scan->initial_scan_failed ? ZERO_SCANTYPE : EXTENT_SCANTYPE;
}

static bool
copy_reg (char const *src_name, char const *dst_name,
          const struct cp_options *x,
          mode_t dst_mode, mode_t omitted_permissions, bool *new_dst,
          struct stat const *src_sb)
{
  char *buf;
  char *buf_alloc = NULL;
  char *name_alloc = NULL;
  int dest_desc;
  int dest_errno;
  int source_desc;
  mode_t src_mode = src_sb->st_mode;
  struct stat sb;
  struct stat src_open_sb;
  union scan_inference scan_inference;
  bool return_val = true;
  bool data_copy_required = x->data_copy_required;

  source_desc = open (src_name,
                      (O_RDONLY | O_BINARY
                       | (x->dereference == DEREF_NEVER ? O_NOFOLLOW : 0)));
  if (source_desc < 0)
    {
      error (0, errno, _("cannot open %s for reading"), quoteaf (src_name));
      return false;
    }

  if (fstat (source_desc, &src_open_sb) != 0)
    {
      error (0, errno, _("cannot fstat %s"), quoteaf (src_name));
      return_val = false;
      goto close_src_desc;
    }
  if (! SAME_INODE (*src_sb, src_open_sb))
    {
      error (0, 0,
             _("skipping file %s, as it was replaced while being copied"),
             quoteaf (src_name));
      return_val = false;
      goto close_src_desc;
    }
  if (! *new_dst)
    {
      int open_flags =
        O_WRONLY | O_BINARY | (x->data_copy_required ? O_TRUNC : 0);
      dest_desc = open (dst_name, open_flags);
      dest_errno = errno;
      if ((x->set_security_context || x->preserve_security_context)
          && 0 <= dest_desc)
        {
          if (! set_file_security_ctx (dst_name, false, x))
            {
              if (x->require_preserve_context)
                {
                  return_val = false;
                  goto close_src_and_dst_desc;
                }
            }
        }

      if (dest_desc < 0 && x->unlink_dest_after_failed_open)
        {
          if (unlink (dst_name) != 0)
            {
              error (0, errno, _("cannot remove %s"), quoteaf (dst_name));
              return_val = false;
              goto close_src_desc;
            }
          if (x->verbose)
            printf (_("removed %s\n"), quoteaf (dst_name));

          *new_dst = true;
          if (x->set_security_context)
            {
              if (! set_process_security_ctx (src_name, dst_name, dst_mode,
                                              *new_dst, x))
                {
                  return_val = false;
                  goto close_src_desc;
                }
            }
        }
    }

  if (*new_dst)
    {
    open_with_O_CREAT:;

      int open_flags = O_WRONLY | O_CREAT | O_BINARY;
      dest_desc = open (dst_name, open_flags | O_EXCL,
                        dst_mode & ~omitted_permissions);
      dest_errno = errno;
      if (dest_desc < 0 && dest_errno == EEXIST && ! x->move_mode)
        {
          struct stat dangling_link_sb;
          if (lstat (dst_name, &dangling_link_sb) == 0
              && S_ISLNK (dangling_link_sb.st_mode))
            {
              if (x->open_dangling_dest_symlink)
                {
                  dest_desc = open (dst_name, open_flags,
                                    dst_mode & ~omitted_permissions);
                  dest_errno = errno;
                }
              else
                {
                  error (0, 0, _("not writing through dangling symlink %s"),
                         quoteaf (dst_name));
                  return_val = false;
                  goto close_src_desc;
                }
            }
        }

      if (dest_desc < 0 && dest_errno == EISDIR
          && *dst_name && dst_name[strlen (dst_name) - 1] == '/')
        dest_errno = ENOTDIR;
    }
  else
    {
      omitted_permissions = 0;
    }

  if (dest_desc < 0)
    {
      if (dest_errno == ENOENT && ! *new_dst && ! x->move_mode)
        {
          *new_dst = 1;
          goto open_with_O_CREAT;
        }

      error (0, dest_errno, _("cannot create regular file %s"),
             quoteaf (dst_name));
      return_val = false;
      goto close_src_desc;
    }

  if (fstat (dest_desc, &sb) != 0)
    {
      error (0, errno, _("cannot fstat %s"), quoteaf (dst_name));
      return_val = false;
      goto close_src_and_dst_desc;
    }
  if (data_copy_required && x->reflink_mode)
    {
      bool clone_ok = clone_file (dest_desc, source_desc) == 0;
      if (clone_ok || x->reflink_mode == REFLINK_ALWAYS)
        {
          if (!clone_ok)
            {
              error (0, errno, _("failed to clone %s from %s"),
                     quoteaf_n (0, dst_name), quoteaf_n (1, src_name));
              return_val = false;
              goto close_src_and_dst_desc;
            }
          data_copy_required = false;
        }
    }

  if (data_copy_required)
    {
      size_t buf_alignment = getpagesize ();
      size_t buf_size = io_blksize (sb);
      size_t hole_size = ST_BLKSIZE (sb);

      enum scantype scantype = infer_scantype (source_desc, &src_open_sb,
                                               &scan_inference);
      if (scantype == ERROR_SCANTYPE)
        {
          error (0, errno, _("cannot lseek %s"), quoteaf (src_name));
          return_val = false;
          goto close_src_and_dst_desc;
        }
      bool make_holes
        = (S_ISREG (sb.st_mode)
           && (x->sparse_mode == SPARSE_ALWAYS
               || (x->sparse_mode == SPARSE_AUTO
                   && scantype != PLAIN_SCANTYPE)));

      fdadvise (source_desc, 0, 0, FADVISE_SEQUENTIAL);
      if (! make_holes)
        {
          size_t blcm_max = MIN (SIZE_MAX, SSIZE_MAX) - buf_alignment;
          size_t blcm = buffer_lcm (io_blksize (src_open_sb), buf_size,
                                    blcm_max);
          if (S_ISREG (src_open_sb.st_mode) && src_open_sb.st_size < buf_size)
            buf_size = src_open_sb.st_size + 1;
          buf_size += blcm - 1;
          buf_size -= buf_size % blcm;
          if (buf_size == 0 || blcm_max < buf_size)
            buf_size = blcm;
        }

      buf_alloc = xmalloc (buf_size + buf_alignment);
      buf = ptr_align (buf_alloc, buf_alignment);

      off_t n_read;
      bool wrote_hole_at_eof = false;
      if (! (scantype == EXTENT_SCANTYPE
             ? extent_copy (source_desc, dest_desc, buf, buf_size, hole_size,
                            src_open_sb.st_size,
                            make_holes ? x->sparse_mode : SPARSE_NEVER,
                            src_name, dst_name, &scan_inference.extent_scan)
#ifdef SEEK_HOLE
             : scantype == LSEEK_SCANTYPE
             ? lseek_copy (source_desc, dest_desc, buf, buf_size, hole_size,
                           scan_inference.ext_start, src_open_sb.st_size,
                           make_holes ? x->sparse_mode : SPARSE_NEVER,
                           src_name, dst_name)
#endif
             : sparse_copy (source_desc, dest_desc, buf, buf_size,
                            make_holes ? hole_size : 0,
                            x->sparse_mode == SPARSE_ALWAYS,
                            src_name, dst_name, UINTMAX_MAX, &n_read,
                            &wrote_hole_at_eof)))
        {
          return_val = false;
          goto close_src_and_dst_desc;
        }
      else if (wrote_hole_at_eof && ftruncate (dest_desc, n_read) < 0)
        {
          error (0, errno, _("failed to extend %s"), quoteaf (dst_name));
          return_val = false;
          goto close_src_and_dst_desc;
        }
    }

  if (x->preserve_timestamps)
    {
      struct timespec timespec[2];
      timespec[0] = get_stat_atime (src_sb);
      timespec[1] = get_stat_mtime (src_sb);

      if (fdutimens (dest_desc, dst_name, timespec) != 0)
        {
          error (0, errno, _("preserving times for %s"), quoteaf (dst_name));
          if (x->require_preserve)
            {
              return_val = false;
              goto close_src_and_dst_desc;
            }
        }
    }
  if (x->preserve_ownership && ! SAME_OWNER_AND_GROUP (*src_sb, sb))
    {
      switch (set_owner (x, dst_name, dest_desc, src_sb, *new_dst, &sb))
        {
        case -1:
          return_val = false;
          goto close_src_and_dst_desc;

        case 0:
          src_mode &= ~ (S_ISUID | S_ISGID | S_ISVTX);
          break;
        }
    }
  if (x->preserve_xattr)
    {
      bool access_changed = false;

      if (!(sb.st_mode & S_IWUSR) && geteuid () != ROOT_UID)
        {
          access_changed = fchmod_or_lchmod (dest_desc, dst_name,
                                             S_IRUSR | S_IWUSR) == 0;
        }

      if (!copy_attr (src_name, source_desc, dst_name, dest_desc, x)
          && x->require_preserve_xattr)
        return_val = false;

      if (access_changed)
        fchmod_or_lchmod (dest_desc, dst_name, dst_mode & ~omitted_permissions);
    }

  set_author (dst_name, dest_desc, src_sb);

  if (x->preserve_mode || x->move_mode)
    {
      if (copy_acl (src_name, source_desc, dst_name, dest_desc, src_mode) != 0
          && x->require_preserve)
        return_val = false;
    }
  else if (x->set_mode)
    {
      if (set_acl (dst_name, dest_desc, x->mode) != 0)
        return_val = false;
    }
  else if (x->explicit_no_preserve_mode && *new_dst)
    {
      if (set_acl (dst_name, dest_desc, MODE_RW_UGO & ~cached_umask ()) != 0)
        return_val = false;
    }
  else if (omitted_permissions)
    {
      omitted_permissions &= ~ cached_umask ();
      if (omitted_permissions
          && fchmod_or_lchmod (dest_desc, dst_name, dst_mode) != 0)
        {
          error (0, errno, _("preserving permissions for %s"),
                 quoteaf (dst_name));
          if (x->require_preserve)
            return_val = false;
        }
    }

close_src_and_dst_desc:
  if (close (dest_desc) < 0)
    {
      error (0, errno, _("failed to close %s"), quoteaf (dst_name));
      return_val = false;
    }
close_src_desc:
  if (close (source_desc) < 0)
    {
      error (0, errno, _("failed to close %s"), quoteaf (src_name));
      return_val = false;
    }

  free (buf_alloc);
  free (name_alloc);
  return return_val;
}

static bool
same_file_ok (char const *src_name, struct stat const *src_sb,
              char const *dst_name, struct stat const *dst_sb,
              const struct cp_options *x, bool *return_now)
{
  const struct stat *src_sb_link;
  const struct stat *dst_sb_link;
  struct stat tmp_dst_sb;
  struct stat tmp_src_sb;

  bool same_link;
  bool same = SAME_INODE (*src_sb, *dst_sb);

  *return_now = false;
  if (same && x->hard_link)
    {
      *return_now = true;
      return true;
    }

  if (x->dereference == DEREF_NEVER)
    {
      same_link = same;
      if (S_ISLNK (src_sb->st_mode) && S_ISLNK (dst_sb->st_mode))
        {
          bool sn = same_name (src_name, dst_name);
          if ( ! sn)
            {
              if (x->backup_type != no_backups)
                return true;
              if (same_link)
                {
                  *return_now = true;
                  return ! x->move_mode;
                }
            }

          return ! sn;
        }

      src_sb_link = src_sb;
      dst_sb_link = dst_sb;
    }
  else
    {
      if (!same)
        return true;

      if (lstat (dst_name, &tmp_dst_sb) != 0
          || lstat (src_name, &tmp_src_sb) != 0)
        return true;

      src_sb_link = &tmp_src_sb;
      dst_sb_link = &tmp_dst_sb;

      same_link = SAME_INODE (*src_sb_link, *dst_sb_link);
      if (S_ISLNK (src_sb_link->st_mode) && S_ISLNK (dst_sb_link->st_mode)
          && x->unlink_dest_before_opening)
        return true;
    }
  if (x->backup_type != no_backups)
    {
      if (!same_link)
        {
          if ( ! x->move_mode
               && x->dereference != DEREF_NEVER
               && S_ISLNK (src_sb_link->st_mode)
               && ! S_ISLNK (dst_sb_link->st_mode))
            return false;

          return true;
        }
      return ! same_name (src_name, dst_name);
    }

#if 0
#endif

  if (x->move_mode || x->unlink_dest_before_opening)
    {
      if (S_ISLNK (dst_sb_link->st_mode))
        return true;
      if (same_link
          && 1 < dst_sb_link->st_nlink
          && ! same_name (src_name, dst_name))
        return ! x->move_mode;
    }
  if (!S_ISLNK (src_sb_link->st_mode) && !S_ISLNK (dst_sb_link->st_mode))
    {
      if (!SAME_INODE (*src_sb_link, *dst_sb_link))
        return true;
      if (x->hard_link)
        {
          *return_now = true;
          return true;
        }
    }

  if (x->move_mode
      && S_ISLNK (src_sb->st_mode)
      && 1 < dst_sb_link->st_nlink)
    {
      char *abs_src = canonicalize_file_name (src_name);
      if (abs_src)
        {
          bool result = ! same_name (abs_src, dst_name);
          free (abs_src);
          return result;
        }
    }

  if (x->symbolic_link && S_ISLNK (dst_sb_link->st_mode))
    return true;

  if (x->dereference == DEREF_NEVER)
    {
      if ( ! S_ISLNK (src_sb_link->st_mode))
        tmp_src_sb = *src_sb_link;
      else if (stat (src_name, &tmp_src_sb) != 0)
        return true;

      if ( ! S_ISLNK (dst_sb_link->st_mode))
        tmp_dst_sb = *dst_sb_link;
      else if (stat (dst_name, &tmp_dst_sb) != 0)
        return true;

      if ( ! SAME_INODE (tmp_src_sb, tmp_dst_sb))
        return true;

      if (x->hard_link)
        {
          *return_now = ! S_ISLNK (dst_sb_link->st_mode);
          return true;
        }
    }

  return false;
}

static bool
writable_destination (char const *file, mode_t mode)
{
  return (S_ISLNK (mode)
          || can_write_any_file ()
          || euidaccess (file, W_OK) == 0);
}

static bool
overwrite_ok (struct cp_options const *x, char const *dst_name,
              struct stat const *dst_sb)
{
  if (! writable_destination (dst_name, dst_sb->st_mode))
    {
      char perms[12];		
      strmode (dst_sb->st_mode, perms);
      perms[10] = '\0';
      fprintf (stderr,
               (x->move_mode || x->unlink_dest_before_opening
                || x->unlink_dest_after_failed_open)
               ? _("%s: replace %s, overriding mode %04lo (%s)? ")
               : _("%s: unwritable %s (mode %04lo, %s); try anyway? "),
               program_name, quoteaf (dst_name),
               (unsigned long int) (dst_sb->st_mode & CHMOD_MODE_BITS),
               &perms[1]);
    }
  else
    {
      fprintf (stderr, _("%s: overwrite %s? "),
               program_name, quoteaf (dst_name));
    }

  return yesno ();
}

extern void
dest_info_init (struct cp_options *x)
{
  x->dest_info
    = hash_initialize (DEST_INFO_INITIAL_CAPACITY,
                       NULL,
                       triple_hash,
                       triple_compare,
                       triple_free);
}

extern void
src_info_init (struct cp_options *x)
{

  x->src_info
    = hash_initialize (DEST_INFO_INITIAL_CAPACITY,
                       NULL,
                       triple_hash_no_name,
                       triple_compare,
                       triple_free);
}

static bool
abandon_move (const struct cp_options *x,
              char const *dst_name,
              struct stat const *dst_sb)
{
  assert (x->move_mode);
  return (x->interactive == I_ALWAYS_NO
          || ((x->interactive == I_ASK_USER
               || (x->interactive == I_UNSPECIFIED
                   && x->stdin_tty
                   && ! writable_destination (dst_name, dst_sb->st_mode)))
              && ! overwrite_ok (x, dst_name, dst_sb)));
}

static void
emit_verbose (char const *src, char const *dst, char const *backup_dst_name)
{
  printf ("%s -> %s", quoteaf_n (0, src), quoteaf_n (1, dst));
  if (backup_dst_name)
    printf (_(" (backup: %s)"), quoteaf (backup_dst_name));
  putchar ('\n');
}
static void
restore_default_fscreatecon_or_die (void)
{
  if (setfscreatecon (NULL) != 0)
    die (EXIT_FAILURE, errno,
         _("failed to restore the default file creation context"));
}

static bool
create_hard_link (char const *src_name, char const *dst_name,
                  bool replace, bool verbose, bool dereference)
{
  int err = force_linkat (AT_FDCWD, src_name, AT_FDCWD, dst_name,
                          dereference ? AT_SYMLINK_FOLLOW : 0,
                          replace, -1);
  if (0 < err)
    {
      error (0, err, _("cannot create hard link %s to %s"),
             quoteaf_n (0, dst_name), quoteaf_n (1, src_name));
      return false;
    }
  if (err < 0 && verbose)
    printf (_("removed %s\n"), quoteaf (dst_name));
  return true;
}

static inline bool _GL_ATTRIBUTE_PURE
should_dereference (const struct cp_options *x, bool command_line_arg)
{
  return x->dereference == DEREF_ALWAYS
         || (x->dereference == DEREF_COMMAND_LINE_ARGUMENTS
             && command_line_arg);
}

static bool
source_is_dst_backup (char const *srcbase, struct stat const *src_st,
                      char const *dst_name)
{
  size_t srcbaselen = strlen (srcbase);
  char const *dstbase = last_component (dst_name);
  size_t dstbaselen = strlen (dstbase);
  size_t suffixlen = strlen (simple_backup_suffix);
  if (! (srcbaselen == dstbaselen + suffixlen
         && memcmp (srcbase, dstbase, dstbaselen) == 0
         && STREQ (srcbase + dstbaselen, simple_backup_suffix)))
    return false;
  size_t dstlen = strlen (dst_name);
  char *dst_back = xmalloc (dstlen + suffixlen + 1);
  strcpy (mempcpy (dst_back, dst_name, dstlen), simple_backup_suffix);
  struct stat dst_back_sb;
  int dst_back_status = stat (dst_back, &dst_back_sb);
  free (dst_back);
  return dst_back_status == 0 && SAME_INODE (*src_st, dst_back_sb);
}
static bool
copy_internal (char const *src_name, char const *dst_name,
               bool new_dst,
               struct stat const *parent,
               struct dir_list *ancestors,
               const struct cp_options *x,
               bool command_line_arg,
               bool *first_dir_created_per_command_line_arg,
               bool *copy_into_self,
               bool *rename_succeeded)
{
  struct stat src_sb;
  struct stat dst_sb;
  mode_t src_mode IF_LINT ( = 0);
  mode_t dst_mode IF_LINT ( = 0);
  mode_t dst_mode_bits;
  mode_t omitted_permissions;
  bool restore_dst_mode = false;
  char *earlier_file = NULL;
  char *dst_backup = NULL;
  bool delayed_ok;
  bool copied_as_regular = false;
  bool dest_is_symlink = false;
  bool have_dst_lstat = false;

  *copy_into_self = false;

  int rename_errno = x->rename_errno;
  if (x->move_mode)
    {
      if (rename_errno < 0)
        rename_errno = (renameatu (AT_FDCWD, src_name, AT_FDCWD, dst_name,
                                   RENAME_NOREPLACE)
                        ? errno : 0);
      new_dst = rename_errno == 0;
      if (rename_succeeded)
        *rename_succeeded = new_dst;
    }

  if (rename_errno == 0
      ? !x->last_file
      : rename_errno != EEXIST || x->interactive != I_ALWAYS_NO)
    {
      char const *name = rename_errno == 0 ? dst_name : src_name;
      int fstatat_flags
        = x->dereference == DEREF_NEVER ? AT_SYMLINK_NOFOLLOW : 0;
      if (follow_fstatat (AT_FDCWD, name, &src_sb, fstatat_flags) != 0)
        {
          error (0, errno, _("cannot stat %s"), quoteaf (name));
          return false;
        }

      src_mode = src_sb.st_mode;

      if (S_ISDIR (src_mode) && !x->recursive)
        {
          error (0, 0, ! x->install_mode /* cp */
                 ? _("-r not specified; omitting directory %s")
                 : _("omitting directory %s"),
                 quoteaf (src_name));
          return false;
        }
    }
#ifdef lint
  else
    {
      assert (x->move_mode);
      memset (&src_sb, 0, sizeof src_sb);
    }
#endif

  if (command_line_arg && x->src_info)
    {
      if ( ! S_ISDIR (src_mode)
           && x->backup_type == no_backups
           && seen_file (x->src_info, src_name, &src_sb))
        {
          error (0, 0, _("warning: source file %s specified more than once"),
                 quoteaf (src_name));
          return true;
        }

      record_file (x->src_info, src_name, &src_sb);
    }

  bool dereference = should_dereference (x, command_line_arg);

  if (!new_dst)
    {
      if (! (rename_errno == EEXIST && x->interactive == I_ALWAYS_NO))
        {
          bool use_lstat
            = ((! S_ISREG (src_mode)
                && (! x->copy_as_regular
                    || S_ISDIR (src_mode) || S_ISLNK (src_mode)))
               || x->move_mode || x->symbolic_link || x->hard_link
               || x->backup_type != no_backups
               || x->unlink_dest_before_opening);
          int fstatat_flags = use_lstat ? AT_SYMLINK_NOFOLLOW : 0;
          if (follow_fstatat (AT_FDCWD, dst_name, &dst_sb, fstatat_flags) == 0)
            {
              have_dst_lstat = use_lstat;
              rename_errno = EEXIST;
            }
          else
            {
              if (errno == ELOOP && x->unlink_dest_after_failed_open)
                /* leave new_dst=false so we unlink later.  */;
              else if (errno != ENOENT)
                {
                  error (0, errno, _("cannot stat %s"), quoteaf (dst_name));
                  return false;
                }
              else
                new_dst = true;
            }
        }

      if (rename_errno == EEXIST)
        {
          bool return_now = false;

          if (x->interactive != I_ALWAYS_NO
              && ! same_file_ok (src_name, &src_sb, dst_name, &dst_sb,
                                 x, &return_now))
            {
              error (0, 0, _("%s and %s are the same file"),
                     quoteaf_n (0, src_name), quoteaf_n (1, dst_name));
              return false;
            }

          if (x->update && !S_ISDIR (src_mode))
            {
              int options = ((x->preserve_timestamps
                              && ! (x->move_mode
                                    && dst_sb.st_dev == src_sb.st_dev))
                             ? UTIMECMP_TRUNCATE_SOURCE
                             : 0);

              if (0 <= utimecmp (dst_name, &dst_sb, &src_sb, options))
                {
                  if (rename_succeeded)
                    *rename_succeeded = true;

                  earlier_file = remember_copied (dst_name, src_sb.st_ino,
                                                  src_sb.st_dev);
                  if (earlier_file)
                    {
                      if (! create_hard_link (earlier_file, dst_name, true,
                                              x->verbose, dereference))
                        {
                          goto un_backup;
                        }
                    }

                  return true;
                }
            }

          if (x->move_mode)
            {
              if (abandon_move (x, dst_name, &dst_sb))
                {
                  if (rename_succeeded)
                    *rename_succeeded = true;
                  return true;
                }
            }
          else
            {
              if (! S_ISDIR (src_mode)
                  && (x->interactive == I_ALWAYS_NO
                      || (x->interactive == I_ASK_USER
                          && ! overwrite_ok (x, dst_name, &dst_sb))))
                return true;
            }

          if (return_now)
            return true;

          if (!S_ISDIR (dst_sb.st_mode))
            {
              if (S_ISDIR (src_mode))
                {
                  if (x->move_mode && x->backup_type != no_backups)
                    {
                    }
                  else
                    {
                      error (0, 0,
                       _("cannot overwrite non-directory %s with directory %s"),
                             quoteaf_n (0, dst_name), quoteaf_n (1, src_name));
                      return false;
                    }
                }
              if (command_line_arg
                  && x->backup_type != numbered_backups
                  && seen_file (x->dest_info, dst_name, &dst_sb))
                {
                  error (0, 0,
                         _("will not overwrite just-created %s with %s"),
                         quoteaf_n (0, dst_name), quoteaf_n (1, src_name));
                  return false;
                }
            }

          if (!S_ISDIR (src_mode))
            {
              if (S_ISDIR (dst_sb.st_mode))
                {
                  if (x->move_mode && x->backup_type != no_backups)
                    {
                    }
                  else
                    {
                      error (0, 0,
                         _("cannot overwrite directory %s with non-directory"),
                             quoteaf (dst_name));
                      return false;
                    }
                }
            }

          if (x->move_mode)
            {
              if (S_ISDIR (src_sb.st_mode) && !S_ISDIR (dst_sb.st_mode)
                  && x->backup_type == no_backups)
                {
                  error (0, 0,
                       _("cannot move directory onto non-directory: %s -> %s"),
                         quotef_n (0, src_name), quotef_n (0, dst_name));
                  return false;
                }
            }

          char const *srcbase;
          if (x->backup_type != no_backups
              && ! dot_or_dotdot (srcbase = last_component (src_name))
              && (x->move_mode || ! S_ISDIR (dst_sb.st_mode)))
            {
              if (x->backup_type != numbered_backups
                  && source_is_dst_backup (srcbase, &src_sb, dst_name))
                {
                  char const *fmt;
                  fmt = (x->move_mode
                 ? _("backing up %s might destroy source;  %s not moved")
                 : _("backing up %s might destroy source;  %s not copied"));
                  error (0, 0, fmt,
                         quoteaf_n (0, dst_name),
                         quoteaf_n (1, src_name));
                  return false;
                }

              char *tmp_backup = backup_file_rename (AT_FDCWD, dst_name,
                                                     x->backup_type);
              if (tmp_backup)
                {
                  ASSIGN_STRDUPA (dst_backup, tmp_backup);
                  free (tmp_backup);
                }
              else if (errno != ENOENT)
                {
                  error (0, errno, _("cannot backup %s"), quoteaf (dst_name));
                  return false;
                }
              new_dst = true;
            }
          else if (! S_ISDIR (dst_sb.st_mode)
                   && ! x->move_mode
                   && (x->unlink_dest_before_opening
                       || (x->data_copy_required
                           && ((x->preserve_links && 1 < dst_sb.st_nlink)
                               || (x->dereference == DEREF_NEVER
                                   && ! S_ISREG (src_sb.st_mode))))
                      ))
            {
              if (unlink (dst_name) != 0 && errno != ENOENT)
                {
                  error (0, errno, _("cannot remove %s"), quoteaf (dst_name));
                  return false;
                }
              new_dst = true;
              if (x->verbose)
                printf (_("removed %s\n"), quoteaf (dst_name));
            }
        }
    }
  if (command_line_arg
      && x->dest_info
      && ! x->move_mode
      && x->backup_type == no_backups)
    {
      bool lstat_ok = true;
      struct stat tmp_buf;
      struct stat *dst_lstat_sb;
      if (have_dst_lstat)
        dst_lstat_sb = &dst_sb;
      else
        {
          if (lstat (dst_name, &tmp_buf) == 0)
            dst_lstat_sb = &tmp_buf;
          else
            lstat_ok = false;
        }
      if (lstat_ok
          && S_ISLNK (dst_lstat_sb->st_mode)
          && seen_file (x->dest_info, dst_name, dst_lstat_sb))
        {
          error (0, 0,
                 _("will not copy %s through just-created symlink %s"),
                 quoteaf_n (0, src_name), quoteaf_n (1, dst_name));
          return false;
        }
    }
  if (x->verbose && !x->move_mode && !S_ISDIR (src_mode))
    emit_verbose (src_name, dst_name, dst_backup);

  if (rename_errno == 0)
    earlier_file = NULL;
  else if (x->recursive && S_ISDIR (src_mode))
    {
      if (command_line_arg)
        earlier_file = remember_copied (dst_name, src_sb.st_ino, src_sb.st_dev);
      else
        earlier_file = src_to_dest_lookup (src_sb.st_ino, src_sb.st_dev);
    }
  else if (x->move_mode && src_sb.st_nlink == 1)
    {
      earlier_file = src_to_dest_lookup (src_sb.st_ino, src_sb.st_dev);
    }
  else if (x->preserve_links
           && !x->hard_link
           && (1 < src_sb.st_nlink
               || (command_line_arg
                   && x->dereference == DEREF_COMMAND_LINE_ARGUMENTS)
               || x->dereference == DEREF_ALWAYS))
    {
      earlier_file = remember_copied (dst_name, src_sb.st_ino, src_sb.st_dev);
    }
  if (earlier_file)
    {
      if (S_ISDIR (src_mode))
        {
          if (same_name (src_name, earlier_file))
            {
              error (0, 0, _("cannot copy a directory, %s, into itself, %s"),
                     quoteaf_n (0, top_level_src_name),
                     quoteaf_n (1, top_level_dst_name));
              *copy_into_self = true;
              goto un_backup;
            }
          else if (same_name (dst_name, earlier_file))
            {
              error (0, 0, _("warning: source directory %s "
                             "specified more than once"),
                     quoteaf (top_level_src_name));
              if (x->move_mode && rename_succeeded)
                *rename_succeeded = true;
              return true;
            }
          else if (x->dereference == DEREF_ALWAYS
                   || (command_line_arg
                       && x->dereference == DEREF_COMMAND_LINE_ARGUMENTS))
            {
              
            }
          else
            {
              error (0, 0, _("will not create hard link %s to directory %s"),
                     quoteaf_n (0, dst_name), quoteaf_n (1, earlier_file));
              goto un_backup;
            }
        }
      else
        {
          if (! create_hard_link (earlier_file, dst_name, true, x->verbose,
                                  dereference))
            goto un_backup;

          return true;
        }
    }

  if (x->move_mode)
    {
      if (rename_errno == EEXIST)
        rename_errno = rename (src_name, dst_name) == 0 ? 0 : errno;

      if (rename_errno == 0)
        {
          if (x->verbose)
            {
              printf (_("renamed "));
              emit_verbose (src_name, dst_name, dst_backup);
            }

          if (x->set_security_context)
            {
              (void) set_file_security_ctx (dst_name, true, x);
            }

          if (rename_succeeded)
            *rename_succeeded = true;

          if (command_line_arg && !x->last_file)
            {
              record_file (x->dest_info, dst_name, &src_sb);
            }

          return true;
        }
      if (rename_errno == EINVAL)
        {
          error (0, 0, _("cannot move %s to a subdirectory of itself, %s"),
                 quoteaf_n (0, top_level_src_name),
                 quoteaf_n (1, top_level_dst_name));

          *copy_into_self = true;
          return true;
        }
      if (rename_errno != EXDEV)
        {
          error (0, rename_errno,
                 _("cannot move %s to %s"),
                 quoteaf_n (0, src_name), quoteaf_n (1, dst_name));
          forget_created (src_sb.st_ino, src_sb.st_dev);
          return false;
        }
      if ((S_ISDIR (src_mode) ? rmdir (dst_name) : unlink (dst_name)) != 0
          && errno != ENOENT)
        {
          error (0, errno,
             _("inter-device move failed: %s to %s; unable to remove target"),
                 quoteaf_n (0, src_name), quoteaf_n (1, dst_name));
          forget_created (src_sb.st_ino, src_sb.st_dev);
          return false;
        }

      if (x->verbose && !S_ISDIR (src_mode))
        {
          printf (_("copied "));
          emit_verbose (src_name, dst_name, dst_backup);
        }
      new_dst = true;
    }
  dst_mode_bits = (x->set_mode ? x->mode : src_mode) & CHMOD_MODE_BITS;
  omitted_permissions =
    (dst_mode_bits
     & (x->preserve_ownership ? S_IRWXG | S_IRWXO
        : S_ISDIR (src_mode) ? S_IWGRP | S_IWOTH
        : 0));

  delayed_ok = true;
  if (! set_process_security_ctx (src_name, dst_name, src_mode, new_dst, x))
    return false;

  if (S_ISDIR (src_mode))
    {
      struct dir_list *dir;

      if (is_ancestor (&src_sb, ancestors))
        {
          error (0, 0, _("cannot copy cyclic symbolic link %s"),
                 quoteaf (src_name));
          goto un_backup;
        }

      dir = alloca (sizeof *dir);
      dir->parent = ancestors;
      dir->ino = src_sb.st_ino;
      dir->dev = src_sb.st_dev;

      if (new_dst || !S_ISDIR (dst_sb.st_mode))
        {
          if (mkdir (dst_name, dst_mode_bits & ~omitted_permissions) != 0)
            {
              error (0, errno, _("cannot create directory %s"),
                     quoteaf (dst_name));
              goto un_backup;
            }
          if (lstat (dst_name, &dst_sb) != 0)
            {
              error (0, errno, _("cannot stat %s"), quoteaf (dst_name));
              goto un_backup;
            }
          else if ((dst_sb.st_mode & S_IRWXU) != S_IRWXU)
            {

              dst_mode = dst_sb.st_mode;
              restore_dst_mode = true;

              if (lchmod (dst_name, dst_mode | S_IRWXU) != 0)
                {
                  error (0, errno, _("setting permissions for %s"),
                         quoteaf (dst_name));
                  goto un_backup;
                }
            }
          if (!*first_dir_created_per_command_line_arg)
            {
              remember_copied (dst_name, dst_sb.st_ino, dst_sb.st_dev);
              *first_dir_created_per_command_line_arg = true;
            }

          if (x->verbose)
            {
              if (x->move_mode)
                printf (_("created directory %s\n"), quoteaf (dst_name));
              else
                emit_verbose (src_name, dst_name, NULL);
            }
        }
      else
        {
          omitted_permissions = 0;
          if (x->set_security_context || x->preserve_security_context)
            if (! set_file_security_ctx (dst_name, false, x))
              {
                if (x->require_preserve_context)
                  goto un_backup;
              }
        }
      if (x->one_file_system && parent && parent->st_dev != src_sb.st_dev)
        {
        }
      else
        {
          delayed_ok = copy_dir (src_name, dst_name, new_dst, &src_sb, dir, x,
                                 first_dir_created_per_command_line_arg,
                                 copy_into_self);
        }
    }
  else if (x->symbolic_link)
    {
      dest_is_symlink = true;
      if (*src_name != '/')
        {
          struct stat dot_sb;
          struct stat dst_parent_sb;
          char *dst_parent;
          bool in_current_dir;

          dst_parent = dir_name (dst_name);

          in_current_dir = (STREQ (".", dst_parent)
                            || stat (".", &dot_sb) != 0
                            || stat (dst_parent, &dst_parent_sb) != 0
                            || SAME_INODE (dot_sb, dst_parent_sb));
          free (dst_parent);

          if (! in_current_dir)
            {
              error (0, 0,
           _("%s: can make relative symbolic links only in current directory"),
                     quotef (dst_name));
              goto un_backup;
            }
        }

      int err = force_symlinkat (src_name, AT_FDCWD, dst_name,
                                 x->unlink_dest_after_failed_open, -1);
      if (0 < err)
        {
          error (0, err, _("cannot create symbolic link %s to %s"),
                 quoteaf_n (0, dst_name), quoteaf_n (1, src_name));
          goto un_backup;
        }
    }
  else if (x->hard_link
           && !(! CAN_HARDLINK_SYMLINKS && S_ISLNK (src_mode)
                && x->dereference == DEREF_NEVER))
    {
      bool replace = (x->unlink_dest_after_failed_open
                      || x->interactive == I_ASK_USER);
      if (! create_hard_link (src_name, dst_name, replace, false, dereference))
        goto un_backup;
    }
  else if (S_ISREG (src_mode)
           || (x->copy_as_regular && !S_ISLNK (src_mode)))
    {
      copied_as_regular = true;
      if (! copy_reg (src_name, dst_name, x, dst_mode_bits & S_IRWXUGO,
                      omitted_permissions, &new_dst, &src_sb))
        goto un_backup;
    }
  else if (S_ISFIFO (src_mode))
    {
      if (mknod (dst_name, src_mode & ~omitted_permissions, 0) != 0)
        if (mkfifo (dst_name, src_mode & ~S_IFIFO & ~omitted_permissions) != 0)
          {
            error (0, errno, _("cannot create fifo %s"), quoteaf (dst_name));
            goto un_backup;
          }
    }
  else if (S_ISBLK (src_mode) || S_ISCHR (src_mode) || S_ISSOCK (src_mode))
    {
      if (mknod (dst_name, src_mode & ~omitted_permissions, src_sb.st_rdev)
          != 0)
        {
          error (0, errno, _("cannot create special file %s"),
                 quoteaf (dst_name));
          goto un_backup;
        }
    }
  else if (S_ISLNK (src_mode))
    {
      char *src_link_val = areadlink_with_size (src_name, src_sb.st_size);
      dest_is_symlink = true;
      if (src_link_val == NULL)
        {
          error (0, errno, _("cannot read symbolic link %s"),
                 quoteaf (src_name));
          goto un_backup;
        }

      int symlink_err = force_symlinkat (src_link_val, AT_FDCWD, dst_name,
                                         x->unlink_dest_after_failed_open, -1);
      if (0 < symlink_err && x->update && !new_dst && S_ISLNK (dst_sb.st_mode)
          && dst_sb.st_size == strlen (src_link_val))
        {
          char *dest_link_val =
            areadlink_with_size (dst_name, dst_sb.st_size);
          if (dest_link_val)
            {
              if (STREQ (dest_link_val, src_link_val))
                symlink_err = 0;
              free (dest_link_val);
            }
        }
      free (src_link_val);
      if (0 < symlink_err)
        {
          error (0, symlink_err, _("cannot create symbolic link %s"),
                 quoteaf (dst_name));
          goto un_backup;
        }

      if (x->preserve_security_context)
        restore_default_fscreatecon_or_die ();

      if (x->preserve_ownership)
        {
          if (HAVE_LCHOWN
              && lchown (dst_name, src_sb.st_uid, src_sb.st_gid) != 0
              && ! chown_failure_ok (x))
            {
              error (0, errno, _("failed to preserve ownership for %s"),
                     dst_name);
              if (x->require_preserve)
                goto un_backup;
            }
          else
            {
            }
        }
    }
  else
    {
      error (0, 0, _("%s has unknown file type"), quoteaf (src_name));
      goto un_backup;
    }
  if (!new_dst && !x->copy_as_regular && !S_ISDIR (src_mode)
      && (x->set_security_context || x->preserve_security_context))
    {
      if (! set_file_security_ctx (dst_name, false, x))
        {
           if (x->require_preserve_context)
             goto un_backup;
        }
    }

  if (command_line_arg && x->dest_info)
    {
      struct stat sb;
      if (lstat (dst_name, &sb) == 0)
        record_file (x->dest_info, dst_name, &sb);
    }
  if (x->hard_link && ! S_ISDIR (src_mode)
      && !(! CAN_HARDLINK_SYMLINKS && S_ISLNK (src_mode)
           && x->dereference == DEREF_NEVER))
    return delayed_ok;

  if (copied_as_regular)
    return delayed_ok;

  if (x->preserve_timestamps)
    {
      struct timespec timespec[2];
      timespec[0] = get_stat_atime (&src_sb);
      timespec[1] = get_stat_mtime (&src_sb);

      if ((dest_is_symlink
           ? utimens_symlink (dst_name, timespec)
           : utimens (dst_name, timespec))
          != 0)
        {
          error (0, errno, _("preserving times for %s"), quoteaf (dst_name));
          if (x->require_preserve)
            return false;
        }
    }

  if (!dest_is_symlink && x->preserve_ownership
      && (new_dst || !SAME_OWNER_AND_GROUP (src_sb, dst_sb)))
    {
      switch (set_owner (x, dst_name, -1, &src_sb, new_dst, &dst_sb))
        {
        case -1:
          return false;

        case 0:
          src_mode &= ~ (S_ISUID | S_ISGID | S_ISVTX);
          break;
        }
    }

  if (x->preserve_xattr && ! copy_attr (src_name, -1, dst_name, -1, x)
      && x->require_preserve_xattr)
    return false;
  if (dest_is_symlink)
    return delayed_ok;

  set_author (dst_name, -1, &src_sb);

  if (x->preserve_mode || x->move_mode)
    {
      if (copy_acl (src_name, -1, dst_name, -1, src_mode) != 0
          && x->require_preserve)
        return false;
    }
  else if (x->set_mode)
    {
      if (set_acl (dst_name, -1, x->mode) != 0)
        return false;
    }
  else if (x->explicit_no_preserve_mode && new_dst)
    {
      int default_permissions = S_ISDIR (src_mode) || S_ISSOCK (src_mode)
                                ? S_IRWXUGO : MODE_RW_UGO;
      if (set_acl (dst_name, -1, default_permissions & ~cached_umask ()) != 0)
        return false;
    }
  else
    {
      if (omitted_permissions)
        {
          omitted_permissions &= ~ cached_umask ();

          if (omitted_permissions && !restore_dst_mode)
            {
              if (new_dst && lstat (dst_name, &dst_sb) != 0)
                {
                  error (0, errno, _("cannot stat %s"), quoteaf (dst_name));
                  return false;
                }
              dst_mode = dst_sb.st_mode;
              if (omitted_permissions & ~dst_mode)
                restore_dst_mode = true;
            }
        }

      if (restore_dst_mode)
        {
          if (lchmod (dst_name, dst_mode | omitted_permissions) != 0)
            {
              error (0, errno, _("preserving permissions for %s"),
                     quoteaf (dst_name));
              if (x->require_preserve)
                return false;
            }
        }
    }

  return delayed_ok;

un_backup:

  if (x->preserve_security_context)
    restore_default_fscreatecon_or_die ();
  if (earlier_file == NULL)
    forget_created (src_sb.st_ino, src_sb.st_dev);

  if (dst_backup)
    {
      if (rename (dst_backup, dst_name) != 0)
        error (0, errno, _("cannot un-backup %s"), quoteaf (dst_name));
      else
        {
          if (x->verbose)
            printf (_("%s -> %s (unbackup)\n"),
                    quoteaf_n (0, dst_backup), quoteaf_n (1, dst_name));
        }
    }
  return false;
}

static bool _GL_ATTRIBUTE_PURE
valid_options (const struct cp_options *co)
{
  assert (co != NULL);
  assert (VALID_BACKUP_TYPE (co->backup_type));
  assert (VALID_SPARSE_MODE (co->sparse_mode));
  assert (VALID_REFLINK_MODE (co->reflink_mode));
  assert (!(co->hard_link && co->symbolic_link));
  assert (!
          (co->reflink_mode == REFLINK_ALWAYS
           && co->sparse_mode != SPARSE_AUTO));
  return true;
}

extern bool
copy (char const *src_name, char const *dst_name,
      bool nonexistent_dst, const struct cp_options *options,
      bool *copy_into_self, bool *rename_succeeded)
{
  assert (valid_options (options));
  top_level_src_name = src_name;
  top_level_dst_name = dst_name;

  bool first_dir_created_per_command_line_arg = false;
  return copy_internal (src_name, dst_name, nonexistent_dst, NULL, NULL,
                        options, true,
                        &first_dir_created_per_command_line_arg,
                        copy_into_self, rename_succeeded);
}


extern void
cp_options_default (struct cp_options *x)
{
  memset (x, 0, sizeof *x);
#ifdef PRIV_FILE_CHOWN
  {
    priv_set_t *pset = priv_allocset ();
    if (!pset)
      xalloc_die ();
    if (getppriv (PRIV_EFFECTIVE, pset) == 0)
      {
        x->chown_privileges = priv_ismember (pset, PRIV_FILE_CHOWN);
        x->owner_privileges = priv_ismember (pset, PRIV_FILE_OWNER);
      }
    priv_freeset (pset);
  }
#else
  x->chown_privileges = x->owner_privileges = (geteuid () == ROOT_UID);
#endif
  x->rename_errno = -1;
}


extern bool
chown_failure_ok (struct cp_options const *x)
{

  return ((errno == EPERM || errno == EINVAL) && !x->chown_privileges);
}

static bool
owner_failure_ok (struct cp_options const *x)
{
  return ((errno == EPERM || errno == EINVAL) && !x->owner_privileges);
}
extern mode_t
cached_umask (void)
{
  static mode_t mask = (mode_t) -1;
  if (mask == (mode_t) -1)
    {
      mask = umask (0);
      umask (mask);
    }
  return mask;
}
