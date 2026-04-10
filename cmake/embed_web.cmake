# Reads web/index.html, web/style.css, web/app.js and writes a C++ source file
# that exposes them as std::string_view constants in the embedded:: namespace.
#
# Called at build time via add_custom_command; expects:
#   OUTPUT  — full path to write the generated .cc file
#   WEB_DIR — directory containing the three web files

set(FILES index.html style.css app.js favicon.svg)

# Map filenames to C++ identifier names.
set(VARNAME_index.html  index_html)
set(VARNAME_style.css   style_css)
set(VARNAME_app.js      app_js)
set(VARNAME_favicon.svg favicon_svg)

# Map filenames to MIME types.
set(MIME_index.html  text/html)
set(MIME_style.css   text/css)
set(MIME_app.js      application/javascript)
set(MIME_favicon.svg image/svg+xml)

set(SRC "#include \"embedded_web.hh\"\nnamespace embedded {\n")

foreach(F IN LISTS FILES)
   file(READ "${WEB_DIR}/${F}" CONTENT)
   set(VAR  "${VARNAME_${F}}")
   set(MIME "${MIME_${F}}")
   # Use a delimiter that will never appear in HTML/CSS/JS.
   string(APPEND SRC
      "const std::string_view ${VAR} = R\"GDWEB(${CONTENT})GDWEB\";\n"
      "const std::string_view ${VAR}_mime = \"${MIME}\";\n"
   )
endforeach()

string(APPEND SRC "}\n")
file(WRITE "${OUTPUT}" "${SRC}")
