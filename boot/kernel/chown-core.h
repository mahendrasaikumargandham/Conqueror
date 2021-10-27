#ifndef CHOWN_CORE_H
# define CHOWN_CORE_H
# include "dev-ino.h"
enum Change_status
{
  CH_NOT_APPLIED = 1,
  CH_SUCCEEDED,
  CH_FAILED,
  CH_NO_CHANGE_REQUESTED
};

enum Verbosity
{
  V_high,
  V_changes_only,
  V_off
};

struct Chown_option
{
  enum Verbosity verbosity;
  bool recurse;
  struct dev_ino *root_dev_ino;
  bool affect_symlink_referent;
  bool force_silent;
  char *user_name;
  char *group_name;
};

void
chopt_init (struct Chown_option *);

void
chopt_free (struct Chown_option *);

char *
gid_to_name (gid_t) _GL_ATTRIBUTE_MALLOC;

char *
uid_to_name (uid_t) _GL_ATTRIBUTE_MALLOC;

bool
chown_files (char **files, int bit_flags, uid_t uid, gid_t gid, uid_t required_uid, gid_t required_gid, struct Chown_option const *chopt);

#endif 
