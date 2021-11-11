#include <config.h>
#include <stdio.h>
#include <getopt.h>
#include <sys/types.h>

#include "system.h"
#include "error.h"
#include "prog-fprintf.h"
static bool remove_empty_parents;
static bool ignore_fail_on_non_empty;

static bool verbose;
enum
{
  IGNORE_FAIL_ON_NON_EMPTY_OPTION = CHAR_MAX + 1
};

static struct option const longopts[] =
{
  {"ignore-fail-on-non-empty", no_argument, NULL,
   IGNORE_FAIL_ON_NON_EMPTY_OPTION},

  {"path", no_argument, NULL, 'p'},  /* Deprecated.  */
  {"parents", no_argument, NULL, 'p'},
  {"verbose", no_argument, NULL, 'v'},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0}
};
static bool errno_rmdir_non_empty (int error_number)
{
  return error_number == ENOTEMPTY || error_number == EEXIST;
}
static bool errno_may_be_non_empty (int error_number)
{
  switch (error_number)
    {
    case EACCES:
    case EPERM:
    case EROFS:
    case EBUSY:
      return true;
    default:
      return false;
    }
}

static bool ignorable_failure (int error_number, char const *dir)
{
  return (ignore_fail_on_non_empty && (errno_rmdir_non_empty (error_number) || (errno_may_be_non_empty (error_number) && ! is_empty_dir (AT_FDCWD, dir) && errno == 0 /* definitely non empty  */)));
}

static bool remove_parents (char *dir)
{
  char *slash;
  bool ok = true;

  strip_trailing_slashes (dir);
  while (1)
    {
      slash = strrchr (dir, '/');
      if (slash == NULL)
        break;
      while (slash > dir && *slash == '/')
        --slash;
      slash[1] = 0;
      if (verbose)
        prog_fprintf (stdout, _("removing directory, %s"), quoteaf (dir));
      ok = (rmdir (dir) == 0);
      int rmdir_errno = errno;

      if (! ok)
        {
          if (ignorable_failure (rmdir_errno, dir))
            {
              ok = true;
            }
          else
            {
              char const* error_msg;
              if (rmdir_errno != ENOTDIR)
                {
                  error_msg = N_("failed to remove directory %s");
                }
              else
                {
                  error_msg = N_("failed to remove %s");
                }
              error (0, rmdir_errno, _(error_msg), quoteaf (dir));
            }
          break;
        }
    }
  return ok;
}

void usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("Usage: %s [OPTION]... DIRECTORY...\n"), program_name);
      fputs (_("\
Remove the DIRECTORY(ies), if they are empty.\n\
\n\
      --ignore-fail-on-non-empty\n\
                  ignore each failure that is solely because a directory\n\
                    is non-empty\n\
"), stdout);
      fputs (_("\
  -p, --parents   remove DIRECTORY and its ancestors; e.g., 'rmdir -p a/b/c' is\
\n\
                    similar to 'rmdir a/b/c a/b a'\n\
  -v, --verbose   output a diagnostic for every directory processed\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

int main (int argc, char **argv)
{
  bool ok = true;
  int optc;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);

  remove_empty_parents = false;

  while ((optc = getopt_long (argc, argv, "pv", longopts, NULL)) != -1)
    {
      switch (optc)
        {
        case 'p':
          remove_empty_parents = true;
          break;
        case IGNORE_FAIL_ON_NON_EMPTY_OPTION:
          ignore_fail_on_non_empty = true;
          break;
        case 'v':
          verbose = true;
          break;
        case_GETOPT_HELP_CHAR;
        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);
        default:
          usage (EXIT_FAILURE);
        }
    }

  if (optind == argc)
    {
      error (0, 0, _("missing operand"));
      usage (EXIT_FAILURE);
    }

  for (; optind < argc; ++optind)
    {
      char *dir = argv[optind];

      /* Give a diagnostic for each attempted removal if --verbose.  */
      if (verbose)
        prog_fprintf (stdout, _("removing directory, %s"), quoteaf (dir));

      if (rmdir (dir) != 0)
        {
          int rmdir_errno = errno;
          if (ignorable_failure (rmdir_errno, dir))
            continue;
          bool custom_error = false;
          if (rmdir_errno == ENOTDIR)
            {
              char const *last_unix_slash = strrchr (dir, '/');
              if (last_unix_slash && (*(last_unix_slash + 1) == '\0'))
                {
                  struct stat st;
                  int ret = stat (dir, &st);
                  if ((ret != 0 && errno != ENOTDIR) || S_ISDIR (st.st_mode))
                    {
                      char* dir_arg = xstrdup (dir);
                      strip_trailing_slashes (dir);
                      ret = lstat (dir, &st);
                      if (ret == 0 && S_ISLNK (st.st_mode))
                        {
                          error (0, 0, _("failed to remove %s:" " Symbolic link not followed"), quoteaf (dir_arg));
                          custom_error = true;
                        }
                      free (dir_arg);
                    }
                }
            }

          if (! custom_error)
            error (0, rmdir_errno, _("failed to remove %s"), quoteaf (dir));

          ok = false;
        }
      else if (remove_empty_parents)
        {
          ok &= remove_parents (dir);
        }
    }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
