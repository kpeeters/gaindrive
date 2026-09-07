cmake_minimum_required(VERSION 3.20)

# Removes everything the last `cmake --install` of this build directory wrote.
#
# Run through the generated target rather than directly:
#
#     sudo cmake --build build --target uninstall-dry-run   # look first
#     sudo cmake --build build --target uninstall
#
# Expects MANIFEST (install_manifest.txt), PREFIX (CMAKE_INSTALL_PREFIX) and
# optionally DRY_RUN.
#
# The manifest is the authority and CMake writes no other record, which has
# three consequences worth knowing before trusting the result:
#
#   * It is REWRITTEN by every install, so it describes the last one only. An
#     install under different options -- a different prefix, or a version of
#     this tree whose install() rules differed -- left files this cannot see.
#     So uninstall BEFORE reconfiguring or reinstalling, never after.
#   * It lists files and never directories, so the directories those files sat
#     in are pruned here by hand, and only when empty.
#   * It records nothing gaindrive itself wrote. `--install-service` writes a
#     unit and a config, and a running server writes databases, transcodes and
#     thumbnails; none of that is CMake's and none of it is removed here.

if(NOT EXISTS "${MANIFEST}")
	message(FATAL_ERROR
		"No install manifest at ${MANIFEST}.\n"
		"  Nothing has been installed from this build directory, or the build "
		"directory has been\n"
		"  cleaned since. CMake keeps no other record, so there is nothing to "
		"undo from here.")
endif()

file(STRINGS "${MANIFEST}" FILES)
if(NOT FILES)
	message(FATAL_ERROR "${MANIFEST} is empty; there is nothing to undo.")
endif()

set(REMOVED 0)
set(MISSING 0)
set(FAILED "")
set(DIRS "")
set(GONE "")

foreach(F IN LISTS FILES)
	# EXISTS follows symlinks, so a link whose target has already gone reads as
	# absent and would be left behind -- and shared libraries are installed as
	# chains of them (libarchive.so -> .so.13 -> .so.13.8.9). IS_SYMLINK is what
	# catches the dangling ones.
	if(NOT EXISTS "${F}" AND NOT IS_SYMLINK "${F}")
		message(STATUS "already gone: ${F}")
		math(EXPR MISSING "${MISSING} + 1")
		continue()
	endif()

	get_filename_component(D "${F}" DIRECTORY)
	list(APPEND DIRS "${D}")

	if(DRY_RUN)
		message(STATUS "would remove: ${F}")
		math(EXPR REMOVED "${REMOVED} + 1")
		list(APPEND GONE "${F}")
		continue()
	endif()

	# file(REMOVE) is silent about failure, so the check is after the fact
	# rather than on a return code -- otherwise "uninstalled" would be reported
	# for a file that is still there because this was not run with sudo.
	file(REMOVE "${F}")
	if(EXISTS "${F}" OR IS_SYMLINK "${F}")
		message(STATUS "CANNOT REMOVE: ${F}")
		list(APPEND FAILED "${F}")
	else()
		message(STATUS "removed: ${F}")
		math(EXPR REMOVED "${REMOVED} + 1")
		list(APPEND GONE "${F}")
	endif()
endforeach()

# Prune the directories those files sat in, deepest first so a parent is
# considered only once its children are gone.
#
# GONE carries what has been removed, and both modes consult it rather than
# only the filesystem.  That is what lets the dry run answer the question it
# exists for: nothing has actually been emptied, so asking the disk would
# report every directory as still occupied and the preview would show none of
# them -- which is the half of the blast radius somebody running this as root
# most wants to see beforehand.
#
# A directory goes only when empty, only strictly below the install prefix, and
# never if it is one of the standard first-level ones: /usr/local/include being
# empty on some machine is not a licence to delete it, and /etc -- which holds
# gaindrive.conf.example -- is outside the prefix and so never a candidate at
# all.  Deleting a directory another package also uses is not recoverable by
# reinstalling gaindrive.
set(KEEP bin sbin lib lib64 libexec include share etc var man doc)
list(REMOVE_DUPLICATES DIRS)
# Reverse lexicographic order puts a child before its parent, since a longer
# path sharing a prefix always sorts after the shorter one.
list(SORT DIRS)
list(REVERSE DIRS)

get_filename_component(PREFIX_ABS "${PREFIX}" ABSOLUTE)
set(PRUNED 0)

foreach(D IN LISTS DIRS)
	set(WALKING ON)
	while(WALKING)
		set(WALKING OFF)
		if(NOT IS_DIRECTORY "${D}")
			break()
		endif()
		# Strictly below the prefix: equal is not below, and a path not
		# starting with it is somewhere else entirely.
		string(FIND "${D}" "${PREFIX_ABS}/" AT)
		if(NOT AT EQUAL 0)
			break()
		endif()
		string(LENGTH "${PREFIX_ABS}/" PLEN)
		string(SUBSTRING "${D}" ${PLEN} -1 REL)
		if(REL IN_LIST KEEP)
			break()
		endif()

		file(GLOB CONTENTS "${D}/*" "${D}/.*")
		set(LEFT "")
		foreach(C IN LISTS CONTENTS)
			get_filename_component(N "${C}" NAME)
			if(N STREQUAL "." OR N STREQUAL "..")
				continue()
			endif()
			if(NOT C IN_LIST GONE)
				list(APPEND LEFT "${C}")
			endif()
		endforeach()
		if(LEFT)
			break()
		endif()

		if(DRY_RUN)
			message(STATUS "would remove empty directory: ${D}")
		else()
			file(REMOVE_RECURSE "${D}")
			if(IS_DIRECTORY "${D}")
				message(STATUS "CANNOT REMOVE DIRECTORY: ${D}")
				list(APPEND FAILED "${D}")
				break()
			endif()
			message(STATUS "removed empty directory: ${D}")
		endif()
		math(EXPR PRUNED "${PRUNED} + 1")
		list(APPEND GONE "${D}")
		get_filename_component(D "${D}" DIRECTORY)   # its parent may now be empty
		set(WALKING ON)
	endwhile()
endforeach()

message(STATUS "")
if(DRY_RUN)
	message(STATUS "Dry run: ${REMOVED} file(s) and ${PRUNED} empty "
	               "directory/ies would be removed, ${MISSING} already gone.")
	message(STATUS "Nothing has been changed. Drop -dry-run from the target "
	               "name to do it.")
else()
	message(STATUS "Removed ${REMOVED} file(s) and ${PRUNED} empty "
	               "directory/ies; ${MISSING} were already gone.")
endif()

# Said every time rather than only when something is left, because what is NOT
# removed here is the part somebody is most likely to assume was.
message(STATUS "")
message(STATUS "Not touched, because CMake never installed them:")
message(STATUS "  the systemd unit and config written by --install-service")
message(STATUS "      undo those with: sudo gaindrive --uninstall-service")
message(STATUS "  your own /etc/gaindrive.conf, your databases, and the")
message(STATUS "      transcode cache")

if(FAILED)
	list(LENGTH FAILED N)
	message(STATUS "")
	message(FATAL_ERROR
		"${N} file(s) could not be removed. Installed files are usually owned "
		"by root:\n"
		"      sudo cmake --build ${BUILD_DIR} --target uninstall")
endif()
