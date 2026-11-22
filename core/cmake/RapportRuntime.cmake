# rapport_require_shared_runtime(<target>): a shared rapport library (the
# core, a backend plugin) built with MSVC must use the shared C++ runtime.
# std:: types (std::function, std::string, std::vector, ...) cross between the
# app, the core and each backend DLL, one module freeing what another
# allocated, so they must all use one runtime DLL (/MD). The static runtime
# (/MT) is for RAPPORT_STATIC builds, one executable with one runtime (#136).
#
# The target's runtime, as it is once its directory is processed (so a later
# set_target_properties in the same CMakeLists counts), must be unset (it's
# then given MultiThreaded$<$<CONFIG:Debug>:Debug>DLL) or exactly one of
# the three below: an allowlist, as an expression can't be judged for every
# configuration by its text. /MT or /MTd in the C++ flags or the target's
# compile options is refused too.
function(rapport_require_shared_runtime target)
    if(MSVC)
        # A deferred call's arguments are expanded when it runs: the name's
        # written into it now.
        cmake_language(EVAL CODE "cmake_language(DEFER CALL rapport_check_shared_runtime [[${target}]])")
    endif()
endfunction()

function(rapport_check_shared_runtime target)
    set(why "std:: types cross the plugin DLLs, so each must use the same runtime DLL (#136)")
    set(rapport_shared_runtimes MultiThreadedDLL MultiThreadedDebugDLL
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    get_target_property(runtime ${target} MSVC_RUNTIME_LIBRARY)
    if(runtime)
        if(NOT runtime IN_LIST rapport_shared_runtimes)
            message(FATAL_ERROR "rapport: ${target}, a shared rapport library, needs the shared C++ "
                                "runtime, but its MSVC runtime library is '${runtime}': ${why}. "
                                "Leave CMAKE_MSVC_RUNTIME_LIBRARY unset, or use one of "
                                "MultiThreadedDLL, MultiThreadedDebugDLL or "
                                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL; for the static runtime, "
                                "-DRAPPORT_STATIC=ON.")
        endif()
    else()
        set_target_properties(${target} PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    get_target_property(options ${target} COMPILE_OPTIONS)
    if(NOT options)
        set(options "")
    endif()
    foreach(flags CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
                  CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL options)
        string(REPLACE ";" " " text "${${flags}}")
        if(text MATCHES "(^| )[/-]MTd?( |$)")
            message(FATAL_ERROR "rapport: ${target}, a shared rapport library, needs the shared C++ "
                                "runtime, but ${flags} has /MT: ${why}. Drop it, or use "
                                "-DRAPPORT_STATIC=ON for the static runtime.")
        endif()
    endforeach()
endfunction()
