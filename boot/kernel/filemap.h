#ifndef _LINUX_FIEMAP_H
# define _LINUX_FIEMAP_H
# include <stdint.h>

struct fiemap_extent
{
  uint64_t fe_logical;
  uint64_t fe_physical;
  uint64_t fe_length;

  uint64_t fe_reserved64[2];
  uint32_t fe_flags;

  uint32_t fe_reserved[3];
};

struct fiemap
{
  uint64_t fm_start;
  uint64_t fm_length;
  uint32_t fm_flags;
  uint32_t fm_mapped_extents;
  uint32_t fm_extent_count;

  uint32_t fm_reserved;
  struct fiemap_extent fm_extents[FLEXIBLE_ARRAY_MEMBER];
};
# define FIEMAP_MAX_OFFSET       (~0ULL)
# define FIEMAP_FLAG_SYNC        0x00000001
# define FIEMAP_FLAG_XATTR       0x00000002

# define FIEMAP_FLAGS_COMPAT     (FIEMAP_FLAG_SYNC | FIEMAP_FLAG_XATTR)
# define FIEMAP_EXTENT_LAST              0x00000001
# define FIEMAP_EXTENT_UNKNOWN           0x00000002
# define FIEMAP_EXTENT_DELALLOC          0x00000004
# define FIEMAP_EXTENT_ENCODED           0x00000008
# define FIEMAP_EXTENT_DATA_ENCRYPTED    0x00000080
# define FIEMAP_EXTENT_NOT_ALIGNED       0x00000100
# define FIEMAP_EXTENT_DATA_INLINE       0x00000200
# define FIEMAP_EXTENT_DATA_TAIL         0x00000400
# define FIEMAP_EXTENT_UNWRITTEN         0x00000800
# define FIEMAP_EXTENT_MERGED		0x00001000
# define FIEMAP_EXTENT_SHARED            0x00002000

#endif
