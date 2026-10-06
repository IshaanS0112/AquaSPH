//go:build !linux && !windows

package solver

import "syscall"

// Pdeathsig is Linux-only. Elsewhere an orphaned solver keeps running until it finishes; the
// fencing token still stops its result from being recorded.
func sysProcAttr() *syscall.SysProcAttr {
	return &syscall.SysProcAttr{Setpgid: true}
}
