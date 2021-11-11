#ifndef COREUTILS_SELINUX_H
# define COREUTILS_SELINUX_H
struct selabel_handle;
static inline bool
ignorable_ctx_err (int err)
{
  return err == ENOTSUP || err == ENODATA;
}

# if HAVE_SELINUX_LABEL_H

extern bool
restorecon (struct selabel_handle *selabel_handle,
            char const *path, bool recurse);
extern int
defaultcon (struct selabel_handle *selabel_handle,
            char const *path, mode_t mode);

# else

static inline bool
restorecon (struct selabel_handle *selabel_handle,
            char const *path, bool recurse)
{ errno = ENOTSUP; return false; }

static inline int
defaultcon (struct selabel_handle *selabel_handle,
            char const *path, mode_t mode)
{ errno = ENOTSUP; return -1; }

# endif

#endif
