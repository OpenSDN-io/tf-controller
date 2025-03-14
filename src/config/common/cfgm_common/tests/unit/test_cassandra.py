# -*- coding: utf-8 -*-

#
# Copyright (c) 2020 Juniper Networks, Inc. All rights reserved.
#

import mock
import unittest

from cfgm_common.datastore import api as datastore_api
from cfgm_common.exceptions import NoIdError
from pysandesh.connection_info import ConnectionState
from pysandesh.gen_py.process_info.ttypes import ConnectionStatus
from pysandesh.gen_py.process_info.ttypes import ConnectionType as ConnType

# Drivers
from cfgm_common.datastore.drivers import cassandra_cql


class FakeDriver(datastore_api.CassandraDriver):
    def _Get_CF_Batch(self, cf_name, keyspace_name=None):
        pass

    def _Get_Range(self, cf_name, columns=None, column_count=100000, include_timetamps=False):
        pass

    def _Multiget(self, cf_name, keys, columns=None, start='', finish='',
                  timestamp=False, num_columns=None):
        pass

    def _Get(self, keyspace_name, cf_name, key, columns=None, start='',
             finish=''):
        pass

    def _XGet(self, cf_name, key, columns=None, start='', finish=''):
        pass

    def _Get_Count(self, cf_name, key, start='', finish='', keyspace_name=None):
        pass

    def _Get_One_Col(self, cf_name, key, column):
        pass

    def _Get_Keys(self, cf_name, rows):
        pass

    def _Insert(self, key, columns, keyspace_name=None, cf_name=None,
                batch=None, column_family=None):
        pass

    def _Remove(self, key, columns=None, keyspace_name=None, cf_name=None,
                batch=None, column_family=None):
        pass

    def _Init_Cluster(self):
        pass

    def _Column_Families(self, keyspace, prefixed=False):
        pass

    def _Create_Session(self, cf_name, **cf_args):
        pass


class TestOptions(unittest.TestCase):

    def test_keyspace_wipe(self):
        drv = FakeDriver([])
        self.assertEqual(
            "my_keyspace", drv.keyspace("my_keyspace"))

        drv = FakeDriver([], db_prefix='a_prefix')
        self.assertEqual(
            "a_prefix_my_keyspace", drv.keyspace("my_keyspace"))

    def test_reset_config_wipe(self):
        drv = FakeDriver([])
        self.assertFalse(drv.options.reset_config)

        drv = FakeDriver([], reset_config=True)
        self.assertTrue(drv.options.reset_config)

    def test_server_list(self):
        drv = FakeDriver(['a', 'b', 'c'])
        self.assertEqual(['a', 'b', 'c'], drv._server_list)

    def test_pool_size(self):
        drv = FakeDriver(['a', 'b', 'c'])
        self.assertEqual(6, drv.pool_size())

        drv = FakeDriver(['a', 'b', 'c'], pool_size=8)
        self.assertEqual(8, drv.pool_size())

    def test_nodes(self):
        drv = FakeDriver(['a', 'b', 'c'])
        self.assertEqual(3, drv.nodes())

    def test_logger_wipe(self):
        drv = FakeDriver([])
        self.assertIsNone(drv.options.logger)

        drv = FakeDriver([], logger='<something>')
        self.assertEqual('<something>', drv.options.logger)

    def test_credential_wipe(self):
        drv = FakeDriver([])
        self.assertIsNone(drv.options.credential)

        drv = FakeDriver([], credential='<creds>')
        self.assertEqual('<creds>', drv.options.credential)

    def test_ssl_enabled(self):
        drv = FakeDriver([])
        self.assertFalse(drv.options.ssl_enabled)

        drv = FakeDriver([], ssl_enabled=True)
        self.assertTrue(drv.options.ssl_enabled)

    def test_ca_certs(self):
        drv = FakeDriver([])
        self.assertIsNone(drv.options.ca_certs)

        drv = FakeDriver([], ca_certs='<certificats>')
        self.assertEqual('<certificats>', drv.options.ca_certs)

    def test_ro_keyspaces(self):
        drv = FakeDriver([])
        self.assertEqual({
            'config_db_uuid': {
                'obj_fq_name_table': {
                    'cf_args': {'autopack_values': False}},
                'obj_shared_table': {},
                'obj_uuid_table': {
                    'cf_args': {'autopack_names': False,
                                'autopack_values': False}}
            }}, drv.options.ro_keyspaces)

        drv = FakeDriver([], ro_keyspaces={'a': 'b'})
        self.assertEqual({
            'a': 'b',
            'config_db_uuid': {
                'obj_fq_name_table': {
                    'cf_args': {'autopack_values': False}},
                'obj_shared_table': {},
                'obj_uuid_table': {
                    'cf_args': {'autopack_names': False,
                                'autopack_values': False}}
            }}, drv.options.ro_keyspaces)

    def test_rw_keyspaces(self):
        drv = FakeDriver([])
        self.assertEqual({}, drv.options.rw_keyspaces)

        drv = FakeDriver([], rw_keyspaces={'c': 'd'})
        self.assertEqual({'c': 'd'}, drv.options.rw_keyspaces)

    def test_log_response_time(self):
        drv = FakeDriver([])
        self.assertIsNone(drv.options.log_response_time)
        # TODO(sahid): Should be removed
        self.assertIsNone(drv.log_response_time)

        drv = FakeDriver([], log_response_time='<time>')
        self.assertEqual('<time>', drv.options.log_response_time)
        # TODO(sahid): Should be removed
        self.assertEqual('<time>', drv.log_response_time)

    def test_genereate_url(self):
        drv = FakeDriver([])
        self.assertIsNotNone(drv.options.generate_url)
        # TODO(sahid): Should be removed
        self.assertIsNotNone(drv._generate_url)

        drv = FakeDriver([], generate_url='<url>')
        self.assertEqual('<url>', drv.options.generate_url)
        # TODO(sahid): Should be removed
        self.assertEqual('<url>', drv._generate_url)


class TestStatus(unittest.TestCase):

    def test_status(self):
        drv = FakeDriver(['a', 'b', 'c'])
        self.assertEqual(ConnectionStatus.INIT, drv.get_status())

    @mock.patch.object(ConnectionState, 'update')
    def test_status_up(self, mock_state):
        drv = FakeDriver(['a', 'b', 'c'])

        drv.report_status_up()
        mock_state.assert_called_once_with(
            conn_type=ConnType.DATABASE,
            name='Cassandra',
            status=ConnectionStatus.UP,
            message='',
            server_addrs=['a', 'b', 'c'])
        self.assertEqual(ConnectionStatus.UP, drv.get_status())

    @mock.patch.object(ConnectionState, 'update')
    def test_status_down(self, mock_state):
        drv = FakeDriver(['a', 'b', 'c'])

        drv.report_status_down('i want chocolate!')
        mock_state.assert_called_once_with(
            conn_type=ConnType.DATABASE,
            name='Cassandra',
            status=ConnectionStatus.DOWN,
            message='i want chocolate!',
            server_addrs=['a', 'b', 'c'])
        self.assertEqual(ConnectionStatus.DOWN, drv.get_status())

    @mock.patch.object(ConnectionState, 'update')
    def test_status_init(self, mock_state):
        drv = FakeDriver(['a', 'b', 'c'])

        drv.report_status_init()
        mock_state.assert_called_once_with(
            conn_type=ConnType.DATABASE,
            name='Cassandra',
            status=ConnectionStatus.INIT,
            message='',
            server_addrs=['a', 'b', 'c'])
        self.assertEqual(ConnectionStatus.INIT, drv.get_status())


class TestCassandraDriverCQL(unittest.TestCase):
    def setUp(self):
        # Mock the libraries
        cassandra_cql.connector = mock.MagicMock()

        # Mock creating keyspaces
        def _Init_Cluster(self):
            self._cf_dict = {
                datastore_api.OBJ_UUID_CF_NAME: mock.MagicMock(),
                datastore_api.OBJ_FQ_NAME_CF_NAME: mock.MagicMock(),
                datastore_api.OBJ_SHARED_CF_NAME: mock.MagicMock(),
            }
            self._cluster = mock.MagicMock()
        p = []
        p.append(mock.patch(
            'cfgm_common.datastore.drivers.cassandra_cql.CassandraDriverCQL._Init_Cluster',
            _Init_Cluster))
        p.append(mock.patch(
            'cfgm_common.datastore.drivers.cassandra_cql.JsonToObject',
            lambda x: x))
        [x.start() for x in p]

        self.drv = cassandra_cql.CassandraDriverCQL(['a', 'b'], logger=mock.MagicMock(),
                                                    inserts_use_batch=False,
                                                    removes_use_batch=False)
        self.drv.pool = cassandra_cql.DummyPool(
            1, 1, self.drv.worker, self.drv.initializer)

        # Ensure to cleanup mockings
        [self.addCleanup(x.stop) for x in p]

    def test_import_error(self):
        cassandra_cql.connector = None
        self.assertRaises(ImportError, cassandra_cql.CassandraDriverCQL, ['a', 'b'])

    def test_default_session(self):
        self.drv.get_default_session()
        self.drv._cluster.connect.assert_called_once_with()

    def assertCql(self, wanted, mocked, cql_arg_idx=0):
        f = lambda x: x.strip().replace(' ', '').replace('\n', '')
        args, kwargs = mocked.call_args
        cql = args[cql_arg_idx]
        self.assertEqual(
            f(wanted), f(cql), msg="\nWanted:\n{}\nGiven:\n{}".format(
                wanted, cql))

    def test_safe_drop_keyspace(self):
        session = mock.MagicMock()
        self.drv._cluster.connect.return_value = session
        self.drv.safe_drop_keyspace(datastore_api.UUID_KEYSPACE_NAME)
        self.assertCql(
            """
            DROP KEYSPACE "{}"
            """.format(datastore_api.UUID_KEYSPACE_NAME),
            session.execute)

    def test_safe_create_keyspace(self):
        session = mock.MagicMock()
        self.drv._cluster.connect.return_value = session
        self.drv._zk_client = mock.MagicMock()
        self.drv.safe_create_keyspace(keyspace=datastore_api.UUID_KEYSPACE_NAME)
        self.assertCql(
            """
            CREATE KEYSPACE IF NOT EXISTS "{}"
              WITH REPLICATION = {{
                'class': 'SimpleStrategy',
                'replication_factor': '2'
            }}""".format(datastore_api.UUID_KEYSPACE_NAME),
            session.execute)

    def test_ensure_keyspace_replication(self):
        session = mock.MagicMock()
        self.drv._zk_client = mock.MagicMock()
        self.drv._cluster.connect.return_value = session
        self.drv.ensure_keyspace_replication(
            datastore_api.OBJ_UUID_CF_NAME)
        self.assertCql(
            """
            ALTER KEYSPACE "obj_uuid_table" WITH REPLICATION = {
              'class': 'SimpleStrategy',
              'replication_factor': '2'
            }
            """,
            session.execute)

    def test_safe_create_table(self):
        session = self.drv.get_cf(datastore_api.OBJ_UUID_CF_NAME)
        self.drv._zk_client = mock.MagicMock()
        self.drv.safe_create_table(cf_name=datastore_api.OBJ_UUID_CF_NAME)
        self.assertCql(
            """
            CREATE TABLE IF NOT EXISTS "{}" (
              key blob,
              column1 blob,
              value text,
            PRIMARY KEY (key, column1)
            ) WITH COMPACT STORAGE AND CLUSTERING ORDER BY (column1 ASC)
            """.format(datastore_api.OBJ_UUID_CF_NAME),
            session.execute)

    def test_ensure_table_properties(self):
        session = self.drv.get_cf(datastore_api.OBJ_UUID_CF_NAME)
        self.drv._zk_client = mock.MagicMock()
        self.drv.ensure_table_properties(datastore_api.OBJ_UUID_CF_NAME)
        self.assertCql(
            """
            ALTER TABLE "obj_uuid_table"
              WITH gc_grace_seconds=864000
            """,
            session.execute)

    def test_get_range(self):
        session = self.drv.get_cf(datastore_api.OBJ_UUID_CF_NAME)
        list(self.drv.get_range(datastore_api.OBJ_UUID_CF_NAME,
                                columns=['a_col1', 'a_col2']))
        self.assertCql(
            """
            SELECT blobAsText(key), blobAsText(column1), value
            FROM "obj_uuid_table"
            WHERE column1 IN (textAsBlob(%s), textAsBlob(%s)) ALLOW FILTERING
            """, session.execute)
        session.execute.assert_called_once_with(
            mock.ANY, [u'a_col1', u'a_col2'])

    @mock.patch('cfgm_common.datastore.drivers.cassandra_cql.Iter.next')
    def test_cql_select(self, mock_Iter_next):
        mock_Iter_next.side_effect = [
            (True, ('a_col1', 'a_value1', 1)),
            (True, ('a_col2', 'a_value2', 2)),
            (True, ('a_col3', 'a_value3', 3)),
            StopIteration
        ]
        self.drv.apply = mock.MagicMock()
        ses = self.drv.get_cf(datastore_api.OBJ_UUID_CF_NAME)
        self.drv._cql_select(
            cf_name=datastore_api.OBJ_UUID_CF_NAME,
            keys=['a_key1', 'a_key2'],
            columns=['a_col1', 'a_col2'],
            start='from', finish='end')
        self.assertCql(
            """
            SELECT blobAsText(column1), value
            FROM "obj_uuid_table"
            WHERE key = textAsBlob(%s)
            AND column1 IN (textAsBlob(%s), textAsBlob(%s))
            AND column1 >= textAsBlob(%s)
            AND column1 <= textAsBlob(%s) ALLOW FILTERING
            """, self.drv.apply, 1)
        self.drv.apply.assert_called_once_with(
            ses, mock.ANY, [[u'a_key1', u'a_col1', u'a_col2',
                             u'from', u'end'],
                            [u'a_key2', u'a_col1', u'a_col2',
                             u'from', u'end']])

    @mock.patch('cfgm_common.datastore.drivers.cassandra_cql.'
                'CassandraDriverCQL._cql_select')
    def test_get(self, select):
        self.drv.get(datastore_api.OBJ_UUID_CF_NAME,
                     key='a_key1',
                     columns=['a_col1', 'a_col2'],
                     start='from', finish='end')
        select.assert_called_once_with(
            cf_name='obj_uuid_table',
            columns=['a_col1', 'a_col2'],
            decode_json=None,
            finish='end',
            keys=['a_key1'],
            start='from')

    @mock.patch('cfgm_common.datastore.drivers.cassandra_cql.'
                'CassandraDriverCQL._cql_select')
    def test_xget(self, select):
        self.drv.xget(datastore_api.OBJ_UUID_CF_NAME,
                      key='a_key1',
                      columns=['a_col1', 'a_col2'],
                      start='from', finish='end')
        select.assert_called_once_with(
            cf_name='obj_uuid_table',
            columns=['a_col1', 'a_col2'],
            decode_json=False,
            finish='end',
            keys=['a_key1'],
            start='from')

    @mock.patch('cfgm_common.datastore.drivers.cassandra_cql.'
                'CassandraDriverCQL._cql_select')
    def test_get_one_col(self, select):
        select.return_value = iter([('a_key1', {'a_col1': 'v'})])
        self.drv.get_one_col(datastore_api.OBJ_UUID_CF_NAME,
                             key='a_key1', column='a_col1')
        select.assert_called_once_with(
            cf_name='obj_uuid_table',
            columns=['a_col1'],
            decode_json=None,
            finish='',
            keys=['a_key1'],
            start='')

    @mock.patch('cfgm_common.datastore.drivers.cassandra_cql.'
                'CassandraDriverCQL._cql_select')
    def test_get_one_col_no_id(self, select):
        select.return_value = iter([])
        self.assertRaises(NoIdError,
                          self.drv.get_one_col,
                          datastore_api.OBJ_UUID_CF_NAME,
                          'a_key', column='a_col1')

    def test_get_count(self):
        session = self.drv.get_cf(datastore_api.OBJ_UUID_CF_NAME)
        self.drv.get_count(datastore_api.OBJ_UUID_CF_NAME,
                           'a_key', start='from', finish='end')
        self.assertCql(
            """
            SELECT COUNT(*) FROM "obj_uuid_table"
            WHERE key = textAsBlob(%s)
            AND column1 >= textAsBlob(%s)
            AND column1 <= textAsBlob(%s)
            """, session.execute)
        session.execute.assert_called_once_with(
            mock.ANY, ['a_key', 'from', 'end'])

    def test_insert(self):
        ses = self.drv.get_cf(datastore_api.OBJ_UUID_CF_NAME)
        self.drv._cql_execute = mock.MagicMock()
        self.drv.insert(cf_name=datastore_api.OBJ_UUID_CF_NAME,
                        key='a_key', columns={'a_col1': 'a_val1'})
        self.assertCql(
            """
            INSERT INTO "obj_uuid_table"
            (key, column1, value)
            VALUES (textAsBlob(%s), textAsBlob(%s), %s)
            """, self.drv._cql_execute, 1)
        self.drv._cql_execute.assert_called_once_with(
            ses, mock.ANY, 'a_key', {'a_col1': 'a_val1'})

    def test_insert_multi(self):
        ses = self.drv.get_cf(datastore_api.OBJ_UUID_CF_NAME)
        self.drv._cql_execute = mock.MagicMock()
        self.drv.insert(cf_name=datastore_api.OBJ_UUID_CF_NAME,
                        key='a_key', columns={'a_col1': 'a_val1',
                                              'a_col2': 'a_val2'})
        self.assertCql(
            """
            INSERT INTO "obj_uuid_table"
            (key, column1, value)
            VALUES (textAsBlob(%s), textAsBlob(%s), %s)
            """, self.drv._cql_execute, 1)
        self.drv._cql_execute.assert_called_once_with(
            ses, mock.ANY, 'a_key', {'a_col1': 'a_val1',
                                     'a_col2': 'a_val2'})

    @mock.patch('cfgm_common.datastore.drivers.cassandra_cql.CassandraDriverCQL._Get_CF_Batch')
    def test_insert_batch(self, mock__Get_CF_Batch):
        batch = mock.MagicMock()
        batch.cf_name = datastore_api.OBJ_UUID_CF_NAME
        mock__Get_CF_Batch.return_value = batch
        self.drv.insert(cf_name=datastore_api.OBJ_UUID_CF_NAME,
                        key='a_key', columns={'a_col1': 'a_val1'},
                        batch=batch)
        self.assertCql(
            """
            INSERT INTO "obj_uuid_table"
            (key, column1, value)
            VALUES (textAsBlob(%s), textAsBlob(%s), %s)
            """, batch.add_insert, 1)
        batch.add_insert.assert_called_once_with(
            'a_key', mock.ANY, ['a_key', 'a_col1', 'a_val1'])
        batch.send.assert_not_called()

    @mock.patch('cfgm_common.datastore.drivers.cassandra_cql.CassandraDriverCQL._Get_CF_Batch')
    def test_insert_batch_multi(self, mock__Get_CF_Batch):
        batch = mock.MagicMock()
        batch.cf_name = datastore_api.OBJ_UUID_CF_NAME
        mock__Get_CF_Batch.return_value = batch
        self.drv.insert(cf_name=datastore_api.OBJ_UUID_CF_NAME,
                        key='a_key', columns={'a_col1': 'a_val1',
                                              'a_col2': 'a_val2'},
                        batch=batch)
        self.assertCql(
            """
            INSERT INTO "obj_uuid_table"
            (key, column1, value)
            VALUES (textAsBlob(%s), textAsBlob(%s), %s)
            """, batch.add_insert, 1)
        batch.add_insert.assert_has_calls([
            mock.call('a_key', mock.ANY, ['a_key', 'a_col1', 'a_val1']),
            mock.call('a_key', mock.ANY, ['a_key', 'a_col2', 'a_val2'])],
            any_order=True)

    def test_remove_key(self):
        ses = self.drv.get_cf(datastore_api.OBJ_UUID_CF_NAME)
        self.drv.remove(cf_name=datastore_api.OBJ_UUID_CF_NAME,
                        key='a_key')
        self.assertCql(
            """
            DELETE FROM "obj_uuid_table"
            WHERE key = textAsBlob(%s)
            """, ses.execute)
        ses.execute.assert_called_once_with(
            mock.ANY, ['a_key'])

    @mock.patch('cfgm_common.datastore.drivers.cassandra_cql.CassandraDriverCQL._Get_CF_Batch')
    def test_remove_key_batch(self, mock__Get_CF_Batch):
        batch = mock.MagicMock()
        batch.cf_name = datastore_api.OBJ_UUID_CF_NAME
        mock__Get_CF_Batch.return_value = batch
        self.drv.remove(cf_name=datastore_api.OBJ_UUID_CF_NAME,
                        key='a_key',
                        batch=batch)
        self.assertCql(
            """
            DELETE FROM "obj_uuid_table"
            WHERE key = textAsBlob(%s)
            """, batch.add_remove, 1)
        batch.add_remove.assert_called_once_with(
            'a_key', mock.ANY, ['a_key'])

    def test_remove_col(self):
        ses = self.drv.get_cf(datastore_api.OBJ_UUID_CF_NAME)
        self.drv._cql_execute = mock.MagicMock()
        self.drv.remove(cf_name=datastore_api.OBJ_UUID_CF_NAME,
                        key='a_key', columns=['a_col1', 'a_col2'])
        self.assertCql(
            """
            DELETE FROM "obj_uuid_table"
            WHERE key = textAsBlob(%s)
            AND column1 = textAsBlob(%s)
            """, self.drv._cql_execute, 1)
        self.drv._cql_execute.assert_called_with(
            ses, mock.ANY, 'a_key', ['a_col1', 'a_col2'])

    @mock.patch('cfgm_common.datastore.drivers.cassandra_cql.CassandraDriverCQL._Get_CF_Batch')
    def test_remove_col_batch(self, mock__Get_CF_Batch):
        batch = mock.MagicMock()
        batch.cf_name = datastore_api.OBJ_UUID_CF_NAME
        mock__Get_CF_Batch.return_value = batch
        self.drv.remove(cf_name=datastore_api.OBJ_UUID_CF_NAME,
                        key='a_key', columns=['a_col1', 'a_col2'],
                        batch=batch)
        self.assertCql(
            """
            DELETE FROM "obj_uuid_table"
            WHERE key = textAsBlob(%s)
            AND column1 = textAsBlob(%s)
            """, batch.add_remove, 1)
        batch.add_remove.assert_has_calls([
            mock.call('a_key', mock.ANY, ['a_key', 'a_col1']),
            mock.call('a_key', mock.ANY, ['a_key', 'a_col2'])],
            any_order=True)
        batch.send.assert_not_called()
