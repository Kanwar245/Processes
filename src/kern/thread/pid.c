/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process ID management.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <limits.h>
#include <lib.h>
#include <array.h>
#include <clock.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <pid.h>
#include <kern/sysexits.h>

/*
 * Structure for holding PID and return data for a thread.
 *
 * If pi_ppid is INVALID_PID, the parent has gone away and will not be
 * waiting. If pi_ppid is INVALID_PID and pi_exited is true, the
 * structure can be freed.
 */
struct pidinfo {
	pid_t pi_pid;			// process id of this thread
	pid_t pi_ppid;			// process id of parent thread
	volatile bool pi_exited;	// true if thread has exited
	int pi_exitstatus;		// status (only valid if exited)
	struct cv *pi_cv;		// use to wait for thread exit
    int flag;                       // flag of the pid
};


/*
 * Global pid and exit data.
 *
 * The process table is an el-cheapo hash table. It's indexed by
 * (pid % PROCS_MAX), and only allows one process per slot. If a
 * new pid allocation would cause a hash collision, we just don't
 * use that pid.
 */
static struct lock *pidlock;		// lock for global exit data
static struct pidinfo *pidinfo[PROCS_MAX]; // actual pid info
static pid_t nextpid;			// next candidate pid
static int nprocs;			// number of allocated pids



/*
 * Create a pidinfo structure for the specified pid.
 */
static
struct pidinfo *
pidinfo_create(pid_t pid, pid_t ppid)
{
	struct pidinfo *pi;

	KASSERT(pid != INVALID_PID);

	pi = kmalloc(sizeof(struct pidinfo));
	if (pi==NULL) {
		return NULL;
	}

	pi->pi_cv = cv_create("pidinfo cv");
	if (pi->pi_cv == NULL) {
		kfree(pi);
		return NULL;
	}

	pi->pi_pid = pid;
	pi->pi_ppid = ppid;
	pi->pi_exited = false;
	pi->pi_exitstatus = 0xbaad;  /* Recognizably invalid value */

	return pi;
}

/*
 * Clean up a pidinfo structure.
 */
static
void
pidinfo_destroy(struct pidinfo *pi)
{
	KASSERT(pi->pi_exited == true);
	KASSERT(pi->pi_ppid == INVALID_PID);
	cv_destroy(pi->pi_cv);
	kfree(pi);
}

////////////////////////////////////////////////////////////

/*
 * pid_bootstrap: initialize.
 */
void
pid_bootstrap(void)
{
	int i;

	pidlock = lock_create("pidlock");
	if (pidlock == NULL) {
		panic("Out of memory creating pid lock\n");
	}

	/* not really necessary - should start zeroed */
	for (i=0; i<PROCS_MAX; i++) {
		pidinfo[i] = NULL;
	}

	pidinfo[BOOTUP_PID] = pidinfo_create(BOOTUP_PID, INVALID_PID);
	if (pidinfo[BOOTUP_PID]==NULL) {
		panic("Out of memory creating bootup pid data\n");
	}

	nextpid = PID_MIN;
	nprocs = 1;
}

/*
 * pi_get: look up a pidinfo in the process table.
 */
static
struct pidinfo *
pi_get(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(pid>=0);
	KASSERT(pid != INVALID_PID);
	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	if (pi==NULL) {
		return NULL;
	}
	if (pi->pi_pid != pid) {
		return NULL;
	}
	return pi;
}

/*
 * pi_put: insert a new pidinfo in the process table. The right slot
 * must be empty.
 */
static
void
pi_put(pid_t pid, struct pidinfo *pi)
{
	KASSERT(lock_do_i_hold(pidlock));

	KASSERT(pid != INVALID_PID);

	KASSERT(pidinfo[pid % PROCS_MAX] == NULL);
	pidinfo[pid % PROCS_MAX] = pi;
	nprocs++;
}

/*
 * pi_drop: remove a pidinfo structure from the process table and free
 * it. It should reflect a process that has already exited and been
 * waited for.
 */
static
void
pi_drop(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	KASSERT(pi != NULL);
	KASSERT(pi->pi_pid == pid);

	pidinfo_destroy(pi);
	pidinfo[pid % PROCS_MAX] = NULL;
	nprocs--;
}

////////////////////////////////////////////////////////////

/*
 * Helper function for pid_alloc.
 */
static
void
inc_nextpid(void)
{
	KASSERT(lock_do_i_hold(pidlock));

	nextpid++;
	if (nextpid > PID_MAX) {
		nextpid = PID_MIN;
	}
}

/*
 * pid_alloc: allocate a process id.
 */
int
pid_alloc(pid_t *retval)
{
	struct pidinfo *pi;
	pid_t pid;
	int count;

	KASSERT(curthread->t_pid != INVALID_PID);

	/* lock the table */
	lock_acquire(pidlock);

	if (nprocs == PROCS_MAX) {
		lock_release(pidlock);
		return EAGAIN;
	}

	/*
	 * The above test guarantees that this loop terminates, unless
	 * our nprocs count is off. Even so, assert we aren't looping
	 * forever.
	 */
	count = 0;
	while (pidinfo[nextpid % PROCS_MAX] != NULL) {

		/* avoid various boundary cases by allowing extra loops */
		KASSERT(count < PROCS_MAX*2+5);
		count++;

		inc_nextpid();
	}

	pid = nextpid;

	pi = pidinfo_create(pid, curthread->t_pid);
	if (pi==NULL) {
		lock_release(pidlock);
		return ENOMEM;
	}

	pi_put(pid, pi);

	inc_nextpid();

	lock_release(pidlock);

	*retval = pid;
	return 0;
}

/*
 * pid_unalloc - unallocate a process id (allocated with pid_alloc) that
 * hasn't run yet.
 */
void
pid_unalloc(pid_t theirpid)
{
	struct pidinfo *them;

	KASSERT(theirpid >= PID_MIN && theirpid <= PID_MAX);

	lock_acquire(pidlock);

	them = pi_get(theirpid);
	KASSERT(them != NULL);
	KASSERT(them->pi_exited == false);
	KASSERT(them->pi_ppid == curthread->t_pid);

	/* keep pidinfo_destroy from complaining */
	them->pi_exitstatus = 0xdead;
	them->pi_exited = true;
	them->pi_ppid = INVALID_PID;

	pi_drop(theirpid);

	lock_release(pidlock);
}

/*
 * pid_detach - disavows interest in the child thread's exit status, so
 * it can be freed as soon as it exits. May only be called by the
 * parent thread.
 */
int
pid_detach(pid_t childpid)
{
    // set lock
    lock_acquire(pidlock);

    if (childpid < PID_MIN || childpid > PID_MAX) {
        // childpid is invalid
        // exists outside of the min/max values for process ids
        lock_release(pidlock);
        return EINVAL;
    }

    // verify the child exists
    struct pidinfo* getChild = pi_get(childpid);
    if (getChild == NULL) {
        // no child corresponds to that pid
        lock_release(pidlock);
        return ESRCH;
    }

    // verify the child is actually the child of the caller process
    if (getChild->pi_ppid != curthread->t_pid) {
        // this is not the caller's child
        lock_release(pidlock);
        return EINVAL;
    }

    if (childpid == INVALID_PID || childpid == BOOTUP_PID) {
	lock_release(pidlock);
	return EINVAL;
    }

    // verify the child is attached to the caller process
    if (getChild->pi_ppid == INVALID_PID) {
        // this child has already been detached
        lock_release(pidlock);
        return EINVAL;
    }

    // if we reach here, we know the child is valid and should be detached
    // set child's parent to INVALID_PID
    getChild->pi_ppid = INVALID_PID;
    if (getChild->pi_exited) {
        pi_drop(childpid);
    }

    // release lock
    lock_release(pidlock);

    return 0;
}

/*
 * pid_exit
 *  - sets the exit status of this thread (i.e. curthread).
 *  - disowns children.
 *  - if dodetach is true, children are also detached.
 *  - wakes any thread waiting for the curthread to exit.
 *  - frees the PID and exit status if the curthread has been detached.
 *  - must be called only if the thread has had a pid assigned.
 */
void
pid_exit(int status, bool dodetach)
{

    struct pidinfo *pi;

    //set lock
    lock_acquire(pidlock);

    // verify current pid is valid
    pi = pi_get(curthread->t_pid);
    KASSERT(pi != NULL);

    // set exit status
    pi->pi_exited = true;
    pi->pi_exitstatus = status;
    // wake waiting threads
    cv_broadcast(pi->pi_cv, pidlock);

    if (dodetach) {
    // disown children
    	int i;
    	for (i = 0; i < PROCS_MAX; i++) {
            if ((pidinfo[i]) && (pidinfo[i]->pi_ppid == curthread->t_pid)) {
                pid_detach(pidinfo[i]->pi_pid);
            }
        }
    }
    // if I've already been detached
    if (pi->pi_ppid == INVALID_PID) {
        pi_drop(pi->pi_pid);
    }

    // release lock
    lock_release(pidlock);
}

/*
 * pid_join - returns the exit status of the thread associated with
 * targetpid as soon as it is available. If the thread has not yet
 * exited, curthread waits unless the flag WNOHANG is sent.
 *
 */
int
pid_join(pid_t targetpid, int *status, int flags)
{
        // set lock
        lock_acquire(pidlock);

        struct pidinfo *getThread = pi_get(targetpid);

        // verify the pid is valid
        if (getThread == NULL) {
	    // release lock
            lock_release(pidlock);
            return -ESRCH;
        }

        // I am not the parent (or thread already detached)
        if (getThread->pi_ppid != curthread->t_pid) {
            lock_release(pidlock);
            return -EINVAL;
        }

        if (targetpid == INVALID_PID || targetpid == BOOTUP_PID) {
            lock_release(pidlock);
            return -EINVAL;
        }

        // argument targetpid refers to the calling thread
        if (getThread->pi_pid == curthread->t_pid) {
            lock_release(pidlock);
            return -EDEADLK;
        }

        // child has already exited
        if (getThread->pi_exited) {
            if (status != NULL) {
                *status = getThread->pi_exitstatus;
                getThread->pi_ppid = INVALID_PID;
                pi_drop(targetpid);
                lock_release(pidlock);
                //return *status;
                return targetpid;
            } else {  // status was null
                getThread->pi_ppid = INVALID_PID;
                pi_drop(targetpid);
                lock_release(pidlock);
                return targetpid;
            }
        }

        //KASSERT()
        // child has not yet exited
        if (flags == WNOHANG) { // do not wait
			lock_release(pidlock);
			return 0;
        } else { // wait
	    DEBUG(DB_THREADS, "WAIT %d\n", getThread->pi_pid);
            cv_wait(getThread->pi_cv, pidlock);
            KASSERT(getThread->pi_exited == true);
            if (status != NULL) {
                *status = getThread->pi_exitstatus;
                getThread->pi_ppid = INVALID_PID;
                pi_drop(targetpid);
                lock_release(pidlock);
                return targetpid;
                //return *status;
            } else { // status pointer is null
                getThread->pi_ppid = INVALID_PID;
                pi_drop(targetpid);
                lock_release(pidlock);
                return targetpid;
            }
        }
}

/*
 * Get flag for the given thread pid.
 */
int
pid_get_flag(pid_t curthreadpid) {

    //set lock
    lock_acquire(pidlock);

    // verify pid is valid
    if (curthreadpid < PID_MIN || curthreadpid > PID_MAX || curthreadpid == INVALID_PID) {
        // exists outside of the min/max values for process ids
            lock_release(pidlock);
            return EINVAL;
    }

    struct pidinfo* curpidinfo = pi_get(curthreadpid);
    // verify pid exists
    if (curpidinfo == NULL) {
        // pid is null
        lock_release(pidlock);
        return ESRCH;
    }

    int retval = curpidinfo->flag;
    lock_release(pidlock);
    return retval;

}

/*
 *  Set flag for the given thread pid.
 *  Return 0 on success.
 */
int
pid_set_flag(pid_t curthreadpid, int flag) {

    //set lock
    lock_acquire(pidlock);

    // verify pid is valid
    if (curthreadpid < PID_MIN || curthreadpid > PID_MAX || curthreadpid == INVALID_PID) {
        // exists outside of the min/max values for process ids
            lock_release(pidlock);
            return -EINVAL;
    }

    struct pidinfo* curpidinfo = pi_get(curthreadpid);
    // verify pid exists
    if (curpidinfo == NULL) {
        // pid is null
        lock_release(pidlock);
        return -ESRCH;
    }
    curpidinfo->flag = flag;
    lock_release(pidlock);
    return 0;
}
