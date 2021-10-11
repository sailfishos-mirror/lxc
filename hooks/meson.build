# SPDX-License-Identifier: LGPL-2.1-or-later

hooks_include_directories = include_directories(
		'../',
		'../src/include')

hooks_unmount_namespace_sources = files('unmount-namespace.c')

hook_programs += executable(
		'unmount-namespace',
		hooks_unmount_namespace_sources,
		include_directories : hooks_include_directories,
		install : true,
		install_dir : lxchookdir)

hook_programs = configure_file(
			configuration : dummy_config_data,
                        input : 'clonehostname',
                        output : 'clonehostname')
install_data(join_paths(project_build_root, 'hooks/clonehostname'), install_dir : lxchookdir)

hook_programs = configure_file(
			configuration : dummy_config_data,
                        input : 'dhclient-script',
                        output : 'dhclient-script')
install_data(join_paths(project_build_root, 'hooks/dhclient-script'), install_dir : lxchookdir)

dhclient_config_data = configuration_data()
dhclient_config_data.set_quoted('LXCHOOKDIR', lxchookdir)
dhclient_config_data.set_quoted('SYSCONFDIR', sysconfdir)

hook_programs = configure_file(
			configuration : dhclient_config_data,
                        input : 'dhclient.in',
                        output : 'dhclient')
install_data(join_paths(project_build_root, 'hooks/dhclient'), install_dir : lxchookdir)

hook_programs = configure_file(
			configuration : dummy_config_data,
                        input : 'mountecryptfsroot',
                        output : 'mountecryptfsroot')
install_data(join_paths(project_build_root, 'hooks/mountecryptfsroot'), install_dir : lxchookdir)

hook_programs = configure_file(
			configuration : dummy_config_data,
                        input : 'nvidia',
                        output : 'nvidia')
install_data(join_paths(project_build_root, 'hooks/nvidia'), install_dir : lxchookdir)

hook_programs = configure_file(
			configuration : dummy_config_data,
                        input : 'squid-deb-proxy-client',
                        output : 'squid-deb-proxy-client')
install_data(join_paths(project_build_root, 'hooks/squid-deb-proxy-client'), install_dir : lxchookdir)

hook_programs = configure_file(
			configuration : dummy_config_data,
                        input : 'ubuntu-cloud-prep',
                        output : 'ubuntu-cloud-prep')
install_data(join_paths(project_build_root, 'hooks/ubuntu-cloud-prep'), install_dir : lxchookdir)