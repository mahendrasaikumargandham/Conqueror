#include <config.h>
#include "system.h"
#include "uname.h"
int single_binary_main_uname (int argc, char **argv);
int single_binary_main_arch (int argc, char **argv);

int single_binary_main_arch (int argc, char **argv)
{
  uname_mode = UNAME_ARCH;
  return single_binary_main_uname (argc, argv);
}
