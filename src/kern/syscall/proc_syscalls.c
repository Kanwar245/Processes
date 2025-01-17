/*
 * Process-related syscalls.
 * New for ASST1.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <pid.h>
#include <machine/trapframe.h>
#include <syscall.h>
 #include <copyinout.h>

/*
 * sys_fork
 * 
 * create a new process, which begins executing in md_forkentry().
 */


int
sys_fork(struct trapframe *tf, pid_t *retval)
{
	struct trapframe *ntf; /* new trapframe, copy of tf */
	int result;

	/*
	 * Copy the trapframe to the heap, because we might return to
	 * userlevel and make another syscall (changing the trapframe)
	 * before the child runs. The child will free the copy.
	 */

	ntf = kmalloc(sizeof(struct trapframe));
	if (ntf==NULL) {
		return ENOMEM;
	}
	*ntf = *tf; /* copy the trapframe */

	result = thread_fork(curthread->t_name, enter_forked_process, 
			     ntf, 0, retval);
	if (result) {
		kfree(ntf);
		return result;
	}

	return 0;
}

/*
 * sys_getpid
 * Return the process id of the current process.
 */
int
sys_getpid(pid_t *retval)
{
	*retval = curthread->t_pid;
	return 0;
}


/*
 * sys_waitpid
 * Wait for for the process "pid" to exit
 * Save integer exit status pointed to by "status"
 * If "options" is WNOHANG and process "pid" has not yet exited,
 * return 0. On error, return -1 and set the errno with the suitable
 * error code.
 */
int
sys_waitpid(pid_t pid, int *status, int options, pid_t *retval)
{
    // The options argument should be 0 or WNOHANG
    if (options != 0 && options != WNOHANG) {
        DEBUG(DB_SYSCALL, "waitpid invalid option: %d\n", options);
        return EINVAL; // options argument requested invalid or unsupported options
    }
    
    // the pid argument named a nonexistent process
    if (pid == INVALID_PID) {
        return ESRCH; 
    }
    
    // status argument was an invalid pointer
    if (status == NULL) {
        return EFAULT; 
    }
    
    int result = pid_join(pid, k_status, options);
copyout(status, k_status)
    //kprintf("Actual result: %d\n", result);
    if (options == WNOHANG) {
        KASSERT(result == 0);
    }
    if (result < 0) { // fail
        DEBUG (DB_SYSCALL, "thread_join failure: %d\n", result);
        *retval = -result;
        return -1;
    } else { // success
    	//*retval = pid;
    	return 0;
    }
}

/*
 * sys_kill
 * Send the signal "signal" to process "pid"
 * If "signal" is 0, then no signal is sent.
 * On success, 0 is returned. On error, -1 is returned
 * and errno is set appropriately.
 */
int sys_kill(pid_t pid, int signal) {

    int retval = pid_set_flag(pid, signal);




    if (retval < 0) {
        return -1;
    }
    else {
        return 0;
    }
}




