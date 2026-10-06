//go:build !windows

package solver

import (
	"os/exec"
	"syscall"
)

// signalGroup signals the whole process group led by the solver (negative PID).
func signalGroup(cmd *exec.Cmd, sig syscall.Signal) {
	if err := syscall.Kill(-cmd.Process.Pid, sig); err != nil {
		_ = cmd.Process.Signal(sig)
	}
}
