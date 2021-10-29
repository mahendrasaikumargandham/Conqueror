#include <config.h>
#include <stdio.h>
#include <sys/types.h>
#include <getopt.h>
#include <selinux/label.h>
#include "system.h"
#include "argmatch.h"
#include "backupfile.h"
#include "copy.h"
#include "cp-hash.h"
#include "die.h"
#include "error.h"
#include "filenamecat.h"
#include "ignore-value.h"
#include "quote.h"
#include "stat-time.h"
#include "utimens.h"
#include "acl.h"

#if ! HAVE_LCHOWN
# define lchown(name, uid, gid) chown (name, uid, gid)
#endif
#define PROGRAM_NAME "cp"
struct dir_attr
{
  struct stat st;
  bool restore_mode;
  size_t slash_offset;
  struct dir_attr *next;
};
enum
{
  ATTRIBUTES_ONLY_OPTION = CHAR_MAX + 1,
  COPY_CONTENTS_OPTION,
  NO_PRESERVE_ATTRIBUTES_OPTION,
  PARENTS_OPTION,
  PRESERVE_ATTRIBUTES_OPTION,
  REFLINK_OPTION,
  SPARSE_OPTION,
  STRIP_TRAILING_SLASHES_OPTION,
  UNLINK_DEST_BEFORE_OPENING
};
static bool selinux_enabled;
static bool parents_option = false;
static bool remove_trailing_slashes;

static char const *const sparse_type_string[] =
{
  "never", "auto", "always", NULL
};
static enum Sparse_type const sparse_type[] =
{
  SPARSE_NEVER, SPARSE_AUTO, SPARSE_ALWAYS
};
ARGMATCH_VERIFY (sparse_type_string, sparse_type);

static char const *const reflink_type_string[] =
{
  "auto", "always", "never", NULL
};
static enum Reflink_type const reflink_type[] =
{
  REFLINK_AUTO, REFLINK_ALWAYS, REFLINK_NEVER
};
ARGMATCH_VERIFY (reflink_type_string, reflink_type);

static struct option const long_opts[] =
{
  {"archive", no_argument, NULL, 'a'},
  {"attributes-only", no_argument, NULL, ATTRIBUTES_ONLY_OPTION},
  {"backup", optional_argument, NULL, 'b'},
  {"copy-contents", no_argument, NULL, COPY_CONTENTS_OPTION},
  {"dereference", no_argument, NULL, 'L'},
  {"force", no_argument, NULL, 'f'},
  {"interactive", no_argument, NULL, 'i'},
  {"link", no_argument, NULL, 'l'},
  {"no-clobber", no_argument, NULL, 'n'},
  {"no-dereference", no_argument, NULL, 'P'},
  {"no-preserve", required_argument, NULL, NO_PRESERVE_ATTRIBUTES_OPTION},
  {"no-target-directory", no_argument, NULL, 'T'},
  {"one-file-system", no_argument, NULL, 'x'},
  {"parents", no_argument, NULL, PARENTS_OPTION},
  {"path", no_argument, NULL, PARENTS_OPTION},   /* Deprecated.  */
  {"preserve", optional_argument, NULL, PRESERVE_ATTRIBUTES_OPTION},
  {"recursive", no_argument, NULL, 'R'},
  {"remove-destination", no_argument, NULL, UNLINK_DEST_BEFORE_OPENING},
  {"sparse", required_argument, NULL, SPARSE_OPTION},
  {"reflink", optional_argument, NULL, REFLINK_OPTION},
  {"strip-trailing-slashes", no_argument, NULL, STRIP_TRAILING_SLASHES_OPTION},
  {"suffix", required_argument, NULL, 'S'},
  {"symbolic-link", no_argument, NULL, 's'},
  {"target-directory", required_argument, NULL, 't'},
  {"update", no_argument, NULL, 'u'},
  {"verbose", no_argument, NULL, 'v'},
  {GETOPT_SELINUX_CONTEXT_OPTION_DECL},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0}
};

void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s [OPTION]... [-T] SOURCE DEST\n\
  or:  %s [OPTION]... SOURCE... DIRECTORY\n\
  or:  %s [OPTION]... -t DIRECTORY SOURCE...\n\
"),
              program_name, program_name, program_name);
      fputs (_("\
Copy SOURCE to DEST, or multiple SOURCE(s) to DIRECTORY.\n\
"), stdout);

      emit_mandatory_arg_note ();

      fputs (_("\
  -a, --archive                same as -dR --preserve=all\n\
      --attributes-only        don't copy the file data, just the attributes\n\
      --backup[=CONTROL]       make a backup of each existing destination file\
\n\
  -b                           like --backup but does not accept an argument\n\
      --copy-contents          copy contents of special files when recursive\n\
  -d                           same as --no-dereference --preserve=links\n\
"), stdout);
      fputs (_("\
  -f, --force                  if an existing destination file cannot be\n\
                                 opened, remove it and try again (this option\n\
                                 is ignored when the -n option is also used)\n\
  -i, --interactive            prompt before overwrite (overrides a previous -n\
\n\
                                  option)\n\
  -H                           follow command-line symbolic links in SOURCE\n\
"), stdout);
      fputs (_("\
  -l, --link                   hard link files instead of copying\n\
  -L, --dereference            always follow symbolic links in SOURCE\n\
"), stdout);
      fputs (_("\
  -n, --no-clobber             do not overwrite an existing file (overrides\n\
                                 a previous -i option)\n\
  -P, --no-dereference         never follow symbolic links in SOURCE\n\
"), stdout);
      fputs (_("\
  -p                           same as --preserve=mode,ownership,timestamps\n\
      --preserve[=ATTR_LIST]   preserve the specified attributes (default:\n\
                                 mode,ownership,timestamps), if possible\n\
                                 additional attributes: context, links, xattr,\
\n\
                                 all\n\
"), stdout);
      fputs (_("\
      --no-preserve=ATTR_LIST  don't preserve the specified attributes\n\
      --parents                use full source file name under DIRECTORY\n\
"), stdout);
      fputs (_("\
  -R, -r, --recursive          copy directories recursively\n\
      --reflink[=WHEN]         control clone/CoW copies. See below\n\
      --remove-destination     remove each existing destination file before\n\
                                 attempting to open it (contrast with --force)\
\n"), stdout);
      fputs (_("\
      --sparse=WHEN            control creation of sparse files. See below\n\
      --strip-trailing-slashes  remove any trailing slashes from each SOURCE\n\
                                 argument\n\
"), stdout);
      fputs (_("\
  -s, --symbolic-link          make symbolic links instead of copying\n\
  -S, --suffix=SUFFIX          override the usual backup suffix\n\
  -t, --target-directory=DIRECTORY  copy all SOURCE arguments into DIRECTORY\n\
  -T, --no-target-directory    treat DEST as a normal file\n\
"), stdout);
      fputs (_("\
  -u, --update                 copy only when the SOURCE file is newer\n\
                                 than the destination file or when the\n\
                                 destination file is missing\n\
  -v, --verbose                explain what is being done\n\
  -x, --one-file-system        stay on this file system\n\
"), stdout);
      fputs (_("\
  -Z                           set SELinux security context of destination\n\
                                 file to default type\n\
      --context[=CTX]          like -Z, or if CTX is specified then set the\n\
                                 SELinux or SMACK security context to CTX\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      fputs (_("\
\n\
By default, sparse SOURCE files are detected by a crude heuristic and the\n\
corresponding DEST file is made sparse as well.  That is the behavior\n\
selected by --sparse=auto.  Specify --sparse=always to create a sparse DEST\n\
file whenever the SOURCE file contains a long enough sequence of zero bytes.\n\
Use --sparse=never to inhibit creation of sparse files.\n\
"), stdout);
      fputs (_("\
\n\
When --reflink[=always] is specified, perform a lightweight copy, where the\n\
data blocks are copied only when modified.  If this is not possible the copy\n\
fails, or if --reflink=auto is specified, fall back to a standard copy.\n\
Use --reflink=never to ensure a standard copy is performed.\n\
"), stdout);
      emit_backup_suffix_note ();
      fputs (_("\
\n\
As a special case, cp makes a backup of SOURCE when the force and backup\n\
options are given and SOURCE and DEST are the same name for an existing,\n\
regular file.\n\
"), stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

static bool
re_protect (char const *const_dst_name, size_t src_offset,
            struct dir_attr *attr_list, const struct cp_options *x)
{
  struct dir_attr *p;
  char *dst_name;		
  char *src_name;		

  ASSIGN_STRDUPA (dst_name, const_dst_name);
  src_name = dst_name + src_offset;

  for (p = attr_list; p; p = p->next)
    {
      dst_name[p->slash_offset] = '\0';

      if (x->preserve_timestamps)
        {
          struct timespec timespec[2];

          timespec[0] = get_stat_atime (&p->st);
          timespec[1] = get_stat_mtime (&p->st);

          if (utimens (dst_name, timespec))
            {
              error (0, errno, _("failed to preserve times for %s"),
                     quoteaf (dst_name));
              return false;
            }
        }

      if (x->preserve_ownership)
        {
          if (lchown (dst_name, p->st.st_uid, p->st.st_gid) != 0)
            {
              if (! chown_failure_ok (x))
                {
                  error (0, errno, _("failed to preserve ownership for %s"),
                         quoteaf (dst_name));
                  return false;
                }
              ignore_value (lchown (dst_name, -1, p->st.st_gid));
            }
        }

      if (x->preserve_mode)
        {
          if (copy_acl (src_name, -1, dst_name, -1, p->st.st_mode) != 0)
            return false;
        }
      else if (p->restore_mode)
        {
          if (lchmod (dst_name, p->st.st_mode) != 0)
            {
              error (0, errno, _("failed to preserve permissions for %s"),
                     quoteaf (dst_name));
              return false;
            }
        }

      dst_name[p->slash_offset] = '/';
    }
  return true;
}

static bool
make_dir_parents_private (char const *const_dir, size_t src_offset,
                          char const *verbose_fmt_string,
                          struct dir_attr **attr_list, bool *new_dst,
                          const struct cp_options *x)
{
  struct stat stats;
  char *dir;		
  char *src;		
  char *dst_dir;	
  size_t dirlen;	

  ASSIGN_STRDUPA (dir, const_dir);

  src = dir + src_offset;

  dirlen = dir_len (dir);
  dst_dir = alloca (dirlen + 1);
  memcpy (dst_dir, dir, dirlen);
  dst_dir[dirlen] = '\0';

  *attr_list = NULL;
  if (stat (dst_dir, &stats) != 0)
    {
      char *slash;

      slash = src;
      while (*slash == '/')
        slash++;
      while ((slash = strchr (slash, '/')))
        {
          struct dir_attr *new IF_LINT ( = NULL);
          bool missing_dir;

          *slash = '\0';
          missing_dir = (stat (dir, &stats) != 0);

          if (missing_dir || x->preserve_ownership || x->preserve_mode
              || x->preserve_timestamps)
            {
              struct stat src_st;
              int src_errno = (stat (src, &src_st) != 0
                               ? errno
                               : S_ISDIR (src_st.st_mode)
                               ? 0
                               : ENOTDIR);
              if (src_errno)
                {
                  error (0, src_errno, _("failed to get attributes of %s"),
                         quoteaf (src));
                  return false;
                }

              new = xmalloc (sizeof *new);
              new->st = src_st;
              new->slash_offset = slash - dir;
              new->restore_mode = false;
              new->next = *attr_list;
              *attr_list = new;
            }
          if (! set_process_security_ctx (src, dir,
                                          missing_dir ? new->st.st_mode : 0,
                                          missing_dir, x))
            return false;

          if (missing_dir)
            {
              mode_t src_mode;
              mode_t omitted_permissions;
              mode_t mkdir_mode;
              *new_dst = true;
              src_mode = new->st.st_mode;
              omitted_permissions = (src_mode
                                     & (x->preserve_ownership
                                        ? S_IRWXG | S_IRWXO
                                        : x->preserve_mode
                                        ? S_IWGRP | S_IWOTH
                                        : 0));
              mkdir_mode = x->explicit_no_preserve_mode ? S_IRWXUGO : src_mode;
              mkdir_mode &= CHMOD_MODE_BITS & ~omitted_permissions;
              if (mkdir (dir, mkdir_mode) != 0)
                {
                  error (0, errno, _("cannot make directory %s"),
                         quoteaf (dir));
                  return false;
                }
              else
                {
                  if (verbose_fmt_string != NULL)
                    printf (verbose_fmt_string, src, dir);
                }

              if (lstat (dir, &stats))
                {
                  error (0, errno, _("failed to get attributes of %s"),
                         quoteaf (dir));
                  return false;
                }


              if (! x->preserve_mode)
                {
                  if (omitted_permissions & ~stats.st_mode)
                    omitted_permissions &= ~ cached_umask ();
                  if (omitted_permissions & ~stats.st_mode
                      || (stats.st_mode & S_IRWXU) != S_IRWXU)
                    {
                      new->st.st_mode = stats.st_mode | omitted_permissions;
                      new->restore_mode = true;
                    }
                }

              if ((stats.st_mode & S_IRWXU) != S_IRWXU)
                {

                  if (lchmod (dir, stats.st_mode | S_IRWXU) != 0)
                    {
                      error (0, errno, _("setting permissions for %s"),
                             quoteaf (dir));
                      return false;
                    }
                }
            }
          else if (!S_ISDIR (stats.st_mode))
            {
              error (0, 0, _("%s exists but is not a directory"),
                     quoteaf (dir));
              return false;
            }
          else
            *new_dst = false;
          if (! *new_dst
              && (x->set_security_context || x->preserve_security_context))
            {
              if (! set_file_security_ctx (dir, false, x)
                  && x->require_preserve_context)
                return false;
            }

          *slash++ = '/';

          while (*slash == '/')
            slash++;
        }
    }

  else if (!S_ISDIR (stats.st_mode))
    {
      error (0, 0, _("%s exists but is not a directory"), quoteaf (dst_dir));
      return false;
    }
  else
    {
      *new_dst = false;
    }
  return true;
}

static bool
target_directory_operand (char const *file, struct stat *st,
                          bool *new_dst, bool forcing)
{
  int err = (stat (file, st) == 0 ? 0 : errno);
  bool is_a_dir = !err && S_ISDIR (st->st_mode);
  if (err)
    {
      if (err == ENOENT)
        *new_dst = true;
      else if (forcing)
        st->st_mode = 0;  
      else
        die (EXIT_FAILURE, err, _("failed to access %s"), quoteaf (file));
    }
  return is_a_dir;
}

static bool
do_copy (int n_files, char **file, char const *target_directory,
         bool no_target_directory, struct cp_options *x)
{
  struct stat sb;
  bool new_dst = false;
  bool ok = true;
  bool forcing = x->unlink_dest_before_opening
                 || x->unlink_dest_after_failed_open;

  if (n_files <= !target_directory)
    {
      if (n_files <= 0)
        error (0, 0, _("missing file operand"));
      else
        error (0, 0, _("missing destination file operand after %s"),
               quoteaf (file[0]));
      usage (EXIT_FAILURE);
    }

  if (no_target_directory)
    {
      if (target_directory)
        die (EXIT_FAILURE, 0,
             _("cannot combine --target-directory (-t) "
               "and --no-target-directory (-T)"));
      if (2 < n_files)
        {
          error (0, 0, _("extra operand %s"), quoteaf (file[2]));
          usage (EXIT_FAILURE);
        }
      /* Update NEW_DST and SB, which may be checked below.  */
      ignore_value (target_directory_operand (file[n_files -1], &sb, &new_dst,
                                              forcing));
    }
  else if (!target_directory)
    {
      if (2 <= n_files
          && target_directory_operand (file[n_files - 1], &sb, &new_dst,
                                       forcing))
        target_directory = file[--n_files];
      else if (2 < n_files)
        die (EXIT_FAILURE, 0, _("target %s is not a directory"),
             quoteaf (file[n_files - 1]));
    }

  if (target_directory)
    {
      if (2 <= n_files)
        {
          dest_info_init (x);
          src_info_init (x);
        }

      for (int i = 0; i < n_files; i++)
        {
          char *dst_name;
          bool parent_exists = true; 
          struct dir_attr *attr_list;
          char *arg_in_concat = NULL;
          char *arg = file[i];

          if (remove_trailing_slashes)
            strip_trailing_slashes (arg);

          if (parents_option)
            {
              char *arg_no_trailing_slash;
              ASSIGN_STRDUPA (arg_no_trailing_slash, arg);
              strip_trailing_slashes (arg_no_trailing_slash);
              dst_name = file_name_concat (target_directory,
                                           arg_no_trailing_slash,
                                           &arg_in_concat);
              parent_exists =
                (make_dir_parents_private
                 (dst_name, arg_in_concat - dst_name,
                  (x->verbose ? "%s -> %s\n" : NULL),
                  &attr_list, &new_dst, x));
            }
          else
            {
              char *arg_base;
              ASSIGN_STRDUPA (arg_base, last_component (arg));
              strip_trailing_slashes (arg_base);
              dst_name = (STREQ (arg_base, "..")
                          ? xstrdup (target_directory)
                          : file_name_concat (target_directory, arg_base,
                                              NULL));
            }

          if (!parent_exists)
            {
              ok = false;
            }
          else
            {
              bool copy_into_self;
              ok &= copy (arg, dst_name, new_dst, x, &copy_into_self, NULL);

              if (parents_option)
                ok &= re_protect (dst_name, arg_in_concat - dst_name,
                                  attr_list, x);
            }

          if (parents_option)
            {
              while (attr_list)
                {
                  struct dir_attr *p = attr_list;
                  attr_list = attr_list->next;
                  free (p);
                }
            }

          free (dst_name);
        }
    }
  else 
    {
      char const *new_dest;
      char const *source = file[0];
      char const *dest = file[1];
      bool unused;

      if (parents_option)
        {
          error (0, 0,
                 _("with --parents, the destination must be a directory"));
          usage (EXIT_FAILURE);
        }

      if (x->unlink_dest_after_failed_open
          && x->backup_type != no_backups
          && STREQ (source, dest)
          && !new_dst && S_ISREG (sb.st_mode))
        {
          static struct cp_options x_tmp;

          new_dest = find_backup_file_name (AT_FDCWD, dest, x->backup_type);
          x_tmp = *x;
          x_tmp.backup_type = no_backups;
          x = &x_tmp;
        }
      else
        {
          new_dest = dest;
        }

      ok = copy (source, new_dest, 0, x, &unused, NULL);
    }

  return ok;
}

static void
cp_option_init (struct cp_options *x)
{
  cp_options_default (x);
  x->copy_as_regular = true;
  x->dereference = DEREF_UNDEFINED;
  x->unlink_dest_before_opening = false;
  x->unlink_dest_after_failed_open = false;
  x->hard_link = false;
  x->interactive = I_UNSPECIFIED;
  x->move_mode = false;
  x->install_mode = false;
  x->one_file_system = false;
  x->reflink_mode = REFLINK_AUTO;

  x->preserve_ownership = false;
  x->preserve_links = false;
  x->preserve_mode = false;
  x->preserve_timestamps = false;
  x->explicit_no_preserve_mode = false;
  x->preserve_security_context = false; 
  x->require_preserve_context = false;  
  x->set_security_context = NULL;       
  x->preserve_xattr = false;
  x->reduce_diagnostics = false;
  x->require_preserve_xattr = false;

  x->data_copy_required = true;
  x->require_preserve = false;
  x->recursive = false;
  x->sparse_mode = SPARSE_AUTO;
  x->symbolic_link = false;
  x->set_mode = false;
  x->mode = 0;

  /* Not used.  */
  x->stdin_tty = false;

  x->update = false;
  x->verbose = false;
  x->open_dangling_dest_symlink = getenv ("POSIXLY_CORRECT") != NULL;

  x->dest_info = NULL;
  x->src_info = NULL;
}
static void
decode_preserve_arg (char const *arg, struct cp_options *x, bool on_off)
{
  enum File_attribute
    {
      PRESERVE_MODE,
      PRESERVE_TIMESTAMPS,
      PRESERVE_OWNERSHIP,
      PRESERVE_LINK,
      PRESERVE_CONTEXT,
      PRESERVE_XATTR,
      PRESERVE_ALL
    };
  static enum File_attribute const preserve_vals[] =
    {
      PRESERVE_MODE, PRESERVE_TIMESTAMPS,
      PRESERVE_OWNERSHIP, PRESERVE_LINK, PRESERVE_CONTEXT, PRESERVE_XATTR,
      PRESERVE_ALL
    };
  static char const* const preserve_args[] =
    {
      "mode", "timestamps",
      "ownership", "links", "context", "xattr", "all", NULL
    };
  ARGMATCH_VERIFY (preserve_args, preserve_vals);

  char *arg_writable = xstrdup (arg);
  char *s = arg_writable;
  do
    {
      char *comma = strchr (s, ',');
      enum File_attribute val;
      if (comma)
        *comma++ = 0;
      val = XARGMATCH (on_off ? "--preserve" : "--no-preserve",
                       s, preserve_args, preserve_vals);
      switch (val)
        {
        case PRESERVE_MODE:
          x->preserve_mode = on_off;
          x->explicit_no_preserve_mode = !on_off;
          break;

        case PRESERVE_TIMESTAMPS:
          x->preserve_timestamps = on_off;
          break;

        case PRESERVE_OWNERSHIP:
          x->preserve_ownership = on_off;
          break;

        case PRESERVE_LINK:
          x->preserve_links = on_off;
          break;

        case PRESERVE_CONTEXT:
          x->require_preserve_context = on_off;
          x->preserve_security_context = on_off;
          break;

        case PRESERVE_XATTR:
          x->preserve_xattr = on_off;
          x->require_preserve_xattr = on_off;
          break;

        case PRESERVE_ALL:
          x->preserve_mode = on_off;
          x->preserve_timestamps = on_off;
          x->preserve_ownership = on_off;
          x->preserve_links = on_off;
          x->explicit_no_preserve_mode = !on_off;
          if (selinux_enabled)
            x->preserve_security_context = on_off;
          x->preserve_xattr = on_off;
          break;

        default:
          abort ();
        }
      s = comma;
    }
  while (s);

  free (arg_writable);
}

int
main (int argc, char **argv)
{
  int c;
  bool ok;
  bool make_backups = false;
  char const *backup_suffix = NULL;
  char *version_control_string = NULL;
  struct cp_options x;
  bool copy_contents = false;
  char *target_directory = NULL;
  bool no_target_directory = false;
  char const *scontext = NULL;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdin);

  selinux_enabled = (0 < is_selinux_enabled ());
  cp_option_init (&x);

  while ((c = getopt_long (argc, argv, "abdfHilLnprst:uvxPRS:TZ",
                           long_opts, NULL))
         != -1)
    {
      switch (c)
        {
        case SPARSE_OPTION:
          x.sparse_mode = XARGMATCH ("--sparse", optarg,
                                     sparse_type_string, sparse_type);
          break;

        case REFLINK_OPTION:
          if (optarg == NULL)
            x.reflink_mode = REFLINK_ALWAYS;
          else
            x.reflink_mode = XARGMATCH ("--reflink", optarg,
                                       reflink_type_string, reflink_type);
          break;

        case 'a':
          x.dereference = DEREF_NEVER;
          x.preserve_links = true;
          x.preserve_ownership = true;
          x.preserve_mode = true;
          x.preserve_timestamps = true;
          x.require_preserve = true;
          if (selinux_enabled)
             x.preserve_security_context = true;
          x.preserve_xattr = true;
          x.reduce_diagnostics = true;
          x.recursive = true;
          break;

        case 'b':
          make_backups = true;
          if (optarg)
            version_control_string = optarg;
          break;

        case ATTRIBUTES_ONLY_OPTION:
          x.data_copy_required = false;
          break;

        case COPY_CONTENTS_OPTION:
          copy_contents = true;
          break;

        case 'd':
          x.preserve_links = true;
          x.dereference = DEREF_NEVER;
          break;

        case 'f':
          x.unlink_dest_after_failed_open = true;
          break;

        case 'H':
          x.dereference = DEREF_COMMAND_LINE_ARGUMENTS;
          break;

        case 'i':
          x.interactive = I_ASK_USER;
          break;

        case 'l':
          x.hard_link = true;
          break;

        case 'L':
          x.dereference = DEREF_ALWAYS;
          break;

        case 'n':
          x.interactive = I_ALWAYS_NO;
          break;

        case 'P':
          x.dereference = DEREF_NEVER;
          break;

        case NO_PRESERVE_ATTRIBUTES_OPTION:
          decode_preserve_arg (optarg, &x, false);
          break;

        case PRESERVE_ATTRIBUTES_OPTION:
          if (optarg == NULL)
            {
            }
          else
            {
              decode_preserve_arg (optarg, &x, true);
              x.require_preserve = true;
              break;
            }
          FALLTHROUGH;

        case 'p':
          x.preserve_ownership = true;
          x.preserve_mode = true;
          x.preserve_timestamps = true;
          x.require_preserve = true;
          break;

        case PARENTS_OPTION:
          parents_option = true;
          break;

        case 'r':
        case 'R':
          x.recursive = true;
          break;

        case UNLINK_DEST_BEFORE_OPENING:
          x.unlink_dest_before_opening = true;
          break;

        case STRIP_TRAILING_SLASHES_OPTION:
          remove_trailing_slashes = true;
          break;

        case 's':
          x.symbolic_link = true;
          break;

        case 't':
          if (target_directory)
            die (EXIT_FAILURE, 0,
                 _("multiple target directories specified"));
          else
            {
              struct stat st;
              if (stat (optarg, &st) != 0)
                die (EXIT_FAILURE, errno, _("failed to access %s"),
                     quoteaf (optarg));
              if (! S_ISDIR (st.st_mode))
                die (EXIT_FAILURE, 0, _("target %s is not a directory"),
                     quoteaf (optarg));
            }
          target_directory = optarg;
          break;

        case 'T':
          no_target_directory = true;
          break;

        case 'u':
          x.update = true;
          break;

        case 'v':
          x.verbose = true;
          break;

        case 'x':
          x.one_file_system = true;
          break;

        case 'Z':
          if (selinux_enabled)
            {
              if (optarg)
                scontext = optarg;
              else
                {
                  x.set_security_context = selabel_open (SELABEL_CTX_FILE,
                                                         NULL, 0);
                  if (! x.set_security_context)
                    error (0, errno, _("warning: ignoring --context"));
                }
            }
          else if (optarg)
            {
              error (0, 0,
                     _("warning: ignoring --context; "
                       "it requires an SELinux-enabled kernel"));
            }
          break;

        case 'S':
          make_backups = true;
          backup_suffix = optarg;
          break;

        case_GETOPT_HELP_CHAR;

        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          usage (EXIT_FAILURE);
        }
    }

  if (x.hard_link && x.symbolic_link)
    {
      error (0, 0, _("cannot make both hard and symbolic links"));
      usage (EXIT_FAILURE);
    }

  if (x.interactive == I_ALWAYS_NO)
    x.update = false;

  if (make_backups && x.interactive == I_ALWAYS_NO)
    {
      error (0, 0,
             _("options --backup and --no-clobber are mutually exclusive"));
      usage (EXIT_FAILURE);
    }

  if (x.reflink_mode == REFLINK_ALWAYS && x.sparse_mode != SPARSE_AUTO)
    {
      error (0, 0, _("--reflink can be used only with --sparse=auto"));
      usage (EXIT_FAILURE);
    }

  x.backup_type = (make_backups
                   ? xget_version (_("backup type"),
                                   version_control_string)
                   : no_backups);
  set_simple_backup_suffix (backup_suffix);

  if (x.dereference == DEREF_UNDEFINED)
    {
      if (x.recursive && ! x.hard_link)
        x.dereference = DEREF_NEVER;
      else
        x.dereference = DEREF_ALWAYS;
    }

  if (x.recursive)
    x.copy_as_regular = copy_contents;
  if ((x.set_security_context || scontext)
      && ! x.require_preserve_context)
    x.preserve_security_context = false;

  if (x.preserve_security_context && (x.set_security_context || scontext))
    die (EXIT_FAILURE, 0,
         _("cannot set target context and preserve it"));

  if (x.require_preserve_context && ! selinux_enabled)
    die (EXIT_FAILURE, 0,
         _("cannot preserve security context "
           "without an SELinux-enabled kernel"));
  if (scontext && setfscreatecon (scontext) < 0)
    die (EXIT_FAILURE, errno,
         _("failed to set default file creation context to %s"),
         quote (scontext));

#if !USE_XATTR
  if (x.require_preserve_xattr)
    die (EXIT_FAILURE, 0, _("cannot preserve extended attributes, cp is "
                            "built without xattr support"));
#endif
  hash_init ();
  ok = do_copy (argc - optind, argv + optind,
                target_directory, no_target_directory, &x);
#ifdef lint
  forget_all ();
#endif
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
