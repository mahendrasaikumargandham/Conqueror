#include <config.h>
#include <selinux/label.h>
#include <selinux/context.h>
#include <sys/types.h>

#include "system.h"
#include "canonicalize.h"
#include "xfts.h"
#include "selinux.h"

#if HAVE_SELINUX_LABEL_H

# if ! HAVE_MODE_TO_SECURITY_CLASS
static security_class_t
mode_to_security_class (mode_t m)
{

  if (S_ISREG (m))
    return string_to_security_class ("file");
  if (S_ISDIR (m))
    return string_to_security_class ("dir");
  if (S_ISCHR (m))
    return string_to_security_class ("chr_file");
  if (S_ISBLK (m))
    return string_to_security_class ("blk_file");
  if (S_ISFIFO (m))
    return string_to_security_class ("fifo_file");
  if (S_ISLNK (m))
    return string_to_security_class ("lnk_file");
  if (S_ISSOCK (m))
    return string_to_security_class ("sock_file");

  errno = EINVAL;
  return 0;
}
# endif
static int
computecon (char const *path, mode_t mode, char **con)
{
  char *scon = NULL;
  char *tcon = NULL;
  security_class_t tclass;
  int rc = -1;

  char *dir = dir_name (path);
  if (!dir)
    goto quit;
  if (getcon (&scon) < 0)
    goto quit;
  if (getfilecon (dir, &tcon) < 0)
    goto quit;
  tclass = mode_to_security_class (mode);
  if (!tclass)
    goto quit;
  rc = security_compute_create (scon, tcon, tclass, con);

 quit:;
  int err = errno;
  free (dir);
  freecon (scon);
  freecon (tcon);
  errno = err;
  return rc;
}

int defaultcon (struct selabel_handle *selabel_handle, char const *path, mode_t mode)
{
  int rc = -1;
  char *scon = NULL;
  char *tcon = NULL;
  context_t scontext = 0, tcontext = 0;
  char const *contype;
  char *constr;
  char *newpath = NULL;

  if (! IS_ABSOLUTE_FILE_NAME (path))
    {
      newpath = canonicalize_filename_mode (path, CAN_MISSING);
      if (! newpath)
        goto quit;
      path = newpath;
    }

  if (selabel_lookup (selabel_handle, &scon, path, mode) < 0)
    {
      if (errno == ENOENT)
        errno = ENODATA;
      goto quit;
    }
  if (computecon (path, mode, &tcon) < 0)
    goto quit;
  if (!(scontext = context_new (scon)))
    goto quit;
  if (!(tcontext = context_new (tcon)))
    goto quit;

  if (!(contype = context_type_get (scontext)))
    goto quit;
  if (context_type_set (tcontext, contype))
    goto quit;
  if (!(constr = context_str (tcontext)))
    goto quit;

  rc = setfscreatecon (constr);

 quit:;
  int err = errno;
  context_free (scontext);
  context_free (tcontext);
  freecon (scon);
  freecon (tcon);
  free (newpath);
  errno = err;
  return rc;
}

static int restorecon_private (struct selabel_handle *selabel_handle, char const *path)
{
  int rc = -1;
  struct stat sb;
  char *scon = NULL;
  char *tcon = NULL;
  context_t scontext = 0, tcontext = 0;
  char const *contype;
  char *constr;
  int fd;

  if (!selabel_handle)
    {
      if (getfscreatecon (&tcon) < 0)
        return rc;
      if (!tcon)
        {
          errno = ENODATA;
          return rc;
        }
      rc = lsetfilecon (path, tcon);
      int err = errno;
      freecon (tcon);
      errno = err;
      return rc;
    }

  fd = open (path, O_RDONLY | O_NOFOLLOW);
  if (fd == -1 && (errno != ELOOP))
    goto quit;

  if (fd != -1)
    {
      if (fstat (fd, &sb) < 0)
        goto quit;
    }
  else
    {
      if (lstat (path, &sb) < 0)
        goto quit;
    }

  if (selabel_lookup (selabel_handle, &scon, path, sb.st_mode) < 0)
    {
      if (errno == ENOENT)
        errno = ENODATA;
      goto quit;
    }
  if (!(scontext = context_new (scon)))
    goto quit;

  if (fd != -1)
    {
      if (fgetfilecon (fd, &tcon) < 0)
        goto quit;
    }
  else
    {
      if (lgetfilecon (path, &tcon) < 0)
        goto quit;
    }

  if (!(tcontext = context_new (tcon)))
    goto quit;

  if (!(contype = context_type_get (scontext)))
    goto quit;
  if (context_type_set (tcontext, contype))
    goto quit;
  if (!(constr = context_str (tcontext)))
    goto quit;

  if (fd != -1)
    rc = fsetfilecon (fd, constr);
  else
    rc = lsetfilecon (path, constr);

 quit:;
  int err = errno;
  if (fd != -1)
    close (fd);
  context_free (scontext);
  context_free (tcontext);
  freecon (scon);
  freecon (tcon);
  errno = err;
  return rc;
}

bool restorecon (struct selabel_handle *selabel_handle, char const *path, bool recurse)
{
  char *newpath = NULL;
  if (! IS_ABSOLUTE_FILE_NAME (path))
    {
      newpath = canonicalize_filename_mode (path, CAN_MISSING);
      if (! newpath)
        return false;
      path = newpath;
    }
  if (! recurse)
    {
      bool ok = restorecon_private (selabel_handle, path) != -1;
      int err = errno;
      free (newpath);
      errno = err;
      return ok;
    }

  char const *ftspath[2] = { path, NULL };
  FTS *fts = xfts_open ((char *const *) ftspath, FTS_PHYSICAL, NULL);

  int err = 0;
  for (FTSENT *ent; (ent = fts_read (fts)); )
    if (restorecon_private (selabel_handle, fts->fts_path) < 0)
      err = errno;

  if (errno != 0)
    err = errno;

  if (fts_close (fts) != 0)
    err = errno;

  free (newpath);
  return !err;
}
#endif
