#include <config.h>
#include <sys/types.h>
#include "system.h"
#include "hash.h"
#include "cp-hash.h"
struct Src_to_dest
{
  ino_t st_ino;
  dev_t st_dev;
  char *name;
};

static Hash_table *src_to_dest;
#define INITIAL_TABLE_SIZE 103

static size_t
src_to_dest_hash (void const *x, size_t table_size)
{
  struct Src_to_dest const *p = x;
  return (uintmax_t) p->st_ino % table_size;
}
static bool
src_to_dest_compare (void const *x, void const *y)
{
  struct Src_to_dest const *a = x;
  struct Src_to_dest const *b = y;
  return SAME_INODE (*a, *b) ? true : false;
}

static void
src_to_dest_free (void *x)
{
  struct Src_to_dest *a = x;
  free (a->name);
  free (x);
}
extern void
forget_created (ino_t ino, dev_t dev)
{
  struct Src_to_dest probe;
  struct Src_to_dest *ent;

  probe.st_ino = ino;
  probe.st_dev = dev;
  probe.name = NULL;

  ent = hash_remove (src_to_dest, &probe);
  if (ent)
    src_to_dest_free (ent);
}

extern char *src_to_dest_lookup (ino_t ino, dev_t dev)
{
  struct Src_to_dest ent;
  struct Src_to_dest const *e;
  ent.st_ino = ino;
  ent.st_dev = dev;
  e = hash_lookup (src_to_dest, &ent);
  return e ? e->name : NULL;
}

extern char *
remember_copied (char const *name, ino_t ino, dev_t dev)
{
  struct Src_to_dest *ent;
  struct Src_to_dest *ent_from_table;

  ent = xmalloc (sizeof *ent);
  ent->name = xstrdup (name);
  ent->st_ino = ino;
  ent->st_dev = dev;

  ent_from_table = hash_insert (src_to_dest, ent);
  if (ent_from_table == NULL)
    {
      xalloc_die ();
    }
  if (ent_from_table != ent)
    {
      src_to_dest_free (ent);
      return (char *) ent_from_table->name;
    }
  return NULL;
}

extern void
hash_init (void)
{
  src_to_dest = hash_initialize (INITIAL_TABLE_SIZE, NULL,
                                 src_to_dest_hash,
                                 src_to_dest_compare,
                                 src_to_dest_free);
  if (src_to_dest == NULL)
    xalloc_die ();
}

extern void
forget_all (void)
{
  hash_free (src_to_dest);
}
