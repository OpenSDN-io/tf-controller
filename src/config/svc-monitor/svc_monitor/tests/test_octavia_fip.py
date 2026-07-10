"""Unit tests for the issue#107 Octavia FIP failover reconciler.

Exercises LoadbalancerAgent.reconcile_fip / audit_octavia_fips and the
config_db evaluate() hooks with mocked SM caches (stdlib unittest.mock),
asserting on vnc_lib ref_update / floating_ip_update. Spec: svc-monitor lane.
Run: python -m unittest svc_monitor.tests.test_octavia_fip -v
"""
import unittest
from unittest import mock

from svc_monitor import config_db
from svc_monitor import loadbalancer_agent

VIP = '10.0.0.5'
NET = 'net-1'
VIP_PORT = 'vip-port'
IIP = 'iip-vip'


class Rec(object):
    """Minimal attribute holder standing in for a config_db SM object."""
    def __init__(self, **kw):
        self.__dict__.update(kw)


class OctaviaFipReconcilerTest(unittest.TestCase):

    def setUp(self):
        self.vmis = {}
        self.vns = {}
        self.iips = {}
        self.vms = {}
        self.fips = {}
        self.vnc_lib = mock.Mock()
        self.agent = loadbalancer_agent.LoadbalancerAgent.__new__(
            loadbalancer_agent.LoadbalancerAgent)
        self.agent._vnc_lib = self.vnc_lib
        self.agent._svc_mon = mock.Mock()
        self.agent._octavia_project_id = ''
        self.agent._octavia_filter_warned = False
        self.agent._fip_reconcile_timers = {}
        for cls, store in (
                (config_db.VirtualMachineInterfaceSM, self.vmis),
                (config_db.VirtualNetworkSM, self.vns),
                (config_db.InstanceIpSM, self.iips),
                (config_db.VirtualMachineSM, self.vms)):
            p = mock.patch.object(cls, 'get', side_effect=store.get)
            p.start()
            self.addCleanup(p.stop)
        p = mock.patch.object(config_db.FloatingIpSM, 'values',
                              side_effect=lambda: list(self.fips.values()))
        p.start()
        self.addCleanup(p.stop)

        # liveness is now an authoritative api-server read: a VM in self.vms
        # resolves, anything else raises NoIdError (dead).
        def _vm_read(id=None):
            if id in self.vms:
                return object()
            raise loadbalancer_agent.vnc_exc.NoIdError(id)
        self.vnc_lib.virtual_machine_read.side_effect = _vm_read
        # default fresh-read result: markers intact, fixed_ip unset
        self.vnc_lib.floating_ip_read.return_value = self.fresh_fip_obj()
    # end setUp

    def fresh_fip_obj(self, is_vip=True, vip_port_id=VIP_PORT,
                      fixed_ip=None):
        """Fake vnc FloatingIp object returned by the write-gate fresh read."""
        kvp = mock.Mock()
        kvp.get_key.return_value = 'vip_port_id'
        kvp.get_value.return_value = vip_port_id
        ann = mock.Mock()
        ann.get_key_value_pair.return_value = [kvp] if vip_port_id else []
        o = mock.Mock()
        o.get_floating_ip_is_virtual_ip.return_value = is_vip
        o.get_annotations.return_value = ann
        o.get_floating_ip_fixed_ip_address.return_value = fixed_ip
        return o

    # ---- fake cache builders ---------------------------------------------

    def vip_port(self, uuid=VIP_PORT):
        # Octavia VIP placeholder port: instance-ip carries the VIP.
        self.iips[IIP] = Rec(address=VIP)
        self.vmis[uuid] = Rec(uuid=uuid, instance_ips={IIP},
                              virtual_network=NET,
                              aaps=None, virtual_machine=None,
                              virtual_ip=None, loadbalancer=None)
        return uuid

    def live_amphora(self, uuid, vm):
        self.vms[vm] = object()
        self.vmis[uuid] = Rec(uuid=uuid, instance_ips=set(),
                              virtual_network=NET,
                              aaps=[{'ip': {'ip_prefix': VIP}}],
                              virtual_machine=vm, virtual_ip=None,
                              loadbalancer=None)
        return uuid

    def stale_amphora(self, uuid):
        # matching AAP but the backing VM does not resolve (dead)
        self.vmis[uuid] = Rec(uuid=uuid, instance_ips=set(),
                              virtual_network=NET,
                              aaps=[{'ip': {'ip_prefix': VIP}}],
                              virtual_machine='vm-dead', virtual_ip=None,
                              loadbalancer=None)
        return uuid

    def build_vn(self, *vmi_ids):
        self.vns[NET] = Rec(virtual_machine_interfaces=set(vmi_ids))

    def add_fip(self, uuid, refs, fixed_ip=VIP, is_virtual_ip=True,
                vip_port_id=VIP_PORT, vip_address=None):
        f = Rec(uuid=uuid, virtual_machine_interfaces=set(refs),
                fixed_ip=fixed_ip, is_virtual_ip=is_virtual_ip,
                vip_port_id=vip_port_id, vip_address=vip_address,
                address='203.0.113.9')
        self.fips[uuid] = f
        return f

    def ref_calls(self):
        # call[0] = (obj_type, obj_uuid, ref_type, ref_uuid, ref_fq_name, op)
        return [c[0] for c in self.vnc_lib.ref_update.call_args_list]

    # ---- cases ------------------------------------------------------------

    def test_failover_full_recovery(self):
        # The real failover state: cascade-strip wiped ALL refs and nulled
        # fixed_ip. Rebuild VIP port + 2 live amphora from the annotation and
        # restore fixed_ip.
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.live_amphora('amp-2', 'vm-2')
        self.build_vn(VIP_PORT, 'amp-1', 'amp-2')
        fip = self.add_fip('fip-1', [], fixed_ip=None)  # fully stripped

        self.agent.reconcile_fip(fip)

        added = {c[3] for c in self.ref_calls() if c[5] == 'ADD'}
        self.assertEqual(added, {VIP_PORT, 'amp-1', 'amp-2'})
        self.assertNotIn('DELETE', [c[5] for c in self.ref_calls()])
        # fixed_ip restored (was null) to the VIP - never the public
        # address.
        self.vnc_lib.floating_ip_update.assert_called_once()
        fip_obj = self.vnc_lib.floating_ip_read.return_value
        fip_obj.set_floating_ip_fixed_ip_address.assert_called_once_with(VIP)

    def test_stale_amphora_skipped(self):
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.stale_amphora('amp-stale')
        self.build_vn(VIP_PORT, 'amp-1', 'amp-stale')
        fip = self.add_fip('fip-1', [], fixed_ip=None)

        self.agent.reconcile_fip(fip)

        added = {c[3] for c in self.ref_calls() if c[5] == 'ADD'}
        self.assertEqual(added, {VIP_PORT, 'amp-1'})

    def test_no_annotation_noop(self):
        # is_virtual_ip but no vip_port_id pointer -> not ours.
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.build_vn(VIP_PORT, 'amp-1')
        fip = self.add_fip('fip-1', [], fixed_ip=None, vip_port_id=None)

        self.agent.reconcile_fip(fip)

        self.vnc_lib.ref_update.assert_not_called()
        self.vnc_lib.floating_ip_update.assert_not_called()

    def test_vip_port_transient_miss_noop(self):
        # annotation points at a VIP port not in cache, but a fresh read shows
        # it still exists (cache lag) -> retry next tick, no writes, no clear.
        fip = self.add_fip('fip-1', [], fixed_ip=None, vip_port_id='ghost')
        self.vnc_lib.virtual_machine_interface_read.return_value = mock.Mock()
        self.agent.reconcile_fip(fip)
        self.vnc_lib.ref_update.assert_not_called()
        self.vnc_lib.floating_ip_update.assert_not_called()

    def test_vip_port_deleted_clears_orphan_markers(self):
        # annotation points at a VIP port a fresh read confirms is GONE (LB
        # deleted) -> clear is_virtual_ip + the annotations so the FIP stops
        # lingering as an orphan.
        fip = self.add_fip('fip-1', [], fixed_ip=None, vip_port_id='ghost')
        self.vnc_lib.virtual_machine_interface_read.side_effect = \
            loadbalancer_agent.vnc_exc.NoIdError('ghost')
        o = self.fresh_fip_obj()  # is_virtual_ip True, has annotations
        self.vnc_lib.floating_ip_read.return_value = o

        self.agent.reconcile_fip(fip)

        o.set_floating_ip_is_virtual_ip.assert_called_once_with(False)
        self.vnc_lib.floating_ip_update.assert_called_once()
        self.vnc_lib.ref_update.assert_not_called()

    def test_idempotent(self):
        # steady state: refs already correct AND fixed_ip already the VIP.
        # No writes, and no fresh read either (the audit stays read-only).
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.live_amphora('amp-2', 'vm-2')
        self.build_vn(VIP_PORT, 'amp-1', 'amp-2')
        fip = self.add_fip('fip-1', [VIP_PORT, 'amp-1', 'amp-2'], fixed_ip=VIP)

        self.agent.reconcile_fip(fip)

        self.vnc_lib.ref_update.assert_not_called()
        self.vnc_lib.floating_ip_read.assert_not_called()
        self.vnc_lib.floating_ip_update.assert_not_called()

    def test_never_strip_when_no_live_amphora(self):
        # mid-failover gap: no live amphora yet -> zero writes; existing stale
        # ref is NOT deleted this pass (self-heals when a healthy amphora
        # lands).
        self.vip_port()
        self.stale_amphora('amp-stale')
        self.build_vn(VIP_PORT, 'amp-stale')
        fip = self.add_fip('fip-1', [VIP_PORT, 'amp-stale'])

        self.agent.reconcile_fip(fip)

        self.vnc_lib.ref_update.assert_not_called()
        self.vnc_lib.floating_ip_update.assert_not_called()

    def test_audit_only_sweeps_virtual_ip(self):
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.build_vn(VIP_PORT, 'amp-1')
        self.add_fip('fip-vip', [], is_virtual_ip=True)
        self.add_fip('fip-plain', [], is_virtual_ip=False, vip_port_id=None)

        self.agent.schedule_fip_reconcile = mock.Mock()
        self.agent.audit_octavia_fips()

        calls = self.agent.schedule_fip_reconcile.call_args_list
        scheduled = {c[0][0].uuid for c in calls}
        self.assertIn('fip-vip', scheduled)
        self.assertNotIn('fip-plain', scheduled)
        # audit is the missed-event backstop: it must not trust the cache.
        self.assertTrue(all(c.kwargs.get('trust_cache') is False
                            for c in calls))

    # ---- destructive path (prune) ------------------------------------------

    def test_prune_deletes_only_dead_refs(self):
        # refs hold: vip_port, a live amphora, a dead-VM stale ref, a ref
        # whose VMI is not cached at all, and an LBaaS-owned VMI. Only the
        # dead-VM stale ref may be DELETEd.
        self.vip_port()
        self.live_amphora('amp-live', 'vm-live')
        self.stale_amphora('amp-dead')
        self.vmis['amp-lbaas'] = Rec(uuid='amp-lbaas', instance_ips=set(),
                                     virtual_network=NET, aaps=None,
                                     virtual_machine=None,
                                     virtual_ip='vip-x', loadbalancer=None)
        self.build_vn(VIP_PORT, 'amp-live', 'amp-dead', 'amp-lbaas')
        fip = self.add_fip('fip-1', [VIP_PORT, 'amp-live', 'amp-dead',
                                     'amp-uncached', 'amp-lbaas'])

        self.agent.reconcile_fip(fip)

        deleted = [c[3] for c in self.ref_calls() if c[5] == 'DELETE']
        self.assertEqual(deleted, ['amp-dead'])
        # the uncached ref (in-flight create under HA event reordering) and
        # the LBaaS-owned ref must survive; the VIP port always survives.
        self.assertNotIn('amp-uncached', deleted)
        self.assertNotIn('amp-lbaas', deleted)
        self.assertNotIn(VIP_PORT, deleted)

    # ---- write gate (fresh read) --------------------------------------------

    def test_write_gate_aborts_when_markers_cleared(self):
        # concurrent re-associate to a normal port cleared is_virtual_ip:
        # stale writes landing after the plugin's would corrupt the FIP.
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.build_vn(VIP_PORT, 'amp-1')
        fip = self.add_fip('fip-1', [], fixed_ip=None)
        self.vnc_lib.floating_ip_read.return_value = \
            self.fresh_fip_obj(is_vip=False)

        self.agent.reconcile_fip(fip)

        self.vnc_lib.ref_update.assert_not_called()
        self.vnc_lib.floating_ip_update.assert_not_called()

    def test_write_gate_aborts_on_foreign_annotation(self):
        # concurrent re-associate to ANOTHER Octavia LB re-pointed the
        # annotation: writes computed for the old VIP must not land.
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.build_vn(VIP_PORT, 'amp-1')
        fip = self.add_fip('fip-1', [], fixed_ip=None)
        self.vnc_lib.floating_ip_read.return_value = \
            self.fresh_fip_obj(vip_port_id='other-lb-vip')

        self.agent.reconcile_fip(fip)

        self.vnc_lib.ref_update.assert_not_called()
        self.vnc_lib.floating_ip_update.assert_not_called()

    # ---- VIP address resolution ---------------------------------------------

    def test_vip_address_annotation_preferred(self):
        self.vip_port()
        fip = self.add_fip('fip-1', [], vip_address='10.0.0.7')
        self.assertEqual(
            self.agent._vip_address(fip, self.vmis[VIP_PORT]), '10.0.0.7')

    def test_vip_address_family_filter(self):
        # dual-stack VIP port: the v6 instance-ip must not be picked for a
        # v4 FIP; a single v4 candidate wins.
        self.vip_port()
        self.iips['iip-v6'] = Rec(address='fd00::5')
        self.vmis[VIP_PORT].instance_ips = {IIP, 'iip-v6'}
        fip = self.add_fip('fip-1', [])
        self.assertEqual(
            self.agent._vip_address(fip, self.vmis[VIP_PORT]), VIP)

    def test_vip_address_ambiguous_fails_closed(self):
        # additional_vips: two v4 VIPs on one port and no annotation ->
        # never guess (a wrong guess writes a wrong fixed_ip and converges
        # on it). Reconcile becomes a no-op.
        self.vip_port()
        self.iips['iip-2'] = Rec(address='10.0.0.6')
        self.vmis[VIP_PORT].instance_ips = {IIP, 'iip-2'}
        self.live_amphora('amp-1', 'vm-1')
        self.build_vn(VIP_PORT, 'amp-1')
        fip = self.add_fip('fip-1', [], fixed_ip=None)

        self.agent.reconcile_fip(fip)

        self.vnc_lib.ref_update.assert_not_called()
        self.vnc_lib.floating_ip_update.assert_not_called()

    # ---- event fast-path --------------------------------------------------

    def test_reconcile_fip_safe_swallows(self):
        # hooks run inside evaluate_dependency(), which does not catch. A raise
        # there would take down the notification greenlet.
        self.agent.reconcile_fip = mock.Mock(side_effect=ValueError('boom'))
        self.agent.reconcile_fip_safe(self.add_fip('fip-1', []))
        self.agent._svc_mon.logger.error.assert_called_once()

    def test_schedule_defers_and_dedups(self):
        # hook must not reconcile inside the strip window (it would race
        # neutron's own port_delete and 409 Octavia's cleanup); repeated
        # schedules for one FIP coalesce into a single pending timer.
        fip = self.add_fip('fip-1', [])
        t1, t2 = mock.Mock(), mock.Mock()
        with mock.patch.object(loadbalancer_agent.gevent, 'spawn_later',
                               side_effect=[t1, t2]) as sl:
            self.agent.schedule_fip_reconcile(fip)
            self.agent.schedule_fip_reconcile(fip)  # second event, same FIP
        self.assertEqual(sl.call_count, 2)
        sl.assert_called_with(self.agent.FIP_RECONCILE_DELAY,
                              self.agent._run_scheduled_reconcile, fip, True)
        t1.kill.assert_called_once()  # first pending timer cancelled

    def test_amphora_vmi_event_reconciles_matching_fip(self):
        # the new amphora lands after the cascade-strip: no ref links it to the
        # FIP, so the (VN, VIP) match is the only way back.
        self.vip_port()
        amp = self.live_amphora('amp-new', 'vm-new')
        self.build_vn(VIP_PORT, 'amp-new')
        self.add_fip('fip-1', [])
        self.agent.schedule_fip_reconcile = mock.Mock()

        self.agent.reconcile_amphora_vmi(self.vmis[amp])

        sched = self.agent.schedule_fip_reconcile.call_args_list
        reconciled = [c[0][0].uuid for c in sched]
        self.assertEqual(reconciled, ['fip-1'])

    def test_amphora_vmi_event_ignores_other_vn(self):
        self.vip_port()
        amp = self.live_amphora('amp-new', 'vm-new')
        self.vmis[amp].virtual_network = 'net-other'
        self.build_vn(VIP_PORT, 'amp-new')
        self.add_fip('fip-1', [])
        self.agent.schedule_fip_reconcile = mock.Mock()

        self.agent.reconcile_amphora_vmi(self.vmis[amp])

        self.agent.schedule_fip_reconcile.assert_not_called()

    def test_amphora_vmi_event_ignores_non_vip_aap(self):
        # an unrelated VRRP/keepalived port on the same VN: AAP is not the VIP.
        self.vip_port()
        amp = self.live_amphora('amp-new', 'vm-new')
        self.vmis[amp].aaps = [{'ip': {'ip_prefix': '10.0.0.99'}}]
        self.build_vn(VIP_PORT, 'amp-new')
        self.add_fip('fip-1', [])
        self.agent.schedule_fip_reconcile = mock.Mock()

        self.agent.reconcile_amphora_vmi(self.vmis[amp])

        self.agent.schedule_fip_reconcile.assert_not_called()

    def test_amphora_vmi_event_ignores_plain_fip(self):
        self.vip_port()
        amp = self.live_amphora('amp-new', 'vm-new')
        self.build_vn(VIP_PORT, 'amp-new')
        self.add_fip('fip-plain', [], is_virtual_ip=False, vip_port_id=None)
        self.agent.schedule_fip_reconcile = mock.Mock()

        self.agent.reconcile_amphora_vmi(self.vmis[amp])

        self.agent.schedule_fip_reconcile.assert_not_called()

    def test_amphora_vmi_event_rejects_foreign_project(self):
        # R1 parity: with octavia_project_id set, a tenant's own AAP=VIP port
        # is rejected before the FIP scan (the plugin refuses it too).
        self.agent._octavia_project_id = 'octaviaproj'
        self.vip_port()
        amp = self.live_amphora('amp-evil', 'vm-evil')
        self.vmis[amp].parent_key = 'tenant-b'  # not the octavia project
        self.build_vn(VIP_PORT, 'amp-evil')
        self.add_fip('fip-1', [])
        self.agent.schedule_fip_reconcile = mock.Mock()

        self.agent.reconcile_amphora_vmi(self.vmis[amp])

        self.agent.schedule_fip_reconcile.assert_not_called()

    def test_filter_off_warns_once(self):
        # R1 hardening: filter unset (fail-open) must warn once, not silently.
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.build_vn(VIP_PORT, 'amp-1')
        fip = self.add_fip('fip-1', [], fixed_ip=None)

        self.agent.reconcile_fip(fip)
        self.agent.reconcile_fip(fip)

        warns = self.agent._svc_mon.logger.warning.call_count
        self.assertEqual(warns, 1)

    def test_live_amphora_excludes_foreign_project(self):
        # R1 parity in the reconcile lane: an attacker port aliasing the VIP
        # is not wired even though its AAP matches and its VM is live.
        self.agent._octavia_project_id = 'octaviaproj'
        self.vip_port()
        self.vmis[VIP_PORT].parent_key = 'tenant-a'
        good = self.live_amphora('amp-good', 'vm-good')
        self.vmis[good].parent_key = 'octavia-proj'
        evil = self.live_amphora('amp-evil', 'vm-evil')
        self.vmis[evil].parent_key = 'tenant-b'
        self.build_vn(VIP_PORT, good, evil)
        fip = self.add_fip('fip-1', [], fixed_ip=None)

        self.agent.reconcile_fip(fip)

        added = {c[3] for c in self.ref_calls() if c[5] == 'ADD'}
        self.assertEqual(added, {VIP_PORT, 'amp-good'})
        self.assertNotIn('amp-evil', added)

    def test_audit_forces_fresh_read_on_clean_cache(self):
        # R6: a fully-missed event burst leaves the cache falsely steady;
        # audit (trust_cache=False) must still read through and repair.
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.build_vn(VIP_PORT, 'amp-1')
        # cache claims fully wired + fixed_ip set, but server was stripped
        fip = self.add_fip('fip-1', [VIP_PORT, 'amp-1'], fixed_ip=VIP)
        self.vnc_lib.floating_ip_read.return_value = \
            self.fresh_fip_obj(fixed_ip=None)

        self.agent.reconcile_fip(fip, trust_cache=False)

        self.vnc_lib.floating_ip_read.assert_called_once()
        # fixed_ip restored
        self.vnc_lib.floating_ip_update.assert_called_once()

    def test_vip_added_before_amphora(self):
        # R12 write-order: the VIP port ref is ADDed before any amphora ref so
        # a neutron read never sees amphora-only refs.
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.build_vn(VIP_PORT, 'amp-1')
        fip = self.add_fip('fip-1', [], fixed_ip=None)

        self.agent.reconcile_fip(fip)

        adds = [c[3] for c in self.ref_calls() if c[5] == 'ADD']
        self.assertEqual(adds[0], VIP_PORT)

    def test_gate_aborts_on_foreign_vip_address(self):
        # R8: additional_vips re-point between two VIPs on the same port -
        # vip_port_id is unchanged but vip_address moved; stale write aborts.
        self.vip_port()
        self.live_amphora('amp-1', 'vm-1')
        self.build_vn(VIP_PORT, 'amp-1')
        fip = self.add_fip('fip-1', [], fixed_ip=None, vip_address=VIP)
        o = self.fresh_fip_obj()
        k1, k2 = mock.Mock(), mock.Mock()
        k1.get_key.return_value = 'vip_port_id'
        k1.get_value.return_value = VIP_PORT
        k2.get_key.return_value = 'vip_address'
        k2.get_value.return_value = '10.0.0.99'  # re-pointed
        (o.get_annotations.return_value
         .get_key_value_pair.return_value) = [k1, k2]
        self.vnc_lib.floating_ip_read.return_value = o

        self.agent.reconcile_fip(fip)

        self.vnc_lib.ref_update.assert_not_called()
        self.vnc_lib.floating_ip_update.assert_not_called()


class OctaviaFipEvaluateHookTest(unittest.TestCase):
    """The SM.evaluate() hooks that VncAmqpHandle fires on a config change."""

    def setUp(self):
        self.mgr = mock.Mock()
        for cls in (config_db.FloatingIpSM,
                    config_db.VirtualMachineInterfaceSM):
            p = mock.patch.object(cls, '_manager', self.mgr, create=True)
            p.start()
            self.addCleanup(p.stop)

    def fip(self, **kw):
        f = config_db.FloatingIpSM.__new__(config_db.FloatingIpSM)
        f.uuid = 'fip-1'
        f.is_virtual_ip = kw.get('is_virtual_ip', True)
        return f

    def vmi(self, aaps):
        v = config_db.VirtualMachineInterfaceSM.__new__(
            config_db.VirtualMachineInterfaceSM)
        v.uuid = 'vmi-1'
        v.aaps = aaps
        v.virtual_machine = None
        return v

    def test_fip_evaluate_schedules_octavia_fip(self):
        # deferred (schedule_fip_reconcile), never inline: an inline
        # reconcile would race neutron's port_delete mid-strip.
        config_db.FloatingIpSM.evaluate(self.fip(is_virtual_ip=True))
        self.mgr.loadbalancer_agent.schedule_fip_reconcile.assert_called_once()
        # the native LBaaS path must still run
        self.mgr.netns_manager.add_fip_to_vip_vmi.assert_called_once()

    def test_fip_evaluate_skips_plain_fip(self):
        config_db.FloatingIpSM.evaluate(self.fip(is_virtual_ip=False))
        self.mgr.loadbalancer_agent.schedule_fip_reconcile.assert_not_called()

    def test_vmi_evaluate_reconciles_aap_port(self):
        vmi = self.vmi([{'ip': {'ip_prefix': VIP}}])
        config_db.VirtualMachineInterfaceSM.evaluate(vmi)
        (self.mgr.loadbalancer_agent.reconcile_amphora_vmi
         .assert_called_once_with(vmi))

    def test_vmi_evaluate_skips_plain_port(self):
        config_db.VirtualMachineInterfaceSM.evaluate(self.vmi(None))
        self.mgr.loadbalancer_agent.reconcile_amphora_vmi.assert_not_called()
        # the pre-existing port-tuple path is untouched
        self.mgr.port_tuple_agent.update_vmi_port_tuples.assert_called_once()


class FloatingIpSMParseTest(unittest.TestCase):
    """FloatingIpSM.update() against a real object dict.

    Every reconciler test above hand-builds parsed fakes, so a typo in the
    annotation keys or the key_value_pair shape would pass the whole suite
    while breaking all recovery in production. This is the seam test.
    """

    def test_update_parses_octavia_markers(self):
        f = config_db.FloatingIpSM.__new__(config_db.FloatingIpSM)
        f.uuid = 'fip-1'
        f.virtual_machine_interfaces = set()
        f.update({
            'fq_name': ['default-domain', 'proj', 'fip-1'],
            'floating_ip_address': '203.0.113.9',
            'floating_ip_fixed_ip_address': VIP,
            'floating_ip_is_virtual_ip': True,
            'annotations': {'key_value_pair': [
                {'key': 'other', 'value': 'x'},
                {'key': 'vip_port_id', 'value': VIP_PORT},
                {'key': 'vip_address', 'value': VIP},
            ]},
        })
        self.assertEqual(f.vip_port_id, VIP_PORT)
        self.assertEqual(f.vip_address, VIP)
        self.assertTrue(f.is_virtual_ip)
        self.assertEqual(f.fixed_ip, VIP)
        self.assertEqual(f.address, '203.0.113.9')

    def test_update_without_markers(self):
        f = config_db.FloatingIpSM.__new__(config_db.FloatingIpSM)
        f.uuid = 'fip-1'
        f.virtual_machine_interfaces = set()
        f.update({
            'fq_name': ['default-domain', 'proj', 'fip-1'],
            'floating_ip_address': '203.0.113.9',
        })
        self.assertIsNone(f.vip_port_id)
        self.assertIsNone(f.vip_address)
        self.assertFalse(f.is_virtual_ip)
        self.assertIsNone(f.fixed_ip)


if __name__ == '__main__':
    unittest.main()
