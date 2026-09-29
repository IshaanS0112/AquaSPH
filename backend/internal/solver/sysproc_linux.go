package solver

import "syscall"

// sysProcAttr puts the solver in its own process group, so a Ctrl-C aimed at the worker's
// terminal does not also hit the solver (the worker decides how its jobs stop), and asks the
// kernel to SIGKILL the solver if the worker dies.
func sysProcAttr() *syscall.SysProcAttr {
	return &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGKILL}
}
