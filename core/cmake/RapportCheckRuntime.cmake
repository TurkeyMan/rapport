# cmake -DDUMPBIN=<dumpbin.exe> -DFILE=<module> -P RapportCheckRuntime.cmake
#
# Fails unless FILE imports MSVC's shared C++ runtime: vcruntime140*.dll, and
# ucrtbase.dll or the api-ms-win-crt-* sets. Run after each shared rapport
# library is linked (rapport_require_shared_runtime): what the module
# imports is the runtime it was built with, however its flags were set (#136).
if(NOT DUMPBIN OR NOT FILE)
    message(FATAL_ERROR "RapportCheckRuntime.cmake: needs -DDUMPBIN=... -DFILE=...")
endif()
execute_process(COMMAND "${DUMPBIN}" /nologo /dependents "${FILE}"
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "rapport: couldn't read ${FILE}'s imports with ${DUMPBIN}: ${err}")
endif()
string(TOLOWER "${out}" out)
string(REGEX MATCHALL "[a-z0-9_.-]+\\.dll" dlls "${out}")
list(FILTER dlls EXCLUDE REGEX "^.*[\\/]")
set(vcruntime FALSE)
set(ucrt FALSE)
foreach(dll IN LISTS dlls)
    if(dll MATCHES "^vcruntime140(_1)?d?\\.dll$")
        set(vcruntime TRUE)
    elseif(dll MATCHES "^ucrtbased?\\.dll$" OR dll MATCHES "^api-ms-win-crt-")
        set(ucrt TRUE)
    endif()
endforeach()
if(NOT vcruntime OR NOT ucrt)
    list(REMOVE_DUPLICATES dlls)
    get_filename_component(name "${FILE}" NAME)
    list(REMOVE_ITEM dlls "${name}")
    string(JOIN ", " imports ${dlls})
    message(FATAL_ERROR "rapport: ${FILE} doesn't import the shared C++ runtime (it imports: "
                        "${imports}). A shared rapport library must be built with /MD: std:: "
                        "types cross the plugin DLLs, so each must use the same runtime DLL "
                        "(#136). Something set /MT for it; for the static runtime, use "
                        "-DRAPPORT_STATIC=ON.")
endif()
