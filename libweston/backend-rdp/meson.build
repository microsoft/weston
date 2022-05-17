if not get_option('backend-rdp')
	subdir_done()
endif

config_h.set('BUILD_RDP_COMPOSITOR', '1')

dep_frdp = dependency('freerdp3', version: '>= 3.0.0', required: false)
if not dep_frdp.found()
	dep_frdp = dependency('freerdp2', version: '>= 2.2.0', required: false)
	if not dep_frdp.found()
		error('RDP-backend requires freerdp >= 2.2.0 which was not found. Or, you can use \'-Dbackend-rdp=false\'.')
	endif
endif

dep_frdp_server = dependency('freerdp-server3', version: '>= 3.0.0', required: false)
if not dep_frdp_server.found()
	dep_frdp_server = dependency('freerdp-server2', version: '>= 2.2.0', required: false)
	if not dep_frdp_server.found()
		error('RDP-backend requires freerdp-server >= 2.2.0 which was not found. Or, you can use \'-Dbackend-rdp=false\'.')
	endif
endif

dep_wpr = dependency('winpr3', version: '>= 3.0.0', required: false)
if not dep_wpr.found()
	dep_wpr = dependency('winpr2', version: '>= 2.2.0', required: false)
	if not dep_wpr.found()
		error('RDP-backend requires winpr >= 2.2.0 which was not found. Or, you can use \'-Dbackend-rdp=false\'.')
	endif
endif

dep_rdpapplist = dependency('rdpapplist', version: '>= 1.0.0', required: false)
if dep_rdpapplist.found()
	config_h.set('HAVE_FREERDP_RDPAPPLIST_H', '1')
endif

if cc.has_header('freerdp/channels/gfxredir.h', dependencies: dep_frdp)
	config_h.set('HAVE_FREERDP_GFXREDIR_H', '1')
endif

if cc.has_member(
	'rdpsnd_server_context', 'use_dynamic_virtual_channel',
	dependencies : dep_frdp,
	prefix : '#include <freerdp/server/rdpsnd.h>'
)
	config_h.set('HAVE_RDPSND_DYNAMIC_VIRTUAL_CHANNEL', '1')
endif

deps_rdp = [
	dep_threads,
	dep_libdl,
	dep_libweston_private,
	dep_frdp,
	dep_frdp_server,
	dep_wpr,
	dep_rdpapplist,
]

dep_openssl = dependency('openssl', version: '>= 1.1.1', required: false)
if dep_openssl.found()
	config_h.set('HAVE_OPENSSL', '1')
	deps_rdp += dep_openssl
endif

srcs_rdp = [
        'hash.c',
        'rdp.c',
        'rdpdisp.c',
        'rdpclip.c',
        'rdprail.c',
        'rdputil.c',
]

plugin_rdp = shared_library(
	'rdp-backend',
	srcs_rdp,
	include_directories: common_inc,
	dependencies: deps_rdp,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston
)
env_modmap += 'rdp-backend.so=@0@;'.format(plugin_rdp.full_path())
install_headers(backend_rdp_h, subdir: dir_include_libweston_install)
