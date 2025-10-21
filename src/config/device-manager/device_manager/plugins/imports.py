#
# Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
#
# This file contains utility method for importing all plugin
# imeplementations for self registration. All plugins must be
# imported before DeviceConf invokes plugin registrations.
# Please add an entry here if there is a new plugin
#
# flake8: noqa


def import_plugins():
    from device_manager.plugins.juniper.juniper_conf import JuniperConf
    from device_manager.plugins.juniper.mx.mx_conf import MxConf
    from device_manager.plugins.juniper.qfx.qfx_conf import QfxConf
    from device_manager.plugins.juniper.qfx.series5K.qfx_5k import Qfx5kConf
    from device_manager.plugins.juniper.qfx.series10K.qfx_10k import Qfx10kConf
    from device_manager.plugins.juniper.mxe2.e2_conf import MxE2Conf
# end import_plugins


def import_ansible_plugins():
    from device_manager.plugins.ansible.ansible_conf import AnsibleConf
    from device_manager.plugins.ansible.overlay.overlay_conf import OverlayConf
# end import_ansible_plugins


def import_feature_plugins():
    from device_manager.plugins.feature.underlay_ip_clos_feature import UnderlayIpClosFeature
    from device_manager.plugins.feature.overlay_bgp_feature import OverlayBgpFeature
    from device_manager.plugins.feature.l2_gateway_feature import L2GatewayFeature
    from device_manager.plugins.feature.l3_gateway_feature import L3GatewayFeature
    from device_manager.plugins.feature.vn_interconnect_feature import VnInterconnectFeature
    from device_manager.plugins.feature.assisted_replicator_feature import AssistedReplicatorFeature
    from device_manager.plugins.feature.port_profile_feature import PortProfileFeature
    from device_manager.plugins.feature.telemetry_feature import TelemetryFeature
    from device_manager.plugins.feature.infra_bms_access_feature import InfraBMSAccessFeature
    from device_manager.plugins.feature.security_group_feature import SecurityGroupFeature
    from device_manager.plugins.feature.dc_gateway_feature import DcGatewayFeature
    from device_manager.plugins.feature.pnf_service_chaining_feature import PNFSrvcChainingFeature
# end import_feature_plugins
