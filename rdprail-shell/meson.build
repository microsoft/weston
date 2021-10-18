if get_option('shell-rdprail')
	dep_winpr = dependency('winpr3', version: '>= 3.0.0', required: false)
	if dep_winpr.found()
		config_h.set('HAVE_WINPR', '1')
	else
		dep_winpr = dependency('winpr2', version: '>= 2.0.0', required: false)
		if dep_winpr.found()
			config_h.set('HAVE_WINPR', '1')
		endif
	endif
	dep_librsvg = dependency('librsvg-2.0', version: '>= 2.36.0', required: false)
	if dep_librsvg.found()
		config_h.set('HAVE_LIBRSVG2', '1')
	endif
	srcs_shell_rdprail = [
		'shell.c',
		'input-panel.c',
		'app-list.c',
		'img-load.c',
		weston_rdprail_shell_server_protocol_h,
		weston_rdprail_shell_protocol_c,
		input_method_unstable_v1_server_protocol_h,
		input_method_unstable_v1_protocol_c,
	]
	deps_shell_rdprail = [
		dep_libm,
		dep_libexec_weston,
		dep_libshared,
		dep_lib_desktop,
		dep_libweston_public,
		dep_librsvg,
		dep_winpr,
	]
	plugin_shell_rdprail = shared_library(
		'rdprail-shell',
		srcs_shell_rdprail,
		link_with: lib_cairo_shared,
		include_directories: common_inc,
		dependencies: deps_shell_rdprail,
		name_prefix: '',
		install: true,
		install_dir: dir_module_weston,
		install_rpath: '$ORIGIN'
	)
	env_modmap += 'rdprail-shell.so=@0@;'.format(plugin_shell_rdprail.full_path())
endif
