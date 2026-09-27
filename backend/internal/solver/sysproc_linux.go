package solver

import "syscall"

// sysProcAttr puts the solver in its own process group, so a Ctrl-C
// aimed at the worker's terminal does not also hit the solver (the
// worker decides how its jobs stop), and asks the kernel to SIGKILL the
// solver if the worker dies. Without Pdeathsig, a worker killed with
// SIGKILL would leave an orphaned solver burning every core for a job
// the reaper has already handed to someone else.
//
// Caveat (golang/go#27505): Pdeathsig fires when the OS *thread* that
// forked the child exits. The Go runtime does not retire threads unless
// a goroutine exits while holding runtime.LockOSThread, which this
// package never does.
func sysProcAttr() *syscall.SysProcAttr {
	return &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGKILL}
}
