# Test targets. Classification (AGENTS.md §3.1) follows the directory a test
# source lives in rather than a name pattern: everything under unit/ is built
# into the unit binaries and labelled "unit", everything under functional/ into
# the functional binaries and labelled "functional". Adding a test file is
# enough; there is no separate list to keep in step.

include(GoogleTest)

set(videosynth_project_fixture_dir "${CMAKE_SOURCE_DIR}/projects")

# Derived project fixtures (Y/C renderings, impairment-free copies) are
# generated into the build tree instead of being committed. Without a Python
# interpreter the committed fixtures are still exercised; the generated ones
# are simply absent, which the suites tolerate.
find_package(Python3 QUIET COMPONENTS Interpreter)

set(videosynth_generated_project_dir "")
if(Python3_Interpreter_FOUND)
    # At the build root, not under tests/, so scripts/run-projects.sh can find
    # it without knowing how the test targets are laid out.
    set(videosynth_generated_project_dir
        "${CMAKE_BINARY_DIR}/generated-projects")
    set(generate_stamp "${CMAKE_BINARY_DIR}/generated-projects.stamp")

    file(GLOB_RECURSE videosynth_project_fixtures CONFIGURE_DEPENDS
        "${videosynth_project_fixture_dir}/*.yaml")

    add_custom_command(
        OUTPUT "${generate_stamp}"
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/scripts/generate_test_projects.py"
                --source "${videosynth_project_fixture_dir}"
                --output "${videosynth_generated_project_dir}"
                --stamp "${generate_stamp}"
                --quiet
        DEPENDS
            "${CMAKE_SOURCE_DIR}/scripts/generate_test_projects.py"
            "${videosynth_project_fixture_dir}/variants.json"
            ${videosynth_project_fixtures}
        COMMENT "Generating derived project fixtures"
        VERBATIM
    )
    add_custom_target(videosynth_generated_projects
        DEPENDS "${generate_stamp}")

    # In-memory self-test of the generator's text transformations.
    add_test(NAME GenerateTestProjects.SelfTest
        COMMAND "${Python3_EXECUTABLE}" -I
                "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_generate_test_projects.py")
    set_tests_properties(GenerateTestProjects.SelfTest
        PROPERTIES LABELS unit)
else()
    message(STATUS
        "Python3 not found — derived project fixtures will not be generated")
endif()

# Defines one test target: sources from `directory`, every discovered test
# labelled `label`. `extra_sources` carries shared entry points and the like.
function(videosynth_add_test_target target directory label)
    file(GLOB target_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${directory}/*.cpp")
    if(NOT target_sources)
        message(FATAL_ERROR "No test sources found in ${directory}")
    endif()

    add_executable(${target} ${target_sources} ${ARGN})

    target_include_directories(${target}
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")

    target_compile_definitions(${target}
        PRIVATE
            VIDEOSYNTH_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
            VIDEOSYNTH_PROJECT_DIR="${videosynth_project_fixture_dir}"
            VIDEOSYNTH_GENERATED_PROJECT_DIR="${videosynth_generated_project_dir}"
    )

    if(TARGET videosynth_generated_projects AND label STREQUAL "functional")
        add_dependencies(${target} videosynth_generated_projects)
    endif()

    gtest_discover_tests(${target}
        PROPERTIES LABELS "${label}"
        DISCOVERY_TIMEOUT 60)
endfunction()

videosynth_add_test_target(videosynth_unit_tests unit unit)
target_link_libraries(videosynth_unit_tests
    PRIVATE videosynth_core GTest::gtest GTest::gtest_main)

videosynth_add_test_target(videosynth_functional_tests functional functional)
target_link_libraries(videosynth_functional_tests
    PRIVATE videosynth_core GTest::gtest GTest::gtest_main)

if(VIDEOSYNTH_BUILD_GUI)
    # Both GUI binaries share the QCoreApplication entry point.
    set(gui_test_main "${CMAKE_CURRENT_SOURCE_DIR}/gui/support/gui_test_main.cpp")

    videosynth_add_test_target(videosynth_gui_unit_tests gui/unit unit
        "${gui_test_main}")
    target_link_libraries(videosynth_gui_unit_tests
        PRIVATE videosynth_gui_lib GTest::gtest)

    videosynth_add_test_target(videosynth_gui_functional_tests gui/functional
        functional "${gui_test_main}")
    target_link_libraries(videosynth_gui_functional_tests
        PRIVATE videosynth_gui_lib GTest::gtest)
endif()

# Aggregate: one stable name that builds every test binary, whatever the GUI
# option is set to. CI and README.md build this rather than naming each binary,
# so splitting or adding a suite does not break them.
add_custom_target(videosynth_tests)
add_dependencies(videosynth_tests
    videosynth_unit_tests
    videosynth_functional_tests)

if(VIDEOSYNTH_BUILD_GUI)
    add_dependencies(videosynth_tests
        videosynth_gui_unit_tests
        videosynth_gui_functional_tests)
endif()

set(videosynth_test_targets
    videosynth_unit_tests
    videosynth_functional_tests
    PARENT_SCOPE)

if(VIDEOSYNTH_BUILD_GUI)
    set(videosynth_gui_test_targets
        videosynth_gui_unit_tests
        videosynth_gui_functional_tests
        PARENT_SCOPE)
endif()
