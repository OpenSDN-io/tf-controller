# vim: tabstop=4 shiftwidth=4 softtabstop=4

# Copyright 2012 Locaweb.
# All Rights Reserved.
#
#    Licensed under the Apache License, Version 2.0 (the "License"); you may
#    not use this file except in compliance with the License. You may obtain
#    a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
#    WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
#    License for the specific language governing permissions and limitations
#    under the License.
#
# @author: Juliano Martinez, Locaweb.

import fcntl
import os
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
from eventlet import greenthread

def _subprocess_setup():
    """Set the signal behavior to default (especially SIGPIPE) to non-Python subprocesses."""
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

def subprocess_popen(args, stdin=None, stdout=None, stderr=None, shell=False, env=None):
    """Helper to open a subprocess."""
    return subprocess.Popen(args, shell=shell, stdin=stdin, stdout=stdout,
                            stderr=stderr, preexec_fn=_subprocess_setup,
                            close_fds=True, env=env)

def create_process(cmd, root_helper=None, addl_env=None):
    """Create and return a subprocess.Popen object and the command list."""
    if root_helper:
        cmd = shlex.split(root_helper) + cmd
    cmd = list(map(str, cmd))

    env = os.environ.copy()
    if addl_env:
        env.update(addl_env)

    obj = subprocess_popen(cmd, shell=False, stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    return obj, cmd

def execute(cmd, root_helper=None, process_input=None, addl_env=None,
            check_exit_code=True, return_stderr=False):
    """Execute a command in the subprocess and handle the output."""
    try:
        obj, cmd = create_process(cmd, root_helper=root_helper, addl_env=addl_env)
        _stdout, _stderr = obj.communicate(process_input)
        _stdout = _stdout.decode('utf-8') if _stdout else ""
        _stderr = _stderr.decode('utf-8') if _stderr else ""

        if obj.returncode:
            if check_exit_code:
                raise RuntimeError(
                    f"\nCommand: {cmd}\nExit code: {obj.returncode}\nStdout: {_stdout}\nStderr: {_stderr}"
                )
    finally:
        if sys.version_info[:2] != (2, 6):
            greenthread.sleep(0)

    return (_stdout, _stderr) if return_stderr else _stdout

def get_interface_mac(interface):
    """Get the MAC address for the specified interface."""
    DEVICE_NAME_LEN = 15
    MAC_START = 18
    MAC_END = 24
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    info = fcntl.ioctl(s.fileno(), 0x8927, struct.pack('256s', interface[:DEVICE_NAME_LEN].encode('utf-8')))
    return ':'.join(f"{b:02x}" for b in info[MAC_START:MAC_END])

def replace_file(file_name, data):
    """Replace the contents of a file with data safely."""
    base_dir = os.path.dirname(os.path.abspath(file_name))
    tmp_file = tempfile.NamedTemporaryFile('w+', dir=base_dir, delete=False)
    tmp_file.write(data)
    tmp_file.close()
    os.chmod(tmp_file.name, 0o644)
    os.rename(tmp_file.name, file_name)

def find_child_pids(pid):
    """Find and return child PIDs for a given PID."""
    try:
        raw_pids = execute(['ps', '--ppid', str(pid), '-o', 'pid='], check_exit_code=False)
        return [x.strip() for x in raw_pids.split() if x.strip()]
    except RuntimeError as e:
        no_children_found = 'Exit code: 1' in str(e)
        if no_children_found:
            return []
        raise

