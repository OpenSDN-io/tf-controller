#
# Copyright (c) 2026 OpenSDN Authors. All rights reserved.
#

import argparse
import io
import unittest

import mock

from schema_transformer import to_bgp


class TestFatalErrorIsReported(unittest.TestCase):
    """The error is reported before the cleanup ends the process."""

    def setUp(self):
        super(TestFatalErrorIsReported, self).setUp()
        self.calls = []
        self.stderr = io.StringIO()
        patcher = mock.patch.object(to_bgp.sys, 'stderr', self.stderr)
        patcher.start()
        self.addCleanup(patcher.stop)

    def _logger(self):
        logger = mock.Mock()
        logger.error.side_effect = lambda msg: self.calls.append('logged')
        return logger

    def test_log_fatal(self):
        logger = mock.Mock()
        try:
            raise ValueError('boom')
        except ValueError:
            to_bgp._log_fatal(logger, 'Something failed')

        msg = logger.error.call_args[0][0]
        self.assertIn('Something failed', msg)
        self.assertIn('ValueError: boom', msg)
        self.assertIn('ValueError: boom', self.stderr.getvalue())

    def test_initialization_failure(self):
        logger = self._logger()
        args = argparse.Namespace(zk_timeout=120, yield_in_evaluate=False)

        with mock.patch.object(to_bgp, 'STAmqpHandle'), \
                mock.patch.object(to_bgp, 'SchemaTransformerDB',
                                  side_effect=RuntimeError('cassandra')), \
                mock.patch.object(
                    to_bgp.SchemaTransformer, 'destroy_instance',
                    side_effect=lambda: self.calls.append('destroyed')):
            self.assertRaises(RuntimeError, to_bgp.SchemaTransformer,
                              logger, args)

        self.assertEqual(['logged', 'destroyed'], self.calls)
        self.assertIn('RuntimeError: cassandra',
                      logger.error.call_args[0][0])

    def test_failure_after_election(self):
        st_logger = self._logger()
        zookeeper_client = mock.Mock()
        zookeeper_client.master_election.side_effect = RuntimeError('reinit')
        transformer = mock.Mock()
        transformer.destroy_instance.side_effect = (
            lambda: self.calls.append('destroyed'))
        args = argparse.Namespace(
            cluster_id='', collectors=None, host_ip='127.0.0.1',
            zk_server_ip='127.0.0.1:2181', zk_timeout=120,
            zookeeper_ssl_enable=False)

        with mock.patch.object(to_bgp, 'parse_args', return_value=args), \
                mock.patch.object(to_bgp, 'SchemaTransformerLogger',
                                  return_value=st_logger), \
                mock.patch.object(to_bgp, 'STAmqpHandle'), \
                mock.patch.object(to_bgp, 'ZookeeperClient',
                                  return_value=zookeeper_client), \
                mock.patch.object(to_bgp, 'transformer', transformer):
            self.assertRaises(RuntimeError, to_bgp.main, '--cluster_id x')

        self.assertEqual(['logged', 'destroyed'], self.calls)

    def test_system_exit_is_not_reported_as_failure(self):
        # e.g. "Api-server connection lost. Exiting", already logged
        st_logger = mock.Mock()
        zookeeper_client = mock.Mock()
        zookeeper_client.master_election.side_effect = SystemExit(2)
        args = argparse.Namespace(
            cluster_id='', collectors=None, host_ip='127.0.0.1',
            zk_server_ip='127.0.0.1:2181', zk_timeout=120,
            zookeeper_ssl_enable=False)

        with mock.patch.object(to_bgp, 'parse_args', return_value=args), \
                mock.patch.object(to_bgp, 'SchemaTransformerLogger',
                                  return_value=st_logger), \
                mock.patch.object(to_bgp, 'STAmqpHandle'), \
                mock.patch.object(to_bgp, 'ZookeeperClient',
                                  return_value=zookeeper_client), \
                mock.patch.object(to_bgp, 'transformer', None):
            self.assertRaises(SystemExit, to_bgp.main, '--cluster_id x')

        st_logger.error.assert_not_called()
        self.assertEqual('', self.stderr.getvalue())
