# Shared by the freestanding driver and host image/test programs.
EXT4_CONFIG = -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_HAVE_OWN_ERRNO=1 \
              -DCONFIG_DEBUG_PRINTF=0 -DCONFIG_DEBUG_ASSERT=0 \
              -DCONFIG_JOURNALING_ENABLE=1 -DCONFIG_XATTR_ENABLE=1 \
              -DCONFIG_BLOCK_DEV_CACHE_SIZE=64
EXT4_SRC = $(filter-out third_party/lwext4/src/ext4_mkfs.c third_party/lwext4/src/ext4_mbr.c,$(wildcard third_party/lwext4/src/*.c))
EXT4_INC = -Ithird_party/lwext4/include
