#
# Copyright (c) 2026 OpenSDN Authors. All rights reserved.
#

import socket
import unittest

import amqp.exceptions
import gevent
from gevent.queue import Empty
import mock

from cfgm_common import kombu_amqp
from cfgm_common.kombu_amqp import KombuAmqpClient


CONFIG = dict(servers='127.0.0.1', port=5672, user='guest', password='guest',
              vhost='/', ha_mode=False, use_ssl=False)


class TestKombuAmqpClientPublisher(unittest.TestCase):
    """With heartbeats, the publisher greenlet services its idle connection."""

    def _client(self, heartbeat):
        client = KombuAmqpClient(mock.Mock(), CONFIG, heartbeat=heartbeat)
        client.add_exchange('test_exchange')
        return client

    def test_without_heartbeat_waits_for_the_next_message(self):
        client = self._client(0)
        client._publisher_queue = mock.Mock()
        connection = mock.Mock()

        client._next_payload(connection)

        client._publisher_queue.get.assert_called_once_with()
        connection.heartbeat_check.assert_not_called()

    def test_idle_connection_is_serviced(self):
        client = self._client(60)
        client._publisher_queue = mock.Mock()
        client._publisher_queue.get.side_effect = Empty
        connection = mock.Mock()
        connection._connection.heartbeat = 10
        connection.drain_events.side_effect = [None, socket.timeout]

        self.assertIsNone(client._next_payload(connection))

        # the negotiated interval, shorter here, sets the pace
        client._publisher_queue.get.assert_called_once_with(timeout=5.0)
        self.assertEqual(2, connection.drain_events.call_count)
        connection.heartbeat_check.assert_called_once_with()

    def test_dead_connection_is_reestablished(self):
        client = self._client(0.02)
        connection = mock.MagicMock()
        connection.drain_events.side_effect = socket.timeout
        connection.heartbeat_check.side_effect = (
            [amqp.exceptions.ConnectionForced('Too many heartbeats missed')] +
            [None] * 1000)
        producer = mock.Mock()
        client._running = True

        with mock.patch.object(client._connection, 'clone',
                               return_value=connection), \
                mock.patch.object(client, '_ensure_connection') as ensure, \
                mock.patch.object(kombu_amqp.kombu, 'Producer',
                                  return_value=producer):
            publisher = gevent.spawn(client._start_publishing)
            gevent.sleep(0.1)
            client.publish({'n': 1}, 'test_exchange', routing_key='r')
            gevent.sleep(0.1)
            client._running = False
            publisher.kill()

        self.assertEqual(2, ensure.call_count)
        producer.publish.assert_called_once_with(
            {'n': 1}, exchange=client.get_exchange('test_exchange'),
            routing_key='r')
