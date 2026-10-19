cmake_minimum_required(VERSION 3.20)

# A project of its own rather than an option of the server's, so that building
# the server never needs gtkmm and the two packages are configured apart. The
# Chromecast code and its helpers are compiled from ../src by path; nothing is
# copied.
get_filename_component(GD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

# Same single source of the version as the server and the mobile apps.
file(STRINGS ${GD_ROOT}/VERSION _gd_version REGEX "^version=")
string(REGEX REPLACE "^version=" "" _gd_version "${_gd_version}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GD_ROOT}/VERSION)
if(NOT _gd_version MATCHES "^[0-9]+(\\.[0-9]+)*$")
	message(FATAL_ERROR "VERSION: no usable `version=' line (got '${_gd_version}')")
endif()
project(gaindrive-player VERSION ${_gd_version} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(GD_APP_ID org.gaindrive.linux-player)

include(GNUInstallDirs)
include(FetchContent)

find_package(PkgConfig REQUIRED)
# 4.10 is what Ubuntu 24.04 ships, the oldest distribution we aim at.
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0>=4.10)
pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Distributions package nlohmann_json, and a distro build must use theirs; the
# fetch is for machines without it.
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
	set(JSON_Install OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		nlohmann_json
		GIT_REPOSITORY https://github.com/nlohmann/json.git
		GIT_TAG        v3.12.0
		EXCLUDE_FROM_ALL
	)
	FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---- Code shared with the server -------------------------------------------

add_library(gdshared STATIC
	${GD_ROOT}/src/castmanager.cc
	${GD_ROOT}/src/stamp.cc
	${GD_ROOT}/src/jsonread.cc
	${GD_ROOT}/src/md5.cc
	${GD_ROOT}/src/textutil.cc
	${GD_ROOT}/src/wiimeq.cc
	${GD_ROOT}/third_party/httplib.cc
)
target_include_directories(gdshared PUBLIC
	${GD_ROOT}/src
	${GD_ROOT}/third_party)
# PUBLIC: the macro changes the layout of httplib's classes, so every unit
# including httplib.h must agree with the one that compiled httplib.cc.
target_compile_definitions(gdshared PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(gdshared PUBLIC
	nlohmann_json::nlohmann_json
	OpenSSL::SSL
	OpenSSL::Crypto
	Threads::Threads)

# ---- Icons -------------------------------------------------------------------

# Our own symbolic icons, compiled in under the application's resource path,
# where GtkApplication adds them to the icon theme by itself. Themes vary too
# much in which symbolic icons they have; a missing one is drawn as a blob.
find_program(GD_GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
file(GLOB GD_ICONS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/scalable/actions/*.svg)
set(_gd_icon_xml "")
foreach(icon ${GD_ICONS})
	get_filename_component(name ${icon} NAME)
	string(APPEND _gd_icon_xml "    <file>scalable/actions/${name}</file>\n")
endforeach()
string(REPLACE "." "/" _gd_res_path "${GD_APP_ID}")
# The application icon too: on X11 a window that matches no installed desktop
# file, such as a run from the build tree, shows the icon set_icon_name finds
# in the theme, and the hicolor copy installed below is not there yet.
file(CONFIGURE OUTPUT icons.gresource.xml CONTENT
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<gresources>
  <gresource prefix=\"/${_gd_res_path}/icons\">
${_gd_icon_xml}    <file alias=\"scalable/apps/${GD_APP_ID}.svg\">gaindrive.svg</file>
  </gresource>
</gresources>
")
add_custom_command(
	OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/icons.c
	COMMAND ${GD_GLIB_COMPILE_RESOURCES} --generate-source
	        --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data/icons
	        --sourcedir=${GD_ROOT}/graphics
	        --target=${CMAKE_CURRENT_BINARY_DIR}/icons.c
	        ${CMAKE_CURRENT_BINARY_DIR}/icons.gresource.xml
	DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/icons.gresource.xml ${GD_ICONS}
	        ${GD_ROOT}/graphics/gaindrive.svg)

# ---- The player -------------------------------------------------------------

add_executable(gaindrive-player
	src/main.cc
	src/apiclient.cc
	src/castbutton.cc
	src/caster.cc
	src/configfile.cc
	src/coverstore.cc
	src/infoblock.cc
	src/library.cc
	src/localeq.cc
	src/loginview.cc
	src/mainwindow.cc
	src/model.cc
	src/mpris.cc
	src/panes.cc
	src/panestrip.cc
	src/playback.cc
	src/player.cc
	src/playerbar.cc
	src/playlistpicker.cc
	src/secretstore.cc
	src/sections.cc
	src/serverconfig.cc
	src/settings.cc
	src/settingspage.cc
	src/tunebutton.cc
	src/uithread.cc
	src/videocodecs.cc
	src/videoview.cc
	src/wiimpanel.cc
	src/workpool.cc
	${CMAKE_CURRENT_BINARY_DIR}/icons.c
)
target_compile_definitions(gaindrive-player PRIVATE
	GD_APP_ID="${GD_APP_ID}"
	GAINDRIVE_VERSION="${PROJECT_VERSION}")
target_link_libraries(gaindrive-player PRIVATE gdshared PkgConfig::GTKMM PkgConfig::SECRET PkgConfig::GST)

# ---- Video sink -------------------------------------------------------------

# gtk4paintablesink, from gst-plugins-rs, hands decoded frames to GTK without a
# copy. Ubuntu 24.04 does not package it, so it is built here and shipped in
# our package, in a directory of our own that main.cc points GStreamer at.
# 0.12 is the series built against GStreamer 1.24, and --locked keeps its
# dependencies at versions 24.04's Rust 1.75 compiles. Needs cargo and network.
option(GD_BUNDLE_GTK4SINK "Build and ship gtk4paintablesink" ON)
set(GD_PLUGIN_SUBDIR ${CMAKE_INSTALL_LIBDIR}/gaindrive-player/gstreamer-1.0)
target_compile_definitions(gaindrive-player PRIVATE GD_PLUGIN_SUBDIR="${GD_PLUGIN_SUBDIR}")
if(GD_BUNDLE_GTK4SINK)
	find_program(GD_CARGO cargo REQUIRED)
	pkg_check_modules(GTK4 REQUIRED gtk4)
	# dmabuf is the zero-copy path for hardware decoders; it needs 4.14.
	set(_gd_sink_features "wayland,x11egl,x11glx")
	if(GTK4_VERSION VERSION_GREATER_EQUAL 4.14)
		string(APPEND _gd_sink_features ",gtk_v4_14,dmabuf")
	elseif(GTK4_VERSION VERSION_GREATER_EQUAL 4.12)
		string(APPEND _gd_sink_features ",gtk_v4_12")
	endif()
	set(_gd_sink ${CMAKE_CURRENT_BINARY_DIR}/gst-plugins/libgstgtk4.so)
	include(ExternalProject)
	ExternalProject_Add(gtk4sink
		GIT_REPOSITORY    https://gitlab.freedesktop.org/gstreamer/gst-plugins-rs.git
		GIT_TAG           0.12.11
		GIT_SHALLOW       ON
		UPDATE_COMMAND    ""
		CONFIGURE_COMMAND ""
		BUILD_IN_SOURCE   ON
		BUILD_COMMAND     ${GD_CARGO} build --release --locked -p gst-plugin-gtk4
		                  --features ${_gd_sink_features} --target-dir <BINARY_DIR>
		INSTALL_COMMAND   ${CMAKE_COMMAND} -E copy <BINARY_DIR>/release/libgstgtk4.so ${_gd_sink}
		BUILD_BYPRODUCTS  ${_gd_sink})
	add_dependencies(gaindrive-player gtk4sink)
	install(FILES ${_gd_sink} DESTINATION ${GD_PLUGIN_SUBDIR})
endif()

# ---- Installation -----------------------------------------------------------

install(TARGETS gaindrive-player RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(data/${GD_APP_ID}.desktop.in ${GD_APP_ID}.desktop @ONLY)
# AppStream requires a date on each release. VERSION carries none, so the
# configure date stands in; it honours SOURCE_DATE_EPOCH for distro builds.
string(TIMESTAMP GD_RELEASE_DATE "%Y-%m-%d" UTC)
configure_file(data/${GD_APP_ID}.metainfo.xml.in ${GD_APP_ID}.metainfo.xml @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${GD_APP_ID}.desktop
	DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${GD_APP_ID}.metainfo.xml
	DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# The desktop file names the icon by app id, so the file must carry that name.
install(FILES ${GD_ROOT}/graphics/gaindrive.svg
	DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
	RENAME ${GD_APP_ID}.svg)

# ---- Packaging --------------------------------------------------------------

set(CPACK_PACKAGE_NAME gaindrive-player)
set(CPACK_PACKAGE_VENDOR "GainDrive")
set(CPACK_PACKAGE_CONTACT "Kasper Peeters <kasper.peeters@phi-sci.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Desktop player for GainDrive and Subsonic servers")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/kpeeters/gaindrive")
set(CPACK_RESOURCE_FILE_LICENSE ${GD_ROOT}/LICENSE)
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)

# Only the generators whose tools are present, so a Fedora machine can make its
# rpm without dpkg and an Ubuntu machine its deb without rpmbuild.
set(CPACK_GENERATOR "")
find_program(GD_DPKG_DEB dpkg-deb)
find_program(GD_RPMBUILD rpmbuild)
if(GD_DPKG_DEB)
	list(APPEND CPACK_GENERATOR DEB)
endif()
if(GD_RPMBUILD)
	list(APPEND CPACK_GENERATOR RPM)
endif()

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION sound)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# playbin is a plugin, and so is the HTTP source; nothing links them, so
# shlibdeps cannot find them.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "gstreamer1.0-plugins-base, gstreamer1.0-plugins-good")
# libav decodes what films mostly hold (HEVC, AC-3, DTS); gl has glsinkbin,
# which spares the video sink a copy.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gstreamer1.0-plugins-bad, gstreamer1.0-libav, gstreamer1.0-gl")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_REQUIRES "gstreamer1-plugins-base, gstreamer1-plugins-good")
# Directories owned by the filesystem and hicolor-icon-theme packages; listing
# them in our rpm makes it conflict with those on install.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
	/usr/share/applications
	/usr/share/metainfo
	/usr/share/icons
	/usr/share/icons/hicolor
	/usr/share/icons/hicolor/scalable
	/usr/share/icons/hicolor/scalable/apps)

include(CPack)
