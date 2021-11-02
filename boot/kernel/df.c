#include <config.h>
#include <stdio.h>
#include <sys/types.h>
#include <getopt.h>
#include <assert.h>
#include <c-ctype.h>
#include <wchar.h>
#include <wctype.h>
#include "system.h"
#include "canonicalize.h"
#include "die.h"
#include "error.h"
#include "fsusage.h"
#include "human.h"
#include "mbsalign.h"
#include "mbswidth.h"
#include "mountlist.h"
#include "quote.h"
#include "find-mount-point.h"
#include "hash.h"
#include "xstrtol-error.h"
#define PROGRAM_NAME "df"
struct devlist
{
  dev_t dev_num;
  struct mount_entry *me;
  struct devlist *next;
};
static Hash_table *devlist_table;
static bool show_all_fs;
static bool show_local_fs;
static bool show_listed_fs;
static int human_output_opts;
static uintmax_t output_block_size;
static bool file_systems_processed;
static bool require_sync;
static int exit_status;

struct fs_type_list
{
  char *fs_name;
  struct fs_type_list *fs_next;
};

static struct fs_type_list *fs_select_list;
static struct fs_type_list *fs_exclude_list;
static struct mount_entry *mount_list;
static bool print_type;
static bool print_grand_total;
static struct fs_usage grand_fsu;
enum
{
  DEFAULT_MODE,
  INODES_MODE,
  HUMAN_MODE,
  POSIX_MODE,
  OUTPUT_MODE
};
static int header_mode = DEFAULT_MODE;
typedef enum
{
  SOURCE_FIELD, 
  FSTYPE_FIELD, 
  SIZE_FIELD,  
  USED_FIELD,   
  AVAIL_FIELD, 
  PCENT_FIELD,
  ITOTAL_FIELD, 
  IUSED_FIELD,  
  IAVAIL_FIELD, 
  IPCENT_FIELD, 
  TARGET_FIELD, 
  FILE_FIELD,   
  INVALID_FIELD 
} display_field_t;
typedef enum
{
  BLOCK_FLD, 
  INODE_FLD, 
  OTHER_FLD  
} field_type_t;
struct field_data_t
{
  display_field_t field;
  char const *arg;
  field_type_t field_type;
  char const *caption;
  size_t width;       
  mbs_align_t align;  
  bool used;
};
static struct field_data_t field_data[] = {
  [SOURCE_FIELD] = { SOURCE_FIELD,
    "source", OTHER_FLD, N_("Filesystem"), 14, MBS_ALIGN_LEFT,  false },

  [FSTYPE_FIELD] = { FSTYPE_FIELD,
    "fstype", OTHER_FLD, N_("Type"),        4, MBS_ALIGN_LEFT,  false },

  [SIZE_FIELD] = { SIZE_FIELD,
    "size",   BLOCK_FLD, N_("blocks"),      5, MBS_ALIGN_RIGHT, false },

  [USED_FIELD] = { USED_FIELD,
    "used",   BLOCK_FLD, N_("Used"),        5, MBS_ALIGN_RIGHT, false },

  [AVAIL_FIELD] = { AVAIL_FIELD,
    "avail",  BLOCK_FLD, N_("Available"),   5, MBS_ALIGN_RIGHT, false },

  [PCENT_FIELD] = { PCENT_FIELD,
    "pcent",  BLOCK_FLD, N_("Use%"),        4, MBS_ALIGN_RIGHT, false },

  [ITOTAL_FIELD] = { ITOTAL_FIELD,
    "itotal", INODE_FLD, N_("Inodes"),      5, MBS_ALIGN_RIGHT, false },

  [IUSED_FIELD] = { IUSED_FIELD,
    "iused",  INODE_FLD, N_("IUsed"),       5, MBS_ALIGN_RIGHT, false },

  [IAVAIL_FIELD] = { IAVAIL_FIELD,
    "iavail", INODE_FLD, N_("IFree"),       5, MBS_ALIGN_RIGHT, false },

  [IPCENT_FIELD] = { IPCENT_FIELD,
    "ipcent", INODE_FLD, N_("IUse%"),       4, MBS_ALIGN_RIGHT, false },

  [TARGET_FIELD] = { TARGET_FIELD,
    "target", OTHER_FLD, N_("Mounted on"),  0, MBS_ALIGN_LEFT,  false },

  [FILE_FIELD] = { FILE_FIELD,
    "file",   OTHER_FLD, N_("File"),        0, MBS_ALIGN_LEFT,  false }
};

static char const *all_args_string =
  "source,fstype,itotal,iused,iavail,ipcent,size,"
  "used,avail,pcent,file,target";
static struct field_data_t **columns;
static size_t ncolumns;
struct field_values_t
{
  uintmax_t input_units;
  uintmax_t output_units;
  uintmax_t total;
  uintmax_t available;
  bool negate_available;
  uintmax_t available_to_root;
  uintmax_t used;
  bool negate_used;
};
static char ***table;
static size_t nrows;
enum
{
  NO_SYNC_OPTION = CHAR_MAX + 1,
  SYNC_OPTION,
  TOTAL_OPTION,
  OUTPUT_OPTION
};

static struct option const long_options[] =
{
  {"all", no_argument, NULL, 'a'},
  {"block-size", required_argument, NULL, 'B'},
  {"inodes", no_argument, NULL, 'i'},
  {"human-readable", no_argument, NULL, 'h'},
  {"si", no_argument, NULL, 'H'},
  {"local", no_argument, NULL, 'l'},
  {"output", optional_argument, NULL, OUTPUT_OPTION},
  {"portability", no_argument, NULL, 'P'},
  {"print-type", no_argument, NULL, 'T'},
  {"sync", no_argument, NULL, SYNC_OPTION},
  {"no-sync", no_argument, NULL, NO_SYNC_OPTION},
  {"total", no_argument, NULL, TOTAL_OPTION},
  {"type", required_argument, NULL, 't'},
  {"exclude-type", required_argument, NULL, 'x'},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0}
};

static void
replace_control_chars (char *cell)
{
  char *p = cell;
  while (*p)
    {
      if (c_iscntrl (to_uchar (*p)))
        *p = '?';
      p++;
    }
}

static void
replace_invalid_chars (char *cell)
{
  char *srcend = cell + strlen (cell);
  char *dst = cell;
  mbstate_t mbstate = { 0, };
  size_t n;

  for (char *src = cell; src != srcend; src += n)
    {
      wchar_t wc;
      size_t srcbytes = srcend - src;
      n = mbrtowc (&wc, src, srcbytes, &mbstate);
      bool ok = n <= srcbytes;

      if (ok)
        ok = !iswcntrl (wc);
      else
        n = 1;

      if (ok)
        {
          memmove (dst, src, n);
          dst += n;
        }
      else
        {
          *dst++ = '?';
          memset (&mbstate, 0, sizeof mbstate);
        }
    }

  *dst = '\0';
}

static void
replace_problematic_chars (char *cell)
{
  static int tty_out = -1;
  if (tty_out < 0)
    tty_out = isatty (STDOUT_FILENO);

  (tty_out ? replace_invalid_chars : replace_control_chars) (cell) ;
}
static void
alloc_table_row (void)
{
  nrows++;
  table = xnrealloc (table, nrows, sizeof (char **));
  table[nrows - 1] = xnmalloc (ncolumns, sizeof (char *));
}

static void
print_table (void)
{
  size_t row;

  for (row = 0; row < nrows; row++)
    {
      size_t col;
      for (col = 0; col < ncolumns; col++)
        {
          char *cell = table[row][col];
          if (col != 0)
            putchar (' ');

          int flags = 0;
          if (col == ncolumns - 1) 
            flags = MBA_NO_RIGHT_PAD;

          size_t width = columns[col]->width;
          cell = ambsalign (cell, &width, columns[col]->align, flags);
          fputs (cell ? cell : table[row][col], stdout);
          free (cell);

          IF_LINT (free (table[row][col]));
        }
      putchar ('\n');
      IF_LINT (free (table[row]));
    }

  IF_LINT (free (table));
}

static void
alloc_field (int f, char const *c)
{
  ncolumns++;
  columns = xnrealloc (columns, ncolumns, sizeof (struct field_data_t *));
  columns[ncolumns - 1] = &field_data[f];
  if (c != NULL)
    columns[ncolumns - 1]->caption = c;

  if (field_data[f].used)
    assert (!"field used");
  field_data[f].used = true;
}
static void
decode_output_arg (char const *arg)
{
  char *arg_writable = xstrdup (arg);
  char *s = arg_writable;
  do
    {
      char *comma = strchr (s, ',');
      if (comma)
        *comma++ = 0;
      display_field_t field = INVALID_FIELD;
      for (unsigned int i = 0; i < ARRAY_CARDINALITY (field_data); i++)
        {
          if (STREQ (field_data[i].arg, s))
            {
              field = i;
              break;
            }
        }
      if (field == INVALID_FIELD)
        {
          error (0, 0, _("option --output: field %s unknown"), quote (s));
          usage (EXIT_FAILURE);
        }

      if (field_data[field].used)
        {
          error (0, 0, _("option --output: field %s used more than once"),
                 quote (field_data[field].arg));
          usage (EXIT_FAILURE);
        }

      switch (field)
        {
        case SOURCE_FIELD:
        case FSTYPE_FIELD:
        case USED_FIELD:
        case PCENT_FIELD:
        case ITOTAL_FIELD:
        case IUSED_FIELD:
        case IAVAIL_FIELD:
        case IPCENT_FIELD:
        case TARGET_FIELD:
        case FILE_FIELD:
          alloc_field (field, NULL);
          break;

        case SIZE_FIELD:
          alloc_field (field, N_("Size"));
          break;

        case AVAIL_FIELD:
          alloc_field (field, N_("Avail"));
          break;

        default:
          assert (!"invalid field");
        }
      s = comma;
    }
  while (s);

  free (arg_writable);
}
static void
get_field_list (void)
{
  switch (header_mode)
    {
    case DEFAULT_MODE:
      alloc_field (SOURCE_FIELD, NULL);
      if (print_type)
        alloc_field (FSTYPE_FIELD, NULL);
      alloc_field (SIZE_FIELD,   NULL);
      alloc_field (USED_FIELD,   NULL);
      alloc_field (AVAIL_FIELD,  NULL);
      alloc_field (PCENT_FIELD,  NULL);
      alloc_field (TARGET_FIELD, NULL);
      break;

    case HUMAN_MODE:
      alloc_field (SOURCE_FIELD, NULL);
      if (print_type)
        alloc_field (FSTYPE_FIELD, NULL);

      alloc_field (SIZE_FIELD,   N_("Size"));
      alloc_field (USED_FIELD,   NULL);
      alloc_field (AVAIL_FIELD,  N_("Avail"));
      alloc_field (PCENT_FIELD,  NULL);
      alloc_field (TARGET_FIELD, NULL);
      break;

    case INODES_MODE:
      alloc_field (SOURCE_FIELD, NULL);
      if (print_type)
        alloc_field (FSTYPE_FIELD, NULL);
      alloc_field (ITOTAL_FIELD,  NULL);
      alloc_field (IUSED_FIELD,   NULL);
      alloc_field (IAVAIL_FIELD,  NULL);
      alloc_field (IPCENT_FIELD,  NULL);
      alloc_field (TARGET_FIELD,  NULL);
      break;

    case POSIX_MODE:
      alloc_field (SOURCE_FIELD, NULL);
      if (print_type)
        alloc_field (FSTYPE_FIELD, NULL);
      alloc_field (SIZE_FIELD,   NULL);
      alloc_field (USED_FIELD,   NULL);
      alloc_field (AVAIL_FIELD,  NULL);
      alloc_field (PCENT_FIELD,  N_("Capacity"));
      alloc_field (TARGET_FIELD, NULL);
      break;

    case OUTPUT_MODE:
      if (!ncolumns)
        {
          decode_output_arg (all_args_string);
        }
      break;

    default:
      assert (!"invalid header_mode");
    }
}

static void
get_header (void)
{
  size_t col;

  alloc_table_row ();

  for (col = 0; col < ncolumns; col++)
    {
      char *cell = NULL;
      char const *header = _(columns[col]->caption);

      if (columns[col]->field == SIZE_FIELD
          && (header_mode == DEFAULT_MODE
              || (header_mode == OUTPUT_MODE
                  && !(human_output_opts & human_autoscale))))
        {
          char buf[LONGEST_HUMAN_READABLE + 1];

          int opts = (human_suppress_point_zero
                      | human_autoscale | human_SI
                      | (human_output_opts
                         & (human_group_digits | human_base_1024 | human_B)));

          uintmax_t q1000 = output_block_size;
          uintmax_t q1024 = output_block_size;
          bool divisible_by_1000;
          bool divisible_by_1024;

          do
            {
              divisible_by_1000 = q1000 % 1000 == 0;  q1000 /= 1000;
              divisible_by_1024 = q1024 % 1024 == 0;  q1024 /= 1024;
            }
          while (divisible_by_1000 & divisible_by_1024);

          if (divisible_by_1000 < divisible_by_1024)
            opts |= human_base_1024;
          if (divisible_by_1024 < divisible_by_1000)
            opts &= ~human_base_1024;
          if (! (opts & human_base_1024))
            opts |= human_B;

          char *num = human_readable (output_block_size, buf, opts, 1, 1);
          header = _("blocks");
          if (asprintf (&cell, _("%s-%s"), num, header) == -1)
            cell = NULL;
        }
      else if (header_mode == POSIX_MODE && columns[col]->field == SIZE_FIELD)
        {
          char buf[INT_BUFSIZE_BOUND (uintmax_t)];
          char *num = umaxtostr (output_block_size, buf);
          if (asprintf (&cell, _("%s-%s"), num, header) == -1)
            cell = NULL;
        }
      else
        cell = strdup (header);

      if (!cell)
        xalloc_die ();

      replace_problematic_chars (cell);

      table[nrows - 1][col] = cell;

      size_t cell_width = mbswidth (cell, 0);
      columns[col]->width = MAX (columns[col]->width, cell_width);
    }
}

static bool _GL_ATTRIBUTE_PURE
selected_fstype (char const *fstype)
{
  const struct fs_type_list *fsp;

  if (fs_select_list == NULL || fstype == NULL)
    return true;
  for (fsp = fs_select_list; fsp; fsp = fsp->fs_next)
    if (STREQ (fstype, fsp->fs_name))
      return true;
  return false;
}

static bool _GL_ATTRIBUTE_PURE
excluded_fstype (char const *fstype)
{
  const struct fs_type_list *fsp;

  if (fs_exclude_list == NULL || fstype == NULL)
    return false;
  for (fsp = fs_exclude_list; fsp; fsp = fsp->fs_next)
    if (STREQ (fstype, fsp->fs_name))
      return true;
  return false;
}

static size_t
devlist_hash (void const *x, size_t table_size)
{
  struct devlist const *p = x;
  return (uintmax_t) p->dev_num % table_size;
}

static bool
devlist_compare (void const *x, void const *y)
{
  struct devlist const *a = x;
  struct devlist const *b = y;
  return a->dev_num == b->dev_num;
}

static struct devlist *
devlist_for_dev (dev_t dev)
{
  if (devlist_table == NULL)
    return NULL;
  struct devlist dev_entry;
  dev_entry.dev_num = dev;
  return hash_lookup (devlist_table, &dev_entry);
}

static void
devlist_free (void *p)
{
  free (p);
}

static void
filter_mount_list (bool devices_only)
{
  struct mount_entry *me;
  struct devlist *device_list = NULL;
  int mount_list_size = 0;

  for (me = mount_list; me; me = me->me_next)
    mount_list_size++;

  devlist_table = hash_initialize (mount_list_size, NULL,
                                 devlist_hash,
                                 devlist_compare,
                                 devlist_free);
  if (devlist_table == NULL)
    xalloc_die ();
  for (me = mount_list; me;)
    {
      struct stat buf;
      struct mount_entry *discard_me = NULL;
      if ((me->me_remote && show_local_fs)
          || (me->me_dummy && !show_all_fs && !show_listed_fs)
          || (!selected_fstype (me->me_type) || excluded_fstype (me->me_type))
          || -1 == stat (me->me_mountdir, &buf))
        {
          buf.st_dev = me->me_dev;
        }
      else
        {
          struct devlist *seen_dev = devlist_for_dev (buf.st_dev);

          if (seen_dev)
            {
              bool target_nearer_root = strlen (seen_dev->me->me_mountdir)
                                        > strlen (me->me_mountdir);
              bool source_below_root = seen_dev->me->me_mntroot != NULL
                                       && me->me_mntroot != NULL
                                       && (strlen (seen_dev->me->me_mntroot)
                                           < strlen (me->me_mntroot));
              if (! print_grand_total
                  && me->me_remote && seen_dev->me->me_remote
                  && ! STREQ (seen_dev->me->me_devname, me->me_devname))
                {
                }
              else if ((strchr (me->me_devname, '/')
                        && ! strchr (seen_dev->me->me_devname, '/'))
                       || (target_nearer_root && ! source_below_root)
                       || (! STREQ (seen_dev->me->me_devname, me->me_devname)
                           && STREQ (me->me_mountdir,
                                     seen_dev->me->me_mountdir)))
                {
                  discard_me = seen_dev->me;
                  seen_dev->me = me;
                }
              else
                {
                  discard_me = me;
                }

            }
        }

      if (discard_me)
        {
          me = me->me_next;
          if (! devices_only)
            free_mount_entry (discard_me);
        }
      else
        {
          struct devlist *devlist = xmalloc (sizeof *devlist);
          devlist->me = me;
          devlist->dev_num = buf.st_dev;
          devlist->next = device_list;
          device_list = devlist;
          if (hash_insert (devlist_table, devlist) == NULL)
            xalloc_die ();

          me = me->me_next;
        }
    }
  if (! devices_only) {
    mount_list = NULL;
    while (device_list)
      {
        me = device_list->me;
        me->me_next = mount_list;
        mount_list = me;
        device_list = device_list->next;
      }

      hash_free (devlist_table);
      devlist_table = NULL;
  }
}

static struct mount_entry const * _GL_ATTRIBUTE_PURE
me_for_dev (dev_t dev)
{
  struct devlist *dl = devlist_for_dev (dev);
  if (dl)
        return dl->me;

  return NULL;
}
static bool
known_value (uintmax_t n)
{
  return n < UINTMAX_MAX - 1;
}

static char const *
df_readable (bool negative, uintmax_t n, char *buf,
             uintmax_t input_units, uintmax_t output_units)
{
  if (! known_value (n) && !negative)
    return "-";
  else
    {
      char *p = human_readable (negative ? -n : n, buf + negative,
                                human_output_opts, input_units, output_units);
      if (negative)
        *--p = '-';
      return p;
    }
}
#define LOG_EQ(a, b) (!(a) == !(b))
static void
add_uint_with_neg_flag (uintmax_t *dest, bool *dest_neg,
                        uintmax_t src, bool src_neg)
{
  if (LOG_EQ (*dest_neg, src_neg))
    {
      *dest += src;
      return;
    }

  if (*dest_neg)
    *dest = -*dest;

  if (src_neg)
    src = -src;

  if (src < *dest)
    *dest -= src;
  else
    {
      *dest = src - *dest;
      *dest_neg = src_neg;
    }

  if (*dest_neg)
    *dest = -*dest;
}
static bool _GL_ATTRIBUTE_PURE
has_uuid_suffix (char const *s)
{
  size_t len = strlen (s);
  return (36 < len
          && strspn (s + len - 36, "-0123456789abcdefABCDEF") == 36);
}
static void
get_field_values (struct field_values_t *bv,
                  struct field_values_t *iv,
                  const struct fs_usage *fsu)
{
  iv->input_units = iv->output_units = 1;
  iv->total = fsu->fsu_files;
  iv->available = iv->available_to_root = fsu->fsu_ffree;
  iv->negate_available = false;

  iv->used = UINTMAX_MAX;
  iv->negate_used = false;
  if (known_value (iv->total) && known_value (iv->available_to_root))
    {
      iv->used = iv->total - iv->available_to_root;
      iv->negate_used = (iv->total < iv->available_to_root);
    }
  bv->input_units = fsu->fsu_blocksize;
  bv->output_units = output_block_size;
  bv->total = fsu->fsu_blocks;
  bv->available = fsu->fsu_bavail;
  bv->available_to_root = fsu->fsu_bfree;
  bv->negate_available = (fsu->fsu_bavail_top_bit_set
                         && known_value (fsu->fsu_bavail));

  bv->used = UINTMAX_MAX;
  bv->negate_used = false;
  if (known_value (bv->total) && known_value (bv->available_to_root))
    {
      bv->used = bv->total - bv->available_to_root;
      bv->negate_used = (bv->total < bv->available_to_root);
    }
}

static void
add_to_grand_total (struct field_values_t *bv, struct field_values_t *iv)
{
  if (known_value (iv->total))
    grand_fsu.fsu_files += iv->total;
  if (known_value (iv->available))
    grand_fsu.fsu_ffree += iv->available;

  if (known_value (bv->total))
    grand_fsu.fsu_blocks += bv->input_units * bv->total;
  if (known_value (bv->available_to_root))
    grand_fsu.fsu_bfree += bv->input_units * bv->available_to_root;
  if (known_value (bv->available))
    add_uint_with_neg_flag (&grand_fsu.fsu_bavail,
                            &grand_fsu.fsu_bavail_top_bit_set,
                            bv->input_units * bv->available,
                            bv->negate_available);
}


static void
get_dev (char const *disk, char const *mount_point, char const* file,
         char const *stat_file, char const *fstype,
         bool me_dummy, bool me_remote,
         const struct fs_usage *force_fsu,
         bool process_all)
{
  if (me_remote && show_local_fs)
    return;

  if (me_dummy && !show_all_fs && !show_listed_fs)
    return;

  if (!selected_fstype (fstype) || excluded_fstype (fstype))
    return;
  if (!force_fsu && mount_point && ! IS_ABSOLUTE_FILE_NAME (mount_point))
    return;

  if (!stat_file)
    stat_file = mount_point ? mount_point : disk;

  struct fs_usage fsu;
  if (force_fsu)
    fsu = *force_fsu;
  else if (get_fs_usage (stat_file, disk, &fsu))
    {
      if (process_all && (errno == EACCES || errno == ENOENT))
        {
          if (! show_all_fs)
            return;

          fstype = "-";
          fsu.fsu_bavail_top_bit_set = false;
          fsu.fsu_blocksize = fsu.fsu_blocks = fsu.fsu_bfree =
          fsu.fsu_bavail = fsu.fsu_files = fsu.fsu_ffree = UINTMAX_MAX;
        }
      else
        {
          error (0, errno, "%s", quotef (stat_file));
          exit_status = EXIT_FAILURE;
          return;
        }
    }
  else if (process_all && show_all_fs)
    {
      struct stat sb;
      if (stat (stat_file, &sb) == 0)
        {
          struct mount_entry const * dev_me = me_for_dev (sb.st_dev);
          if (dev_me && ! STREQ (dev_me->me_devname, disk)
              && (! dev_me->me_remote || ! me_remote))
            {
              fstype = "-";
              fsu.fsu_bavail_top_bit_set = false;
              fsu.fsu_blocksize = fsu.fsu_blocks = fsu.fsu_bfree =
              fsu.fsu_bavail = fsu.fsu_files = fsu.fsu_ffree = UINTMAX_MAX;
            }
        }
    }

  if (fsu.fsu_blocks == 0 && !show_all_fs && !show_listed_fs)
    return;

  if (! force_fsu)
    file_systems_processed = true;

  alloc_table_row ();

  if (! disk)
    disk = "-";			

  if (! file)
    file = "-";			

  char *dev_name = xstrdup (disk);
  char *resolved_dev;
  if (process_all
      && has_uuid_suffix (dev_name)
      && (resolved_dev = canonicalize_filename_mode (dev_name, CAN_EXISTING)))
    {
      free (dev_name);
      dev_name = resolved_dev;
    }

  if (! fstype)
    fstype = "-";		

  struct field_values_t block_values;
  struct field_values_t inode_values;
  get_field_values (&block_values, &inode_values, &fsu);
  if (print_grand_total && ! force_fsu)
    add_to_grand_total (&block_values, &inode_values);

  size_t col;
  for (col = 0; col < ncolumns; col++)
    {
      char buf[LONGEST_HUMAN_READABLE + 2];
      char *cell;

      struct field_values_t *v;
      switch (columns[col]->field_type)
        {
        case BLOCK_FLD:
          v = &block_values;
          break;
        case INODE_FLD:
          v = &inode_values;
          break;
        case OTHER_FLD:
          v = NULL;
          break;
        default:
          v = NULL; 
          assert (!"bad field_type");
        }

      switch (columns[col]->field)
        {
        case SOURCE_FIELD:
          cell = xstrdup (dev_name);
          break;

        case FSTYPE_FIELD:
          cell = xstrdup (fstype);
          break;

        case SIZE_FIELD:
        case ITOTAL_FIELD:
          cell = xstrdup (df_readable (false, v->total, buf,
                                       v->input_units, v->output_units));
          break;

        case USED_FIELD:
        case IUSED_FIELD:
          cell = xstrdup (df_readable (v->negate_used, v->used, buf,
                                       v->input_units, v->output_units));
          break;

        case AVAIL_FIELD:
        case IAVAIL_FIELD:
          cell = xstrdup (df_readable (v->negate_available, v->available, buf,
                                       v->input_units, v->output_units));
          break;

        case PCENT_FIELD:
        case IPCENT_FIELD:
          {
            double pct = -1;
            if (! known_value (v->used) || ! known_value (v->available))
              ;
            else if (!v->negate_used
                     && v->used <= TYPE_MAXIMUM (uintmax_t) / 100
                     && v->used + v->available != 0
                     && (v->used + v->available < v->used)
                     == v->negate_available)
              {
                uintmax_t u100 = v->used * 100;
                uintmax_t nonroot_total = v->used + v->available;
                pct = u100 / nonroot_total + (u100 % nonroot_total != 0);
              }
            else
              {
                double u = v->negate_used ? - (double) - v->used : v->used;
                double a = v->negate_available
                           ? - (double) - v->available : v->available;
                double nonroot_total = u + a;
                if (nonroot_total)
                  {
                    long int lipct = pct = u * 100 / nonroot_total;
                    double ipct = lipct;
                    if (ipct - 1 < pct && pct <= ipct + 1)
                      pct = ipct + (ipct < pct);
                  }
              }

            if (0 <= pct)
              {
                if (asprintf (&cell, "%.0f%%", pct) == -1)
                  cell = NULL;
              }
            else
              cell = strdup ("-");

            if (!cell)
              xalloc_die ();

            break;
          }

        case FILE_FIELD:
          cell = xstrdup (file);
          break;

        case TARGET_FIELD:
#ifdef HIDE_AUTOMOUNT_PREFIX
          if (STRNCMP_LIT (mount_point, "/auto/") == 0)
            mount_point += 5;
          else if (STRNCMP_LIT (mount_point, "/tmp_mnt/") == 0)
            mount_point += 8;
#endif
          cell = xstrdup (mount_point);
          break;

        default:
          assert (!"unhandled field");
        }

      if (!cell)
        assert (!"empty cell");

      replace_problematic_chars (cell);
      size_t cell_width = mbswidth (cell, 0);
      columns[col]->width = MAX (columns[col]->width, cell_width);
      table[nrows - 1][col] = cell;
    }
  free (dev_name);
}

static char *
last_device_for_mount (char const* mount)
{
  struct mount_entry const *me;
  struct mount_entry const *le = NULL;

  for (me = mount_list; me; me = me->me_next)
    {
      if (STREQ (me->me_mountdir, mount))
        le = me;
    }

  if (le)
    {
      char *devname = le->me_devname;
      char *canon_dev = canonicalize_file_name (devname);
      if (canon_dev && IS_ABSOLUTE_FILE_NAME (canon_dev))
        return canon_dev;
      free (canon_dev);
      return xstrdup (le->me_devname);
    }
  else
    return NULL;
}

static bool
get_disk (char const *disk)
{
  struct mount_entry const *me;
  struct mount_entry const *best_match = NULL;
  bool best_match_accessible = false;
  bool eclipsed_device = false;
  char const *file = disk;

  char *resolved = canonicalize_file_name (disk);
  if (resolved && IS_ABSOLUTE_FILE_NAME (resolved))
    disk = resolved;

  size_t best_match_len = SIZE_MAX;
  for (me = mount_list; me; me = me->me_next)
    {
      char *devname = me->me_devname;
      char *canon_dev = canonicalize_file_name (me->me_devname);
      if (canon_dev && IS_ABSOLUTE_FILE_NAME (canon_dev))
        devname = canon_dev;

      if (STREQ (disk, devname))
        {
          char *last_device = last_device_for_mount (me->me_mountdir);
          eclipsed_device = last_device && ! STREQ (last_device, devname);
          size_t len = strlen (me->me_mountdir);

          if (! eclipsed_device
              && (! best_match_accessible || len < best_match_len))
            {
              struct stat disk_stats;
              bool this_match_accessible = false;

              if (stat (me->me_mountdir, &disk_stats) == 0)
                best_match_accessible = this_match_accessible = true;

              if (this_match_accessible
                  || (! best_match_accessible && len < best_match_len))
                {
                  best_match = me;
                  if (len == 1) 
                    {
                      free (last_device);
                      free (canon_dev);
                      break;
                    }
                  else
                    best_match_len = len;
                }
            }

          free (last_device);
        }

      free (canon_dev);
    }

  free (resolved);

  if (best_match)
    {
      get_dev (best_match->me_devname, best_match->me_mountdir, file, NULL,
               best_match->me_type, best_match->me_dummy,
               best_match->me_remote, NULL, false);
      return true;
    }
  else if (eclipsed_device)
    {
      error (0, 0, _("cannot access %s: over-mounted by another device"),
             quoteaf (file));
      exit_status = EXIT_FAILURE;
      return true;
    }

  return false;
}
static void
get_point (char const *point, const struct stat *statp)
{
  struct stat disk_stats;
  struct mount_entry *me;
  struct mount_entry const *best_match = NULL;
  char *resolved = canonicalize_file_name (point);
  if (resolved && resolved[0] == '/')
    {
      size_t resolved_len = strlen (resolved);
      size_t best_match_len = 0;

      for (me = mount_list; me; me = me->me_next)
        {
          if (!STREQ (me->me_type, "lofs")
              && (!best_match || best_match->me_dummy || !me->me_dummy))
            {
              size_t len = strlen (me->me_mountdir);
              if (best_match_len <= len && len <= resolved_len
                  && (len == 1 
                      || ((len == resolved_len || resolved[len] == '/')
                          && STREQ_LEN (me->me_mountdir, resolved, len))))
                {
                  best_match = me;
                  best_match_len = len;
                }
            }
        }
    }
  free (resolved);
  if (best_match
      && (stat (best_match->me_mountdir, &disk_stats) != 0
          || disk_stats.st_dev != statp->st_dev))
    best_match = NULL;

  if (! best_match)
    for (me = mount_list; me; me = me->me_next)
      {
        if (me->me_dev == (dev_t) -1)
          {
            if (stat (me->me_mountdir, &disk_stats) == 0)
              me->me_dev = disk_stats.st_dev;
            else
              {
                if (errno == EIO)
                  {
                    error (0, errno, "%s", quotef (me->me_mountdir));
                    exit_status = EXIT_FAILURE;
                  }

                me->me_dev = (dev_t) -2;
              }
          }

        if (statp->st_dev == me->me_dev
            && !STREQ (me->me_type, "lofs")
            && (!best_match || best_match->me_dummy || !me->me_dummy))
          {
            if (stat (me->me_mountdir, &disk_stats) != 0
                || disk_stats.st_dev != me->me_dev)
              me->me_dev = (dev_t) -2;
            else
              best_match = me;
          }
      }

  if (best_match)
    get_dev (best_match->me_devname, best_match->me_mountdir, point, point,
             best_match->me_type, best_match->me_dummy, best_match->me_remote,
             NULL, false);
  else
    {
      char *mp = find_mount_point (point, statp);
      if (mp)
        {
          get_dev (NULL, mp, point, NULL, NULL, false, false, NULL, false);
          free (mp);
        }
    }
}

static void
get_entry (char const *name, struct stat const *statp)
{
  if ((S_ISBLK (statp->st_mode) || S_ISCHR (statp->st_mode))
      && get_disk (name))
    return;

  get_point (name, statp);
}

static void
get_all_entries (void)
{
  struct mount_entry *me;

  filter_mount_list (show_all_fs);

  for (me = mount_list; me; me = me->me_next)
    get_dev (me->me_devname, me->me_mountdir, NULL, NULL, me->me_type,
             me->me_dummy, me->me_remote, NULL, true);
}

static void
add_fs_type (char const *fstype)
{
  struct fs_type_list *fsp;

  fsp = xmalloc (sizeof *fsp);
  fsp->fs_name = (char *) fstype;
  fsp->fs_next = fs_select_list;
  fs_select_list = fsp;
}


static void
add_excluded_fs_type (char const *fstype)
{
  struct fs_type_list *fsp;

  fsp = xmalloc (sizeof *fsp);
  fsp->fs_name = (char *) fstype;
  fsp->fs_next = fs_exclude_list;
  fs_exclude_list = fsp;
}

void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("Usage: %s [OPTION]... [FILE]...\n"), program_name);
      fputs (_("\
Show information about the file system on which each FILE resides,\n\
or all file systems by default.\n\
"), stdout);

      emit_mandatory_arg_note ();

      fputs (_("\
  -a, --all             include pseudo, duplicate, inaccessible file systems\n\
  -B, --block-size=SIZE  scale sizes by SIZE before printing them; e.g.,\n\
                           '-BM' prints sizes in units of 1,048,576 bytes;\n\
                           see SIZE format below\n\
  -h, --human-readable  print sizes in powers of 1024 (e.g., 1023M)\n\
  -H, --si              print sizes in powers of 1000 (e.g., 1.1G)\n\
"), stdout);
      fputs (_("\
  -i, --inodes          list inode information instead of block usage\n\
  -k                    like --block-size=1K\n\
  -l, --local           limit listing to local file systems\n\
      --no-sync         do not invoke sync before getting usage info (default)\
\n\
"), stdout);
      fputs (_("\
      --output[=FIELD_LIST]  use the output format defined by FIELD_LIST,\n\
                               or print all fields if FIELD_LIST is omitted.\n\
  -P, --portability     use the POSIX output format\n\
      --sync            invoke sync before getting usage info\n\
"), stdout);
      fputs (_("\
      --total           elide all entries insignificant to available space,\n\
                          and produce a grand total\n\
"), stdout);
      fputs (_("\
  -t, --type=TYPE       limit listing to file systems of type TYPE\n\
  -T, --print-type      print file system type\n\
  -x, --exclude-type=TYPE   limit listing to file systems not of type TYPE\n\
  -v                    (ignored)\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_blocksize_note ("DF");
      emit_size_note ();
      fputs (_("\n\
FIELD_LIST is a comma-separated list of columns to be included.  Valid\n\
field names are: 'source', 'fstype', 'itotal', 'iused', 'iavail', 'ipcent',\n\
'size', 'used', 'avail', 'pcent', 'file' and 'target' (see info page).\n\
"), stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

int
main (int argc, char **argv)
{
  struct stat *stats IF_LINT ( = 0);

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);

  fs_select_list = NULL;
  fs_exclude_list = NULL;
  show_all_fs = false;
  show_listed_fs = false;
  human_output_opts = -1;
  print_type = false;
  file_systems_processed = false;
  exit_status = EXIT_SUCCESS;
  print_grand_total = false;
  grand_fsu.fsu_blocksize = 1;

  bool posix_format = false;

  char const *msg_mut_excl = _("options %s and %s are mutually exclusive");

  while (true)
    {
      int oi = -1;
      int c = getopt_long (argc, argv, "aB:iF:hHklmPTt:vx:", long_options,
                           &oi);
      if (c == -1)
        break;

      switch (c)
        {
        case 'a':
          show_all_fs = true;
          break;
        case 'B':
          {
            enum strtol_error e = human_options (optarg, &human_output_opts,
                                                 &output_block_size);
            if (e != LONGINT_OK)
              xstrtol_fatal (e, oi, c, long_options, optarg);
          }
          break;
        case 'i':
          if (header_mode == OUTPUT_MODE)
            {
              error (0, 0, msg_mut_excl, "-i", "--output");
              usage (EXIT_FAILURE);
            }
          header_mode = INODES_MODE;
          break;
        case 'h':
          human_output_opts = human_autoscale | human_SI | human_base_1024;
          output_block_size = 1;
          break;
        case 'H':
          human_output_opts = human_autoscale | human_SI;
          output_block_size = 1;
          break;
        case 'k':
          human_output_opts = 0;
          output_block_size = 1024;
          break;
        case 'l':
          show_local_fs = true;
          break;
        case 'm':
          human_output_opts = 0;
          output_block_size = 1024 * 1024;
          break;
        case 'T':
          if (header_mode == OUTPUT_MODE)
            {
              error (0, 0, msg_mut_excl, "-T", "--output");
              usage (EXIT_FAILURE);
            }
          print_type = true;
          break;
        case 'P':
          if (header_mode == OUTPUT_MODE)
            {
              error (0, 0, msg_mut_excl, "-P", "--output");
              usage (EXIT_FAILURE);
            }
          posix_format = true;
          break;
        case SYNC_OPTION:
          require_sync = true;
          break;
        case NO_SYNC_OPTION:
          require_sync = false;
          break;

        case 'F':
        case 't':
          add_fs_type (optarg);
          break;

        case 'v':		
          break;
        case 'x':
          add_excluded_fs_type (optarg);
          break;

        case OUTPUT_OPTION:
          if (header_mode == INODES_MODE)
            {
              error (0, 0, msg_mut_excl, "-i", "--output");
              usage (EXIT_FAILURE);
            }
          if (posix_format && header_mode == DEFAULT_MODE)
            {
              error (0, 0, msg_mut_excl, "-P", "--output");
              usage (EXIT_FAILURE);
            }
          if (print_type)
            {
              error (0, 0, msg_mut_excl, "-T", "--output");
              usage (EXIT_FAILURE);
            }
          header_mode = OUTPUT_MODE;
          if (optarg)
            decode_output_arg (optarg);
          break;

        case TOTAL_OPTION:
          print_grand_total = true;
          break;

        case_GETOPT_HELP_CHAR;
        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          usage (EXIT_FAILURE);
        }
    }

  if (human_output_opts == -1)
    {
      if (posix_format)
        {
          human_output_opts = 0;
          output_block_size = (getenv ("POSIXLY_CORRECT") ? 512 : 1024);
        }
      else
        human_options (getenv ("DF_BLOCK_SIZE"),
                       &human_output_opts, &output_block_size);
    }

  if (header_mode == INODES_MODE || header_mode == OUTPUT_MODE)
    ;
  else if (human_output_opts & human_autoscale)
    header_mode = HUMAN_MODE;
  else if (posix_format)
    header_mode = POSIX_MODE;
  {
    bool match = false;
    struct fs_type_list *fs_incl;
    for (fs_incl = fs_select_list; fs_incl; fs_incl = fs_incl->fs_next)
      {
        struct fs_type_list *fs_excl;
        for (fs_excl = fs_exclude_list; fs_excl; fs_excl = fs_excl->fs_next)
          {
            if (STREQ (fs_incl->fs_name, fs_excl->fs_name))
              {
                error (0, 0,
                       _("file system type %s both selected and excluded"),
                       quote (fs_incl->fs_name));
                match = true;
                break;
              }
          }
      }
    if (match)
      return EXIT_FAILURE;
  }

  assume (0 < optind);

  if (optind < argc)
    {
      stats = xnmalloc (argc - optind, sizeof *stats);
      for (int i = optind; i < argc; ++i)
        {
          if (stat (argv[i], &stats[i - optind]))
            {
              error (0, errno, "%s", quotef (argv[i]));
              exit_status = EXIT_FAILURE;
              argv[i] = NULL;
            }
          else if (! S_ISFIFO (stats[i - optind].st_mode))
            {
              int fd = open (argv[i], O_RDONLY | O_NOCTTY);
              if (0 <= fd)
                close (fd);
            }
        }
    }

  mount_list =
    read_file_system_list ((fs_select_list != NULL
                            || fs_exclude_list != NULL
                            || print_type
                            || field_data[FSTYPE_FIELD].used
                            || show_local_fs));

  if (mount_list == NULL)
    {
      int status = 0;
      if ( ! (optind < argc)
           || (show_all_fs
               || show_local_fs
               || fs_select_list != NULL
               || fs_exclude_list != NULL))
        {
          status = EXIT_FAILURE;
        }
      char const *warning = (status == 0 ? _("Warning: ") : "");
      error (status, errno, "%s%s", warning,
             _("cannot read table of mounted file systems"));
    }

  if (require_sync)
    sync ();

  get_field_list ();
  get_header ();

  if (optind < argc)
    {
      show_listed_fs = true;

      for (int i = optind; i < argc; ++i)
        if (argv[i])
          get_entry (argv[i], &stats[i - optind]);

      IF_LINT (free (stats));
    }
  else
    get_all_entries ();

  if (file_systems_processed)
    {
      if (print_grand_total)
        get_dev ("total",
                 (field_data[SOURCE_FIELD].used ? "-" : "total"),
                 NULL, NULL, NULL, false, false, &grand_fsu, false);

      print_table ();
    }
  else
    {
      if (exit_status == EXIT_SUCCESS)
        die (EXIT_FAILURE, 0, _("no file systems processed"));
    }

  IF_LINT (free (columns));

  return exit_status;
}
