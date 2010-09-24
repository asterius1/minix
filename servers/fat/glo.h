/* Global variables used by the FAT file system
 *
 * Auteur: Antoine Leca, aout 2010.
 */

#ifndef FAT_GLO_H_
#define FAT_GLO_H_

/* EXTERN should be extern except for the table file */
#ifdef _TABLE
#undef EXTERN
#define EXTERN
#endif

EXTERN struct superblock sb;	/* file system fundamental values */

/* User-settable options */
EXTERN int read_only;		/* is the file system mounted read-only? */

EXTERN uid_t use_uid;		/* use this uid as owner of files */
EXTERN gid_t use_gid;		/* use this gid as owner of files */
EXTERN mode_t use_file_mask;	/* show files with this mode */
EXTERN mode_t use_dir_mask;	/* show directories with this mode */

EXTERN int verbose;		/* emit comments on the console; debugging */

#ifdef MORE_OPTIONS
/* The following options are not currently implemented: */
EXTERN int keep_atime;		/* do not update atime and access bits */
EXTERN int dont_calc_freespace;	/* avoid costly operation (full FAT lookup)*/
EXTERN int prevent_exec;	/* prevent files to be executed (chmod a-x)*/
EXTERN int mark_unclean;	/* mark filesystem as unclean while mounted*/
EXTERN uint8_t default_lcase;	/* map the 8.3 name to lowercase? */
EXTERN enum { LFN_IGNORE, LFN_LOOK, LFN_USE } lfn_state;
EXTERN mode_t use_hidden_mask;	/* remove this rwx bits when HIDDEN */
EXTERN mode_t use_system_mask;	/* remove this rwx bits when SYSTEM */
EXTERN uid_t use_system_uid;	/* use this uid as owner of SYSTEM files */
EXTERN gid_t use_system_gid;	/* use this gid as owner of SYSTEM files */
#endif

/* Service internals */
EXTERN enum { NAKED, MOUNTED, UNMOUNTED } state;
EXTERN endpoint_t SELF_E;
EXTERN message m_in, m_out;

/* Communication with VFS */
EXTERN _PROTOTYPE (int (*vfs_req_vec[]), (void) ); /* VFS requests table */

/* Communication with driver */
EXTERN endpoint_t driver_e;	/* driver to use for the device */
EXTERN dev_t dev;		/* device the file system is mounted on */
EXTERN char fs_dev_label[16+1];	/* name of the device driver that is handled
				 * by this FS proc.
				 */
EXTERN int hard_errors;		/* count of hardware errors signalled so far */

/* Buffer cache. */
EXTERN struct bufcache_ctrl bcc; /* buffer cache fundamental values */
EXTERN int bufs_in_use;		/* # bufs currently in use (not on free list)*/
EXTERN int may_use_vmcache;	/* secondary cache using VM */

/* Buffers and associated list pointers are private to cache.c */

/* Inodes array is private to inode.c */

#endif
