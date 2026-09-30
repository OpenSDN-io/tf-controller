# -*- coding: utf-8 -*-

#
# Copyright (c) 2026 OpenSDN Authors. All rights reserved.
#

import unittest

import mock

from cfgm_common.vnc_kombu import VncKombuClient


def _client(**attrs):
    # client without connections or greenlets
    client = VncKombuClient.__new__(VncKombuClient)
    client._logger = mock.Mock()
    client.__dict__.update(attrs)
    return client


class TestVncKombuSubscribe(unittest.TestCase):

    def test_callback_error_is_logged_and_acked(self):
        client = _client(_subscribe_cb=mock.Mock(side_effect=ValueError('x')))
        message = mock.Mock()

        client._subscribe({'oper': 'UPDATE'}, message)

        message.ack.assert_called_once_with()
        client._logger.assert_called_once_with(
            'Error in rabbitmq subscribe callback: x', level=mock.ANY)

    def test_system_exit_is_not_swallowed(self):
        client = _client(_subscribe_cb=mock.Mock(side_effect=SystemExit(2)))
        message = mock.Mock()

        self.assertRaises(SystemExit, client._subscribe, {}, message)
        message.ack.assert_called_once_with()
