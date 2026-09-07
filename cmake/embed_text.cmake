# Reads one text file and writes a C++ source exposing it as a std::string_view.
#
# Deliberately separate from embed_web.cmake rather than another entry in its
# TEXT_FILES list.  That script is keyed to a single WEB_DIR, carries a filename
# -> identifier and a filename -> MIME map, and declares into embedded_web.hh --
# none of which fits a systemd unit, and a unit filed among the web assets is an
# invitation to serve it.  This one takes what it needs as arguments:
#
#   INPUT   -- the file to embed
#   OUTPUT  -- full path of the .cc to write
#   HEADER  -- header to #include in the generated file
#   VAR     -- name of the std::string_view, inside namespace embedded
#
# Whoever adds a second caller must also name INPUT *and* this script in the
# DEPENDS of its add_custom_command, or editing the embedded file will not
# rebuild -- the same trap embed_web.cmake carries a warning about.
file(READ "${INPUT}" CONTENT)

# A raw string literal ends at its delimiter, so a file containing the closing
# sequence would silently truncate the embedded copy at that point and leave the
# rest as stray C++.  Refuse instead: this is a build-time check on a file in
# this repository, so failing is free and being wrong is not.
if(CONTENT MATCHES "\\)GDTEXT\"")
   message(FATAL_ERROR
      "${INPUT} contains the raw string delimiter )GDTEXT\", which would "
      "truncate the embedded copy. Change the delimiter in embed_text.cmake.")
endif()

file(WRITE "${OUTPUT}"
   "#include \"${HEADER}\"\nnamespace embedded {\n"
   "const std::string_view ${VAR} = R\"GDTEXT(${CONTENT})GDTEXT\";\n}\n")
