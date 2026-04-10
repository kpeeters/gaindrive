#pragma once
#include <string_view>

// Web client files embedded at build time by cmake/embed_web.cmake.
namespace embedded {
   extern const std::string_view index_html;
   extern const std::string_view index_html_mime;

   extern const std::string_view style_css;
   extern const std::string_view style_css_mime;

   extern const std::string_view app_js;
   extern const std::string_view app_js_mime;

   extern const std::string_view favicon_svg;
   extern const std::string_view favicon_svg_mime;
   }
