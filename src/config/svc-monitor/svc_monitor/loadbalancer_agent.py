from vnc_api.vnc_api import *

from cfgm_common import importutils
from cfgm_common import exceptions as vnc_exc
from cfgm_common import svc_info
from cfgm_common import PERMS_RWX, PERMS_RX
import traceback

import gevent

from .agent import Agent
from .config_db import ServiceApplianceSM, ServiceApplianceSetSM, \
    LoadbalancerPoolSM, InstanceIpSM, VirtualMachineInterfaceSM, \
    VirtualIpSM, LoadbalancerSM, LoadbalancerListenerSM, LoadbalancerMemberSM, \
    HealthMonitorSM, FloatingIpSM, VirtualMachineSM, VirtualNetworkSM

from .sandesh.loadbalancer.ttypes import \
    LoadbalancerConfig, UveLoadbalancerConfig, UveLoadbalancerConfigTrace


class LoadbalancerAgent(Agent):

    def __init__(self, svc_mon, vnc_lib, object_db, config_section):
        # Loadbalancer
        super(LoadbalancerAgent, self).__init__(svc_mon, vnc_lib,
                                                object_db, config_section)
        self._vnc_lib = vnc_lib
        self._svc_mon = svc_mon
        self._object_db = object_db
        self._pool_driver = {}
        self._args = config_section
        self._loadbalancer_driver = {}
        # Octavia service project; gates amphora FIP legs
        # (AAP-hijack guard). Spec: gate ladder.
        self._octavia_project_id = (
            getattr(self._args, 'octavia_project_id', '') or ''
        ).replace('-', '').lower()
        self._octavia_filter_warned = False
        # uuid -> pending reconcile greenlet; coalesce event storms.
        self._fip_reconcile_timers = {}
        # create default service appliance set
        self._create_default_service_appliance_set(
            "opencontrail",
            "svc_monitor.services.loadbalancer.drivers.ha_proxy.driver.OpencontrailLoadbalancerDriver"
        )
        # create native service appliance set
        self._create_default_service_appliance_set(
            "native",
            "svc_monitor.services.loadbalancer.drivers.native.driver.OpencontrailLoadbalancerDriver"
        )
        self._default_provider = "opencontrail"
    # end __init__

    def handle_service_type(self):
        return svc_info.get_lb_service_type()

    def pre_create_service_vm(self, instance_index, si, st, vm):
        for nic in si.vn_info:
            if nic['type'] == svc_info.get_right_if_str():
                vmi = self._get_vip_vmi(si)
                if not vmi:
                    return False
                for iip_id in vmi.instance_ips:
                    nic['iip-id'] = iip_id
                    break
                for fip_id in vmi.floating_ips:
                    nic['fip-id'] = fip_id
                    break
                if len(vmi.security_groups):
                    nic['sg-list'] = vmi.security_groups
                if len(vmi.tags):
                    nic['tags'] = vmi.tags
                nic['user-visible'] = False
            elif nic['type'] == svc_info.get_left_if_str():
                nic['user-visible'] = False

        return True

    def _get_vip_vmi(self, si):
        lb = LoadbalancerSM.get(si.loadbalancer)
        if lb:
            vmi_id = lb.virtual_machine_interface
            vmi = VirtualMachineInterfaceSM.get(vmi_id)
            return vmi

        pool = LoadbalancerPoolSM.get(si.loadbalancer_pool)
        if pool:
            vip = VirtualIpSM.get(pool.virtual_ip)
            if vip:
                vmi_id = vip.virtual_machine_interface
                vmi = VirtualMachineInterfaceSM.get(vmi_id)
                return vmi

        return None

    # create default loadbalancer driver
    def _create_default_service_appliance_set(self, sa_set_name, driver_name):
        default_gsc_name = "default-global-system-config"
        default_gsc_fq_name = [default_gsc_name]
        sa_set_fq_name = [default_gsc_name, sa_set_name]

        try:
            sa_set_obj = self._vnc_lib.service_appliance_set_read(fq_name=sa_set_fq_name)
        except vnc_exc.NoIdError:
            gsc_obj = self._vnc_lib.global_system_config_read(fq_name=default_gsc_fq_name)
            sa_set_obj = ServiceApplianceSet(sa_set_name, gsc_obj)
            perms2 = PermType2('cloud-admin',  PERMS_RWX,  PERMS_RX)
            sa_set_obj.set_perms2(perms2)
            sa_set_obj.set_service_appliance_driver(driver_name)
            sa_set_uuid = self._vnc_lib.service_appliance_set_create(sa_set_obj)
            ServiceApplianceSetSM.locate(sa_set_uuid)

    def load_drivers(self):
        for sas in list(ServiceApplianceSetSM.values()):
            if sas.driver:
                config = self._args.config_sections
                config.add_section(sas.name)
                for kvp in sas.kvpairs or []:
                    config.set(sas.name, kvp['key'], kvp['value'])
                if sas.ha_mode:
                    config.set(sas.name, 'ha_mode', str(sas.ha_mode))
                for sa in sas.service_appliances or []:
                    saobj = ServiceApplianceSM.get(sa)
                    config.set(sas.name, 'device_ip', saobj.ip_address)
                    if saobj.user_credential:
                        if 'username' in saobj.user_credential:
                            config.set(sas.name, 'user',
                                saobj.user_credential['username'])
                        if 'password' in saobj.user_credential:
                            config.set(sas.name, 'password',
                                saobj.user_credential['password'])
                self._loadbalancer_driver[sas.name] = \
                    importutils.import_object(sas.driver, sas.name,
                                              self._svc_mon, self._vnc_lib,
                                              self._object_db, self._args)
    # end load_drivers

    def audit_lb_pools(self):
        for hm_id, config_data, driver_data in self._object_db.health_monitor_list():
            if HealthMonitorSM.get(hm_id):
                continue
            hm = config_data
            if not hasattr(hm, 'provider'):
                continue
            driver = self._get_driver_for_provider(hm['provider'])
            pools = set()
            for i in hm['pools'] or []:
                pools.add(i['pool_id'])
            try:
                if driver is not None:
                    for pool in pools:
                        driver.delete_health_monitor(hm, pool)
            except Exception:
                self._svc_mon.logger.error(traceback.format_exc())
            self._object_db.healthmonitor_remove(hm_id)
        for lb_id, config_data, driver_data in self._object_db.loadbalancer_list():
            if LoadbalancerSM.get(lb_id):
                continue
            # Delete the lb from the driver
            driver = self._get_driver_for_provider(config_data['provider'])
            if driver is not None:
                driver.delete_loadbalancer(config_data)
            self._object_db.loadbalancer_remove(lb_id)
            self._delete_driver_for_loadbalancer(lb_id)
        for pool_id, config_data, driver_data in self._object_db.pool_list():
            if LoadbalancerPoolSM.get(pool_id):
                continue
            # Delete the pool from the driver
            driver = self._get_driver_for_provider(config_data['provider'])
            if driver is not None:
                driver.delete_pool(config_data)
            self._object_db.pool_remove(pool_id)
            self._delete_driver_for_pool(pool_id)

    # Octavia FIP failover reconciler. Spec: svc-monitor lane.
    FIP_RECONCILE_DELAY = 3

    def audit_octavia_fips(self):
        # Periodic backstop for missed/reordered events; trust_cache=False (a
        # dropped-event cache looks steady but is stale).
        for fip_sm in list(FloatingIpSM.values()):
            if fip_sm.is_virtual_ip:
                self.schedule_fip_reconcile(fip_sm, trust_cache=False)
    # end audit_octavia_fips

    def schedule_fip_reconcile(self, fip_sm, trust_cache=True):
        # Defer + coalesce per-uuid: strip races neutron's in-flight
        # port_delete; immediate reconcile 409s Octavia. Spec: handle.
        uuid = fip_sm.uuid
        old = self._fip_reconcile_timers.get(uuid)
        if old is not None:
            old.kill(block=False)
        self._fip_reconcile_timers[uuid] = gevent.spawn_later(
            self.FIP_RECONCILE_DELAY, self._run_scheduled_reconcile,
            fip_sm, trust_cache)
    # end schedule_fip_reconcile

    def _run_scheduled_reconcile(self, fip_sm, trust_cache):
        self._fip_reconcile_timers.pop(fip_sm.uuid, None)
        self.reconcile_fip_safe(fip_sm, trust_cache)
    # end _run_scheduled_reconcile

    def reconcile_fip_safe(self, fip_sm, trust_cache=True):
        # evaluate_dependency() does not swallow raises; all entry points
        # funnel here.
        try:
            self.reconcile_fip(fip_sm, trust_cache)
        except vnc_exc.NoIdError:
            pass  # FIP or ref target deleted mid-reconcile: benign race
        except Exception:
            self._svc_mon.logger.error(
                'reconcile failed for floating-ip %s: %s' %
                (fip_sm.uuid, traceback.format_exc()))
    # end reconcile_fip_safe

    def reconcile_amphora_vmi(self, vmi):
        # New-amphora fast path: strip broke the ref-graph, so match by (VIP
        # network, VIP address). Ownership pre-filter before the scan.
        if not self._vmi_in_octavia_project(vmi):
            return
        for fip_sm in list(FloatingIpSM.values()):
            if not fip_sm.is_virtual_ip or not fip_sm.vip_port_id:
                continue
            vip_port = VirtualMachineInterfaceSM.get(fip_sm.vip_port_id)
            if vip_port is None:
                continue
            if vip_port.virtual_network != vmi.virtual_network:
                continue
            vip = self._vip_address(fip_sm, vip_port)
            if not vip or not self._vmi_has_vip_aap(vmi, vip):
                continue
            self.schedule_fip_reconcile(fip_sm)
    # end reconcile_amphora_vmi

    def reconcile_fip(self, fip_sm, trust_cache=True):
        # Rebuild an Octavia VIP FIP's refs from the durable markers (failover
        # cascade-strips vmi_refs + fixed_ip). Never touch floating_ip_address.
        # Spec: svc-monitor lane.
        if not fip_sm.is_virtual_ip:
            return
        vpid = fip_sm.vip_port_id
        if not vpid:
            return  # not a reconciler-managed FIP (no durable pointer)
        vip_port = VirtualMachineInterfaceSM.get(vpid)
        if vip_port is None:
            # Cache miss: fresh-read to tell transient (retry) from deleted LB
            # (clear markers, orphan cleanup).
            try:
                self._vnc_lib.virtual_machine_interface_read(id=vpid)
                return
            except vnc_exc.NoIdError:
                self._clear_octavia_markers(fip_sm.uuid)
                return
        vip = self._vip_address(fip_sm, vip_port)
        if not vip:
            return
        vn = VirtualNetworkSM.get(vip_port.virtual_network)
        if vn is None:
            return
        amphora = self._live_amphora_vmis(vn, vip, vpid)
        if not amphora:
            return  # mid-failover gap: nothing healthy yet, retry next tick
        desired = {vpid} | amphora
        current = set(fip_sm.virtual_machine_interfaces)
        to_add = desired - current
        to_del = set()
        for vmi_id in current - desired:
            if vmi_id == vpid:
                continue  # never delete the VIP port ref
            vmi = VirtualMachineInterfaceSM.get(vmi_id)
            if vmi is None:
                continue  # in-flight create/delete; skipping is always safe
            if vmi.virtual_ip or vmi.loadbalancer:
                continue  # svc-monitor native LBaaS ref, not ours to prune
            if self._vmi_backed_by_live_vm(vmi):
                continue  # still-live VMI we just did not select; keep it
            to_del.add(vmi_id)
        if (trust_cache and not to_add and not to_del and
                fip_sm.fixed_ip == vip):
            return  # event fast path: cache fresh + clean diff = steady state
        # Fresh read gates every write: a concurrent (re)associate may have
        # re-pointed the markers; stale writes would leave an unrepairable
        # mixed state. Narrow TOCTOU; A-lane anchor closes it. Spec: known
        # residuals.
        fip_obj = self._vnc_lib.floating_ip_read(id=fip_sm.uuid)
        if not fip_obj.get_floating_ip_is_virtual_ip():
            return
        if self._fip_ann(fip_obj, 'vip_port_id') != vpid:
            return  # re-associated to another LB mid-reconcile
        if self._fip_ann(fip_obj, 'vip_address') not in (None, vip):
            return  # re-pointed to another VIP on the port (additional_vips)
        # Restore fixed_ip BEFORE the ADDs: a crash between would leave refs
        # but no fixed_ip, tripping _check_port_fip_assoc on the next
        # associate.
        restored = ''
        if fip_obj.get_floating_ip_fixed_ip_address() != vip:
            fip_obj.set_floating_ip_fixed_ip_address(vip)
            self._vnc_lib.floating_ip_update(fip_obj)
            restored = ', fixed_ip restored to %s' % vip
        # ADD the VIP port ref FIRST so a read never sees an amphora-only FIP
        # (would project a foreign port as port_id).
        add_order = ([vpid] if vpid in to_add else [])
        add_order += sorted(to_add - {vpid})
        for vmi_id in add_order:
            self._vnc_lib.ref_update(
                'floating-ip', fip_sm.uuid,
                'virtual-machine-interface', vmi_id, None, 'ADD')
        for vmi_id in to_del:
            self._vnc_lib.ref_update(
                'floating-ip', fip_sm.uuid,
                'virtual-machine-interface', vmi_id, None, 'DELETE')
        self._svc_mon.logger.info(
            'reconciled floating-ip %s: add %s del %s%s' %
            (fip_sm.uuid, sorted(to_add), sorted(to_del), restored))
    # end reconcile_fip

    @staticmethod
    def _fip_ann(fip_obj, key):
        # read one bare annotation off a vnc FloatingIp object
        kvps = fip_obj.get_annotations()
        for kvp in (kvps.get_key_value_pair() if kvps else None) or []:
            if kvp.get_key() == key:
                return kvp.get_value()
        return None
    # end _fip_ann

    def _clear_octavia_markers(self, fip_id):
        # LB gone (VIP port confirmed deleted): drop is_virtual_ip + vip
        # annotations so the FIP is not an is_virtual_ip=True orphan forever.
        # Reconciler-only (plugin can't: cascade already dropped the back-ref).
        fip_obj = self._vnc_lib.floating_ip_read(id=fip_id)
        if not fip_obj.get_floating_ip_is_virtual_ip():
            return
        fip_obj.set_floating_ip_is_virtual_ip(False)
        kvps = fip_obj.get_annotations()
        if kvps:
            pairs = [p for p in kvps.get_key_value_pair() or []
                     if p.get_key() not in ('vip_port_id', 'vip_address')]
            fip_obj.set_annotations(KeyValuePairs(key_value_pair=pairs))
        self._vnc_lib.floating_ip_update(fip_obj)
        self._svc_mon.logger.info(
            'cleared orphan Octavia markers on floating-ip %s '
            '(VIP port gone, LB deleted)' % fip_id)
    # end _clear_octavia_markers

    def _vip_address(self, fip_sm, vip_port):
        # Prefer the vip_address annotation (disambiguates
        # dual-stack/additional_vips, which the strip nulls); else the
        # family-matched VIP-port iip; ambiguous -> fail closed.
        if fip_sm.vip_address:
            return fip_sm.vip_address
        want_v6 = ':' in (fip_sm.address or '')
        cands = []
        for iip_id in vip_port.instance_ips:
            iip = InstanceIpSM.get(iip_id)
            if iip and iip.address and (':' in iip.address) == want_v6:
                cands.append(iip.address)
        if len(cands) == 1:
            return cands[0]
        return None  # 0 = nothing to wire; >1 = ambiguous, never guess
    # end _vip_address

    def _live_amphora_vmis(self, vn, vip, vip_port_id):
        # Live amphora VRRP VMIs whose active-standby AAP carries the VIP.
        # O(ports on VIP net); revisit for big shared nets. Spec: gate ladder.
        if not self._octavia_project_id and not self._octavia_filter_warned:
            # ownership filter OFF: a tenant could alias the VIP via AAP. Warn
            # once, don't be silent.
            self._octavia_filter_warned = True
            self._svc_mon.logger.warning(
                'octavia_project_id is not set; the Octavia VIP '
                'AAP-hijack guard is DISABLED. Set it to the Octavia service '
                'project uuid to enable the ownership filter.')
        out = set()
        for vmi_id in vn.virtual_machine_interfaces:
            if vmi_id == vip_port_id:
                continue
            vmi = VirtualMachineInterfaceSM.get(vmi_id)
            if vmi is None or not self._vmi_has_vip_aap(vmi, vip):
                continue
            if not self._vmi_in_octavia_project(vmi):
                continue
            if not self._vmi_backed_by_live_vm(vmi):
                continue
            out.add(vmi.uuid)
        return out
    # end _live_amphora_vmis

    def _vmi_in_octavia_project(self, vmi):
        # AAP-hijack guard: only Octavia-project ports may be amphora
        # legs. Enforced when octavia_project_id set. Spec: gate ladder (gate
        # 3).
        if not self._octavia_project_id:
            return True
        parent = (getattr(vmi, 'parent_key', '')
                  or '').replace('-', '').lower()
        return parent == self._octavia_project_id
    # end _vmi_in_octavia_project

    @staticmethod
    def _vmi_has_vip_aap(vmi, vip):
        # aap['ip'] is a SubnetType dict {ip_prefix, ip_prefix_len}.
        for aap in vmi.aaps or []:
            if (aap.get('ip') or {}).get('ip_prefix') == vip:
                return True
        return False
    # end _vmi_has_vip_aap

    def _vmi_backed_by_live_vm(self, vmi):
        # Live-VM check via fresh api-server read, NOT the SM cache (a
        # not-yet-drained deleted VM reads as a live ghost). Spec: gate ladder
        # (gate 4).
        if not vmi.virtual_machine:
            return False
        try:
            self._vnc_lib.virtual_machine_read(id=vmi.virtual_machine)
            return True
        except vnc_exc.NoIdError:
            return False
    # end _vmi_backed_by_live_vm

    def load_driver(self, sas):
        if sas.name in self._loadbalancer_driver:
            del(self._loadbalancer_driver[sas.name])
        if sas.driver:
            config = self._args.config_sections
            try:
                config.remove_section(sas.name)
            except Exception:
                self._svc_mon.logger.error(traceback.format_exc())
            config.add_section(sas.name)
            for kvp in sas.kvpairs or []:
                config.set(sas.name, kvp['key'], kvp['value'])
            if sas.ha_mode:
                config.set(sas.name, 'ha_mode', sas.ha_mode)
            for sa in sas.service_appliances or []:
                saobj = ServiceApplianceSM.get(sa)
                config.set(sas.name, 'device_ip', saobj.ip_address)
                config.set(sas.name, 'user', saobj.user_credential['username'])
                config.set(sas.name, 'password',
                           saobj.user_credential['password'])
            self._loadbalancer_driver[sas.name] = \
                importutils.import_object(sas.driver, sas.name,
                                          self._svc_mon, self._vnc_lib,
                                          self._object_db, self._args)
    # end load_driver

    def unload_driver(self, sas):
        if sas.name not in self._loadbalancer_driver:
            return
        del(self._loadbalancer_driver[sas.name])
    # end unload_driver

    def _get_driver_for_provider(self, provider_name):
        return self._loadbalancer_driver.get(provider_name)
    # end _get_driver_for_provider

    def _get_driver_for_pool(self, pool_id, provider=None):
        if pool_id in self._pool_driver:
            return self._pool_driver[pool_id]
        if not provider:
            pool = LoadbalancerPoolSM.get(pool_id)
            provider = pool.provider
        if provider:
            driver = self._get_driver_for_provider(provider)
            if driver is not None:
                self._pool_driver[pool_id] = driver

            return driver
        return self._loadbalancer_driver.get(self._default_provider)
    # end _get_driver_fr_pool

    def _update_driver_for_pool(self, driver, pool_id):
        self._pool_driver[pool_id] = driver
    # end _update_driver_for_pool

    def _delete_driver_for_pool(self, pool_id):
        if pool_id in self._pool_driver:
            del self._pool_driver[pool_id]
    # end _delete_driver_for_pool

    def _get_driver_for_loadbalancer(self, lb_id, provider=None):
        if lb_id in self._loadbalancer_driver:
            return self._loadbalancer_driver[lb_id]
        if not provider:
            lb = LoadbalancerSM.get(lb_id)
            provider = lb.provider
        if provider:
            driver = self._get_driver_for_provider(provider)
            if driver is not None:
                self._loadbalancer_driver[lb_id] = driver

            return driver
        return self._loadbalancer_driver.get(self._default_provider)
    # end _get_driver_for_loadbalancer

    def _delete_driver_for_loadbalancer(self, lb_id):
        if lb_id in self._loadbalancer_driver:
            del self._loadbalancer_driver[lb_id]
    # end _delete_driver_for_loadbalancer

    # Loadbalancer
    def loadbalancer_pool_add(self, pool):
        p = self.loadbalancer_pool_get_reqdict(pool)
        driver = self._get_driver_for_pool(p['id'], p['provider'])
        try:
            if not pool.last_sent:
                driver.create_pool(p)
            else:
                driver.update_pool(pool.last_sent, p)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        if p['loadbalancer_version'] == 'v1':
            self._object_db.pool_config_insert(p['id'], p)
        return p
    # end loadbalancer_pool_add

    def loadbalancer_member_add(self, member):
        m = self.loadbalancer_member_get_reqdict(member)
        driver = self._get_driver_for_pool(m['pool_id'])
        try:
            if not member.last_sent:
                driver.create_member(m)
            elif m != member.last_sent:
                driver.update_member(member.last_sent, m)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        return m
    # end loadbalancer_member_add

    def virtual_ip_add(self, vip):
        v = self.virtual_ip_get_reqdict(vip)
        driver = self._get_driver_for_pool(v['pool_id'])
        try:
            driver.set_config_v1(vip.loadbalancer_pool)
            if not vip.last_sent:
                driver.create_vip(v)
            elif v != vip.last_sent:
                driver.update_vip(vip.last_sent, v)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        return v
    # end  virtual_ip_add

    def delete_virtual_ip(self, obj):
        v = obj.last_sent
        driver = self._get_driver_for_pool(v['pool_id'])
        try:
            driver.delete_vip(v)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
    # end delete_virtual_ip

    def loadbalancer_add(self, loadbalancer):
        lb = self.loadbalancer_get_reqdict(loadbalancer)
        driver = self._get_driver_for_loadbalancer(lb['id'], lb['provider'])
        self.send_lb_config_uve(lb['id'], False)
        try:
            lbaas_config = driver.set_config_v2(loadbalancer.uuid)
            lb['config'] = lbaas_config
            if not loadbalancer.last_sent:
                driver.create_loadbalancer(lb)
            elif lb != loadbalancer.last_sent:
                driver.update_loadbalancer(loadbalancer.last_sent, lb)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        self._object_db.loadbalancer_config_insert(lb['id'], lb)
        return lb

    def suspend_loadbalancer(self, loadbalancer):
        if loadbalancer.provider != 'native':
            return
        lb = self.loadbalancer_get_reqdict(loadbalancer)
        driver = self._get_driver_for_loadbalancer(lb['id'], lb['provider'])
        try:
            driver.suspend_loadbalancer(lb)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        self._object_db.loadbalancer_remove(lb['id'])
        self._delete_driver_for_loadbalancer(lb['id'])

    def delete_loadbalancer(self, loadbalancer):
        lb = self.loadbalancer_get_reqdict(loadbalancer)
        driver = self._get_driver_for_loadbalancer(lb['id'], lb['provider'])
        self.send_lb_config_uve(lb['id'], True)
        try:
            driver.delete_loadbalancer(lb)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        self._object_db.loadbalancer_remove(lb['id'])
        self._delete_driver_for_loadbalancer(lb['id'])

    def listener_add(self, listener):
        ll = self.listener_get_reqdict(listener)
        driver = self._get_driver_for_loadbalancer(ll['loadbalancer_id'])
        try:
            if not listener.last_sent:
                driver.create_listener(ll)
            elif ll != listener.last_sent:
                driver.update_listener(listener.last_sent, ll)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        return ll

    def delete_listener(self, listener):
        ll = self.listener_get_reqdict(listener)
        driver = self._get_driver_for_loadbalancer(ll['loadbalancer_id'])
        try:
            if driver is not None:
                driver.delete_listener(ll)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())

    def delete_loadbalancer_member(self, obj):
        m = obj.last_sent
        driver = self._get_driver_for_pool(m['pool_id'])
        try:
            if driver is not None:
                driver.delete_member(m)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
    # end delete_loadbalancer_member

    def delete_loadbalancer_pool(self, obj):
        p = obj.last_sent
        driver = self._get_driver_for_pool(p['id'], p['provider'])
        try:
            if driver is not None:
                driver.delete_pool(p)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        if p['loadbalancer_version'] == 'v1':
            self._object_db.pool_remove(p['id'])
        self._delete_driver_for_pool(p['id'])
    # end delete_loadbalancer_pool

    def loadbalancer_health_monitor_add(self, obj):
        hm = self.hm_get_reqdict(obj)
        if 'provider' not in hm:
            return None
        current_pools = hm['pools'] or []
        old_pools = []
        if obj.last_sent:
            old_hm = obj.last_sent
            old_pools = old_hm['pools'] or []

        set_current_pools = set()
        set_old_pools = set()
        for i in current_pools:
            set_current_pools.add(i['pool_id'])
        for i in old_pools:
            set_old_pools.add(i['pool_id'])
        update_pools = set_current_pools & set_old_pools
        delete_pools = set_old_pools - set_current_pools
        add_pools = set_current_pools - set_old_pools
        try:
            driver = self._get_driver_for_provider(hm['provider'])
            if driver is not None:
                for pool in add_pools:
                    driver.create_health_monitor(hm, pool)
                for pool in delete_pools:
                    driver.delete_health_monitor(hm, pool)
                for pool in update_pools:
                    driver.update_health_monitor(old_hm, hm, pool)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        if hm['provider'] == 'native':
            self._object_db.health_monitor_config_insert(hm['id'], hm)
        return hm
    # end loadbalancer_health_monitor_add

    def suspend_loadbalancer_health_monitor(self, obj):
        hm = self._object_db.health_monitor_config_get(obj.uuid)
        if not hasattr(hm, 'provider') or hm['provider'] != 'native':
            return
        pools = set()
        for i in hm['pools'] or []:
            pools.add(i['pool_id'])
        try:
            driver = self._get_driver_for_provider(hm['provider'])
            if driver is not None:
                for pool in pools:
                    driver.delete_health_monitor(hm, pool)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        self._object_db.healthmonitor_remove(hm['id'])
    # end suspend_loadbalancer_health_monitor

    def delete_loadbalancer_health_monitor(self, obj):
        if obj.last_sent is None:
            return
        hm = obj.last_sent
        if not hasattr(hm, 'provider') or hm['provider'] != 'native':
            return
        pools = set()
        for i in hm['pools'] or []:
            pools.add(i['pool_id'])
        try:
            driver = self._get_driver_for_provider(hm['provider'])
            if driver is not None:
                for pool in pools:
                    driver.delete_health_monitor(hm, pool)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
        self._object_db.healthmonitor_remove(hm['id'])
    # end delete_loadbalancer_health_monitor

    def _get_vip_pool_id(self, vip):
        pool_refs = vip.loadbalancer_pool
        if pool_refs is None:
            return None
        return pool_refs
    # end _get_vip_pool_id

    def _get_interface_params(self, port_id, props):
        if port_id is None:
            return None

        if not props['address']:
            vmi = VirtualMachineInterfaceSM.get(port_id)
            for iip_id in vmi.instance_ips:
                iip = InstanceIpSM.get(iip_id)
                props['address'] = iip.address
                break

        return port_id
    # end _get_interface_params

    def virtual_ip_get_reqdict(self, vip):
        props = vip.params
        port_id = self._get_interface_params(vip.virtual_machine_interface,
            props)

        res = {'id': vip.uuid,
               'tenant_id': vip.parent_uuid.replace('-', ''),
               'name': vip.display_name,
               'description': self._get_object_description(vip),
               'subnet_id': props['subnet_id'],
               'address': props['address'],
               'port_id': port_id,
               'protocol_port': props['protocol_port'],
               'protocol': props['protocol'],
               'pool_id': self._get_vip_pool_id(vip),
               'session_persistence': None,
               'connection_limit': props['connection_limit'],
               'admin_state_up': props['admin_state'],
               'status': self._get_object_status(vip)}

        if props['persistence_type']:
            sp = {'type': props['persistence_type']}
            if props['persistence_type'] == 'APP_COOKIE':
                sp['cookie_name'] = props['persistence_cookie_name']
            res['session_persistence'] = sp

        return res
    # end virtual_ip_get_reqdict

    def loadbalancer_get_reqdict(self, lb):
        props = lb.params
        res = {'id': lb.uuid,
               'config': None,
               'tenant_id': lb.parent_uuid.replace('-', ''),
               'name': lb.display_name,
               'description': self._get_object_description(lb),
               'subnet_id': props['vip_subnet_id'],
               'address': props['vip_address'],
               'port_id': lb.virtual_machine_interface,
               'provider': lb.provider,
               'status': self._get_object_status(lb)}

        return res
    # end loadbalancer_get_reqdict

    def listener_get_reqdict(self, listener):
        props = listener.params

        res = {'id': listener.uuid,
               'tenant_id': listener.parent_uuid.replace('-', ''),
               'name': listener.display_name,
               'description': self._get_object_description(listener),
               'protocol_port': props['protocol_port'],
               'protocol': props['protocol'],
               'loadbalancer_id': listener.loadbalancer,
               'admin_state_up': props['admin_state'],
               'connection_limit': props['connection_limit'],
               'default_tls_container': getattr(props, 'default_tls_container', None),
               'sni_containers': getattr(props, 'sni_containers', None),
               'status': self._get_object_status(listener)}

        return res

    _loadbalancer_health_type_mapping = {
        'admin_state': 'admin_state_up',
        'monitor_type': 'type',
        'delay': 'delay',
        'timeout': 'timeout',
        'max_retries': 'max_retries',
        'http_method': 'http_method',
        'url_path': 'url_path',
        'expected_codes': 'expected_codes'
    }

    def hm_get_reqdict(self, health_monitor):
        res = {'id': health_monitor.uuid,
               'tenant_id': health_monitor.parent_uuid.replace('-', ''),
               'status': self._get_object_status(health_monitor)}

        props = health_monitor.params
        for key, mapping in list(self._loadbalancer_health_type_mapping.items()):
            if key in props and props[key]:
                res[mapping] = props[key]

        pool_ids = []
        pool_back_refs = health_monitor.loadbalancer_pools
        for pool_back_ref in pool_back_refs or []:
            pool_id = {}
            pool_id['pool_id'] = pool_back_ref
            pool_ids.append(pool_id)
        res['pools'] = pool_ids

        if pool_ids:
            pool = LoadbalancerPoolSM.get(pool_ids[0]['pool_id'])
            if pool:
                res['provider'] = pool.provider

        return res
    # end hm_get_reqdict

    _loadbalancer_member_type_mapping = {
        'admin_state': 'admin_state_up',
        'status': 'status',
        'protocol_port': 'protocol_port',
        'weight': 'weight',
        'address': 'address',
        'subnet_id': 'subnet_id',
    }

    def loadbalancer_member_get_reqdict(self, member):
        res = {'id': member.uuid,
               'vmi': member.vmi,
               'pool_id': member.loadbalancer_pool,
               'status': self._get_object_status(member)}

        pool = LoadbalancerPoolSM.get(member.loadbalancer_pool)
        res['tenant_id'] = pool.parent_uuid.replace('-', '')

        props = member.params
        for key, mapping in list(self._loadbalancer_member_type_mapping.items()):
            if key in props and props[key]:
                res[mapping] = props[key]

        return res
    # end loadbalancer_member_get_reqdict

    _loadbalancer_pool_type_mapping = {
        'admin_state': 'admin_state_up',
        'protocol': 'protocol',
        'loadbalancer_method': 'lb_method',
        'subnet_id': 'subnet_id'
    }

    def _get_object_description(self, obj):
        id_perms = obj.id_perms
        if id_perms is None:
            return None
        return id_perms['description']
    # end _get_object_description

    def _get_object_status(self, obj):
        id_perms = obj.id_perms
        if id_perms and id_perms['enable']:
            return "ACTIVE"
        return "PENDING_DELETE"
    # end _get_object_status

    def loadbalancer_pool_get_reqdict(self, pool):
        res = {
            'id': pool.uuid,
            'loadbalancer_id': pool.loadbalancer_id,
            'loadbalancer_version': pool.loadbalancer_version,
            'tenant_id': pool.parent_uuid.replace('-', ''),
            'name': pool.display_name,
            'description': self._get_object_description(pool),
            'status': self._get_object_status(pool),
            'session_persistence': None,
        }

        props = pool.params
        if props:
            for key, mapping in list(self._loadbalancer_pool_type_mapping.items()):
                if key in props and props[key]:
                    res[mapping] = props[key]

            if 'session_persistence' in props and  props['session_persistence']:
                sp = {'type': props['session_persistence']}
                if props['session_persistence'] == 'APP_COOKIE':
                    sp['cookie_name'] = props['persistence_cookie_name']
                res['session_persistence'] = sp

        # provider
        res['provider'] = pool.provider

        # vip_id
        res['vip_id'] = None
        vip_refs = pool.virtual_ip
        if vip_refs is not None:
            res['vip_id'] = vip_refs

        # members
        res['members'] = list(pool.members)

        # health_monitors
        res['health_monitors'] = list(pool.loadbalancer_healthmonitors)

        # TODO: health_monitor_status
        res['health_monitors_status'] = []
        return res
    # end loadbalancer_pool_get_reqdict

    def _send_lb_config_uve(self, lb_id, deleted):
        lb = LoadbalancerSM.get(lb_id)
        if not lb:
            return
        sandesh = self._svc_mon.logger._sandesh
        if deleted == True:
            uve_lb = UveLoadbalancerConfig(name=lb.uuid, deleted=True)
            uve_lb.listener = {}
            uve_lb.pool = {}
            uve_trace = UveLoadbalancerConfigTrace(data=uve_lb, sandesh=sandesh)
            uve_trace.send(sandesh=sandesh)
            return
        uve_lb = UveLoadbalancerConfig()
        uve_lb.name = lb.uuid
        uve_lb.listener = {}
        uve_lb.pool = {}
        pool_found = False
        for ll_id in lb.loadbalancer_listeners:
            ll = LoadbalancerListenerSM.get(ll_id)
            if not ll:
                continue
            if not ll.params['admin_state']:
                continue
            ll_uuid = ll.uuid
            pools = []
            pool =  LoadbalancerPoolSM.get(ll.loadbalancer_pool)
            if pool and pool.params['admin_state']:
                pools.append(pool.uuid)
                uve_lb_listener = LoadbalancerConfig()
                uve_lb_listener.pool_uuid = pools
                uve_lb.listener[ll_uuid] = uve_lb_listener
                pool_uuid = pool.uuid
                pool_found = True
                members = []
                uve_lb_pool = LoadbalancerConfig()
                for member_id in pool.members:
                    member = LoadbalancerMemberSM.get(member_id)
                    if member:
                        members.append(member.uuid)
                uve_lb_pool.member_uuid = members
                uve_lb.pool[pool_uuid] = uve_lb_pool
        if pool_found == True:
            uve_trace = UveLoadbalancerConfigTrace(data=uve_lb, sandesh=sandesh)
            uve_trace.send(sandesh=sandesh)
        else:
            uve_lb = UveLoadbalancerConfig(name=lb.uuid, deleted=True)
            uve_lb.listener = {}
            uve_lb.pool = {}
            uve_trace = UveLoadbalancerConfigTrace(data=uve_lb, sandesh=sandesh)
            uve_trace.send(sandesh=sandesh)

    def send_lb_config_uve(self, lb_id, deleted):
        try:
            self._send_lb_config_uve(lb_id, deleted)
        except Exception:
            self._svc_mon.logger.error(traceback.format_exc())
