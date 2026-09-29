package solver

import "syscall"

// Windows has no process groups or parent-death signal.
func sysProcAttr() *syscall.SysProcAttr {
	return &syscall.SysProcAttr{}
}
