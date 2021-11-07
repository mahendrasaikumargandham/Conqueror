#include <config.h>
#include <stdio.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <getopt.h>
#include "system.h"
#include "die.h"
#include "group-list.h"
#include "quote.h"
static struct option const longopts[] =
{
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
      printf (_("Usage: %s [OPTION]... [USERNAME]...\n"), program_name);
      fputs (_("\
Print group memberships for each USERNAME or, if no USERNAME is specified, for\
\n\
the current process (which may differ if the groups database has changed).\n"),
             stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

int
main (int argc, char **argv)
{
  int optc;
  bool ok = true;
  gid_t rgid, egid;
  uid_t ruid;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);

  while ((optc = getopt_long (argc, argv, "", longopts, NULL)) != -1)
    {
      switch (optc)
        {
        case_GETOPT_HELP_CHAR;
        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);
        default:
          usage (EXIT_FAILURE);
        }
    }

  if (optind == argc)
    {
      /* No arguments.  Divulge the details of the current process. */
      uid_t NO_UID = -1;
      gid_t NO_GID = -1;

      errno = 0;
      ruid = getuid ();
      if (ruid == NO_UID && errno)
        die (EXIT_FAILURE, errno, _("cannot get real UID"));

      errno = 0;
      egid = getegid ();
      if (egid == NO_GID && errno)
        die (EXIT_FAILURE, errno, _("cannot get effective GID"));

      errno = 0;
      rgid = getgid ();
      if (rgid == NO_GID && errno)
        die (EXIT_FAILURE, errno, _("cannot get real GID"));

      if (!print_group_list (NULL, ruid, rgid, egid, true, ' '))
        ok = false;
      putchar ('\n');
    }
  else
    {
      /* At least one argument.  Divulge the details of the specified users.  */
      for ( ; optind < argc; optind++)
        {
          struct passwd *pwd = getpwnam (argv[optind]);
          if (pwd == NULL)
            {
              error (0, 0, _("%s: no such user"), quote (argv[optind]));
              ok = false;
              continue;
            }
          ruid = pwd->pw_uid;
          rgid = egid = pwd->pw_gid;

          printf ("%s : ", argv[optind]);
          if (!print_group_list (argv[optind], ruid, rgid, egid, true, ' '))
            ok = false;
          putchar ('\n');
        }
    }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
