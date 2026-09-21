# Remove MSVC-style auto-link metadata from Xenon ELF objects before oxdklink.
# Static archives are handled too: .deplibs hides inside members, so each archive
# is extracted, cleaned and reassembled. CMake invokes this script as:
#   cmake -DLLVM_OBJCOPY=... -P strip_deplibs.cmake -- object1.o ... lib1.a ...
if (NOT LLVM_OBJCOPY)
    message(FATAL_ERROR "LLVM_OBJCOPY is required")
endif ()

get_filename_component(_llvm_bin "${LLVM_OBJCOPY}" DIRECTORY)
find_program(LLVM_AR
    NAMES llvm-ar
    HINTS "${_llvm_bin}"
    NO_DEFAULT_PATH)
if (NOT LLVM_AR)
    message(FATAL_ERROR "llvm-ar is required next to LLVM_OBJCOPY")
endif ()

set(_objects)
foreach (_index RANGE 0 ${CMAKE_ARGC})
    if (_index GREATER 0)
        set(_arg "${CMAKE_ARGV${_index}}")
        if (_arg MATCHES "\\.(o|a)$")
            list(APPEND _objects "${_arg}")
        endif ()
    endif ()
endforeach ()

foreach (_object IN LISTS _objects)
    execute_process(
        COMMAND "${LLVM_OBJCOPY}" --remove-section=.deplibs "${_object}"
        RESULT_VARIABLE _result
        OUTPUT_QUIET ERROR_QUIET)
    if (NOT _result EQUAL 0)
        message(FATAL_ERROR "llvm-objcopy failed for ${_object}")
    endif ()
endforeach ()

# Archive members carry their own .deplibs; ld.lld reads them even through an
# archive and then demands libc++.lib, which cannot exist on a Xenon build.
foreach (_object IN LISTS _objects)
    if (NOT _object MATCHES "\\.a$")
        continue()
    endif ()
    get_filename_component(_abs "${_object}" ABSOLUTE)
    get_filename_component(_adir "${_abs}" DIRECTORY)
    get_filename_component(_aname "${_abs}" NAME)
    set(_tmp "${_adir}/.strip_deplibs_${_aname}")
    file(REMOVE_RECURSE "${_tmp}")
    file(MAKE_DIRECTORY "${_tmp}")

    execute_process(
        COMMAND "${LLVM_AR}" t "${_abs}"
        WORKING_DIRECTORY "${_tmp}"
        OUTPUT_VARIABLE _members
        RESULT_VARIABLE _result
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if (NOT _result EQUAL 0 OR NOT _members)
        file(REMOVE_RECURSE "${_tmp}")
        continue()
    endif ()
    # llvm-ar lists one member per line; turn that into a CMake list.
    string(REPLACE "\n" ";" _member_list "${_members}")

    execute_process(
        COMMAND "${LLVM_AR}" x "${_abs}"
        WORKING_DIRECTORY "${_tmp}"
        RESULT_VARIABLE _result
        OUTPUT_QUIET ERROR_QUIET)
    if (NOT _result EQUAL 0)
        file(REMOVE_RECURSE "${_tmp}")
        continue()
    endif ()

    foreach (_member IN LISTS _member_list)
        if (NOT EXISTS "${_tmp}/${_member}")
            continue()
        endif ()
        execute_process(
            COMMAND "${LLVM_OBJCOPY}" --remove-section=.deplibs "${_tmp}/${_member}"
            RESULT_VARIABLE _result
            OUTPUT_QUIET ERROR_QUIET)
        if (NOT _result EQUAL 0)
            # Non-ELF members (aliases etc.) have no .deplibs; leave them as-is.
            continue()
        endif ()
    endforeach ()

    # Assemble a fresh archive first; only replace the original once the new
    # one exists, so a failed reassembly can never destroy the input.
    set(_new "${_tmp}/new.a")
    file(REMOVE "${_new}")
    execute_process(
        COMMAND "${LLVM_AR}" rcs "${_new}" ${_member_list}
        WORKING_DIRECTORY "${_tmp}"
        RESULT_VARIABLE _result
        OUTPUT_QUIET ERROR_QUIET)
    if (NOT _result EQUAL 0 OR NOT EXISTS "${_new}")
        message(FATAL_ERROR "llvm-ar failed to reassemble ${_object}")
    endif ()
    file(REMOVE "${_abs}")
    file(RENAME "${_new}" "${_abs}")
    file(REMOVE_RECURSE "${_tmp}")
endforeach ()
