/* IBM device driver utility functions.			Author: Kees J. Bot
 *								7 Dec 1995
 * Entry point:
 *   partition:	partition a disk to the partition table(s) on it.
 */

#include "driver.h"
#include "drvlib.h"
#include <unistd.h>

/* Extended partition? */
#define ext_part(s)	((s) == 0x05 || (s) == 0x0F)

FORWARD _PROTOTYPE( void extpartition, (struct driver *dp, int extdev,
						unsigned long extbase) );
FORWARD _PROTOTYPE( int get_part_table, (struct driver *dp, int device,
			unsigned long offset, struct part_entry *table));
FORWARD _PROTOTYPE( void sort, (struct part_entry *table) );

#ifndef CD_SECTOR_SIZE
#define CD_SECTOR_SIZE 2048
#endif 

/*============================================================================*
 *				partition				      *
 *============================================================================*/
// 读取设备分区表
PUBLIC void partition(dp, device, style, atapi)
struct driver *dp;	/* device dependent entry points */
int device;		/* device to partition */
int style;		/* partitioning style: floppy, primary, sub. */
int atapi;		/* atapi device */
{
/* This routine is called on first open to initialize the partition tables
 * of a device.  It makes sure part_limitthat each partition falls safely within the
 * device's limits.  Depending on the partition style we are either making
 * floppy partitions, primary partitions or subpartitions.  Only primary
 * partitions are sorted, because they are shared with other operating
 * systems that expect this.
 * 在首次打开时调用此例程以初始化设备的分区表。 确保每个分区都安全地位于设备的限制之内。
 *  根据分区样式，我们可以制作软盘分区，主分区或子分区。
 *  仅对主分区进行排序，因为它们与期望该分区的其他操作系统共享。
 */
  struct part_entry table[NR_PARTITIONS], *pe;
  int disk, par;
  struct device *dv;
  unsigned long base, limit, part_limit;

  /* Get the geometry of the device to partition */
  if ((dv = (*dp->dr_prepare)(device)) == NIL_DEV
				|| cmp64u(dv->dv_size, 0) == 0) return;
  base = div64u(dv->dv_base, SECTOR_SIZE);
  limit = base + div64u(dv->dv_size, SECTOR_SIZE);

  /* Read the partition table for the device. */
  if(!get_part_table(dp, device, 0L, table)) {
	  return;
  }

  /* Compute the device number of the first partition. */
  switch (style) {
  case P_FLOPPY:
	device += MINOR_fd0p0;
	break;
  case P_PRIMARY:
	sort(table);		/* sort a primary partition table */
	device += 1;
	break;
  case P_SUB:
    // 磁盘设备号
	disk = device / DEV_PER_DRIVE;
    // 分区设备号
	par = device % DEV_PER_DRIVE - 1;
    // 子分区对应设备号
	device = MINOR_d0p0s0 + (disk * NR_PARTITIONS + par) * NR_PARTITIONS;
  }

  /* Find an array of devices. */
  // 计算起始位置
  if ((dv = (*dp->dr_prepare)(device)) == NIL_DEV) return;

  /* Set the geometry of the partitions from the partition table. */
  for (par = 0; par < NR_PARTITIONS; par++, dv++) {
	/* Shrink the partition to fit within the device. */
    // 分区大小肯定在上级分区以内
	pe = &table[par];
	part_limit = pe->lowsec + pe->size;
	if (part_limit < pe->lowsec) part_limit = limit;
	if (part_limit > limit) part_limit = limit;
	if (pe->lowsec < base) pe->lowsec = base;
	if (part_limit < pe->lowsec) part_limit = pe->lowsec;

    // 设置分区逻辑位置（bytes）
	dv->dv_base = mul64u(pe->lowsec, SECTOR_SIZE);
	dv->dv_size = mul64u(part_limit - pe->lowsec, SECTOR_SIZE);

	if (style == P_PRIMARY) {
		/* Each Minix primary partition can be subpartitioned. */
        // 设置子分区
		if (pe->sysind == MINIX_PART)
			partition(dp, device + par, P_SUB, atapi);

		/* An extended partition has logical partitions. */
        // 拓展分区
		if (ext_part(pe->sysind))
			extpartition(dp, device + par, pe->lowsec);
	}
  }
}

/*============================================================================*
 *				extpartition				      *
 *============================================================================*/
PRIVATE void extpartition(dp, extdev, extbase)
struct driver *dp;	/* device dependent entry points */
int extdev;		/* extended partition to scan */
unsigned long extbase;	/* sector offset of the base extended partition */
{
/* Extended partitions cannot be ignored alas, because people like to move
 * files to and from DOS partitions.  Avoid reading this code, it's no fun.
 */
  struct part_entry table[NR_PARTITIONS], *pe;
  int subdev, disk, par;
  struct device *dv;
  unsigned long offset, nextoffset;

  disk = extdev / DEV_PER_DRIVE;
  par = extdev % DEV_PER_DRIVE - 1;
  subdev = MINOR_d0p0s0 + (disk * NR_PARTITIONS + par) * NR_PARTITIONS;

  offset = 0;
  do {
	if (!get_part_table(dp, extdev, offset, table)) return;
	sort(table);

	/* The table should contain one logical partition and optionally
	 * another extended partition.  (It's a linked list.)
	 */
	nextoffset = 0;
	for (par = 0; par < NR_PARTITIONS; par++) {
		pe = &table[par];
		if (ext_part(pe->sysind)) {
			nextoffset = pe->lowsec;
		} else
		if (pe->sysind != NO_PART) {
			if ((dv = (*dp->dr_prepare)(subdev)) == NIL_DEV) return;

			dv->dv_base = mul64u(extbase + offset + pe->lowsec,
								SECTOR_SIZE);
			dv->dv_size = mul64u(pe->size, SECTOR_SIZE);

			/* Out of devices? */
			if (++subdev % NR_PARTITIONS == 0) return;
		}
	}
  } while ((offset = nextoffset) != 0);
}

/*============================================================================*
 *				get_part_table				      *
 *============================================================================*/
PRIVATE int get_part_table(dp, device, offset, table)
struct driver *dp;
int device;
unsigned long offset;		/* sector offset to the table */
struct part_entry *table;	/* four entries */
{
/* Read the partition table for the device, return true iff there were no
 * errors.
 */
  iovec_t iovec1;
  off_t position;
  static unsigned char partbuf[CD_SECTOR_SIZE];

  position = offset << SECTOR_SHIFT;
  iovec1.iov_addr = (vir_bytes) partbuf;
  iovec1.iov_size = CD_SECTOR_SIZE;
  if ((*dp->dr_prepare)(device) != NIL_DEV) {
	(void) (*dp->dr_transfer)(SELF, DEV_GATHER, position, &iovec1, 1);
  }
  if (iovec1.iov_size != 0) {
	return 0;
  }
  // 判断是不是分区表
  if (partbuf[510] != 0x55 || partbuf[511] != 0xAA) {
	/* Invalid partition table. */
	return 0;
  }
  // 复制磁盘里分区表结构
  memcpy(table, (partbuf + PART_TABLE_OFF), NR_PARTITIONS * sizeof(table[0]));
  return 1;
}

/*===========================================================================*
 *				sort					     *
 *===========================================================================*/
PRIVATE void sort(table)
struct part_entry *table;
{
/* Sort a partition table. */
  struct part_entry *pe, tmp;
  int n = NR_PARTITIONS;

  do {
    // 简单排序 
    // 非法分区放在最后
	for (pe = table; pe < table + NR_PARTITIONS-1; pe++) {
		if (pe[0].sysind == NO_PART
			|| (pe[0].lowsec > pe[1].lowsec
					&& pe[1].sysind != NO_PART)) {
			tmp = pe[0]; pe[0] = pe[1]; pe[1] = tmp;
		}
	}
  } while (--n > 0);
}
