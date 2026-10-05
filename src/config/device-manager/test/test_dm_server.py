#
# Copyright (c) 2026 OpenSDN Authors. All rights reserved.
#

import argparse
import subprocess
import unittest

import mock

from device_manager import dm_server


class _Exit(Exception):
    """Raised by the patched os._exit() so that the tests can go on."""


class TestDmServerElection(unittest.TestCase):

    def setUp(self):
        super(TestDmServerElection, self).setUp()
        self.calls = []
        for name in ('_zookeeper_client', '_child_proc', '_amqp_client'):
            self._patch(dm_server, name, None)
        for name in ('DeviceManager', 'DeviceZtpManager', 'DeviceJobManager'):
            self._patch(dm_server, name, mock.MagicMock())
        self._patch(dm_server.os, '_exit', mock.Mock(side_effect=self._exit))

    def _patch(self, target, name, value):
        patcher = mock.patch.object(target, name, value, create=True)
        patcher.start()
        self.addCleanup(patcher.stop)

    def _exit(self, code):
        self.calls.append(('exit', code))
        raise _Exit()

    def _zookeeper_client(self):
        client = mock.Mock()
        client.stop.side_effect = lambda: self.calls.append('stop')
        return client

    def _args(self, **kwargs):
        args = argparse.Namespace(
            cluster_id='', collectors=None, log_level=0, host_ip='10.0.0.1',
            dm_run_mode=None, zk_server_ip='10.0.0.2:2181',
            zookeeper_ssl_enable=False, zookeeper_ssl_keyfile='',
            zookeeper_ssl_certificate='', zookeeper_ssl_ca_cert='')
        args.__dict__.update(kwargs)
        return args

    @mock.patch.object(dm_server, 'run_partial_dm')
    @mock.patch.object(dm_server.gevent.hub, 'signal')
    @mock.patch.object(dm_server, 'ZookeeperClient')
    def test_zookeeper_ssl_arguments(self, zookeeper_client, *_):
        args = self._args(zookeeper_ssl_enable=True,
                          zookeeper_ssl_keyfile='/key',
                          zookeeper_ssl_certificate='/cert',
                          zookeeper_ssl_ca_cert='/ca')
        with mock.patch.object(dm_server, 'parse_args', return_value=args):
            dm_server.main('--dm_run_mode None')

        zookeeper_client.assert_called_once_with(
            'device-manager', '10.0.0.2:2181', '10.0.0.1',
            zk_ssl_enable=True, zk_ssl_keyfile='/key',
            zk_ssl_certificate='/cert', zk_ssl_ca_cert='/ca')
        self.assertIs(zookeeper_client.return_value,
                      dm_server._zookeeper_client)

    def test_watchdog_releases_election_before_exit(self):
        dm_server._zookeeper_client = self._zookeeper_client()
        child = mock.Mock()
        child.poll.return_value = 2

        with mock.patch.object(dm_server.gevent, 'sleep'):
            self.assertRaises(_Exit, dm_server.dummy_gl, child)

        self.assertEqual(['stop', ('exit', 2)], self.calls)
        self.assertIsNone(dm_server._zookeeper_client)

    def test_sigterm_in_election_process(self):
        dm_server._zookeeper_client = self._zookeeper_client()
        child = dm_server._child_proc = mock.Mock()
        child.poll.return_value = None
        child.wait.side_effect = lambda timeout: self.calls.append('child')

        self.assertRaises(_Exit, dm_server.sigterm_handler)

        child.terminate.assert_called_once_with()
        child.kill.assert_not_called()
        self.assertEqual(['child', 'stop', ('exit', 2)], self.calls)

    def test_sigterm_kills_a_child_that_does_not_stop(self):
        dm_server._zookeeper_client = self._zookeeper_client()
        child = dm_server._child_proc = mock.Mock()
        child.poll.return_value = None
        child.wait.side_effect = subprocess.TimeoutExpired('dm', 5)

        self.assertRaises(_Exit, dm_server.sigterm_handler)

        child.kill.assert_called_once_with()
        self.assertEqual(['stop', ('exit', 2)], self.calls)

    def test_sigterm_in_worker_process(self):
        # Full/Partial mode processes and functional tests: no election.
        dm_server.sigterm_handler()

        dm_server.DeviceManager.destroy_instance.assert_called_once_with()
        self.assertEqual([], self.calls)
