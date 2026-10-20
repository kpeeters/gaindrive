# Removes what the last install from this build directory put in place, as
# listed in the manifest that install writes. Directories are left alone:
# the ones we install into are shared with other packages.
set(manifest "${GD_BINARY_DIR}/install_manifest.txt")
if(NOT EXISTS "${manifest}")
	message(FATAL_ERROR "Nothing to uninstall: ${manifest} not found; install from this build directory first.")
endif()
file(STRINGS "${manifest}" files)
foreach(f ${files})
	if(EXISTS "${f}" OR IS_SYMLINK "${f}")
		message(STATUS "Removing ${f}")
		file(REMOVE "${f}")
	else()
		message(STATUS "Already gone: ${f}")
	endif()
endforeach()
