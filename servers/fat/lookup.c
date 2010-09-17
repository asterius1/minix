/* This file provides path-to-inode lookup functionality.
 *
 * The entry points into this file are:
 *   do_putnode		perform the PUTNODE file system call
 *   do_mountpoint	perform the MOUNTPOINT file system call
 *   do_lookup		perform the LOOKUP file system request
 *
 * Auteur: Antoine Leca, aout 2010.
 * Updated:
 */

#include "inc.h"

#include <string.h>

#include <sys/stat.h>

#ifdef   COMPAT316
#include "compat.h"	/* MUST come before <minix/sysutil.h> */
#endif
#include <minix/safecopies.h>
#include <minix/syslib.h>	/* sys_safecopies{from,to} */
#include <minix/sysutil.h>	/* panic */

PRIVATE char user_path[PATH_MAX+1];  /* pathname to be processed */

/*
FORWARD _PROTOTYPE( int ltraverse, (struct inode *rip, char *suffix)	);
 */
FORWARD _PROTOTYPE( int parse_path, (ino_t dir, ino_t root, int flags,
	struct inode **res_inop, size_t *offsetp, int *symlinkp)	);

FORWARD _PROTOTYPE( char *get_name, (char *name, char string[NAME_MAX+1]) );

/*===========================================================================*
 *				do_putnode				 *
 *===========================================================================*/
PUBLIC int do_putnode(void)
{
/* Decrease an inode's reference count.
 */
  struct inode *ino;
  int count;

  if ((ino = fetch_inode(m_in.REQ_INODE_NR)) == NULL)
	return EINVAL;

  count = m_in.REQ_COUNT;

  if (count <= 0 || count > ino->i_ref) return EINVAL;

  /* Decrease reference counter, but keep one reference; it will be consumed by
   * put_inode(). */ 
  ino->i_ref -= count - 1;

  put_inode(ino);

  return OK;
}

/*===========================================================================*
 *				do_mountpoint				     *
 *===========================================================================*/
PUBLIC int do_mountpoint(void)
{
/* Register a mount point. Nothing really extraordinary for FAT. */
  struct inode *rip;
  int r = OK;
  mode_t bits;
  
  /* Get the inode from the request msg. Do not increase the inode refcount*/
  /*CHECKME: is it needed? is the file open anyway? should we increase ref? */
  if( (rip = fetch_inode((ino_t) m_in.REQ_INODE_NR)) == NULL)
	  return(EINVAL);
  
  if (rip->i_flags & I_MOUNTPOINT) r = EBUSY;

#if 1
  /* It may not be special. */
  bits = rip->i_mode & S_IFMT;
  if (bits == S_IFCHR || bits == S_IFBLK) r = ENOTDIR;
#else
/* !IS_DIR => ENOTDIR */
#endif

  if(r == OK) rip->i_flags |= I_MOUNTPOINT;

  /* put_inode(rip);	we did not increase the refcount, no need to release*/

  return(r);
}

/*===========================================================================*
 *                             do_lookup				 *
 *===========================================================================*/
PUBLIC int do_lookup(void)
{
/* Perform the LOOKUP file system request. Crack the parameters and defer
 * to parse_path which will crunch the path passed.
 */
  cp_grant_id_t grant, grant2;
  int r, r1, flags, symlinks;
  unsigned int len;
  size_t offset = 0, path_size, cred_size;
/* CHECKME:
 * should be moved to globals? to inode?
 * its use is to be around when used by other ops
 * which might not include all the details...
 * _but_ how can we do the mapping?
 */
  vfs_ucred_t credentials;
  mode_t mask;
  ino_t dir_ino, root_ino;
  struct inode *rip;

  grant		= (cp_grant_id_t) m_in.REQ_GRANT;
  path_size	= (size_t) m_in.REQ_PATH_SIZE; /* Size of the buffer */
  len		= (int) m_in.REQ_PATH_LEN; /* including terminating nul */
  dir_ino	= (ino_t) m_in.REQ_DIR_INO;
  root_ino	= (ino_t) m_in.REQ_ROOT_INO;
  flags		= (int) m_in.REQ_FLAGS;

  /* Check length. */
  if(len > sizeof(user_path)) return(E2BIG);	/* too big for buffer */
  if(len == 0) return(EINVAL);			/* too small */

  /* Copy the pathname and set up caller's user and group id */
  r = sys_safecopyfrom(m_in.m_source, grant, /*offset*/ (vir_bytes) 0, 
            (vir_bytes) user_path, (size_t) len, D);
  if(r != OK) return(r);

  /* Verify this is a null-terminated path. */
  if(user_path[len - 1] != '\0') return(EINVAL);

  if(flags & PATH_GET_UCRED) { /* Do we have to copy uid/gid credentials? */
  	grant2 = (cp_grant_id_t) m_in.REQ_GRANT2;
  	cred_size = (size_t) m_in.REQ_UCRED_SIZE;

  	if (cred_size > sizeof(credentials)) return(EINVAL); /* Too big. */
  	r = sys_safecopyfrom(m_in.m_source, grant2, (vir_bytes) 0,
  			 (vir_bytes) &credentials, cred_size, D);
  	if (r != OK) return(r);
/*
  	caller_uid = credentials.vu_uid;
  	caller_gid = credentials.vu_gid;
 */
  } else {
  	memset(&credentials, 0, sizeof(credentials));
	credentials.vu_uid = m_in.REQ_UID;
	credentials.vu_gid = m_in.REQ_GID;
	credentials.vu_ngroups = 0;
/*
	caller_uid	= (uid_t) m_in.REQ_UID;
	caller_gid	= (gid_t) m_in.REQ_GID;
 */
  }
  mask = get_mask(&credentials);	/* CHEKCME: what is it for? */

  DBGprintf(("FATfs: enter lookup dir=%lo, root=%lo, <%.*s>...\n",
	dir_ino, root_ino, len, user_path));

  /* Lookup inode */
  rip = NULL;
  r = parse_path(dir_ino, root_ino, flags, &rip, &offset, &symlinks);

  if(symlinks != 0 && (r == ELEAVEMOUNT || r == EENTERMOUNT || r == ESYMLINK)){
	len = strlen(user_path)+1;
	if(len > path_size) return(ENAMETOOLONG);

	r1 = sys_safecopyto(m_in.m_source, grant, (vir_bytes) 0,
			(vir_bytes) user_path, (size_t) len, D);
	if(r1 != OK) return(r1);
  }

  if(r == ELEAVEMOUNT || r == ESYMLINK) {
	/* Report offset and the error */
	m_out.RES_OFFSET = offset;
	m_out.RES_SYMLOOP = symlinks;

	return(r);
  }

  if (r != OK && r != EENTERMOUNT) return(r);

  m_out.RES_INODE_NR = INODE_NR(rip);
  m_out.RES_MODE = rip->i_mode;
  m_out.RES_FILE_SIZE_LO = rip->i_size;
  m_out.RES_FILE_SIZE_HI = 0;
  m_out.RES_UID = use_uid;
  m_out.RES_GID = use_gid;
  m_out.RES_SYMLOOP		= symlinks;
  
  /* This is only valid for block and character specials. But it doesn't
   * cause any harm to set RES_DEV always.
   */
  m_out.RES_DEV = dev;

  if(r == EENTERMOUNT) {
	m_out.RES_OFFSET	= offset;
	put_inode(rip); /* Only return a reference to the final object */
  }

  return(r);
}

/*===========================================================================*
 *				get_mask				 *
 *===========================================================================*/
PRIVATE int get_mask(
  vfs_ucred_t *ucred)		/* credentials of the caller */
{
  /* Given the caller's credentials, precompute a search access mask to test
   * against directory modes.
   */
  int i;

  if (ucred->vu_uid == use_uid) return S_IXUSR;

  if (ucred->vu_gid == use_gid) return S_IXGRP;

  for (i = 0; i < ucred->vu_ngroups; i++)
	if (ucred->vu_sgroups[i] == use_gid) return S_IXGRP;

  return S_IXOTH;
}

/*===========================================================================*
 *                             parse_path				   *
 *===========================================================================*/
PRIVATE int parse_path(dir_ino, root_ino, flags, res_inop, offsetp, symlinkp)
ino_t dir_ino;
ino_t root_ino;
int flags;
struct inode **res_inop;
size_t *offsetp;
int *symlinkp;
{
/* Parse the path in user_path, starting at dir_ino. If the path is the empty
 * string, just return dir_ino. It is upto the caller to treat an empty
 * path in a special way. Otherwise, if the path consists of just one or
 * more slash ('/') characters, the path is replaced with ".". Otherwise,
 * just look up the first (or only) component in path after skipping any
 * leading slashes. 
 */
  int r, leaving_mount;
  struct inode *rip, *dir_ip;
  char *cp, *next_cp; /* component and next component */
  char component[NAME_MAX+1];

  /* Start parsing path at the first component in user_path */
  cp = user_path;  

  /* No symlinks encountered yet */
  *symlinkp = 0;

  /* Find starting inode inode according to the request message */
  if((rip = fetch_inode( /*fs_dev,*/ dir_ino)) == NULL) {
	DBGprintf(("FATfs: parse_path cannot locate dir inode %lo...\n", dir_ino));
	return(ENOENT);
  }

  /* If dir has been removed return ENOENT. */
/*CHECKME
  if (rip->i_nlinks == NO_LINK) return(ENOENT);
 */
 
  /* Increase the inode refcount. */
  get_inode(rip);

  /* If the given start inode is a mountpoint, we must be here because the file
   * system mounted on top returned an ELEAVEMOUNT error. In this case, we must
   * only accept ".." as the first path component.
   */
  leaving_mount = rip->i_flags & I_MOUNTPOINT; /* True iff rip is mountpoint*/

  /* Scan the path component by component. */
  while (TRUE) {
	if(cp[0] == '\0') {
		/* We're done; either the path was empty or we've parsed all 
		 components of the path */

DBGprintf(("FATfs: parse path returned inode %lo\n", INODE_NR(rip)));
		
		*res_inop = rip;
		*offsetp += cp - user_path;

		/* Return EENTERMOUNT if we are at a mount point */
		if (rip->i_flags & I_MOUNTPOINT) return(EENTERMOUNT);
		
		return(OK);
	}

	next_cp = get_name(cp, component);

	/* Special code for '..'. A process is not allowed to leave a chrooted
	 * environment. A lookup of '..' at the root of a mounted filesystem
	 * has to return ELEAVEMOUNT. In both cases, the caller needs search
	 * permission for the current inode, as it is used as directory.
	 */
	if(strcmp(component, "..") == 0) {
		/* 'rip' is now accessed as directory */
/*
		if ((r = forbidden(rip, X_BIT)) != OK) {
			put_inode(rip);
			return(r);
		}
 */
/* CHECK INDEX? *? */
		if (rip->i_index == root_ino) {
			cp = next_cp;
			continue;	/* Ignore the '..' at a process' root 
					 and move on to the next component */
		}

		if (IS_ROOT(rip)) {
/* NOTE: in FAT filesystems the root directory does NOT have the . and .. entries */
			/* comment: we cannot be the root FS! */
			/* Climbing up to parent FS */

			put_inode(rip);
			*offsetp += cp - user_path; 
			return(ELEAVEMOUNT);
		}
	}

	/* Only check for a mount point if we are not coming from one. */
	if (!leaving_mount && rip->i_flags & I_MOUNTPOINT) {
		/* Going to enter a child FS */

		*res_inop = rip;
		*offsetp += cp - user_path;
		return(EENTERMOUNT);
	}

	/* There is more path.  Keep parsing.
	 * If we're leaving a mountpoint, skip directory permission checks.
	 */
	dir_ip = rip;
	r = advance(dir_ip, &rip, leaving_mount ? dot2 : component, CHK_PERM);
	if(r == ELEAVEMOUNT || r == EENTERMOUNT)
		r = OK;

	if (r != OK) {
		put_inode(dir_ip);
		return(r);
	}
	leaving_mount = 0;

	/* The call to advance() succeeded.  Fetch next component. */
	put_inode(dir_ip);	/* release the current inode */
	cp = next_cp;
  }
}

/*===========================================================================*
 *				advance					   *
 *===========================================================================*/
PUBLIC int advance(
  struct inode *dirp,		/* inode for directory to be searched */
  struct inode **res_inop,
  char string[NAME_MAX],	/* component name to look for */
  int chk_perm)			/* check permissions when string is looked up*/
{
/* Given a directory and a component of a path, look up the component in
 * the directory, find the inode, open it, and return a pointer to its inode
 * slot.
 */
  int r;
  ino_t numb;
  struct inode *rip;

  DBGprintf(("FATfs: advance in dir=%lo, looking for <%s>...\n",
	INODE_NR(dirp), string));

  /* If 'string' is empty, return an error. */
  if (string[0] == '\0') {
	return(ENOENT);
  }

#if 0
FIXME: changed interface...
  /* Check for NULL. */
  if (dirp == NULL) return(*res_inop = NULL);
#else
  assert(dirp != NULL);
#endif

  /* If 'string' is not present in the directory, signal error. */
  if ( (r = lookup_dir(dirp, string, &rip)) != OK) {
	return(r);
  }

  /* The component has been found in the directory.  Get inode. */
  /* get_inode(rip); */	/* alread done before. */

  r = OK;		/* assume success without comments */

  /* The following test is for "mountpoint/.." where mountpoint is a
   * mountpoint. ".." will refer to the root of the mounted filesystem,
   * but has to become a reference to the parent of the 'mountpoint'
   * directory.
   *
   * This case is recognized by the looked up name pointing to a
   * root inode, and the directory in which it is held being a
   * root inode, _and_ the name[1] being '.'. (This is a test for '..'
   * and excludes '.'.)
   */
  if (rip->i_index == /*ROOT_INODE*/0) {
	if (dirp->i_index == /*ROOT_INODE*/0) {
		if (string[1] == '.') {
			/* comment: we cannot be the root FS! */
			/* Climbing up mountpoint */
			r = ELEAVEMOUNT;
		}
	}
  }

/* REVISE COMMENT! */
  /* See if the inode is mounted on.  If so, switch to root directory of the
   * mounted file system.  The super_block provides the linkage between the
   * inode mounted on and the root directory of the mounted file system.
   */
  if (rip->i_flags & I_MOUNTPOINT) {
	/* Mountpoint encountered, report it */
	r = EENTERMOUNT;
  }
  *res_inop = rip;
  return(r);
}

/*===========================================================================*
 *				get_name				   *
 *===========================================================================*/
PRIVATE char *get_name(
  char *path_name,		/* path name to parse */
  char string[NAME_MAX+1])	/* component extracted from 'old_name' */
{
/* Given a pointer to a path name, 'path_name', copy the first component
 * to 'string' (truncated if necessary, always nul terminated).
 * A pointer to the string after the first component of the name as yet
 * unparsed is returned.  Roughly speaking,
 * 'get_name' = 'path_name' - 'string'.
 *
 * This routine follows the standard convention that /usr/ast, /usr//ast,
 * //usr///ast and /usr/ast/ are all equivalent.
 */
  size_t len;
  char *cp, *ep;

  cp = path_name;

  /* Skip leading slashes */
  while (cp[0] == '/') cp++;

  /* Find the end of the first component */
#if 0
  ep = cp;
  while(ep[0] != '\0' && ep[0] != '/')
	ep++;
#else
  ep = strchr(cp, '/');
  if (ep == NULL) {
	ep = cp + strlen(cp);
	assert(*ep == '\0');
  }
#endif

  len = (size_t) (ep - cp);

/* FIXME: no: we should return ENAMETOOLONG instead! */
  /* Truncate the amount to be copied if it exceeds NAME_MAX */
  if (len > NAME_MAX) len = NAME_MAX;

  /* Special case of the string at cp is empty */
  if (len == 0) 
	strcpy(string, ".");  /* Return "." */
  else {
	memcpy(string, cp, len);
	string[len]= '\0';
  }

  return(ep);
}
