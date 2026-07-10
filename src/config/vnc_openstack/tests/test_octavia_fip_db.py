"""issue#107 B1 - unit tests for the Octavia VIP -> amphora VMI resolver.

The resolver is the heart of the fix: given the Octavia VIP (placeholder)
port, find the amphora VRRP VMIs that actually carry the VIP as an
active-standby allowed-address-pair, so the FIP is wired to the forwarding
plane instead of the dead placeholder. The subtle part - proven necessary
by real failover data - is excluding *stale* VRRP ports left behind by a
previous failover: they keep the VIP AAP but their backing VM is gone.
These tests pin that, plus the VIP-address resolution rules (v4-only,
fail-closed on ambiguity) and the service-project ownership filter.
"""
import unittest

from vnc_openstack import neutron_plugin_db as db


class _Prefix(object):
    def __init__(self, p):
        self._p = p

    def get_ip_prefix(self):
        return self._p


class _AAP(object):
    def __init__(self, ip):
        self.ip = _Prefix(ip) if ip is not None else None


class _AAPs(object):
    def __init__(self, ips):
        self.allowed_address_pair = [_AAP(i) for i in ips]


class _VMI(object):
    """Minimal stand-in for a VMI object as the resolver consumes it."""

    def __init__(self, uuid, owner=None, aap_ips=None, vm=None,
                 iips=None, parent_uuid='tenant-proj'):
        self.uuid = uuid
        self.parent_uuid = parent_uuid
        self._owner = owner
        self._aaps = _AAPs(aap_ips) if aap_ips is not None else None
        self._vm = vm  # None -> no VM ref; else a VM uuid (live or dangling)
        self._iips = iips or []  # instance-ip uuids bound to this port

    def get_virtual_machine_interface_device_owner(self):
        return self._owner

    def get_virtual_network_refs(self):
        return [{'uuid': 'net-1'}]

    def get_virtual_machine_interface_allowed_address_pairs(self):
        return self._aaps

    def get_virtual_machine_refs(self):
        if self._vm is None:
            return []
        return [{'uuid': self._vm, 'to': ['vm', self._vm]}]

    def get_instance_ip_back_refs(self):
        return [{'uuid': i} for i in self._iips]


class _IIP(object):
    def __init__(self, address):
        self._address = address

    def get_instance_ip_address(self):
        return self._address


class _Vnc(object):
    def __init__(self, live_vms):
        self._live = live_vms

    def virtual_machine_read(self, id=None):
        if id in self._live:
            return object()
        raise db.NoIdError(id)


class _BadRequest(Exception):
    pass


class _Logger(object):
    def __init__(self):
        self.warnings = []

    def warning(self, msg):
        self.warnings.append(msg)


class _Dbi(db.DBInterface):
    # bypass the heavy DBInterface.__init__; wire only what the resolver
    # touches
    def __init__(self, cand_list, live_vms, iips=None,
                 octavia_project_id=''):
        self._cands = cand_list
        self._vnc_lib = _Vnc(live_vms)
        self._iips = iips or {}
        # normalize exactly as DBInterface.__init__ does (dashless+lower)
        self._octavia_project_id = octavia_project_id.replace('-', '').lower()
        self._octavia_filter_warned = False
        self.logger = _Logger()

    def _virtual_machine_interface_list(self, **kwargs):
        return self._cands

    def _instance_ip_read(self, instance_ip_id=None):
        if instance_ip_id in self._iips:
            return _IIP(self._iips[instance_ip_id])
        raise db.NoIdError(instance_ip_id)

    def _raise_contrail_exception(self, exc, **kwargs):
        raise _BadRequest(exc, kwargs)


class TestOctaviaVipResolver(unittest.TestCase):
    VIP = '10.0.0.5'

    def test_resolves_live_amphora_and_excludes_stale(self):
        vip_port = _VMI('vip-port', owner=db.OCTAVIA_VIP_DEVICE_OWNER)
        master = _VMI('amp-master', aap_ips=[self.VIP], vm='vm-master')
        backup = _VMI('amp-backup', aap_ips=[self.VIP], vm='vm-backup')
        # stale: still carries the VIP AAP but its VM died on failover
        stale = _VMI('amp-stale', aap_ips=[self.VIP], vm='vm-dead')
        # unrelated: different AAP address on the same network
        other = _VMI('other', aap_ips=['10.0.0.9'], vm='vm-other')
        dbi = _Dbi([vip_port, master, backup, stale, other],
                   live_vms={'vm-master', 'vm-backup', 'vm-other'})

        vmis, vip = dbi._octavia_vip_amphora_vmis(vip_port, self.VIP)

        self.assertEqual(vip, self.VIP)
        # only the two live amphora VRRP VMIs; vip_port (self), stale
        # (dead VM) and the wrong-AAP port are all excluded.
        self.assertEqual({v.uuid for v in vmis},
                         {'amp-master', 'amp-backup'})

    def test_non_octavia_port_is_noop(self):
        # regression guard: a normal compute port never triggers the hunt.
        port = _VMI('normal', owner='compute:nova')
        dbi = _Dbi([], live_vms=set())
        self.assertEqual(dbi._octavia_vip_amphora_vmis(port, self.VIP),
                         (None, None))

    def test_no_live_amphora_still_returns_vip(self):
        # amphorae not up yet (booting / mid-failover): the caller needs
        # the VIP to stamp the markers so the reconciler can finish the
        # wiring later. ([], vip), NOT (None, None).
        vip_port = _VMI('vip-port', owner=db.OCTAVIA_VIP_DEVICE_OWNER)
        dbi = _Dbi([vip_port], live_vms=set())
        vmis, vip = dbi._octavia_vip_amphora_vmis(vip_port, self.VIP)
        self.assertEqual(vmis, [])
        self.assertEqual(vip, self.VIP)

    def test_dual_stack_picks_v4_vip(self):
        # no fixed_ip in the request + dual-stack VIP port: neutron FIPs
        # are v4-only, so the v6 instance-ip must never be picked.
        vip_port = _VMI('vip-port', owner=db.OCTAVIA_VIP_DEVICE_OWNER,
                        iips=['iip-4', 'iip-6'])
        amp = _VMI('amp', aap_ips=[self.VIP], vm='vm-1')
        dbi = _Dbi([vip_port, amp], live_vms={'vm-1'},
                   iips={'iip-4': self.VIP, 'iip-6': 'fd00::5'})
        vmis, vip = dbi._octavia_vip_amphora_vmis(vip_port, None)
        self.assertEqual(vip, self.VIP)
        self.assertEqual({v.uuid for v in vmis}, {'amp'})

    def test_multiple_v4_vips_fail_closed(self):
        # additional_vips (two v4 VIPs, no explicit fixed_ip): guessing
        # would pin the wrong VIP silently. Must raise BadRequest, exactly
        # like the vanilla multi-fixed-ip path.
        vip_port = _VMI('vip-port', owner=db.OCTAVIA_VIP_DEVICE_OWNER,
                        iips=['iip-1', 'iip-2'])
        dbi = _Dbi([vip_port], live_vms=set(),
                   iips={'iip-1': self.VIP, 'iip-2': '10.0.0.6'})
        with self.assertRaises(_BadRequest):
            dbi._octavia_vip_amphora_vmis(vip_port, None)

    def test_octavia_project_filter_blocks_foreign_ports(self):
        # AAP-hijack guard: with octavia_project_id configured, a tenant
        # port aliasing the VIP via AAP is excluded; only ports owned by
        # the Octavia service project are wired in.
        vip_port = _VMI('vip-port', owner=db.OCTAVIA_VIP_DEVICE_OWNER)
        amp = _VMI('amp', aap_ips=[self.VIP], vm='vm-amp',
                   parent_uuid='octavia-proj')
        attacker = _VMI('evil', aap_ips=[self.VIP], vm='vm-evil',
                        parent_uuid='tenant-b')
        dbi = _Dbi([vip_port, amp, attacker],
                   live_vms={'vm-amp', 'vm-evil'},
                   octavia_project_id='octavia-proj')
        vmis, _ = dbi._octavia_vip_amphora_vmis(vip_port, self.VIP)
        self.assertEqual({v.uuid for v in vmis}, {'amp'})

    def test_octavia_project_filter_off_by_default(self):
        # knob unset -> previous behaviour (no ownership filtering)
        vip_port = _VMI('vip-port', owner=db.OCTAVIA_VIP_DEVICE_OWNER)
        amp = _VMI('amp', aap_ips=[self.VIP], vm='vm-amp',
                   parent_uuid='anywhere')
        dbi = _Dbi([vip_port, amp], live_vms={'vm-amp'})
        vmis, _ = dbi._octavia_vip_amphora_vmis(vip_port, self.VIP)
        self.assertEqual({v.uuid for v in vmis}, {'amp'})

    def test_octavia_project_misconfig_warns(self):
        # R15: a wrong octavia_project_id rejects every VIP-carrying port ->
        # empty result AND a warning (else the FIP silently loses datapath).
        vip_port = _VMI('vip-port', owner=db.OCTAVIA_VIP_DEVICE_OWNER)
        amp = _VMI('amp', aap_ips=[self.VIP], vm='vm-amp',
                   parent_uuid='octavia-proj')
        dbi = _Dbi([vip_port, amp], live_vms={'vm-amp'},
                   octavia_project_id='WRONG-UUID')
        vmis, _ = dbi._octavia_vip_amphora_vmis(vip_port, self.VIP)
        self.assertEqual(vmis, [])
        self.assertEqual(len(dbi.logger.warnings), 1)

    def test_filter_off_warns_once(self):
        # R1 hardening: filter unset (fail-open) must not be silent - warn the
        # first time a real Octavia VIP FIP is wired, then stay quiet.
        vip_port = _VMI('vip-port', owner=db.OCTAVIA_VIP_DEVICE_OWNER)
        amp = _VMI('amp', aap_ips=[self.VIP], vm='vm-amp')
        dbi = _Dbi([vip_port, amp], live_vms={'vm-amp'})  # no project id
        dbi._octavia_vip_amphora_vmis(vip_port, self.VIP)
        dbi._octavia_vip_amphora_vmis(vip_port, self.VIP)
        self.assertEqual(len(dbi.logger.warnings), 1)

    def test_octavia_project_id_case_insensitive(self):
        # R15: uppercase configured uuid still matches the lowercase VNC one.
        vip_port = _VMI('vip-port', owner=db.OCTAVIA_VIP_DEVICE_OWNER)
        amp = _VMI('amp', aap_ips=[self.VIP], vm='vm-amp',
                   parent_uuid='abc123def')
        dbi = _Dbi([vip_port, amp], live_vms={'vm-amp'},
                   octavia_project_id='ABC-123-DEF')
        vmis, _ = dbi._octavia_vip_amphora_vmis(vip_port, self.VIP)
        self.assertEqual({v.uuid for v in vmis}, {'amp'})


if __name__ == '__main__':
    unittest.main()
