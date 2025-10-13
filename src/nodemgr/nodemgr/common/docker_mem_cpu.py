#
# Copyright (c) 2016 Juniper Networks, Inc. All rights reserved.
#

import docker

from nodemgr.common.sandesh.nodeinfo.cpuinfo.ttypes import ProcessCpuInfo


class DockerMemCpuUsageData(object):
    def __init__(self, _id, last_cpu, last_time):
        self.last_cpu = last_cpu
        self.last_time = last_time
        self.client = docker.from_env()
        if hasattr(self.client, 'api'):
            self.client = self.client.api
        self._id = format(_id, 'x').zfill(64)

    def _get_container_stats(self):
        return self.client.stats(self._id, stream=False)

    def _get_process_mem_usage(self, stats):
        memory_stats = stats.get('memory_stats', dict())
        return memory_stats.get('usage', 0) // 1024

    def _get_process_mem_resident(self, stats):
        memory_stats = stats.get('memory_stats', dict())

        # In the new versions of the kernel, 'cgroups v2' is used, which may not have the 'rss' field
        if 'rss' in memory_stats.get('stats', dict()):
            return memory_stats['stats']['rss'] // 1024

        # Instead of it, we can use the 'anon' field if 'cgroups v2' is used.
        return memory_stats.get('stats', dict()).get('anon', 0) // 1024

    def _get_process_cpu_share(self, stats):
        cpu_stats = stats.get('cpu_stats', dict())
        cpu_usage = cpu_stats.get('cpu_usage', dict())

        # In cgroups v2, 'percpu_usage' may be unavailable
        if 'percpu_usage' in cpu_usage:
            cpu_count = len(cpu_usage['percpu_usage'])
        else:
            cpu_count = cpu_stats.get('online_cpus', 0)

        if cpu_count == 0:
            return 0.0

        cpu_total = float(cpu_usage['total_usage'])
        cpu_system = float(cpu_stats['system_cpu_usage'])

        if self.last_cpu and self.last_time:
            cpu_delta = cpu_total - self.last_cpu
            system_cpu_delta = cpu_system - self.last_time
        else:
            self.last_cpu = cpu_total
            self.last_time = cpu_system
            return 0.0

        if system_cpu_delta <= 0.0:
            return 0.0

        self.last_cpu = cpu_total
        self.last_time = cpu_system

        cpu_share = (cpu_delta / system_cpu_delta) * cpu_count
        return round(cpu_share, 2)

    def get_process_mem_cpu_info(self):
        stats = self._get_container_stats()
        process_mem_cpu = ProcessCpuInfo()

        process_mem_cpu.cpu_share = self._get_process_cpu_share(stats)
        process_mem_cpu.mem_virt = self._get_process_mem_usage(stats)
        process_mem_cpu.mem_res = self._get_process_mem_resident(stats)

        return process_mem_cpu
