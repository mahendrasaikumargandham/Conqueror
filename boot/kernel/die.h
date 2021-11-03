#ifndef DIE_H
# define DIE_H
# include <error.h>
# include <stdbool.h>
# include <verify.h>
# define die(status, ...) \
  verify_expr (status, (error (status, __VA_ARGS__), assume (false)))
#endif 
