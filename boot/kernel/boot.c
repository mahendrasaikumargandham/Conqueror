#include <config.h>
#include <getopt.h>
#include <stdio.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include "system.h"
#include "die.h"
#include "error.h"
#include "ignore-value.h"
#include "mgetgroups.h"
#include "quote.h"
#include "root-dev-ino.h"
#include "userspec.h"
#include "xstrtol.h"
#define PROGRAM_NAME "chroot"
#define AUTHORS proper_name ("Roland McGrath")
#ifndef MAXGID
# define MAXGID GID_T_MAX
#endif
static inline bool uid_unset (uid_t uid) { return uid == (uid_t) -1; }
static inline bool gid_unset (gid_t gid) { return gid == (gid_t) -1; }
#define uid_set(x) (!uid_unset (x))
#define gid_set(x) (!gid_unset (x))
enum
{
  GROUPS = UCHAR_MAX + 1,
  USERSPEC,
  SKIP_CHDIR
};
static struct option const long_opts[] =
{
  {"groups", required_argument, NULL, GROUPS},
  {"userspec", required_argument, NULL, USERSPEC},
  {"skip-chdir", no_argument, NULL, SKIP_CHDIR},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0}
};

#if ! HAVE_SETGROUPS
static int
setgroups (size_t size, gid_t const *list _GL_UNUSED)
{
  if (size == 0)
    {
      return 0;
    }
  else
    {
      errno = ENOTSUP;
      return -1;
    }
}
#endif
static int parse_additional_groups (char const *groups, GETGROUPS_T **pgids, size_t *pn_gids, bool show_errors)
{
  GETGROUPS_T *gids = NULL;
  size_t n_gids_allocated = 0;
  size_t n_gids = 0;
  char *buffer = xstrdup (groups);
  char const *tmp;
  int ret = 0;
  for (tmp = strtok (buffer, ","); tmp; tmp = strtok (NULL, ","))
    {
      struct group *g;
      uintmax_t value;
      if (xstrtoumax (tmp, NULL, 10, &value, "") == LONGINT_OK
          && value <= MAXGID)
        {
          while (isspace (to_uchar (*tmp)))
            tmp++;
          if (*tmp != '+')
            {
             
              g = getgrnam (tmp);
              if (g != NULL)
                value = g->gr_gid;
            }
          g = (struct group *) (intptr_t) ! NULL;
        }
      else
        {
          g = getgrnam (tmp);
          if (g != NULL)
            value = g->gr_gid;
        }

      if (g == NULL)
        {
          ret = -1;

          if (show_errors)
            {
              error (0, errno, _("invalid group %s"), quote (tmp));
              continue;
            }

          break;
        }

      if (n_gids == n_gids_allocated)
        gids = X2NREALLOC (gids, &n_gids_allocated);
      gids[n_gids++] = value;
    }

  if (ret == 0 && n_gids == 0)
    {
      if (show_errors)
        error (0, 0, _("invalid group list %s"), quote (groups));
      ret = -1;
    }

  *pgids = gids;

  if (ret == 0)
    *pn_gids = n_gids;

  free (buffer);
  return ret;
}
static bool is_root (const char* dir)
{
  char *resolved = canonicalize_file_name (dir);
  bool is_res_root = resolved && STREQ ("/", resolved);
  free (resolved);
  return is_res_root;
}

void usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s [OPTION] NEWROOT [COMMAND [ARG]...]\n\
  or:  %s OPTION\n\
"), program_name, program_name);

      fputs (_("\
Run COMMAND with root directory set to NEWROOT.\n\
\n\
"), stdout);

      fputs (_("\
      --groups=G_LIST        specify supplementary groups as g1,g2,..,gN\n\
"), stdout);
      fputs (_("\
      --userspec=USER:GROUP  specify user and group (ID or name) to use\n\
"), stdout);
      printf (_("\
      --skip-chdir           do not change working directory to %s\n\
"), quoteaf ("/"));

      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      fputs (_("\
\n\
If no command is given, run '\"$SHELL\" -i' (default: '/bin/sh -i').\n\
"), stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

int
main (int argc, char **argv)
{
  int c;
  char *userspec = NULL;
  char const *username = NULL;
  char const *groups = NULL;
  bool skip_chdir = false;
  uid_t uid = -1;
  gid_t gid = -1;
  GETGROUPS_T *out_gids = NULL;
  size_t n_gids = 0;
  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);
  initialize_exit_failure (EXIT_CANCELED);
  atexit (close_stdout);
  while ((c = getopt_long (argc, argv, "+", long_opts, NULL)) != -1)
    {
      switch (c)
        {
        case USERSPEC:
          {
            userspec = optarg;
            size_t userlen = strlen (userspec);
            if (userlen && userspec[userlen - 1] == ':')
              userspec[userlen - 1] = '\0';
            break;
          }

        case GROUPS:
          groups = optarg;
          break;

        case SKIP_CHDIR:
          skip_chdir = true;
          break;

        case_GETOPT_HELP_CHAR;
        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);
        default:
          usage (EXIT_CANCELED);
        }
    }

  if (argc <= optind)
    {
      error (0, 0, _("missing operand"));
      usage (EXIT_CANCELED);
    }

  char const *newroot = argv[optind];
  bool is_oldroot = is_root (newroot);

  if (! is_oldroot && skip_chdir)
    {
      error (0, 0, _("option --skip-chdir only permitted if NEWROOT is old %s"),
             quoteaf ("/"));
      usage (EXIT_CANCELED);
    }

  if (! is_oldroot)
    {
      if (userspec)
        ignore_value (parse_user_spec (userspec, &uid, &gid, NULL, NULL));
      if (uid_set (uid) && (! groups || gid_unset (gid)))
        {
          const struct passwd *pwd;
          if ((pwd = getpwuid (uid)))
            {
              if (gid_unset (gid))
                gid = pwd->pw_gid;
              username = pwd->pw_name;
            }
        }

      if (groups && *groups)
        ignore_value (parse_additional_groups (groups, &out_gids, &n_gids, false));
#if HAVE_SETGROUPS
      else if (! groups && gid_set (gid) && username)
        {
          int ngroups = xgetgroups (username, gid, &out_gids);
          if (0 < ngroups)
            n_gids = ngroups;
        }
#endif
    }

  if (chroot (newroot) != 0)
    die (EXIT_CANCELED, errno, _("cannot change root directory to %s"),
         quoteaf (newroot));

  if (! skip_chdir && chdir ("/"))
    die (EXIT_CANCELED, errno, _("cannot chdir to root directory"));

  if (argc == optind + 1)
    {
      char *shell = getenv ("SHELL");
      if (shell == NULL)
        shell = bad_cast ("/bin/sh");
      argv[0] = shell;
      argv[1] = bad_cast ("-i");
      argv[2] = NULL;
    }
  else
    {
      argv += optind + 1;
    }
  if (userspec)
    {
      char const *err = parse_user_spec (userspec, &uid, &gid, NULL, NULL);

      if (err && uid_unset (uid) && gid_unset (gid))
        die (EXIT_CANCELED, errno, "%s", (err));
    }
  if (uid_set (uid) && (! groups || gid_unset (gid)))
    {
      const struct passwd *pwd;
      if ((pwd = getpwuid (uid)))
        {
          if (gid_unset (gid))
            gid = pwd->pw_gid;
          username = pwd->pw_name;
        }
      else if (gid_unset (gid))
        {
          die (EXIT_CANCELED, errno,
               _("no group specified for unknown uid: %d"), (int) uid);
        }
    }

  GETGROUPS_T *gids = out_gids;
  GETGROUPS_T *in_gids = NULL;
  if (groups && *groups)
    {
      if (parse_additional_groups (groups, &in_gids, &n_gids, !n_gids) != 0)
        {
          if (! n_gids)
            return EXIT_CANCELED;
        }
      else
        gids = in_gids;
    }
#if HAVE_SETGROUPS
  else if (! groups && gid_set (gid) && username)
    {
      int ngroups = xgetgroups (username, gid, &in_gids);
      if (ngroups <= 0)
        {
          if (! n_gids)
            die (EXIT_CANCELED, errno,
                 _("failed to get supplemental groups"));
        }
      else
        {
          n_gids = ngroups;
          gids = in_gids;
        }
    }
#endif

  if ((uid_set (uid) || groups) && setgroups (n_gids, gids) != 0)
    die (EXIT_CANCELED, errno, _("failed to set supplemental groups"));

  free (in_gids);
  free (out_gids);

  if (gid_set (gid) && setgid (gid))
    die (EXIT_CANCELED, errno, _("failed to set group-ID"));

  if (uid_set (uid) && setuid (uid))
    die (EXIT_CANCELED, errno, _("failed to set user-ID"));

  /* Execute the given command.  */
  execvp (argv[0], argv);

  int exit_status = errno == ENOENT ? EXIT_ENOENT : EXIT_CANNOT_INVOKE;
  error (0, errno, _("failed to run command %s"), quote (argv[0]));
  return exit_status;
}
