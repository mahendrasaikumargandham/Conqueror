#ifndef COPY_H
# define COPY_H

# include <stdbool.h>
# include "hash.h"

struct selabel_handle;
enum Sparse_type
{
  SPARSE_UNUSED,
  SPARSE_NEVER,
  SPARSE_AUTO,
  SPARSE_ALWAYS
};
enum Reflink_type
{
  REFLINK_NEVER,
  REFLINK_AUTO,
  REFLINK_ALWAYS
};
enum Interactive
{
  I_ALWAYS_YES = 1,
  I_ALWAYS_NO,
  I_ASK_USER,
  I_UNSPECIFIED
};
enum Dereference_symlink
{
  DEREF_UNDEFINED = 1,
  DEREF_NEVER,
  DEREF_COMMAND_LINE_ARGUMENTS,
  DEREF_ALWAYS
};

# define VALID_SPARSE_MODE(Mode)	\
  ((Mode) == SPARSE_NEVER		\
   || (Mode) == SPARSE_AUTO		\
   || (Mode) == SPARSE_ALWAYS)

# define VALID_REFLINK_MODE(Mode)	\
  ((Mode) == REFLINK_NEVER		\
   || (Mode) == REFLINK_AUTO		\
   || (Mode) == REFLINK_ALWAYS)
struct cp_options
{
  enum backup_type backup_type;
  enum Dereference_symlink dereference;
  enum Interactive interactive;
  enum Sparse_type sparse_mode;
  mode_t mode;
  bool copy_as_regular;
  bool unlink_dest_before_opening;
  bool unlink_dest_after_failed_open;
  bool hard_link;
  bool move_mode;
  bool install_mode;
  bool chown_privileges;
  bool owner_privileges;
  bool one_file_system;
  bool preserve_ownership;
  bool preserve_mode;
  bool preserve_timestamps;
  bool explicit_no_preserve_mode;
  struct selabel_handle *set_security_context;
  bool preserve_links;
  bool data_copy_required;
  bool require_preserve;
  bool preserve_security_context;
  bool require_preserve_context;
  bool preserve_xattr;
  bool require_preserve_xattr;
  bool reduce_diagnostics;
  bool recursive;
  bool set_mode;
  bool symbolic_link;
  bool update;
  bool verbose;
  bool stdin_tty;
  bool open_dangling_dest_symlink;
  bool last_file;
  int rename_errno;
  enum Reflink_type reflink_mode;
  Hash_table *dest_info;
  Hash_table *src_info;
};
# if RENAME_TRAILING_SLASH_BUG
int rpl_rename (char const *, char const *);
#  undef rename
#  define rename rpl_rename
# endif

bool copy (char const *src_name, char const *dst_name,
           bool nonexistent_dst, const struct cp_options *options,
           bool *copy_into_self, bool *rename_succeeded);

extern bool set_process_security_ctx (char const *src_name,
                                      char const *dst_name,
                                      mode_t mode, bool new_dst,
                                      const struct cp_options *x);

extern bool set_file_security_ctx (char const *dst_name,
                                   bool recurse, const struct cp_options *x);

void dest_info_init (struct cp_options *);
void src_info_init (struct cp_options *);

void cp_options_default (struct cp_options *);
bool chown_failure_ok (struct cp_options const *) _GL_ATTRIBUTE_PURE;
mode_t cached_umask (void);

#endif
