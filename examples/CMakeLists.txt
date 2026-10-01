# Define a variable with all example targets
set(EXAMPLE_TARGETS
        quark_example_version
)

# Add example executables
foreach (example_target ${EXAMPLE_TARGETS})
    # Determine the source file from the target name
    string(REPLACE "quark_example_" "" source_name ${example_target})
    set(source_file "${source_name}.cpp")

    add_executable(${example_target} ${source_file})
    set_common_compile_options(${example_target})

    target_link_libraries(${example_target} quark quill::quill)
endforeach ()

install(TARGETS ${EXAMPLE_TARGETS}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})