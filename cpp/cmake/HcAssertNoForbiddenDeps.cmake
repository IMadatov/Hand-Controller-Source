# Layering enforcement. The module boundaries in this project are checked by
# the build, not described in a comment, because a comment cannot fail.
#
# The dependency order is one-directional and the whole architecture rests on
# it:
#
#   hc_base  <-  hc_vision  <-  hc_vision_ort
#       ^            ^
#       |            |
#       +-- hc_control <-- hc_platform / hc_app
#
# Arrows point from the dependency to the dependant: hc_vision may use
# hc_base, never the reverse. The two functions below state the two kinds of
# rule that keeps it that way.

# _hc_link_closure(<target> <out_var>)
#
# Collects every entry reachable from <target>'s LINK_LIBRARIES and
# INTERFACE_LINK_LIBRARIES, transitively, into <out_var>.
#
# Recursion only descends into entries that are real CMake targets; plain
# library names, absolute paths and generator expressions are collected as
# text and not followed. That is deliberate -- the text is what the pattern
# match below needs, and an entry naming a target that does not exist yet
# (because its add_subdirectory has not run) must still be caught.
function(_hc_link_closure target out_var)
  set(_hc_found "")
  set(_hc_queue "${target}")
  set(_hc_visited "")

  while(_hc_queue)
    list(POP_FRONT _hc_queue _hc_current)

    if(_hc_current IN_LIST _hc_visited)
      continue()
    endif()
    list(APPEND _hc_visited "${_hc_current}")

    if(NOT TARGET ${_hc_current})
      continue()
    endif()

    foreach(_hc_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_hc_libs ${_hc_current} ${_hc_prop})
      if(_hc_libs)
        foreach(_hc_lib ${_hc_libs})
          list(APPEND _hc_found "${_hc_lib}")
          list(APPEND _hc_queue "${_hc_lib}")
        endforeach()
      endif()
    endforeach()
  endwhile()

  if(_hc_found)
    list(REMOVE_DUPLICATES _hc_found)
  endif()
  set(${out_var} "${_hc_found}" PARENT_SCOPE)
endfunction()

# The two public functions below only REGISTER an assertion. The checking
# itself happens in hc_run_layer_checks(), called once at the bottom of the
# top-level CMakeLists after every add_subdirectory has run.
#
# This split is not stylistic. CMake executes top to bottom, so a check that
# runs where it is written sees only the link libraries added ABOVE it. A
# later target_link_libraries -- further down the same file, or in a sibling
# directory processed afterwards -- slips past it completely, and the guard
# then sits there looking reassuring while enforcing nothing. This was not a
# hypothetical: the first version of this file had exactly that hole, and a
# deliberate violation appended below the call configured cleanly.
#
# Registration goes through global properties rather than
# cmake_language(DEFER), because DEFER evaluates its arguments in the scope
# that runs the deferred call, where a function's parameters no longer exist.

function(_hc_check_no_forbidden_deps target)
  set(_hc_forbidden_patterns "opencv" "x11" "xtst")

  _hc_link_closure(${target} _hc_closure)

  foreach(_hc_lib ${_hc_closure})
    string(TOLOWER "${_hc_lib}" _hc_lib_lower)
    foreach(_hc_pattern ${_hc_forbidden_patterns})
      if(_hc_lib_lower MATCHES "${_hc_pattern}")
        message(FATAL_ERROR
          "hc_assert_no_forbidden_deps: target '${target}' links forbidden "
          "dependency '${_hc_lib}' (matched pattern '${_hc_pattern}'), "
          "directly or transitively. Targets below the platform layer must "
          "not link OpenCV, X11 or Xtst.")
      endif()
    endforeach()
  endforeach()
endfunction()

function(_hc_check_does_not_link target)
  _hc_link_closure(${target} _hc_closure)

  foreach(_hc_lib ${_hc_closure})
    foreach(_hc_bad ${ARGN})
      if(_hc_lib STREQUAL _hc_bad)
        message(FATAL_ERROR
          "hc_assert_does_not_link: target '${target}' links '${_hc_bad}', "
          "directly or transitively. That inverts the project's dependency "
          "order -- see cmake/HcAssertNoForbiddenDeps.cmake for the layer "
          "diagram. Either the logic belongs in the lower layer, or the call "
          "belongs in the higher one.")
      endif()
    endforeach()
  endforeach()
endfunction()

# hc_assert_no_forbidden_deps(<target>)
#
# Asserts that <target> links no OpenCV, X11 or Xtst, directly or
# transitively. Those belong to the platform layer; everything below it must
# stay portable -- to Android, to iOS, to WebAssembly, where none of them
# exist. Call this on every target below the platform layer.
function(hc_assert_no_forbidden_deps target)
  set_property(GLOBAL APPEND PROPERTY HC_PORTABILITY_CHECKS "${target}")
endfunction()

# hc_assert_does_not_link(<target> FORBIDDEN <names...>)
#
# Asserts that <target> links none of <names>, directly or transitively. This
# is the direction check: it is what stops a lower layer from reaching up
# into a higher one.
#
# Matching is exact against the entry text, not a substring, so forbidding
# `hc_vision` does not also forbid `hc_vision_ort`. Each layer lists the
# layers above it explicitly; a new module has to be added to those lists,
# and the deliberate friction of having to do so is the point.
function(hc_assert_does_not_link target)
  cmake_parse_arguments(HC_ADNL "" "" "FORBIDDEN" ${ARGN})

  if(NOT HC_ADNL_FORBIDDEN)
    message(FATAL_ERROR
      "hc_assert_does_not_link(${target}): FORBIDDEN list is empty, so this "
      "call asserts nothing. Pass the targets ${target} must not link.")
  endif()

  # The forbidden list is stashed in a per-target property rather than being
  # flattened into one list, so that a semicolon-separated list of names
  # survives without needing to be escaped and re-split.
  set_property(GLOBAL APPEND PROPERTY HC_DIRECTION_CHECKS "${target}")
  set_property(GLOBAL PROPERTY HC_DIRECTION_FORBIDDEN_${target}
    "${HC_ADNL_FORBIDDEN}")
endfunction()

# hc_run_layer_checks()
#
# Runs every assertion registered above. Call this ONCE, at the bottom of the
# top-level CMakeLists, after all add_subdirectory calls -- at that point the
# link graph is final and nothing can be added behind the check's back.
function(hc_run_layer_checks)
  get_property(_hc_portability GLOBAL PROPERTY HC_PORTABILITY_CHECKS)
  foreach(_hc_target ${_hc_portability})
    _hc_check_no_forbidden_deps("${_hc_target}")
  endforeach()

  get_property(_hc_direction GLOBAL PROPERTY HC_DIRECTION_CHECKS)
  foreach(_hc_target ${_hc_direction})
    get_property(_hc_forbidden GLOBAL PROPERTY
      HC_DIRECTION_FORBIDDEN_${_hc_target})
    _hc_check_does_not_link("${_hc_target}" ${_hc_forbidden})
  endforeach()

  list(LENGTH _hc_portability _hc_n_portability)
  list(LENGTH _hc_direction _hc_n_direction)
  message(STATUS
    "Layer checks passed: ${_hc_n_portability} portability, "
    "${_hc_n_direction} direction")
endfunction()
