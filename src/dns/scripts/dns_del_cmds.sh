#
#Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

python3 disassociate_virtual_dns.py --api_server_ip 10.204.216.39 --api_server_port 8082 --ipam_fqname default-domain:demo:con_ipam 
python3 disassociate_virtual_dns.py --api_server_ip 10.204.216.39 --api_server_port 8082 --ipam_fqname default-domain:demo:con_ipam --vdns_fqname default-domain:con_vdns 
python3 del_virtual_dns_record.py --api_server_ip 10.204.216.39 --api_server_port 8082 --fq_name default-domain:con_vdns:con_rec
python3 del_virtual_dns.py --api_server_ip 10.204.216.39 --api_server_port 8082 --fq_name default-domain:con_vdns

