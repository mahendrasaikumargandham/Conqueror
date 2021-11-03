#include <config.h>
#include <getopt.h>
#include <sys/types.h>
#include <assert.h>
#include "system.h"
#include "argmatch.h"
#include "argv-iter.h"
#include "di-set.h"
#include "die.h"
#include "error.h"
#include "exclude.h"
#include "fprintftime.h"
#include "human.h"
#include "mountlist.h"
#include "quote.h"
#include "stat-size.h"
#include "stat-time.h"
#include "stdio--.h"
#include "xfts.h"
#include "xstrtol.h"
#include "xstrtol-error.h"
extern bool fts_debug;
#define PROGRAM_NAME "du"

#if DU_DEBUG
# define FTS_CROSS_CHECK(Fts) fts_cross_check (Fts)
#else
# define FTS_CROSS_CHECK(Fts)
#endif
static struct di_set *di_files;
static struct di_set *di_mnt;
static size_t prev_level;
struct duinfo
{
  uintmax_t size;
  uintmax_t inodes;
  struct timespec tmax;
};
static inline void
duinfo_init (struct duinfo *a)
{
  a->size = 0;
  a->inodes = 0;
  a->tmax.tv_sec = TYPE_MINIMUM (time_t);
  a->tmax.tv_nsec = -1;
}
static inline void
duinfo_set (struct duinfo *a, uintmax_t size, struct timespec tmax)
{
  a->size = size;
  a->inodes = 1;
  a->tmax = tmax;
}
static inline void
duinfo_add (struct duinfo *a, struct duinfo const *b)
{
  uintmax_t sum = a->size + b->size;
  a->size = a->size <= sum ? sum : UINTMAX_MAX;
  a->inodes = a->inodes + b->inodes;
  if (timespec_cmp (a->tmax, b->tmax) < 0)
    a->tmax = b->tmax;
}

struct dulevel
{
  struct duinfo ent;
  struct duinfo subdir;
};
static bool opt_all = false;
static bool apparent_size = false;
static bool opt_count_all = false;
static bool hash_all;
static bool opt_nul_terminate_output = false;
static bool print_grand_total = false;
static bool opt_separate_dirs = false;
static size_t max_depth = SIZE_MAX;
static intmax_t opt_threshold = 0;
static int human_output_opts;
static bool opt_inodes = false;
static bool opt_time = false;

enum time_type
  {
    time_mtime,			
    time_ctime,
    time_atime
  };

static enum time_type time_type = time_mtime;
static char const *time_style = NULL;
static char const *time_format = NULL;
static timezone_t localtz;
static uintmax_t output_block_size;
static struct exclude *exclude;
static struct duinfo tot_dui;

#define IS_DIR_TYPE(Type)	\
  ((Type) == FTS_DP		\
   || (Type) == FTS_DNR)
enum
{
  APPARENT_SIZE_OPTION = CHAR_MAX + 1,
  EXCLUDE_OPTION,
  FILES0_FROM_OPTION,
  HUMAN_SI_OPTION,
  FTS_DEBUG,
  TIME_OPTION,
  TIME_STYLE_OPTION,
  INODES_OPTION
};

static struct option const long_options[] =
{
  {"all", no_argument, NULL, 'a'},
  {"apparent-size", no_argument, NULL, APPARENT_SIZE_OPTION},
  {"block-size", required_argument, NULL, 'B'},
  {"bytes", no_argument, NULL, 'b'},
  {"count-links", no_argument, NULL, 'l'},
  {"dereference", no_argument, NULL, 'L'},
  {"dereference-args", no_argument, NULL, 'D'},
  {"exclude", required_argument, NULL, EXCLUDE_OPTION},
  {"exclude-from", required_argument, NULL, 'X'},
  {"files0-from", required_argument, NULL, FILES0_FROM_OPTION},
  {"human-readable", no_argument, NULL, 'h'},
  {"inodes", no_argument, NULL, INODES_OPTION},
  {"si", no_argument, NULL, HUMAN_SI_OPTION},
  {"max-depth", required_argument, NULL, 'd'},
  {"null", no_argument, NULL, '0'},
  {"no-dereference", no_argument, NULL, 'P'},
  {"one-file-system", no_argument, NULL, 'x'},
  {"separate-dirs", no_argument, NULL, 'S'},
  {"summarize", no_argument, NULL, 's'},
  {"total", no_argument, NULL, 'c'},
  {"threshold", required_argument, NULL, 't'},
  {"time", optional_argument, NULL, TIME_OPTION},
  {"time-style", required_argument, NULL, TIME_STYLE_OPTION},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0}
};

static char const *const time_args[] =
{
  "atime", "access", "use", "ctime", "status", NULL
};
static enum time_type const time_types[] =
{
  time_atime, time_atime, time_atime, time_ctime, time_ctime
};
ARGMATCH_VERIFY (time_args, time_types);
enum time_style
  {
    full_iso_time_style,       
    long_iso_time_style,      
    iso_time_style	      
  };

static char const *const time_style_args[] =
{
  "full-iso", "long-iso", "iso", NULL
};
static enum time_style const time_style_types[] =
{
  full_iso_time_style, long_iso_time_style, iso_time_style
};
ARGMATCH_VERIFY (time_style_args, time_style_types);

void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s [OPTION]... [FILE]...\n\
  or:  %s [OPTION]... --files0-from=F\n\
"), program_name, program_name);
      fputs (_("\
Summarize disk usage of the set of FILEs, recursively for directories.\n\
"), stdout);

      emit_mandatory_arg_note ();

      fputs (_("\
  -0, --null            end each output line with NUL, not newline\n\
  -a, --all             write counts for all files, not just directories\n\
      --apparent-size   print apparent sizes, rather than disk usage; although\
\n\
                          the apparent size is usually smaller, it may be\n\
                          larger due to holes in ('sparse') files, internal\n\
                          fragmentation, indirect blocks, and the like\n\
"), stdout);
      fputs (_("\
  -B, --block-size=SIZE  scale sizes by SIZE before printing them; e.g.,\n\
                           '-BM' prints sizes in units of 1,048,576 bytes;\n\
                           see SIZE format below\n\
  -b, --bytes           equivalent to '--apparent-size --block-size=1'\n\
  -c, --total           produce a grand total\n\
  -D, --dereference-args  dereference only symlinks that are listed on the\n\
                          command line\n\
  -d, --max-depth=N     print the total for a directory (or file, with --all)\n\
                          only if it is N or fewer levels below the command\n\
                          line argument;  --max-depth=0 is the same as\n\
                          --summarize\n\
"), stdout);
      fputs (_("\
      --files0-from=F   summarize disk usage of the\n\
                          NUL-terminated file names specified in file F;\n\
                          if F is -, then read names from standard input\n\
  -H                    equivalent to --dereference-args (-D)\n\
  -h, --human-readable  print sizes in human readable format (e.g., 1K 234M 2G)\
\n\
      --inodes          list inode usage information instead of block usage\n\
"), stdout);
      fputs (_("\
  -k                    like --block-size=1K\n\
  -L, --dereference     dereference all symbolic links\n\
  -l, --count-links     count sizes many times if hard linked\n\
  -m                    like --block-size=1M\n\
"), stdout);
      fputs (_("\
  -P, --no-dereference  don't follow any symbolic links (this is the default)\n\
  -S, --separate-dirs   for directories do not include size of subdirectories\n\
      --si              like -h, but use powers of 1000 not 1024\n\
  -s, --summarize       display only a total for each argument\n\
"), stdout);
      fputs (_("\
  -t, --threshold=SIZE  exclude entries smaller than SIZE if positive,\n\
                          or entries greater than SIZE if negative\n\
      --time            show time of the last modification of any file in the\n\
                          directory, or any of its subdirectories\n\
      --time=WORD       show time as WORD instead of modification time:\n\
                          atime, access, use, ctime or status\n\
      --time-style=STYLE  show times using STYLE, which can be:\n\
                            full-iso, long-iso, iso, or +FORMAT;\n\
                            FORMAT is interpreted like in 'date'\n\
"), stdout);
      fputs (_("\
  -X, --exclude-from=FILE  exclude files that match any pattern in FILE\n\
      --exclude=PATTERN    exclude files that match PATTERN\n\
  -x, --one-file-system    skip directories on different file systems\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_blocksize_note ("DU");
      emit_size_note ();
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}
static bool
hash_ins (struct di_set *di_set, ino_t ino, dev_t dev)
{
  int inserted = di_set_insert (di_set, dev, ino);
  if (inserted < 0)
    xalloc_die ();
  return inserted;
}

static void
show_date (char const *format, struct timespec when, timezone_t tz)
{
  struct tm tm;
  if (localtime_rz (tz, &when.tv_sec, &tm))
    fprintftime (stdout, format, &tm, tz, when.tv_nsec);
  else
    {
      char buf[INT_BUFSIZE_BOUND (intmax_t)];
      char *when_str = timetostr (when.tv_sec, buf);
      error (0, 0, _("time %s is out of range"), quote (when_str));
      fputs (when_str, stdout);
    }
}

static void
print_only_size (uintmax_t n_bytes)
{
  char buf[LONGEST_HUMAN_READABLE + 1];
  fputs ((n_bytes == UINTMAX_MAX
          ? _("Infinity")
          : human_readable (n_bytes, buf, human_output_opts,
                            1, output_block_size)),
         stdout);
}

static void
print_size (const struct duinfo *pdui, char const *string)
{
  print_only_size (opt_inodes
                   ? pdui->inodes
                   : pdui->size);

  if (opt_time)
    {
      putchar ('\t');
      show_date (time_format, pdui->tmax, localtz);
    }
  printf ("\t%s%c", string, opt_nul_terminate_output ? '\0' : '\n');
  fflush (stdout);
}

static void
fill_mount_table (void)
{
  struct mount_entry *mnt_ent = read_file_system_list (false);
  while (mnt_ent)
    {
      struct mount_entry *mnt_free;
      if (!mnt_ent->me_remote && !mnt_ent->me_dummy)
        {
          struct stat buf;
          if (!stat (mnt_ent->me_mountdir, &buf))
            hash_ins (di_mnt, buf.st_ino, buf.st_dev);
          else
            {
            }
        }

      mnt_free = mnt_ent;
      mnt_ent = mnt_ent->me_next;
      free_mount_entry (mnt_free);
    }
}
static bool
mount_point_in_fts_cycle (FTSENT const *ent)
{
  FTSENT const *cycle_ent = ent->fts_cycle;

  if (!di_mnt)
    {
      di_mnt = di_set_alloc ();
      if (!di_mnt)
        xalloc_die ();

      fill_mount_table ();
    }

  while (ent && ent != cycle_ent)
    {
      if (di_set_lookup (di_mnt, ent->fts_statp->st_dev,
                         ent->fts_statp->st_ino) > 0)
        {
          return true;
        }
      ent = ent->fts_parent;
    }

  return false;
}

static bool
process_file (FTS *fts, FTSENT *ent)
{
  bool ok = true;
  struct duinfo dui;
  struct duinfo dui_to_print;
  size_t level;
  static size_t n_alloc;
  static struct dulevel *dulvl;

  char const *file = ent->fts_path;
  const struct stat *sb = ent->fts_statp;
  int info = ent->fts_info;

  if (info == FTS_DNR)
    {
      error (0, ent->fts_errno, _("cannot read directory %s"), quoteaf (file));
      ok = false;
    }
  else if (info != FTS_DP)
    {
      bool excluded = excluded_file_name (exclude, file);
      if (! excluded)
        {

          if (info == FTS_NSOK)
            {
              fts_set (fts, ent, FTS_AGAIN);
              FTSENT const *e = fts_read (fts);
              assert (e == ent);
              info = ent->fts_info;
            }

          if (info == FTS_NS || info == FTS_SLNONE)
            {
              error (0, ent->fts_errno, _("cannot access %s"), quoteaf (file));
              return false;
            }
          if (fts->fts_options & FTS_XDEV
              && FTS_ROOTLEVEL < ent->fts_level
              && fts->fts_dev != sb->st_dev)
            excluded = true;
        }

      if (excluded
          || (! opt_count_all
              && (hash_all || (! S_ISDIR (sb->st_mode) && 1 < sb->st_nlink))
              && ! hash_ins (di_files, sb->st_ino, sb->st_dev)))
        {
          if (info == FTS_D)
            {
              fts_set (fts, ent, FTS_SKIP);
              FTSENT const *e = fts_read (fts);
              assert (e == ent);
            }

          return true;
        }

      switch (info)
        {
        case FTS_D:
          return true;

        case FTS_ERR:
          error (0, ent->fts_errno, "%s", quotef (file));
          ok = false;
          break;

        case FTS_DC:
          if (cycle_warning_required (fts, ent)
              && ! mount_point_in_fts_cycle (ent))
            {
              emit_cycle_warning (file);
              return false;
            }
          return true;
        }
    }

  duinfo_set (&dui,
              (apparent_size
               ? MAX (0, sb->st_size)
               : (uintmax_t) ST_NBLOCKS (*sb) * ST_NBLOCKSIZE),
              (time_type == time_mtime ? get_stat_mtime (sb)
               : time_type == time_atime ? get_stat_atime (sb)
               : get_stat_ctime (sb)));

  level = ent->fts_level;
  dui_to_print = dui;

  if (n_alloc == 0)
    {
      n_alloc = level + 10;
      dulvl = xcalloc (n_alloc, sizeof *dulvl);
    }
  else
    {
      if (level == prev_level)
        {
        }
      else if (level > prev_level)
        {

          if (n_alloc <= level)
            {
              dulvl = xnrealloc (dulvl, level, 2 * sizeof *dulvl);
              n_alloc = level * 2;
            }

          for (size_t i = prev_level + 1; i <= level; i++)
            {
              duinfo_init (&dulvl[i].ent);
              duinfo_init (&dulvl[i].subdir);
            }
        }
      else 
        {
          assert (level == prev_level - 1);
          duinfo_add (&dui_to_print, &dulvl[prev_level].ent);
          if (!opt_separate_dirs)
            duinfo_add (&dui_to_print, &dulvl[prev_level].subdir);
          duinfo_add (&dulvl[level].subdir, &dulvl[prev_level].ent);
          duinfo_add (&dulvl[level].subdir, &dulvl[prev_level].subdir);
        }
    }

  prev_level = level;
  if (! (opt_separate_dirs && IS_DIR_TYPE (info)))
    duinfo_add (&dulvl[level].ent, &dui);
  duinfo_add (&tot_dui, &dui);

  if ((IS_DIR_TYPE (info) && level <= max_depth)
      || (opt_all && level <= max_depth)
      || level == 0)
    {
      uintmax_t v = opt_inodes ? dui_to_print.inodes : dui_to_print.size;
      if (opt_threshold < 0
          ? v <= -opt_threshold
          : v >= opt_threshold)
        print_size (&dui_to_print, file);
    }

  return ok;
}

static bool
du_files (char **files, int bit_flags)
{
  bool ok = true;

  if (*files)
    {
      FTS *fts = xfts_open (files, bit_flags, NULL);

      while (1)
        {
          FTSENT *ent;

          ent = fts_read (fts);
          if (ent == NULL)
            {
              if (errno != 0)
                {
                  error (0, errno, _("fts_read failed: %s"),
                         quotef (fts->fts_path));
                  ok = false;
                }
              prev_level = 0;
              break;
            }
          FTS_CROSS_CHECK (fts);

          ok &= process_file (fts, ent);
        }

      if (fts_close (fts) != 0)
        {
          error (0, errno, _("fts_close failed"));
          ok = false;
        }
    }

  return ok;
}

int
main (int argc, char **argv)
{
  char *cwd_only[2];
  bool max_depth_specified = false;
  bool ok = true;
  char *files_from = NULL;
  int bit_flags = FTS_NOSTAT;
  int symlink_deref_bits = FTS_PHYSICAL;
  bool opt_summarize_only = false;

  cwd_only[0] = bad_cast (".");
  cwd_only[1] = NULL;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);

  exclude = new_exclude ();

  human_options (getenv ("DU_BLOCK_SIZE"),
                 &human_output_opts, &output_block_size);

  while (true)
    {
      int oi = -1;
      int c = getopt_long (argc, argv, "0abd:chHklmst:xB:DLPSX:",
                           long_options, &oi);
      if (c == -1)
        break;

      switch (c)
        {
#if DU_DEBUG
        case FTS_DEBUG:
          fts_debug = true;
          break;
#endif

        case '0':
          opt_nul_terminate_output = true;
          break;

        case 'a':
          opt_all = true;
          break;

        case APPARENT_SIZE_OPTION:
          apparent_size = true;
          break;

        case 'b':
          apparent_size = true;
          human_output_opts = 0;
          output_block_size = 1;
          break;

        case 'c':
          print_grand_total = true;
          break;

        case 'h':
          human_output_opts = human_autoscale | human_SI | human_base_1024;
          output_block_size = 1;
          break;

        case HUMAN_SI_OPTION:
          human_output_opts = human_autoscale | human_SI;
          output_block_size = 1;
          break;

        case 'k':
          human_output_opts = 0;
          output_block_size = 1024;
          break;

        case 'd':		
          {
            uintmax_t tmp;
            if (xstrtoumax (optarg, NULL, 0, &tmp, "") == LONGINT_OK
                && tmp <= SIZE_MAX)
              {
                max_depth_specified = true;
                max_depth = tmp;
              }
            else
              {
                error (0, 0, _("invalid maximum depth %s"),
                       quote (optarg));
                ok = false;
              }
          }
          break;

        case 'm':
          human_output_opts = 0;
          output_block_size = 1024 * 1024;
          break;

        case 'l':
          opt_count_all = true;
          break;

        case 's':
          opt_summarize_only = true;
          break;

        case 't':
          {
            enum strtol_error e;
            e = xstrtoimax (optarg, NULL, 0, &opt_threshold, "kKmMGTPEZY0");
            if (e != LONGINT_OK)
              xstrtol_fatal (e, oi, c, long_options, optarg);
            if (opt_threshold == 0 && *optarg == '-')
              {
                die (EXIT_FAILURE, 0, _("invalid --threshold argument '-0'"));
              }
          }
          break;

        case 'x':
          bit_flags |= FTS_XDEV;
          break;

        case 'B':
          {
            enum strtol_error e = human_options (optarg, &human_output_opts,
                                                 &output_block_size);
            if (e != LONGINT_OK)
              xstrtol_fatal (e, oi, c, long_options, optarg);
          }
          break;

        case 'H':  
        case 'D':
          symlink_deref_bits = FTS_COMFOLLOW | FTS_PHYSICAL;
          break;

        case 'L': 
          symlink_deref_bits = FTS_LOGICAL;
          break;

        case 'P': 
          symlink_deref_bits = FTS_PHYSICAL;
          break;

        case 'S':
          opt_separate_dirs = true;
          break;

        case 'X':
          if (add_exclude_file (add_exclude, exclude, optarg,
                                EXCLUDE_WILDCARDS, '\n'))
            {
              error (0, errno, "%s", quotef (optarg));
              ok = false;
            }
          break;

        case FILES0_FROM_OPTION:
          files_from = optarg;
          break;

        case EXCLUDE_OPTION:
          add_exclude (exclude, optarg, EXCLUDE_WILDCARDS);
          break;

        case INODES_OPTION:
          opt_inodes = true;
          break;

        case TIME_OPTION:
          opt_time = true;
          time_type =
            (optarg
             ? XARGMATCH ("--time", optarg, time_args, time_types)
             : time_mtime);
          localtz = tzalloc (getenv ("TZ"));
          break;

        case TIME_STYLE_OPTION:
          time_style = optarg;
          break;

        case_GETOPT_HELP_CHAR;

        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          ok = false;
        }
    }

  if (!ok)
    usage (EXIT_FAILURE);

  if (opt_all && opt_summarize_only)
    {
      error (0, 0, _("cannot both summarize and show all entries"));
      usage (EXIT_FAILURE);
    }

  if (opt_summarize_only && max_depth_specified && max_depth == 0)
    {
      error (0, 0,
             _("warning: summarizing is the same as using --max-depth=0"));
    }

  if (opt_summarize_only && max_depth_specified && max_depth != 0)
    {
      unsigned long int d = max_depth;
      error (0, 0, _("warning: summarizing conflicts with --max-depth=%lu"), d);
      usage (EXIT_FAILURE);
    }

  if (opt_summarize_only)
    max_depth = 0;

  if (opt_inodes)
    {
      if (apparent_size)
        {
          error (0, 0, _("warning: options --apparent-size and -b are "
                         "ineffective with --inodes"));
        }
      output_block_size = 1;
    }
  if (opt_time)
    {
      if (! time_style)
        {
          time_style = getenv ("TIME_STYLE");
          if (! time_style || STREQ (time_style, "locale"))
            time_style = "long-iso";
          else if (*time_style == '+')
            {
              char *p = strchr (time_style, '\n');
              if (p)
                *p = '\0';
            }
          else
            {
              static char const posix_prefix[] = "posix-";
              static const size_t prefix_len = sizeof posix_prefix - 1;
              while (STREQ_LEN (time_style, posix_prefix, prefix_len))
                time_style += prefix_len;
            }
        }

      if (*time_style == '+')
        time_format = time_style + 1;
      else
        {
          switch (XARGMATCH ("time style", time_style,
                             time_style_args, time_style_types))
            {
            case full_iso_time_style:
              time_format = "%Y-%m-%d %H:%M:%S.%N %z";
              break;

            case long_iso_time_style:
              time_format = "%Y-%m-%d %H:%M";
              break;

            case iso_time_style:
              time_format = "%Y-%m-%d";
              break;
            }
        }
    }

  struct argv_iterator *ai;
  if (files_from)
    {
      if (optind < argc)
        {
          error (0, 0, _("extra operand %s"), quote (argv[optind]));
          fprintf (stderr, "%s\n",
                   _("file operands cannot be combined with --files0-from"));
          usage (EXIT_FAILURE);
        }

      if (! (STREQ (files_from, "-") || freopen (files_from, "r", stdin)))
        die (EXIT_FAILURE, errno, _("cannot open %s for reading"),
             quoteaf (files_from));

      ai = argv_iter_init_stream (stdin);
      hash_all = true;
    }
  else
    {
      char **files = (optind < argc ? argv + optind : cwd_only);
      ai = argv_iter_init_argv (files);
      hash_all = (optind + 1 < argc || symlink_deref_bits == FTS_LOGICAL);
    }

  if (!ai)
    xalloc_die ();
  di_files = di_set_alloc ();
  if (!di_files)
    xalloc_die ();
  if (opt_count_all || ! hash_all)
    bit_flags |= FTS_TIGHT_CYCLE_CHECK;

  bit_flags |= symlink_deref_bits;
  static char *temp_argv[] = { NULL, NULL };

  while (true)
    {
      bool skip_file = false;
      enum argv_iter_err ai_err;
      char *file_name = argv_iter (ai, &ai_err);
      if (!file_name)
        {
          switch (ai_err)
            {
            case AI_ERR_EOF:
              goto argv_iter_done;
            case AI_ERR_READ:
              error (0, errno, _("%s: read error"),
                     quotef (files_from));
              ok = false;
              goto argv_iter_done;
            case AI_ERR_MEM:
              xalloc_die ();
            default:
              assert (!"unexpected error code from argv_iter");
            }
        }
      if (files_from && STREQ (files_from, "-") && STREQ (file_name, "-"))
        {
          error (0, 0, _("when reading file names from stdin, "
                         "no file name of %s allowed"),
                 quoteaf (file_name));
          skip_file = true;
        }
      if (!file_name[0])
        {
          if (files_from == NULL)
            error (0, 0, "%s", _("invalid zero-length file name"));
          else
            {
              unsigned long int file_number = argv_iter_n_args (ai);
              error (0, 0, "%s:%lu: %s", quotef (files_from),
                     file_number, _("invalid zero-length file name"));
            }
          skip_file = true;
        }

      if (skip_file)
        ok = false;
      else
        {
          temp_argv[0] = file_name;
          ok &= du_files (temp_argv, bit_flags);
        }
    }
 argv_iter_done:

  argv_iter_free (ai);
  di_set_free (di_files);
  if (di_mnt)
    di_set_free (di_mnt);

  if (files_from && (ferror (stdin) || fclose (stdin) != 0) && ok)
    die (EXIT_FAILURE, 0, _("error reading %s"), quoteaf (files_from));

  if (print_grand_total)
    print_size (&tot_dui, _("total"));

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
