# Layering enforcement for hc_core: it must never link OpenCV, X11, or Xtst,
# directly or transitively. Those belong to the app/overlay layers that are
# built in much later tasks; hc_core must stay portable (e.g. to Android,
# where none of them exist).
#
# hc_assert_no_forbidden_deps(<target>) walks <target>'s LINK_LIBRARIES and
# INTERFACE_LINK_LIBRARIES and halts configuration with FATAL_ERROR if any
# entry matches "opencv", "x11", or "xtst", case-insensitively.
function(hc_assert_no_forbidden_deps target)
  set(_hc_forbidden_patterns "opencv" "x11" "xtst")

  foreach(_hc_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_hc_libs ${target} ${_hc_prop})
    if(_hc_libs)
      foreach(_hc_lib ${_hc_libs})
        string(TOLOWER "${_hc_lib}" _hc_lib_lower)
        foreach(_hc_pattern ${_hc_forbidden_patterns})
          if(_hc_lib_lower MATCHES "${_hc_pattern}")
            message(FATAL_ERROR
              "hc_assert_no_forbidden_deps: target '${target}' links "
              "forbidden dependency '${_hc_lib}' (matched pattern "
              "'${_hc_pattern}') via ${_hc_prop}. hc_core must not link "
              "OpenCV, X11, or Xtst.")
          endif()
        endforeach()
      endforeach()
    endif()
  endforeach()
endfunction()
