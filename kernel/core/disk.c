#include "core/disk.h"
#include "core/mbr.h"
#include "fs/vfs.h"

#include "util/list.h"
#include "util/math.h"
#include "util/mem.h"
#include "util/panic.h"
#include "util/printk.h"

#include "core/ahci.h"
#include "mm/vmm.h"

#include "config.h"

#define disk_debg(f, ...) pdebg("Disk: (0x%x) " f, disk, ##__VA_ARGS__)
#define disk_info(f, ...) pinfo("Disk: (0x%x) " f, disk, ##__VA_ARGS__)
#define disk_fail(f, ...) pfail("Disk: (0x%x) " f, disk, ##__VA_ARGS__)

#define DISK_DEFAULT_SECTOR_SIZE 512
disk_t *disk_first = NULL;

disk_part_t *disk_part_next(disk_t *disk, disk_part_t *part) {
  if (NULL == part)
    return disk->parts;
  return part->next;
}

disk_part_t *disk_part_add(disk_t *disk, uint64_t start, uint64_t size) {
#define disk_part_match(p1, _) (p1->start == start && p1->size == size)

  if (NULL == disk)
    return NULL;

  disk_part_t *new = NULL;

  // check if the partition exists
  slist_find(&disk->parts, &new, disk_part_match, NULL, disk_part_t);

  if (NULL != new)
    return new;

  // if not, then create a new one
  if (NULL == (new = vmm_alloc(sizeof(disk_part_t))))
    return NULL;

  bzero(new, sizeof(disk_part_t));
  new->start = start;
  new->size  = size;
  new->disk  = disk;

  slist_add(&disk->parts, new, disk_part_t);

  disk->part_count++;
  return new;
}

void __disk_part_block(disk_t *disk) {
  if (NULL == disk)
    return;

  disk_part_t *trav = disk->parts;

  while (NULL != trav) {
    trav->available = false;
    trav            = trav->next;
  }
}

vfs_t *__disk_part_find_vfs(disk_part_t *part) {
  vfs_t *cur = NULL;

  while (NULL != (cur = vfs_next(cur)))
    if (cur->type == VFS_TYPE_DISK && cur->type_data == part)
      return cur;

  return NULL;
}

void disk_part_clear(disk_t *disk) {
  if (NULL == disk)
    return;

  disk_part_t *trav = disk->parts, *pre = NULL;
  vfs_t       *cur = NULL;

  while (trav != NULL) {
    cur = __disk_part_find_vfs(trav);

    if (trav->available) {
      if (NULL == cur)
        vfs_register(VFS_TYPE_DISK, trav);

      pre  = trav;
      trav = trav->next;
      continue;
    }

    if (NULL != cur)
      vfs_unregister(cur);
    disk->part_count--;

    if (NULL == pre) {
      disk->parts = trav->next;
      vmm_free(trav);
      trav = disk->parts;
      continue;
    }

    pre->next = trav->next;
    vmm_free(trav);
    trav = pre->next;
  }
}

// should be called if the disk gets modified as well
bool disk_scan(disk_t *disk) {
  disk->available = false;
  __disk_part_block(disk);

  if (!disk_do(disk, DISK_OP_INFO, 0, 0, NULL)) {
    disk_fail("failed to load the disk information");
    return false;
  }

#ifdef CONFIG_CORE_GPT
#include "core/gpt.h"
  if (gpt_load(disk)) {
    disk_info("loaded %d GPT partitions", disk->part_count);
    goto done;
  }
#endif

  if (mbr_load(disk)) {
    disk_info("loaded %d MBR partitions", disk->part_count);
    goto done;
  }

  disk_fail("failed to load the disk partitions");
  return false;

done:
  disk_part_clear(disk);
  disk->available = true;
  return true;
}

disk_t *disk_add(disk_controller_t controller, void *data) {
  if (NULL == data)
    return false;

  disk_t *new = vmm_alloc(sizeof(disk_t));
  bzero(new, sizeof(disk_t));

  new->controller  = controller;
  new->data        = data;
  new->sector_size = DISK_DEFAULT_SECTOR_SIZE;

  slist_add(&disk_first, new, disk_t);
  pdebg("Disk: Added a new disk device (Address: 0x%x Controller: %d)", new, new->controller);

  return new;
}

void disk_remove(disk_t *disk) {
  if (NULL == disk || NULL == disk_first)
    return;

  slist_del(&disk_first, disk, disk_t);
  vmm_free(disk);

  return;
}

disk_t *disk_next(disk_t *disk) {
  if (NULL == disk)
    return disk_first;
  return disk->next;
}

/*

 * the disk controllers only allow us to read/write starting from a certain LBA
 * and only allow us to read/write sectors

 * disk_do makes this easier by allowing us to read/write to any offset
 * and allow us to read/write any amounts of data

 * to do so disk_do uses few different functions for abstraction:
 * __disk_do_raw: directly calls controller's functions
 * __disk_do_size: allows us to read/write any amount of data instead of sectors
 * disk_do: allows us to read/write any amount of data from/to any offset

*/
bool __disk_do_raw(disk_t *disk, disk_op_t op, uint64_t lba, uint64_t sector_count, uint8_t *buf) {
  typedef bool port_do_t(void *data, disk_op_t op, uint64_t offset, uint64_t sector_count, uint8_t *buf);

  port_do_t *port_do = NULL;

  switch (disk->controller) {
  case DISK_CONTROLLER_AHCI:
    port_do = (void *)ahci_port_do;
    break;

  default:
    disk_fail("unknown controller (%d)", disk->controller);
    panic(__func__, "Encountered a disk with an unknown controller");
    return false;
  }

  return port_do(disk->data, op, lba, sector_count, buf);
}

bool __disk_do_size(disk_t *disk, disk_op_t op, uint64_t lba, uint64_t size, uint8_t *buf) {
  uint64_t rem = 0, buf_offset = 0, sector_count = div_floor(size, disk->sector_size);

  if ((rem = size % disk->sector_size) == 0)
    return __disk_do_raw(disk, op, lba, sector_count, buf);

  if (size < disk->sector_size)
    goto do_copy;

  while (size != rem) {
    if (!__disk_do_raw(disk, op, lba, 1, buf + buf_offset))
      return false;
    size -= disk->sector_size;
    buf_offset += disk->sector_size;
  }

do_copy:
  if (rem == 0)
    return true;

  uint8_t cb[disk->sector_size];
  bool    ret = false;

  ret = __disk_do_raw(disk, op, lba, 1, cb);

  memcpy(buf + buf_offset, cb, rem);
  return ret;
}

bool disk_do(disk_t *disk, disk_op_t op, uint64_t offset, uint64_t size, uint8_t *buf) {
  // TODO: use any offset instead of LBA
  return __disk_do_size(disk, op, offset, size, buf);
}