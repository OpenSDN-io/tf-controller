# -*- coding: utf-8 -*-
import os
import subprocess
import sys
import unittest

from vnc_api.gen.resource_xsd import AclEntriesType
from vnc_api.gen.resource_xsd import AclRuleType
from vnc_api.gen.resource_xsd import ActionListType
from vnc_api.gen.resource_xsd import MatchConditionType

from cfgm_common.utils import acl_entries_hash
from cfgm_common.utils import CacheContainer
from cfgm_common.utils import decode_string
from cfgm_common.utils import encode_string


class TestCacheContainer(unittest.TestCase):
    def test_cache_container_trimming(self):
        c = CacheContainer(5)
        lst = ['a', 'b', 'c', 'd', 'e', 'f', 'h', 'i', 'j', 'k', 'm']

        for index, value in enumerate(lst):
            c[value] = index + 1

        self.assertEqual(len(list(c.dictionary.keys())), 5)
        self.assertEqual(set(lst[-5:]), set(c.dictionary.keys()))

    def test_cache_container_fetch(self):
        c = CacheContainer(5)
        lst = ['a', 'b', 'c', 'd', 'e']

        for index, value in enumerate(lst):
            c[value] = index + 1

        self.assertEqual(set(lst), set(c.dictionary.keys()))

        # get the oldest value and check on the next set its not lost
        c['a']
        c['f'] = 6
        self.assertEqual(set(['c', 'd', 'e', 'f', 'a']),
                         set(c.dictionary.keys()))

        # put a value for the oldest key and check its not lost
        c['c'] = 'x'
        self.assertEqual(c['c'], 'x')
        self.assertEqual(set(['d', 'e', 'f', 'a', 'c']),
                         set(c.dictionary.keys()))

        c['g'] = 7
        self.assertEqual(set(['e', 'f', 'a', 'c', 'g']),
                         set(c.dictionary.keys()))


class TestFqNameEncode(unittest.TestCase):
    def test_fq_name_encoding(self):
        test_suite = [
            ('only-ascii', 'only-ascii'),
            ('only ascii with space', 'only ascii with space'),
            ('only/ascii/with/forward/slash', 'only/ascii/with/forward/slash'),
            ('only!ascii!with!exclamatory', 'only!ascii!with!exclamatory'),
            ('only~ascii~tilde', 'only~ascii~tilde'),
            ('only+ascii+plus', 'only+ascii+plus'),
            ('foo=bar', 'foo=bar'),
            # (, ),
            # ('non-ascii-é', 'non-ascii-%C3%A9'),
            # ('non ascii with space é', 'non+ascii+with+space+%C3%A9'),
            # ('non-ascii-encoded-\xe9', 'non-ascii-encoded-%C3%A9'),
            # (b'binary', TypeError),
        ]
        for string, expected_result in test_suite:
            if (isinstance(expected_result, type) and
                    issubclass(expected_result, Exception)):
                self.assertRaises(expected_result, encode_string, string)
            else:
                self.assertEqual(expected_result, encode_string(string))
                self.assertEqual(decode_string(expected_result), string)


def _acl_entries():
    return AclEntriesType(dynamic=True, acl_rule=[AclRuleType(
        match_condition=MatchConditionType(protocol='udp', ethertype='IPv4'),
        action_list=ActionListType(simple_action='deny'),
        rule_uuid='ba0e0e1c-0000-0000-0000-000000000001')])


class TestAclEntriesHash(unittest.TestCase):
    def test_stable_across_processes(self):
        # the value is persisted and compared by a later process, so it must
        # not depend on PYTHONHASHSEED the way built-in hash() does
        script = (
            'from cfgm_common.tests.unit.test_utils import _acl_entries;'
            'from cfgm_common.utils import acl_entries_hash;'
            'print(acl_entries_hash(_acl_entries()))')
        env = dict(os.environ, PYTHONHASHSEED='1')
        out = subprocess.check_output([sys.executable, '-c', script], env=env)
        self.assertEqual(acl_entries_hash(_acl_entries()), int(out))

    def test_survives_dict_roundtrip(self):
        # api-server hashes an object rebuilt from the persisted dict, the
        # schema-transformer hashes the one it built - both must agree
        entries = _acl_entries()
        stored = entries.exportDict()['AclEntriesType']
        self.assertEqual(acl_entries_hash(entries),
                         acl_entries_hash(AclEntriesType(params_dict=stored)))

    def test_content_change_changes_hash(self):
        entries = _acl_entries()
        before = acl_entries_hash(entries)
        entries.acl_rule[0].action_list.simple_action = 'pass'
        self.assertNotEqual(before, acl_entries_hash(entries))

    def test_fits_unsigned_long(self):
        # the schema type of access-control-list-hash is xsd:unsignedLong
        value = acl_entries_hash(_acl_entries())
        self.assertGreaterEqual(value, 0)
        self.assertLess(value, 2 ** 64)
