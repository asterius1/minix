/* Function prototypes of the FAT file system
 *
 * Auteur: Antoine Leca, aout 2010.
 */

#ifndef FAT_PROTO_H_
#define FAT_PROTO_H_

#ifndef _ANSI_H
#include <ansi.h>
#endif

#ifndef _IPC_H
#error need to #include <minix/ipc.h>		/* message */
#endif

#ifndef FAT_TYPE_H_
#error need to #include "type.h"
#endif

struct buf;
struct inode;

/* cache.c */
_PROTOTYPE( int do_flush, (void)					);
_PROTOTYPE( int do_sync, (void)						);
_PROTOTYPE( void init_cache, (int bufs)					);
_PROTOTYPE( struct buf *get_block, (dev_t dev, block_t block,int only_search));
_PROTOTYPE( void put_block, (struct buf *bp, int block_type)		);
_PROTOTYPE( void rw_scattered, (dev_t dev,
			struct buf **bufq, int bufqsize, int rw_flag)	);
_PROTOTYPE( void zero_block, (struct buf *bp)				);

/* cluster.c */
_PROTOTYPE( struct buf *new_block, (struct inode *rip, off_t position)	);

/* driver.c */
_PROTOTYPE( int dev_open, (endpoint_t driver_e, dev_t dev, int proc,
			   int flags)					);
_PROTOTYPE( void dev_close, (endpoint_t driver_e, dev_t dev)		);
_PROTOTYPE( int do_new_driver, (void)					);
_PROTOTYPE( int scattered_dev_io,(int op, iovec_t[], u64_t pos, int cnt));
_PROTOTYPE( int seqblock_dev_io, (int op, void *, u64_t pos, int cnt)	);

/* inode.c */
_PROTOTYPE( struct inode *init_inode, (void)				);
_PROTOTYPE( struct inode *find_inode, (ino_t ino_nr)			);
_PROTOTYPE( void get_inode, (struct inode *ino)				);
_PROTOTYPE( void put_inode, (struct inode *ino)				);
_PROTOTYPE( void link_inode, (struct inode *parent, struct inode *ino)	);
_PROTOTYPE( void unlink_inode, (struct inode *ino)			);
_PROTOTYPE( struct inode *get_free_inode, (void)			);
_PROTOTYPE( int have_free_inode, (void)					);
_PROTOTYPE( int have_used_inode, (void)					);
_PROTOTYPE( int do_putnode, (void)					);

/* lookup.c */
_PROTOTYPE( int do_lookup, (void)					);

/* main.c */
_PROTOTYPE( int do_new_driver, (void)					);
_PROTOTYPE( int do_nothing, (void)					);
_PROTOTYPE( int readonly, (void)					);
_PROTOTYPE( int no_sys, (void)						);
_PROTOTYPE( void reply, (int, message *)				);

/* mount.c */
_PROTOTYPE( int do_readsuper, (void)					);
_PROTOTYPE( int do_unmount, (void)					);

/* readwrite.c */
_PROTOTYPE( int do_readwrite, (void)					);
_PROTOTYPE( int do_blockrw, (void)					);
_PROTOTYPE( int do_getdents, (void)					);

/* stat.c */
_PROTOTYPE( mode_t get_mode, (struct inode *ino, int mode)		);
_PROTOTYPE( int do_stat, (void)						);
_PROTOTYPE( int do_chmod, (void)					);
_PROTOTYPE( int do_utime, (void)					);

/* statfs.c */
_PROTOTYPE( int do_fstatfs, (void)					);
_PROTOTYPE( int do_statvfs, (void)					);
#endif
