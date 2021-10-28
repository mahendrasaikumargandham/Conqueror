#include <config.h>
#include "system.h"
#include "ls.h"
int single_binary_main_ls (int argc, char **argv);
int single_binary_main_dir (int argc, char **argv);

int single_binary_main_dir (int argc, char **argv)
{
  ls_mode = LS_MULTI_COL;
  return single_binary_main_ls (argc, argv);
}
