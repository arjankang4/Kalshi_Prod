# test_fpenv is ordered FIRST and everything else depends on it passing: if the FP
# environment drifted, downstream parity failures are noise.
add_executable(test_fpenv unit/test_fpenv.cpp)
target_link_libraries(test_fpenv PRIVATE kp_base kp::fp kp::warnings)
add_test(NAME fpenv COMMAND test_fpenv)
set_tests_properties(fpenv PROPERTIES FIXTURES_SETUP FPENV)

# ... and the vectors must describe the Python they claim to. Same argument as fpenv: if the
# reference moved under the .vec files, every downstream mismatch is noise about the wrong thing.
# This is part of the FPENV fixture rather than a separate one so no suite can opt out of it.
add_test(NAME vectors_fresh
         COMMAND ${CMAKE_SOURCE_DIR}/tools/check_vectors.sh
                 ${CMAKE_SOURCE_DIR}/tests/data/golden ${KP_REF_DEFAULT})
set_tests_properties(vectors_fresh PROPERTIES FIXTURES_SETUP FPENV)

add_executable(test_pycompat unit/test_pycompat.cpp)
target_link_libraries(test_pycompat PRIVATE kp_base kp::fp kp::warnings)
add_test(NAME pycompat
         COMMAND test_pycompat ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(pycompat PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_bs unit/test_bs.cpp)
target_link_libraries(test_bs PRIVATE kp_pricing kp_base kp::fp kp::warnings)
add_test(NAME bs COMMAND test_bs ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(bs PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_pricer unit/test_pricer.cpp)
target_link_libraries(test_pricer PRIVATE kp_pricing kp_base kp::fp kp::warnings)
target_include_directories(test_pricer PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME pricer COMMAND test_pricer ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(pricer PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_risklimit unit/test_risklimit.cpp)
target_link_libraries(test_risklimit PRIVATE kp_strategy kp_pricing kp_base kp::fp kp::warnings)
target_include_directories(test_risklimit PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME risklimit COMMAND test_risklimit ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(risklimit PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_band unit/test_band.cpp)
target_link_libraries(test_band PRIVATE kp_strategy kp_pricing kp_base kp::fp kp::warnings)
target_include_directories(test_band PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME band COMMAND test_band ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(band PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_lean_gapcal unit/test_lean_gapcal.cpp)
target_link_libraries(test_lean_gapcal PRIVATE kp_pricing kp_base kp::fp kp::warnings)
target_include_directories(test_lean_gapcal PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME lean_gapcal
         COMMAND test_lean_gapcal ${CMAKE_SOURCE_DIR}/tests/data/golden ${KALSHI_ROOT_DEFAULT})
set_tests_properties(lean_gapcal PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_hourly unit/test_hourly.cpp)
target_link_libraries(test_hourly PRIVATE kp_pricing kp_base kp::fp kp::warnings)
target_include_directories(test_hourly PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME hourly
         COMMAND test_hourly ${CMAKE_SOURCE_DIR}/tests/data/golden ${KALSHI_ROOT_DEFAULT})
set_tests_properties(hourly PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_universe unit/test_universe.cpp)
target_link_libraries(test_universe PRIVATE kp_pricing kp_base kp::fp kp::warnings)
target_include_directories(test_universe PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME universe COMMAND test_universe ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(universe PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_spot unit/test_spot.cpp)
target_link_libraries(test_spot PRIVATE kp_pricing kp_base kp::fp kp::warnings)
target_include_directories(test_spot PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME spot COMMAND test_spot ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(spot PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_theo_core unit/test_theo_core.cpp)
target_link_libraries(test_theo_core PRIVATE kp_pricing kp_base kp::fp kp::warnings)
add_test(NAME theo_core COMMAND test_theo_core ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(theo_core PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_quoting unit/test_quoting.cpp)
target_link_libraries(test_quoting PRIVATE kp_strategy kp_pricing kp_base kp::fp kp::warnings)
add_test(NAME quoting COMMAND test_quoting ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(quoting PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_skew unit/test_skew.cpp)
target_include_directories(test_skew PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_skew PRIVATE kp_pricing kp_base kp::fp kp::warnings)
add_test(NAME skew COMMAND test_skew ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(skew PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_atmvol unit/test_atmvol.cpp)
target_include_directories(test_atmvol PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_atmvol PRIVATE kp_pricing kp_base kp::fp kp::warnings)
add_test(NAME atmvol COMMAND test_atmvol ${CMAKE_SOURCE_DIR}/tests/data/golden)
set_tests_properties(atmvol PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_json unit/test_json.cpp)
target_link_libraries(test_json PRIVATE kp_base kp::fp kp::warnings)
add_test(NAME json COMMAND test_json ${CMAKE_SOURCE_DIR}/tests/data/golden ${KALSHI_ROOT_DEFAULT})
set_tests_properties(json PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_varcal_edge unit/test_varcal_edge.cpp)
target_include_directories(test_varcal_edge PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_varcal_edge PRIVATE kp_pricing kp_base kp::fp kp::warnings)
add_test(NAME varcal_edge COMMAND test_varcal_edge ${CMAKE_SOURCE_DIR}/tests/data/golden ${KALSHI_ROOT_DEFAULT})
set_tests_properties(varcal_edge PROPERTIES FIXTURES_REQUIRED FPENV)

add_executable(test_greeks unit/test_greeks.cpp)
target_include_directories(test_greeks PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_greeks PRIVATE kp_pricing kp_base kp::fp kp::warnings)
add_test(NAME greeks COMMAND test_greeks ${CMAKE_SOURCE_DIR}/tests/data/golden ${KALSHI_ROOT_DEFAULT})
set_tests_properties(greeks PROPERTIES FIXTURES_REQUIRED FPENV)

# --- the config, generated and hand-written, against the reference ---------------------------
add_test(NAME config_fresh
         COMMAND ${KP_PYTHON} ${CMAKE_SOURCE_DIR}/tools/check_config.py
                 ${CMAKE_SOURCE_DIR} ${KP_REF_DEFAULT})
set_tests_properties(config_fresh PROPERTIES FIXTURES_SETUP FPENV)

# --- every suite under tests/port/, registered by name ----------------------------------------
# One file per ported module, so ports landing in parallel never edit this file. Each is invoked as
#   test_<name> <golden-dir> <kalshi-root>
# and reads tests/data/golden/<name>*.vec, written by tools/golden/gen_<name>.py.
file(GLOB KP_PORT_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/port/test_*.cpp)
foreach(src ${KP_PORT_TESTS})
    get_filename_component(exe ${src} NAME_WE)
    string(REGEX REPLACE "^test_" "" name ${exe})
    add_executable(${exe} ${src})
    target_link_libraries(${exe} PRIVATE kp_app kp_all kp::fp kp::warnings)
    target_include_directories(${exe} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    add_test(NAME ${name} COMMAND ${exe} ${CMAKE_SOURCE_DIR}/tests/data/golden ${KALSHI_ROOT_DEFAULT})
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED FPENV)
endforeach()
