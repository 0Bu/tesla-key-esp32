# Build-time re-assembly of the web UI: splice www/style.css and www/app.js back into
# the marker lines in www/index.html, producing the ONE self-contained page the firmware
# embeds (then gzips — see main/CMakeLists.txt). The sources are split for edit locality
# only; the served asset is byte-identical to the former monolithic index.html.
#
#   cmake -DHTML=<index.html> -DCSS=<style.css> -DJS=<app.js> -DOUT=<out.html> -P inline_assets.cmake
#
# string(FIND/REPLACE) is used instead of configure_file so the CSS/JS content is treated
# as opaque bytes — no @VAR@ / ${VAR} substitution can ever mangle the assets.

foreach(v HTML CSS JS OUT)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "inline_assets.cmake: missing -D${v}=<path>")
    endif()
endforeach()

file(READ "${HTML}" page)
file(READ "${CSS}" css)
file(READ "${JS}" js)

# Fail loudly if a marker went missing (e.g. edited away): silently serving the raw
# template would ship a page without its CSS/JS.
string(FIND "${page}" "/*@@INLINE:style.css@@*/\n" css_at)
if(css_at EQUAL -1)
    message(FATAL_ERROR "inline_assets.cmake: CSS marker not found in ${HTML}")
endif()
string(FIND "${page}" "//@@INLINE:app.js@@\n" js_at)
if(js_at EQUAL -1)
    message(FATAL_ERROR "inline_assets.cmake: JS marker not found in ${HTML}")
endif()

# Replace the whole marker line (incl. its newline) with the asset content, so the
# output matches the pre-split monolithic file byte for byte.
string(REPLACE "/*@@INLINE:style.css@@*/\n" "${css}" page "${page}")
string(REPLACE "//@@INLINE:app.js@@\n" "${js}" page "${page}")

file(WRITE "${OUT}" "${page}")
