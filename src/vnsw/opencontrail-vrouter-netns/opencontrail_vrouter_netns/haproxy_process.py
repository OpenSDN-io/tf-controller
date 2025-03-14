import os
import subprocess
import shlex
import logging
import itertools

from .cert_mgr.cert_manager import CertManager

LBAAS_DIR = "/var/lib/contrail/loadbalancer"
HAPROXY_DIR = LBAAS_DIR + "/" + "haproxy"
HAPROXY_PROCESS = 'haproxy'
HAPROXY_PROCESS_CONF = HAPROXY_PROCESS + ".conf"
SUPERVISOR_BASE_DIR = '/etc/contrail/supervisord_vrouter_files/lbaas-haproxy-'

LOG_FILE= '/var/log/contrail/contrail-lbaas-haproxy-stdout.log'
logging.basicConfig(level=logging.WARNING,
                    format='%(asctime)s %(levelname)-8s %(message)s',
                    datefmt='%m/%d/%Y %H:%M:%S',
                    filename=LOG_FILE)
log_levels = {
    'MSG': {
        'name': 'MSG',
        'value': 35,
    },
}
for log_level_key in list(log_levels.keys()):
    log_level = log_levels[log_level_key]
    logging.addLevelName(log_level['value'], log_level['name'])


def delete_haproxy_dir(base_dir, loadbalancer_id):
    dir_name = base_dir + "/" +  loadbalancer_id
    cmd = "rm -rf " + dir_name
    cmd_list = shlex.split(cmd)
    p = subprocess.Popen(cmd_list)
    p.communicate()


def create_haproxy_dir(base_dir, loadbalancer_id):
    dir_name = base_dir + "/" + loadbalancer_id
    cmd = "mkdir -p " + dir_name
    cmd_list = shlex.split(cmd)
    p = subprocess.Popen(cmd_list)
    p.communicate()
    return dir_name


def update_ssl_config(haproxy_config,
             haproxy_ssl_cert_path, dir_name):
    search_string = 'haproxy_ssl_cert_path'
    for line in haproxy_config.split('\n'):
        if search_string in line:
            haproxy_config = haproxy_config.replace(
                             search_string, haproxy_ssl_cert_path)
            break
    return haproxy_config


def get_haproxy_config_file(cfg_file, dir_name):
    f = open(cfg_file)
    content = f.read()
    f.close()

    lb_dict = dict((k, v) for k, v in (item.split('::::') for item in content.split(':::::')))
    lb_version = lb_dict['lb_version']
    lb_uuid = lb_dict['lb_uuid']
    haproxy_config = lb_dict['haproxy_config']
    if 'ssl crt' in haproxy_config:
        if lb_version == 'v1':
            lb_ssl_cert_path = lb_dict['lb_ssl_cert_path']
            if not (os.path.isfile(lb_ssl_cert_path)):
                msg = "%s is missing for "\
                      "Loadbalancer-ID %s" %(lb_ssl_cert_path, lb_uuid)
                logging.error(msg)
                return None
            haproxy_config = update_ssl_config(haproxy_config,
                             lb_ssl_cert_path, dir_name);
        else:
            if 'lbaas_auth_conf' in lb_dict:
                lbaas_auth_conf = lb_dict['lbaas_auth_conf']
            else:
                lbaas_auth_conf = '/etc/contrail/contrail-lbaas-auth.conf'
            if not (os.path.isfile(lbaas_auth_conf)):
                msg = "%s is missing for "\
                      "Loadbalancer-ID %s" %(lbaas_auth_conf, lb_uuid)
                logging.error(msg)
                return None
            provider = 'barbican'
            if 'orchestrator' in lb_dict:
                orchestrator = lb_dict['orchestrator']
            else:
                orchestrator = 'openstack'
            haproxy_config = CertManager.update_ssl_config(orchestrator,
                provider, haproxy_config, lbaas_auth_conf, dir_name)
        if haproxy_config is None:
            return None

    haproxy_cfg_file = dir_name + "/" + HAPROXY_PROCESS_CONF
    f = open(haproxy_cfg_file, 'w+')
    f.write(haproxy_config)
    f.close()

    return haproxy_cfg_file


def get_pid_file_from_conf_file(conf_file):
    dir_name = os.path.dirname(conf_file)
    pid_file = dir_name + "/" + "haproxy.pid"
    return pid_file


def remove_unmovable_files(scheme, loadbalancer_id):
    if (scheme == "1"):
        # remove the haproxy.sock file
        haproxy_sock = LBAAS_DIR + "/" + loadbalancer_id + "." + "haproxy.sock"
        cmd = "rm -rf " + haproxy_sock
        cmd_list = shlex.split(cmd)
        p = subprocess.Popen(cmd_list)
        p.communicate()


def transform_oldscheme_to_newscheme(loadbalancer_id):
    scheme = ""
    haproxy_json_conf =  LBAAS_DIR + "/" + loadbalancer_id + "." + "conf.json"
    old_haproxy_dir = LBAAS_DIR + "/" + loadbalancer_id
    new_haproxy_dir = HAPROXY_DIR + "/" + loadbalancer_id
    if (os.path.isfile(haproxy_json_conf)): #check for old scheme1
        scheme = "1"
        cmd = "mkdir -p " + new_haproxy_dir
        cmd_list = shlex.split(cmd)
        p = subprocess.Popen(cmd_list)
        p.communicate()
        # move the json.conf file to new scheme haproxy dir
        cmd = "mv " + haproxy_json_conf + " " + new_haproxy_dir
        cmd_list = shlex.split(cmd)
        p = subprocess.Popen(cmd_list)
        p.communicate()
        # move the haproxy.pid file to new scheme haproxy dir
        haproxy_pid = LBAAS_DIR + "/" + loadbalancer_id + "." + "haproxy.pid"
        new_haproxy_pid = new_haproxy_dir + "/haproxy.pid"
        if (os.path.isfile(new_haproxy_pid)):
            cmd = "rm -rf " + haproxy_pid
        else:
            cmd = "mv " + haproxy_pid + " " +  new_haproxy_pid
        cmd_list = shlex.split(cmd)
        p = subprocess.Popen(cmd_list)
        p.communicate()
        # move the haproxy.conf file to new scheme haproxy dir
        haproxy_conf = LBAAS_DIR + "/" + loadbalancer_id + "." + "haproxy.conf"
        new_haproxy_conf = new_haproxy_dir + "/haproxy.conf"
        cmd = "mv " + haproxy_conf + " " +  new_haproxy_conf
        cmd_list = shlex.split(cmd)
        p = subprocess.Popen(cmd_list)
        p.communicate()
    elif (os.path.isdir(old_haproxy_dir)): #check for old scheme2
        scheme = "2"
        cmd = "mkdir -p " + HAPROXY_DIR
        cmd_list = shlex.split(cmd)
        p = subprocess.Popen(cmd_list)
        p.communicate()
        if (os.path.isdir(new_haproxy_dir)):
            cmd = "rm -rf " + old_haproxy_dir
        else:
            cmd = "mv " + old_haproxy_dir + " " + HAPROXY_DIR
        cmd_list = shlex.split(cmd)
        p = subprocess.Popen(cmd_list)
        p.communicate()
    return scheme


def stop_haproxy(loadbalancer_id, daemon_mode=False):
    old_scheme = transform_oldscheme_to_newscheme(loadbalancer_id)
    conf_file = HAPROXY_DIR + "/" +  loadbalancer_id + "/" + HAPROXY_PROCESS_CONF
    try:
        if daemon_mode:
            _stop_haproxy_daemon(loadbalancer_id, conf_file)
        else:
            pool_id = os.path.split(os.path.dirname(conf_file))[1]
            _stop_supervisor_haproxy(pool_id)
    except Exception as e:
        msg = "Exception in Stopping haproxy for Loadbalancer-ID %s" % loadbalancer_id
        logging.exception(msg)

    delete_haproxy_dir(HAPROXY_DIR, loadbalancer_id)
    if (old_scheme):
        remove_unmovable_files(old_scheme, loadbalancer_id)


def start_update_haproxy(loadbalancer_id, cfg_file,
                         netns, daemon_mode=False):
    try:
        pool_id = loadbalancer_id
        dir_name = create_haproxy_dir(HAPROXY_DIR, loadbalancer_id)
        haproxy_cfg_file = get_haproxy_config_file(cfg_file, dir_name)
        if haproxy_cfg_file is None:
            msg = "Failed to Create haproxy config for Loadbalancer-ID %s" %loadbalancer_id
            logging.error(msg)
            stop_haproxy(loadbalancer_id, daemon_mode)
            return False
    except Exception as e:
        msg = "Exception in Createing haproxy config for Loadbalancer-ID %s" %loadbalancer_id
        logging.exception(msg)
        stop_haproxy(loadbalancer_id, daemon_mode)
        return False
    try:
        if daemon_mode:
            _start_haproxy_daemon(pool_id, netns, haproxy_cfg_file)
        else:
            _start_supervisor_haproxy(pool_id, netns, haproxy_cfg_file)
    except Exception as e:
        msg = "Exception in Starting/Updating haproxy for Loadbalancer-ID %s" %loadbalancer_id
        logging.exception(msg)
        return False
    return True


def _get_lbaas_pid(conf_file):
    pid_file = get_pid_file_from_conf_file(conf_file)
    if not os.path.isfile(pid_file):
        return None
    cmd = 'cat %s' % pid_file
    cmd_list = shlex.split(cmd)
    p = subprocess.Popen(cmd_list, stdout=subprocess.PIPE)
    pid, err = p.communicate()
    if err:
        logging.error("failed to get lbaas pid: " + err.decode())
        return None
    return pid.decode()


def _stop_haproxy_daemon(loadbalancer_id, conf_file):
    log_msg = log_levels['MSG']
    last_pid = _get_lbaas_pid(conf_file)
    if last_pid:
        msg = "Stopping haproxy for Loadbalancer-ID %s" %loadbalancer_id
        logging.log(log_msg['value'], msg)
        cmd_list = shlex.split('kill -9 ' + last_pid)
        p = subprocess.Popen(cmd_list, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, stderr = p.communicate()
        if stdout:
            logging.log(log_msg['value'], stdout.decode())
        if stderr:
            logging.log(log_msg['value'], stderr.decode())


def _start_haproxy_daemon(pool_id, netns, conf_file):
    log_msg = log_levels['MSG']
    loadbalancer_id = pool_id
    last_pid = _get_lbaas_pid(conf_file)
    if last_pid:
        msg = "Updating haproxy for Loadbalancer-ID %s" %loadbalancer_id
        sf_opt = '-sf ' + last_pid
    else:
        msg = "Starting haproxy for Loadbalancer-ID %s" %loadbalancer_id
        sf_opt = ''

    pid_file = get_pid_file_from_conf_file(conf_file)
    logging.log(log_msg['value'], msg)

    cmd = 'ip netns exec %s haproxy -f %s -p %s %s' % \
        (netns, conf_file, pid_file, sf_opt)
    cmd_list = shlex.split(cmd)
    p = subprocess.Popen(cmd_list, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout, stderr = p.communicate()
    if stdout:
        logging.log(log_msg['value'], stdout.decode())
    if stderr:
        logging.log(log_msg['value'], stderr.decode())


def _stop_supervisor_haproxy(pool_id):
    pool_suffix = _get_pool_suffix(pool_id)
    file_name = SUPERVISOR_BASE_DIR + pool_suffix + '.ini'
    cmd = "rm " + file_name
    cmd_list = shlex.split(cmd)
    subprocess.Popen(cmd_list)
    _update_supervisor()


def _start_supervisor_haproxy(pool_id, netns, conf_file):
    data = []
    data.extend(_set_config(pool_id, netns, conf_file))
    pool_suffix = _get_pool_suffix(pool_id)
    with open(SUPERVISOR_BASE_DIR + pool_suffix + '.ini', "w") as f:
        f.write('\n'.join(data) + '\n')
    _update_supervisor()


def _get_pool_suffix(pool_id):
    return pool_id.split('-')[0]


def _update_supervisor():
    if os.path.exists('/tmp/supervisord_vrouter.sock'):
        cmd = "supervisorctl -s unix:///tmp/supervisord_vrouter.sock update"
    else:
        cmd = "supervisorctl -s unix:///var/run/supervisord_vrouter.sock update"
    cmd_list = shlex.split(cmd)
    subprocess.Popen(cmd_list)


def _set_config(pool_id, netns, conf_file):
    pool_suffix = _get_pool_suffix(pool_id)
    program_name = 'lbaas-haproxy-%s' % pool_suffix
    if os.path.exists('/tmp/supervisord_vrouter.sock'):
        cmd = "supervisorctl -s unix:///tmp/supervisord_vrouter.sock pid"
    else:
        cmd = "supervisorctl -s unix:///var/run/supervisord_vrouter.sock pid"
    cmd += program_name
    cmd_list = shlex.split(cmd)
    p = subprocess.Popen(cmd_list, stdout=subprocess.PIPE)
    last_pid, _ = p.communicate()
    last_pid = last_pid.decode()
    try:
        int(last_pid)
        sf_opt = '-sf ' + last_pid
    except ValueError:
        sf_opt = ''

    opts = [
        '[program:%s]' % program_name,
        'command=ip netns exec %s haproxy -f %s -db %s' % \
            (netns, conf_file, sf_opt),
        'priority=420',
        'autostart=true',
        'killasgroup=true',
        'stdout_capture_maxbytes=1MB',
        'redirect_stderr=true',
        'stdout_logfile=/var/log/contrail/lbaas-haproxy-stdout.log',
        'stderr_logfile=/dev/null',
        'startsecs=5',
        'exitcodes=0'
    ]

    return itertools.chain(o for o in opts)
