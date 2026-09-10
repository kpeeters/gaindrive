# Reads the files in web/ and writes a C++ source file that exposes them as
# std::string_view constants in the embedded:: namespace.
#
# Called at build time via add_custom_command; expects:
#   OUTPUT  — full path to write the generated .cc file
#   WEB_DIR — directory containing the web files

set(TEXT_FILES index.html link.html style.css app.js favicon.svg)

# A binary file cannot go in a raw string literal — it contains NUL bytes, and
# no compiler is obliged to carry those through a source file — so these are
# emitted as byte arrays instead.
set(BIN_FILES material-symbols-rounded.woff2)

# Map filenames to C++ identifier names.
set(VARNAME_index.html  index_html)
set(VARNAME_link.html   link_html)
set(VARNAME_style.css   style_css)
set(VARNAME_app.js      app_js)
set(VARNAME_favicon.svg favicon_svg)
set(VARNAME_material-symbols-rounded.woff2 material_symbols_woff2)

# Map filenames to MIME types.
set(MIME_index.html  text/html)
set(MIME_link.html   text/html)
set(MIME_style.css   text/css)
set(MIME_app.js      application/javascript)
set(MIME_favicon.svg image/svg+xml)
set(MIME_material-symbols-rounded.woff2 font/woff2)

set(SRC "#include \"embedded_web.hh\"\nnamespace embedded {\n")

foreach(F IN LISTS TEXT_FILES)
   file(READ "${WEB_DIR}/${F}" CONTENT)
   set(VAR  "${VARNAME_${F}}")
   set(MIME "${MIME_${F}}")
   # Use a delimiter that will never appear in HTML/CSS/JS.
   string(APPEND SRC
      "const std::string_view ${VAR} = R\"GDWEB(${CONTENT})GDWEB\";\n"
      "const std::string_view ${VAR}_mime = \"${MIME}\";\n"
   )
endforeach()

foreach(F IN LISTS BIN_FILES)
   file(READ "${WEB_DIR}/${F}" HEXC HEX)
   # Break the hex string every 32 nibbles first, so the generated array is
   # 16 bytes per line rather than one line of several megabytes.
   string(REGEX REPLACE "(................................)" "\\1\n" HEXC "${HEXC}")
   string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEXC}")
   set(VAR  "${VARNAME_${F}}")
   set(MIME "${MIME_${F}}")
   string(APPEND SRC
      "static const unsigned char ${VAR}_data[] = {\n${BYTES}\n};\n"
      "const std::string_view ${VAR}"
      "(reinterpret_cast<const char*>(${VAR}_data), sizeof ${VAR}_data);\n"
      "const std::string_view ${VAR}_mime = \"${MIME}\";\n"
   )
endforeach()

string(APPEND SRC "}\n")
file(WRITE "${OUTPUT}" "${SRC}")
