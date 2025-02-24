#!/usr/bin/python3

import argparse
import os
import sys

import gevent.monkey
gevent.monkey.patch_thread()

import cmd_wrapper

from cliff import app
from cliff import commandmanager

VERSION = '1.1'
CONTRAIL_API_VERSION = '1.1'


def run_command(cmd, cmd_parser, sub_argv):
    _argv = sub_argv
    index = -1
    values_specs = []
    if '--' in sub_argv:
        index = sub_argv.index('--')
        _argv = sub_argv[:index]
        values_specs = sub_argv[index:]
    if '-h' in sub_argv:
        index = sub_argv.index('-h')
        _argv = sub_argv[:index]
        values_specs = sub_argv[index:]
    known_args, _values_specs = cmd_parser.parse_known_args(_argv)
    _argv = sub_argv
    cmd.values_specs = (index == -1 and _values_specs or values_specs)
    return cmd.run(_argv)


COMMAND_LIST = {
    'add-service-policy': cmd_wrapper.CmdServicePolicyAdd,
    'delete-service-policy': cmd_wrapper.CmdServicePolicyDelete,
    'add-vrouter': cmd_wrapper.CmdVrouterProvisionAdd,
    'delete-vrouter': cmd_wrapper.CmdVrouterProvisionDel,
    'add-virtual-dns': cmd_wrapper.CmdAddVirtualDns,
    'add-route-target': cmd_wrapper.CmdAddRouteTarget,
    'add-virtual-dns-record': cmd_wrapper.CmdAddVirtualDnsRecord,
    'associate-virtual-dns': cmd_wrapper.CmdAssociateVirtualDns,
    'delete-projects': cmd_wrapper.CmdDeleteProjects,
    'delete-route-target': cmd_wrapper.CmdDeleteRouteTarget,
    'delete-virtual-dns': cmd_wrapper.CmdDelVirtualDns,
    'delete-virtual-dns-record': cmd_wrapper.CmdDelVirtualDnsRecord,
    'disassociate-virtual-dns': cmd_wrapper.CmdDisassociateVirtualDns,
    'provision-forwarding-mode': cmd_wrapper.CmdForwardingModeSetup,
    'add-static-route': cmd_wrapper.CmdStaticRouteProvisionerAdd,
    'delete-static-route': cmd_wrapper.CmdStaticRouteProvisionerDelete,
    'add-service-instance': cmd_wrapper.CmdServiceInstanceAdd,
    'delete-service-instance': cmd_wrapper.CmdServiceInstanceDelete,
    'list-service-template': cmd_wrapper.CmdServiceTemplateList,
    'add-service-template': cmd_wrapper.CmdServiceTemplateAdd,
    'delete-service-template': cmd_wrapper.CmdServiceTemplateDel,
    'check-contrail-version': cmd_wrapper.CmdContrailVersion,
    'create-floating-ip-pool': cmd_wrapper.CmdCreateFloatingIPPool,
    'add-bgp-router': cmd_wrapper.CmdAddBgpRouter,
    'delete-bgp-router': cmd_wrapper.CmdDeleteBgpRouter,
    'control-provision-add': cmd_wrapper.CmdProvisionControlAdd,
    'control-provision-delete': cmd_wrapper.CmdProvisionControlDelete,
    'add-encapsulation-policy': cmd_wrapper.CmdProvisionEncapsulationAdd,
    'update-encapsulation-policy': cmd_wrapper.CmdProvisionEncapsulationUpdate,
    'add-linklocal': cmd_wrapper.CmdProvisionLinklocalAdd,
    'update-linklocal': cmd_wrapper.CmdProvisionLinklocalUpdate,

}

COMMANDS = {'1.1': COMMAND_LIST}


class HelpAction(argparse.Action):

    def __call__(self, parser, namespace, values, option_string=None):
        outputs = []
        max_len = 0
        app = self.default
        parser.print_help(app.stdout)
        app.stdout.write('\nCommands for Contrail v%s:\n' % app.api_version)
        command_manager = app.command_manager
        for name, ep in sorted(command_manager):
            factory = ep.load()
            cmd = factory(self, None)
            one_liner = cmd.get_description().split('\n')[0]
            outputs.append((name, one_liner))
            max_len = max(len(name), max_len)
        for (name, one_liner) in outputs:
            app.stdout.write(' %s %s\n' % (name.ljust(max_len), one_liner))
        sys.exit(0)


class ContrailShell(app.App):

    def __init__(self, apiversion):
        super(ContrailShell, self).__init__(
            description='Contrail helper utility',
            version=VERSION,
            command_manager=commandmanager.CommandManager('contrail.cli'),)
        self.commands = COMMANDS
        self.api_version = apiversion
        for k, v in self.commands[apiversion].items():
            self.command_manager.add_command(k, v)

    def build_option_parser(self, description, version):
        parser = argparse.ArgumentParser(
            description=description,
            add_help=False, )
        parser.add_argument(
            '--version',
            action='version',
            version=version, )
        parser.add_argument(
            '-h', '--help',
            action=HelpAction,
            default=self,
            nargs=0,
            help='Show this help message and exit.')
        return parser

    def _bash_completion(self):
        commands = set()
        options = set()
        import pdb
        pdb.set_trace()
        for option, _action in self.parser._option_string_actions.items():
            options.add(option)
        for command_name, command in self.command_manager:
            commands.add(command_name)
            cmd_factory = command.load()
            cmd = cmd_factory(self, None)
            cmd_parser = cmd.get_parser('')
            for option, _action in cmd_parser._option_string_actions.items():
                options.add(option)
        print(' '.join(commands | options))

    def run(self, argv):
        try:
            index = 0
            command_pos = -1
            help_pos = -1
            argv_bak = argv
            help_command_pos = -1
            for arg in argv:
                if arg == 'bash-completion':
                    self._bash_completion()
                    return 0
                if arg in self.commands[self.api_version]:
                    if command_pos == -1:
                        command_pos = index
                elif arg in ('-h', '--help'):
                    if help_pos == -1:
                        help_pos = index
                elif arg == 'help':
                    if help_command_pos == -1:
                        help_command_pos = index
                index = index + 1
            if command_pos > -1 and help_pos > command_pos:
                # argv = ['help' argv[command_pos]]
                argv = [argv[command_pos]]
            if help_command_pos > -1 and command_pos == -1:
                argv[help_command_pos] = '--help'
            self.options, remainder = self.parser.parse_known_args(argv)
            remainder = argv_bak
            self.interactive_mode = not remainder
            self.initialize_app(remainder)
        except Exception as err:
            print('Exception:', err)
            return 1
        result = 0
        if self.interactive_mode:
            _argv = [sys.argv[0]]
            sys.argv = _argv
            result = self.interact()
        else:
            result = self.run_subcommand(remainder)
        return result

    def run_subcommand(self, argv):
        subcommand = self.command_manager.find_command(argv)
        cmd_factory, cmd_name, sub_argv = subcommand
        sub_argv = argv
        cmd = cmd_factory(self, self.options)
        err = None
        result = 1
        try:
            self.prepare_to_run_command(cmd)
            full_name = (cmd_name
                         if self.interactive_mode
                         else ' '.join([self.NAME, cmd_name])
                         )
            cmd_parser = cmd.get_parser(full_name)
            return run_command(cmd, cmd_parser, sub_argv)
        except Exception as err:
            print('Exception:', err)
            try:
                self.clean_up(cmd, result, err)
            except Exception as err2:
                print('Exception:', err2)
        else:
            try:
                self.clean_up(cmd, result, None)
            except Exception as err3:
                print("Exception:", err3)
        return result

    def initialize_app(self, argv):
        super(ContrailShell, self).initialize_app(argv)
        self.api_version = {'contrail': self.api_version}
        cmd_name = None
        if argv:
            cmd_info = self.command_manager.find_command(argv)
            cmd_factory, cmd_name, sub_argv = cmd_info

    def clean_up(self, cmd, result, err):
        print('clean_up %s', cmd.__class__.__name__)
        if err:
            print(('Got an error: %s'), unicode(err))


def main(argv=sys.argv[1:]):
    try:
        return ContrailShell(CONTRAIL_API_VERSION).run(argv)
    except Exception as e:
        print(unicode(e))
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
