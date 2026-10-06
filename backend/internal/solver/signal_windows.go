package solver

import (
	"os/exec"
	"syscall"
)

// Windows has no SIGTERM, so any stop request kills the solver outright and no partial
// metrics are written. The platform is deployed on Linux; this keeps the code compiling here.
func signalGroup(cmd *exec.Cmd, _ syscall.Signal) {
	_ = cmd.Process.Kill()
}
