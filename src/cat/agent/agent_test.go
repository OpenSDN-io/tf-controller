// Package agent_test provides unit tests to test agent package methods.
package agent_test

import (
	"os"
	"syscall"
	"testing"

	"cat"
	"cat/agent"
	"cat/sut"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestAgent(t *testing.T) {
	c, err := cat.New()
	require.NoError(t, err, "Failed to create CAT object")

	endpoints := []sut.Endpoint{
		sut.Endpoint{
			IP:   "10.0.0.1",
			Port: 1,
		},
		sut.Endpoint{
			IP:   "10.0.0.2",
			Port: 2,
		},
	}
	a, err := agent.New(c.SUT.Manager, "agent", " ", "test", endpoints)
	require.NoError(t, err, "Failed to create agent")
	assert.Equal(t, "agent", a.Name)

	// Verify that agent process is started
	pid := a.Cmd.Process.Pid
	assert.NotEqual(t, 0, pid, "agent process id is zero; want non-zero")

	// Verify that component directory is created
	_, err = os.Stat(a.Component.ConfDir)
	assert.NoError(t, err, "Conf directory %s is not created", a.Component.ConfDir)
	_, err = os.Stat(a.Component.LogDir)
	assert.NoError(t, err, "Log directory %s is not created", a.Component.LogDir)

	err = a.Teardown()
	require.NoError(t, err, "CAT objects cleanup failed")

	// Verify that process indeed went down.
	err = syscall.Kill(pid, syscall.Signal(0))
	require.NoError(t, err, "agent process %d did not die", pid)
}
