/* This file handles signals, which are asynchronous events and are generally
 * a messy and unpleasant business.  Signals can be generated by the KILL
 * system call, or from the keyboard (SIGINT) or from the clock (SIGALRM).
 * In all cases control eventually passes to check_sig() to see which processes
 * can be signaled.  The actual signaling is done by sig_proc().
 *
 * The entry points into this file are:
 *   do_sigaction:	perform the SIGACTION system call
 *   do_sigpending:	perform the SIGPENDING system call
 *   do_sigprocmask:	perform the SIGPROCMASK system call
 *   do_sigreturn:	perform the SIGRETURN system call
 *   do_sigsuspend:	perform the SIGSUSPEND system call
 *   do_kill:		perform the KILL system call
 *   do_pause:		perform the PAUSE system call
 *   process_ksig:	process a signal an behalf of the kernel
 *   sig_proc:		interrupt or terminate a signaled process
 *   check_sig:		check which processes to signal with sig_proc()
 *   check_pending:	check if a pending signal can now be delivered
 *   restart_sigs: 	restart signal work after finishing a VFS call
 */

#include "pm.h"
#include <sys/stat.h>
#include <sys/ptrace.h>
#include <minix/callnr.h>
#include <minix/endpoint.h>
#include <minix/com.h>
#include <minix/vm.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/sigcontext.h>
#include <string.h>
#include "mproc.h"
#include "param.h"

FORWARD _PROTOTYPE( void unpause, (struct mproc *rmp)			);
FORWARD _PROTOTYPE( int sig_send, (struct mproc *rmp, int signo)	);
FORWARD _PROTOTYPE( void sig_proc_exit, (struct mproc *rmp, int signo)	);

/*===========================================================================*
 *				do_sigaction				     *
 *===========================================================================*/
PUBLIC int do_sigaction()
{
  int r;
  struct sigaction svec;
  struct sigaction *svp;

  if (m_in.sig_nr == SIGKILL) return(OK);
  if (m_in.sig_nr < 1 || m_in.sig_nr >= _NSIG) return(EINVAL);

  svp = &mp->mp_sigact[m_in.sig_nr];
  if ((struct sigaction *) m_in.sig_osa != (struct sigaction *) NULL) {
	r = sys_datacopy(PM_PROC_NR,(vir_bytes) svp,
		who_e, (vir_bytes) m_in.sig_osa, (phys_bytes) sizeof(svec));
	if (r != OK) return(r);
  }

  if ((struct sigaction *) m_in.sig_nsa == (struct sigaction *) NULL) 
  	return(OK);

  /* Read in the sigaction structure. */
  r = sys_datacopy(who_e, (vir_bytes) m_in.sig_nsa,
		PM_PROC_NR, (vir_bytes) &svec, (phys_bytes) sizeof(svec));
  if (r != OK) return(r);

  if (svec.sa_handler == SIG_IGN) {
	(void) sigaddset(&mp->mp_ignore, m_in.sig_nr);
	(void) sigdelset(&mp->mp_sigpending, m_in.sig_nr);
	(void) sigdelset(&mp->mp_ksigpending, m_in.sig_nr);
	(void) sigdelset(&mp->mp_catch, m_in.sig_nr);
  } else if (svec.sa_handler == SIG_DFL) {
	(void) sigdelset(&mp->mp_ignore, m_in.sig_nr);
	(void) sigdelset(&mp->mp_catch, m_in.sig_nr);
  } else {
	(void) sigdelset(&mp->mp_ignore, m_in.sig_nr);
	(void) sigaddset(&mp->mp_catch, m_in.sig_nr);
  }
  mp->mp_sigact[m_in.sig_nr].sa_handler = svec.sa_handler;
  (void) sigdelset(&svec.sa_mask, SIGKILL);
  (void) sigdelset(&svec.sa_mask, SIGSTOP);
  mp->mp_sigact[m_in.sig_nr].sa_mask = svec.sa_mask;
  mp->mp_sigact[m_in.sig_nr].sa_flags = svec.sa_flags;
  mp->mp_sigreturn = (vir_bytes) m_in.sig_ret;
  return(OK);
}

/*===========================================================================*
 *				do_sigpending                                *
 *===========================================================================*/
PUBLIC int do_sigpending()
{
  mp->mp_reply.reply_mask = (long) mp->mp_sigpending;
  return OK;
}

/*===========================================================================*
 *				do_sigprocmask                               *
 *===========================================================================*/
PUBLIC int do_sigprocmask()
{
/* Note that the library interface passes the actual mask in sigmask_set,
 * not a pointer to the mask, in order to save a copy.  Similarly,
 * the old mask is placed in the return message which the library
 * interface copies (if requested) to the user specified address.
 *
 * The library interface must set SIG_INQUIRE if the 'act' argument
 * is NULL.
 *
 * KILL and STOP can't be masked.
 */

  int i;

  mp->mp_reply.reply_mask = (long) mp->mp_sigmask;

  switch (m_in.sig_how) {
      case SIG_BLOCK:
	(void) sigdelset((sigset_t *)&m_in.sig_set, SIGKILL);
	(void) sigdelset((sigset_t *)&m_in.sig_set, SIGSTOP);
	for (i = 1; i < _NSIG; i++) {
		if (sigismember((sigset_t *)&m_in.sig_set, i))
			(void) sigaddset(&mp->mp_sigmask, i);
	}
	break;

      case SIG_UNBLOCK:
	for (i = 1; i < _NSIG; i++) {
		if (sigismember((sigset_t *)&m_in.sig_set, i))
			(void) sigdelset(&mp->mp_sigmask, i);
	}
	check_pending(mp);
	break;

      case SIG_SETMASK:
	(void) sigdelset((sigset_t *) &m_in.sig_set, SIGKILL);
	(void) sigdelset((sigset_t *) &m_in.sig_set, SIGSTOP);
	mp->mp_sigmask = (sigset_t) m_in.sig_set;
	check_pending(mp);
	break;

      case SIG_INQUIRE:
	break;

      default:
	return(EINVAL);
	break;
  }
  return OK;
}

/*===========================================================================*
 *				do_sigsuspend                                *
 *===========================================================================*/
PUBLIC int do_sigsuspend()
{
  mp->mp_sigmask2 = mp->mp_sigmask;	/* save the old mask */
  mp->mp_sigmask = (sigset_t) m_in.sig_set;
  (void) sigdelset(&mp->mp_sigmask, SIGKILL);
  (void) sigdelset(&mp->mp_sigmask, SIGSTOP);
  mp->mp_flags |= SIGSUSPENDED;
  check_pending(mp);
  return(SUSPEND);
}

/*===========================================================================*
 *				do_sigreturn				     *
 *===========================================================================*/
PUBLIC int do_sigreturn()
{
/* A user signal handler is done.  Restore context and check for
 * pending unblocked signals.
 */

  int r;

  mp->mp_sigmask = (sigset_t) m_in.sig_set;
  (void) sigdelset(&mp->mp_sigmask, SIGKILL);
  (void) sigdelset(&mp->mp_sigmask, SIGSTOP);

  r = sys_sigreturn(who_e, (struct sigmsg *) m_in.sig_context);
  check_pending(mp);
  return(r);
}

/*===========================================================================*
 *				do_kill					     *
 *===========================================================================*/
PUBLIC int do_kill()
{
/* Perform the kill(pid, signo) system call. */

  return check_sig(m_in.pid, m_in.sig_nr, FALSE /* ksig */);
}

/*===========================================================================*
 *			      do_srv_kill				     *
 *===========================================================================*/
PUBLIC int do_srv_kill()
{
/* Perform the srv_kill(pid, signo) system call. */

  /* Only RS is allowed to use srv_kill. */
  if (mp->mp_endpoint != RS_PROC_NR)
	return EPERM;

  /* Pretend the signal comes from the kernel when RS wants to deliver a signal
   * to a system process. RS sends a SIGKILL when it wants to perform cleanup.
   * In that case, ksig == TRUE forces PM to exit the process immediately.
   */
  return check_sig(m_in.pid, m_in.sig_nr, TRUE /* ksig */);
}

/*===========================================================================*
 *				process_ksig				     *
 *===========================================================================*/
PUBLIC int process_ksig(endpoint_t proc_nr_e, int signo)
{
  register struct mproc *rmp;
  int proc_nr;
  pid_t proc_id, id;

  if(pm_isokendpt(proc_nr_e, &proc_nr) != OK || proc_nr < 0) {
	printf("PM: process_ksig: %d?? not ok\n", proc_nr_e);
	return EDEADEPT; /* process is gone. */
  }
  rmp = &mproc[proc_nr];
  if ((rmp->mp_flags & (IN_USE | EXITING)) != IN_USE) {
#if 0
	printf("PM: process_ksig: %d?? exiting / not in use\n", proc_nr_e);
#endif
	return EDEADEPT; /* process is gone. */
  }
  proc_id = rmp->mp_pid;
  mp = &mproc[0];			/* pretend signals are from PM */
  mp->mp_procgrp = rmp->mp_procgrp;	/* get process group right */

  /* For SIGVTALRM and SIGPROF, see if we need to restart a
   * virtual timer. For SIGINT, SIGWINCH and SIGQUIT, use proc_id 0
   * to indicate a broadcast to the recipient's process group.  For
   * SIGKILL, use proc_id -1 to indicate a systemwide broadcast.
   */
  switch (signo) {
      case SIGINT:
      case SIGQUIT:
      case SIGWINCH:
  	id = 0; break;	/* broadcast to process group */
      case SIGVTALRM:
      case SIGPROF:
      	check_vtimer(proc_nr, signo);
      	/* fall-through */
      default:
  	id = proc_id;
  	break;
  }
  check_sig(id, signo, TRUE /* ksig */);

  /* If SIGSNDELAY is set, an earlier sys_stop() failed because the process was
   * still sending, and the kernel hereby tells us that the process is now done
   * with that. We can now try to resume what we planned to do in the first
   * place: set up a signal handler. However, the process's message may have
   * been a call to PM, in which case the process may have changed any of its
   * signal settings. The process may also have forked, exited etcetera.
   */
  if (signo == SIGSNDELAY && (rmp->mp_flags & DELAY_CALL)) {
	rmp->mp_flags &= ~DELAY_CALL;

	/*
	 * If the VFS_CALL flag is still set we have a process which is stopped
	 * and we only need to wait for a reply from VFS. We are going to check
	 * the pending signal then
	 */
	if (rmp->mp_flags & VFS_CALL)
		return OK;
	if (rmp->mp_flags & PM_SIG_PENDING)
		panic("process_ksig: bad process state");

	/* Process as many normal signals as possible. */
	check_pending(rmp);

	if (rmp->mp_flags & DELAY_CALL)
		panic("process_ksig: multiple delay calls?");
  }
  
  /* See if the process is still alive */
  if ((mproc[proc_nr].mp_flags & (IN_USE | EXITING)) == IN_USE)  {
      return OK; /* signal has been delivered */
  }
  else {
      return EDEADEPT; /* process is gone */
  }
}

/*===========================================================================*
 *				do_pause				     *
 *===========================================================================*/
PUBLIC int do_pause()
{
/* Perform the pause() system call. */

  mp->mp_flags |= PAUSED;
  return(SUSPEND);
}

/*===========================================================================*
 *				sig_proc				     *
 *===========================================================================*/
PUBLIC void sig_proc(rmp, signo, trace, ksig)
register struct mproc *rmp;	/* pointer to the process to be signaled */
int signo;			/* signal to send to process (1 to _NSIG-1) */
int trace;			/* pass signal to tracer first? */
int ksig;			/* non-zero means signal comes from kernel  */
{
/* Send a signal to a process.  Check to see if the signal is to be caught,
 * ignored, tranformed into a message (for system processes) or blocked.  
 *  - If the signal is to be transformed into a message, request the KERNEL to
 * send the target process a system notification with the pending signal as an 
 * argument. 
 *  - If the signal is to be caught, request the KERNEL to push a sigcontext 
 * structure and a sigframe structure onto the catcher's stack.  Also, KERNEL 
 * will reset the program counter and stack pointer, so that when the process 
 * next runs, it will be executing the signal handler. When the signal handler 
 * returns,  sigreturn(2) will be called.  Then KERNEL will restore the signal 
 * context from the sigcontext structure.
 * If there is insufficient stack space, kill the process.
 */
  int r, slot, badignore;

  slot = (int) (rmp - mproc);
  if ((rmp->mp_flags & (IN_USE | EXITING)) != IN_USE) {
	printf("PM: signal %d sent to exiting process %d\n", signo, slot);
	panic("");
  }

  if (trace == TRUE && rmp->mp_tracer != NO_TRACER && signo != SIGKILL) {
	/* Signal should be passed to the debugger first.
	 * This happens before any checks on block/ignore masks; otherwise,
	 * the process itself could block/ignore debugger signals.
	 */

	(void) sigaddset(&rmp->mp_sigtrace, signo);

	if (!(rmp->mp_flags & STOPPED))
		stop_proc(rmp, signo);	/* a signal causes it to stop */

	return;
  }

  if (rmp->mp_flags & VFS_CALL) {
/* FIXME: check if process could be stopped for job control; what to do? */
	(void) sigaddset(&rmp->mp_sigpending, signo);
	if(ksig)
		(void) sigaddset(&rmp->mp_ksigpending, signo);

	if (!(rmp->mp_flags & PM_SIG_PENDING)) {
		/* No delay calls: VFS_CALL implies the process called us. */
		if ((r = sys_stop(rmp->mp_endpoint)) != OK)
			panic("sys_stop failed: %d", r);

		rmp->mp_flags |= PM_SIG_PENDING;
	}

	return;
  }

  /* Handle system signals for system processes first. */
  if(rmp->mp_flags & PRIV_PROC) {
   	/* Always skip signals for PM (only necessary when broadcasting). */
   	if(rmp->mp_endpoint == PM_PROC_NR) {
 		return;
   	}

   	/* System signals have always to go through the kernel first to let it
   	 * pick the right signal manager. If PM is the assigned signal manager,
   	 * the signal will come back and will actually be processed.
   	 */
   	if(!ksig) {
 		sys_kill(rmp->mp_endpoint, signo);
 		return;
   	}

  	/* Print stacktrace if necessary. */
  	if(SIGS_IS_STACKTRACE(signo)) {
 		sys_sysctl_stacktrace(rmp->mp_endpoint);
  	}

  	if(!SIGS_IS_TERMINATION(signo)) {
		/* Translate every non-termination sys signal into a message. */
		message m;
		m.m_type = SIGS_SIGNAL_RECEIVED;
		m.SIGS_SIG_NUM = signo;
		asynsend3(rmp->mp_endpoint, &m, AMF_NOREPLY);
	}
	else {
		/* Exit the process in case of termination system signal. */
		sig_proc_exit(rmp, signo);
	}
	return;
  }

  /* Handle user processes now. First check if SIGSTOP or analoguous will
   * erase pending SIGCONT, or conversely if SIGCONT will erase pending stop.
   */
#if defined(_POSIX_JOB_CONTROL) && _POSIX_JOB_CONTROL > 0
  if (sigismember(&stop_sset,signo)&&sigismember(&rmp->mp_sigpending,SIGCONT)){
	(void) sigdelset(&rmp->mp_sigpending,SIGCONT);
	(void) sigdelset(&rmp->mp_ksigpending,SIGCONT);
  }
  if (signo==SIGCONT) {
	(void) sigdelset(&rmp->mp_sigpending,SIGSTOP);
	(void) sigdelset(&rmp->mp_ksigpending,SIGSTOP);
	(void) sigdelset(&rmp->mp_sigpending,SIGTSTP);
	(void) sigdelset(&rmp->mp_ksigpending,SIGTSTP);
	(void) sigdelset(&rmp->mp_sigpending,SIGTTIN);
	(void) sigdelset(&rmp->mp_ksigpending,SIGTTIN);
	(void) sigdelset(&rmp->mp_sigpending,SIGTTOU);
	(void) sigdelset(&rmp->mp_ksigpending,SIGTTOU);

	if (rmp->mp_flags & JOBCTL_STOPPED) {
		if ((r = sys_resume(rmp->mp_endpoint)) != OK)
		  	panic("sys_resume failed: %d", r);
		rmp->mp_flags &= ~JOBCTL_STOPPED;
		rmp->mp_stoppingsig = 0;
	}
  }
#endif

  /* See if the signal cannot be safely ignored. */
  badignore = ksig && sigismember(&noign_sset, signo) && (
	  sigismember(&rmp->mp_ignore, signo) ||
	  sigismember(&rmp->mp_sigmask, signo));

  if (!badignore && sigismember(&rmp->mp_ignore, signo)) { 
	/* Signal should be ignored. */
	return;
  }
  if (!badignore && sigismember(&rmp->mp_sigmask, signo)) {
	/* Signal should be blocked. */
	(void) sigaddset(&rmp->mp_sigpending, signo);
	if(ksig)
		(void) sigaddset(&rmp->mp_ksigpending, signo);
	return;
  }

  if ((rmp->mp_flags & STOPPED) && signo != SIGKILL) {
	/* If the process is stopped for a debugger, do not deliver any signals
	 * (except SIGKILL) in order not to confuse the debugger. The signals
	 * will be delivered using the check_pending() calls in do_trace().
	 */
	(void) sigaddset(&rmp->mp_sigpending, signo);
	if(ksig)
		(void) sigaddset(&rmp->mp_ksigpending, signo);
	return;
  }
  if (!badignore && sigismember(&rmp->mp_catch, signo)) {
	/* Signal is caught. First interrupt the process's current call, if
	 * applicable. This may involve a roundtrip to VFS, in which case we'll
	 * have to check back later.
	 */
	if (!(rmp->mp_flags & UNPAUSED)) {
		unpause(rmp);

		if (!(rmp->mp_flags & UNPAUSED)) {
			/* not yet unpaused; continue later */
			(void) sigaddset(&rmp->mp_sigpending, signo);
			if(ksig)
				(void) sigaddset(&rmp->mp_ksigpending, signo);

			return;
		}
	}

	/* Then send the actual signal to the process, by setting up a signal
	 * handler.
	 */
	if (sig_send(rmp, signo))
		return;

	/* We were unable to spawn a signal handler. Kill the process. */
	printf("PM: %d can't catch signal %d - killing\n",
		rmp->mp_pid, signo);
  }
  else if (!badignore && sigismember(&ign_sset, signo)) {
	/* Signal defaults to being ignored. */
	return;
  }
#if defined(_POSIX_JOB_CONTROL) && _POSIX_JOB_CONTROL > 0
  else if (!badignore && sigismember(&stop_sset, signo)) {
	/* Signal defaults to stop process. */
/* FIXME: consider sys_delay_stop */
	if ((r = sys_stop(rmp->mp_endpoint)) != OK)
	  	panic("sys_stop failed: %d", r);
	if (! (rmp->mp_flags & JOBCTL_STOPPED) ) {
		register struct mproc *rpmp;

		rmp->mp_flags |= JOBCTL_STOPPED;
		rmp->mp_stoppingsig = signo;

		if (rmp->mp_parent <= 0)
			panic("sig_proc`STOP: bad value in mp_parent: %d",
					rmp->mp_parent);
		rpmp = &mproc[rmp->mp_parent];
		if (wait_test(rpmp, rmp, WAITING_UNTRC))
			/* parent was waiting with WUNTRACED option: */
			tell_parent_untraced(rmp);
		/* We ought to signal the parent with SIGCHLD... */
	}
	return;
  }
#endif

  /* Terminate process */
  sig_proc_exit(rmp, signo);
}

/*===========================================================================*
 *				sig_proc_exit				     *
 *===========================================================================*/
PRIVATE void sig_proc_exit(rmp, signo)
struct mproc *rmp;		/* process that must exit */
int signo;			/* signal that caused termination */
{
  rmp->mp_sigstatus = (char) signo;
  if (sigismember(&core_sset, signo)) {
	if(!(rmp->mp_flags & PRIV_PROC)) {
		printf("PM: coredump signal %d for %d / %s\n", signo,
			rmp->mp_pid, rmp->mp_name);
		sys_sysctl_stacktrace(rmp->mp_endpoint);
	}
	exit_proc(rmp, 0, TRUE /*dump_core*/);
  }
  else {
  	exit_proc(rmp, 0, FALSE /*dump_core*/);
  }
}

/*===========================================================================*
 *				check_sig				     *
 *===========================================================================*/
PUBLIC int check_sig(proc_id, signo, ksig)
pid_t proc_id;			/* pid of proc to sig, or 0 or -1, or -pgrp */
int signo;			/* signal to send to process (0 to _NSIG-1) */
int ksig;			/* non-zero means signal comes from kernel  */
{
/* Check to see if it is possible to send a signal.  The signal may have to be
 * sent to a group of processes.  This routine is invoked by the KILL system
 * call, and also when the kernel catches a DEL or other signal.
 */

  register struct mproc *rmp;
  int count;			/* count # of signals sent */
  int error_code;

  if (signo < 0 || signo >= _NSIG) return(EINVAL);

  /* Return EINVAL for attempts to send SIGKILL to INIT alone. */
  if (proc_id == INIT_PID && signo == SIGKILL) return(EINVAL);

  /* Signal RS first when broadcasting SIGTERM. */
  if (proc_id == -1 && signo == SIGTERM)
      sys_kill(RS_PROC_NR, signo);

  /* Search the proc table for processes to signal. Start from the end of the
   * table to analyze core system processes at the end when broadcasting.
   * (See forkexit.c about pid magic.)
   */
  count = 0;
  error_code = ESRCH;
  for (rmp = &mproc[NR_PROCS-1]; rmp >= &mproc[0]; rmp--) {
	if (!(rmp->mp_flags & IN_USE)) continue;

	/* Check for selection. */
	if (proc_id > 0 && proc_id != rmp->mp_pid) continue;
	if (proc_id == 0 && mp->mp_procgrp != rmp->mp_procgrp) continue;
	if (proc_id == -1 && rmp->mp_pid <= INIT_PID) continue;
	if (proc_id < -1 && rmp->mp_procgrp != -proc_id) continue;

	/* Do not kill servers and drivers when broadcasting SIGKILL. */
	if (proc_id == -1 && signo == SIGKILL &&
		(rmp->mp_flags & PRIV_PROC)) continue;

	/* Disallow lethal signals sent by user processes to sys processes. */
	if (!ksig && SIGS_IS_LETHAL(signo) && (rmp->mp_flags & PRIV_PROC)) {
	    error_code = EPERM;
	    continue;
	}

	/* Check for permission. */
	if (mp->mp_effuid != SUPER_USER
	    && mp->mp_realuid != rmp->mp_realuid
	    && mp->mp_effuid != rmp->mp_realuid
	    && mp->mp_realuid != rmp->mp_effuid
	    && mp->mp_effuid != rmp->mp_effuid
	    && (signo!=SIGCONT || mp->mp_session!=rmp->mp_session) ) {
		error_code = EPERM;
		continue;
	}

	count++;
	if (signo == 0 || (rmp->mp_flags & EXITING)) continue;

	/* 'sig_proc' will handle the disposition of the signal.  The
	 * signal may be caught, blocked, ignored, or cause process
	 * termination, possibly with core dump.
	 */
	sig_proc(rmp, signo, TRUE /*trace*/, ksig);

	if (proc_id > 0) break;	/* only one process being signaled */
  }

  /* If the calling process has killed itself, don't reply. */
  if ((mp->mp_flags & (IN_USE | EXITING)) != IN_USE) return(SUSPEND);
  return(count > 0 ? OK : error_code);
}

/*===========================================================================*
 *				check_pending				     *
 *===========================================================================*/
PUBLIC void check_pending(rmp)
register struct mproc *rmp;
{
  /* Check to see if any pending signals have been unblocked. Deliver as many
   * of them as we can, until we have to wait for a reply from VFS first.
   *
   * There are several places in this file where the signal mask is
   * changed.  At each such place, check_pending() should be called to
   * check for newly unblocked signals.
   */

  int i;
  int ksig;

  for (i = 1; i < _NSIG; i++) {
	if (sigismember(&rmp->mp_sigpending, i) &&
		!sigismember(&rmp->mp_sigmask, i)) {
		ksig = sigismember(&rmp->mp_ksigpending, i);
		(void) sigdelset(&rmp->mp_sigpending, i);
		(void) sigdelset(&rmp->mp_ksigpending, i);
		sig_proc(rmp, i, FALSE /*trace*/, ksig);

		if (rmp->mp_flags & VFS_CALL)
			break;
	}
  }
}

/*===========================================================================*
 *				restart_sigs				     *
 *===========================================================================*/
PUBLIC void restart_sigs(rmp)
struct mproc *rmp;
{
/* VFS has replied to a request from us; do signal-related work.
 */
  int r;

  if (rmp->mp_flags & (VFS_CALL | EXITING)) return;

  if (rmp->mp_flags & TRACE_EXIT) {
	/* Tracer requested exit with specific exit value */
	exit_proc(rmp, rmp->mp_exitstatus, FALSE /*dump_core*/);
  }
  else if (rmp->mp_flags & PM_SIG_PENDING) {
	/* We saved signal(s) for after finishing a VFS call. Deal with this.
	 * PM_SIG_PENDING remains set to indicate the process is still stopped.
	 */
	check_pending(rmp);

	/* The process may now be VFS-blocked again, because a signal exited the
	 * process or was caught. Restart the process only when this is NOT the
	 * case.
	 */
	if (!(rmp->mp_flags & VFS_CALL)) {
		rmp->mp_flags &= ~(PM_SIG_PENDING | UNPAUSED);
/* FIXME: process could be stopped for job control or tracing... */

		if ((r = sys_resume(rmp->mp_endpoint)) != OK)
			panic("sys_resume failed: %d", r);
	}
  }
}

/*===========================================================================*
 *				unpause					     *
 *===========================================================================*/
PRIVATE void unpause(rmp)
struct mproc *rmp;		/* which process */
{
/* A signal is to be sent to a process.  If that process is hanging on a
 * system call, the system call must be terminated with EINTR.  Possible
 * calls are PAUSE, WAIT, READ and WRITE, the latter two for pipes and ttys.
 * First check if the process is hanging on an PM call.  If not, tell VFS,
 * so it can check for READs and WRITEs from pipes, ttys and the like.
 */
  message m;
  int r;

  /* If we're already waiting for a delayed call, don't do anything now. */
  if (rmp->mp_flags & DELAY_CALL)
	return;

  /* Check to see if process is hanging on a PAUSE, WAIT or SIGSUSPEND call. */
  if (rmp->mp_flags & (PAUSED | WAITING | SIGSUSPENDED)) {
	/* Stop process from running. No delay calls: it called us. */
	if ((r = sys_stop(rmp->mp_endpoint)) != OK)
		panic("sys_stop failed: %d", r);

	rmp->mp_flags |= UNPAUSED;

	/* We interrupt the actual call from sig_send() below. */
	return;
  }

  /* Not paused in PM. Let VFS try to unpause the process. */
  if (!(rmp->mp_flags & PM_SIG_PENDING)) {
	/* Stop process from running. */
	r = sys_delay_stop(rmp->mp_endpoint);

	/* If the process is still busy sending a message, the kernel will give
	 * us EBUSY now and send a SIGSNDELAY to the process as soon as sending
	 * is done.
	 */
	if (r == EBUSY) {
		rmp->mp_flags |= DELAY_CALL;

		return;
	}
	else if (r != OK) panic("sys_stop failed: %d", r);

	rmp->mp_flags |= PM_SIG_PENDING;
  }

  m.m_type = PM_UNPAUSE;
  m.PM_PROC = rmp->mp_endpoint;

  tell_vfs(rmp, &m);

  /* Also tell VM. */
  vm_notify_sig_wrapper(rmp->mp_endpoint);
}

/*===========================================================================*
 *				sig_send				     *
 *===========================================================================*/
PRIVATE int sig_send(rmp, signo)
struct mproc *rmp;		/* what process to spawn a signal handler in */
int signo;			/* signal to send to process (1 to _NSIG-1) */
{
/* The process is supposed to catch this signal. Spawn a signal handler.
 * Return TRUE if this succeeded, FALSE otherwise.
 */
  struct sigmsg sigmsg;
  vir_bytes cur_sp;
  int r, sigflags, slot;

  if (!(rmp->mp_flags & UNPAUSED))
	panic("sig_send: process not unpaused");

  sigflags = rmp->mp_sigact[signo].sa_flags;
  slot = (int) (rmp - mproc);

  if (rmp->mp_flags & SIGSUSPENDED)
	sigmsg.sm_mask = rmp->mp_sigmask2;
  else
	sigmsg.sm_mask = rmp->mp_sigmask;
  sigmsg.sm_signo = signo;
  sigmsg.sm_sighandler =
	(vir_bytes) rmp->mp_sigact[signo].sa_handler;
  sigmsg.sm_sigreturn = rmp->mp_sigreturn;
  rmp->mp_sigmask |= rmp->mp_sigact[signo].sa_mask;

  if (sigflags & SA_NODEFER)
	(void) sigdelset(&rmp->mp_sigmask, signo);
  else
	(void) sigaddset(&rmp->mp_sigmask, signo);

  if (sigflags & SA_RESETHAND) {
	(void) sigdelset(&rmp->mp_catch, signo);
	rmp->mp_sigact[signo].sa_handler = SIG_DFL;
  }
  (void) sigdelset(&rmp->mp_sigpending, signo);
  (void) sigdelset(&rmp->mp_ksigpending, signo);

  if(vm_push_sig(rmp->mp_endpoint, &cur_sp) != OK)
	return(FALSE);

  sigmsg.sm_stkptr = cur_sp;

  /* Ask the kernel to deliver the signal */
  r = sys_sigsend(rmp->mp_endpoint, &sigmsg);
 /* sys_sigsend can fail legitimately with EFAULT if
  * the process memory can't accomodate the signal handler.
  */
  if(r == EFAULT) {
	return(FALSE);
  }
  /* Other errors are unexpected pm/kernel discrepancies. */
  if (r != OK) {
	panic("sys_sigsend failed: %d", r);
  }

  /* Was the process suspended in PM? Then interrupt the blocking call. */
  if (rmp->mp_flags & (PAUSED | WAITING | SIGSUSPENDED)) {
	rmp->mp_flags &= ~(PAUSED | WAITING | WAITING_UNTRC | SIGSUSPENDED);

	setreply(slot, EINTR);
  }

  /* Was the process stopped just for this signal? Then resume it. */
  if ((rmp->mp_flags & (JOBCTL_STOPPED | PM_SIG_PENDING | UNPAUSED)) == UNPAUSED) {
	rmp->mp_flags &= ~UNPAUSED;

	if ((r = sys_resume(rmp->mp_endpoint)) != OK)
		panic("sys_resume failed: %d", r);
  }

  return(TRUE);
}

/*===========================================================================*
 *				vm_notify_sig_wrapper			     *
 *===========================================================================*/
PUBLIC void vm_notify_sig_wrapper(endpoint_t ep)
{
/* get IPC's endpoint,
 * the reason that we directly get the endpoint
 * instead of from DS server is that otherwise
 * it will cause deadlock between PM, VM and DS.
 */
  struct mproc *rmp;
  endpoint_t ipc_ep = 0;

  for (rmp = &mproc[0]; rmp < &mproc[NR_PROCS]; rmp++) {
	if (!(rmp->mp_flags & IN_USE))
		continue;
	if (!strcmp(rmp->mp_name, "ipc")) {
		ipc_ep = rmp->mp_endpoint;
		vm_notify_sig(ep, ipc_ep);

		return;
	}
  }
}
