cmake_minimum_required(VERSION 3.20)
project(CitoQ VERSION 0.1.0 LANGUAGES C CXX)

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

option(CITO_ENABLE_MSQUIC "Build optional MsQuic Request/Reply transport integration" OFF)

add_library(cito_core SHARED src/embedded_host_runtime.cpp)
target_include_directories(cito_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(cito_core PUBLIC cxx_std_20)
target_compile_definitions(cito_core PRIVATE CITO_CORE_BUILD)
target_link_libraries(cito_core PUBLIC Threads::Threads)

add_library(cito INTERFACE)
target_include_directories(cito INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(cito INTERFACE cxx_std_20)
target_link_libraries(cito INTERFACE cito_core Threads::Threads)

add_library(cito_c SHARED src/c_api.cpp)
target_include_directories(cito_c PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(cito_c PRIVATE cxx_std_20)
target_compile_definitions(cito_c PRIVATE CITO_C_BUILD)
target_link_libraries(cito_c PRIVATE cito_core Threads::Threads)
set_target_properties(cito_c PROPERTIES OUTPUT_NAME cito)

if(UNIX AND NOT APPLE)
  add_library(cito_posix_runtime STATIC
    src/posix_udp_control_manager.cpp)
  target_include_directories(cito_posix_runtime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(cito_posix_runtime PUBLIC cxx_std_20)
  target_compile_options(cito_posix_runtime PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(cito_posix_runtime
    PUBLIC cito_core Threads::Threads)
endif()

if(CITO_ENABLE_MSQUIC)
  find_path(MSQUIC_INCLUDE_DIR msquic.h REQUIRED)
  find_library(MSQUIC_LIBRARY msquic REQUIRED)

  add_library(cito_msquic STATIC
    src/msquic_request_session_manager.cpp)

  if(UNIX AND NOT APPLE)
    target_sources(cito_msquic PRIVATE
      src/linux_request_runtime_driver.cpp)
  endif()
  target_include_directories(cito_msquic
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${MSQUIC_INCLUDE_DIR})
  target_compile_features(cito_msquic PUBLIC cxx_std_20)
  target_compile_options(cito_msquic PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(cito_msquic
    PUBLIC cito_core Threads::Threads
    PRIVATE ${MSQUIC_LIBRARY})

  if(UNIX AND NOT APPLE)
    target_link_libraries(cito_msquic PUBLIC cito_posix_runtime)
    # The native driver must be loaded dynamically by a Python process.
    set_target_properties(cito_msquic cito_posix_runtime
      PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(cito_native SHARED src/native_runtime_c.cpp)
    target_include_directories(cito_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(cito_native PUBLIC cxx_std_20)
    target_link_libraries(cito_native PRIVATE
      cito_msquic cito_posix_runtime cito_core Threads::Threads)
    target_compile_options(cito_native PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  function(cito_add_test target source labels)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE cito)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "${labels}")
  endfunction()

  cito_add_test(cito_context_tests tests/context_test.cpp "unit;regression")
  cito_add_test(cito_reentrant_subscription_growth_tests tests/reentrant_subscription_growth_test.cpp "unit;G2;G4;regression")
  cito_add_test(cito_public_data_transport_bridge_tests tests/public_data_transport_bridge_test.cpp "unit;G1;regression")
  cito_add_test(cito_shared_context_tests tests/shared_context_test.cpp "unit;latency;regression")
  cito_add_test(cito_context_runtime_driver_tests tests/context_runtime_driver_test.cpp "unit;regression")
  cito_add_test(cito_context_index_tests tests/context_index_test.cpp "unit;latency;regression")
  cito_add_test(cito_type_tests tests/type_test.cpp "unit;regression")
  cito_add_test(cito_wire_tests tests/wire_test.cpp "unit;regression")
  cito_add_test(cito_interest_tests tests/interest_test.cpp "unit;regression")
  cito_add_test(cito_runtime_tests tests/runtime_test.cpp "unit;regression")
  cito_add_test(cito_demand_router_tests tests/demand_router_test.cpp "unit;regression")
  cito_add_test(cito_type_model_tests tests/type_model_test.cpp "unit;regression")
  cito_add_test(cito_dynamic_complex_tests tests/dynamic_complex_test.cpp "unit;regression")
  cito_add_test(cito_discovery_tests tests/discovery_test.cpp "unit;regression")
  cito_add_test(cito_data_packet_tests tests/data_packet_test.cpp "unit;regression")
  cito_add_test(cito_latency_guard_tests tests/latency_guard_test.cpp "unit;latency;regression")
  cito_add_test(cito_demand_summary_tests tests/demand_summary_test.cpp "unit;latency;regression")
  cito_add_test(cito_demand_control_tests tests/demand_control_test.cpp "unit;latency;regression")
  cito_add_test(cito_coordinator_lifecycle_tests tests/coordinator_lifecycle_test.cpp "unit;latency;regression")
  cito_add_test(cito_remote_routes_tests tests/remote_routes_test.cpp "unit;latency;regression")
  cito_add_test(cito_transport_selector_tests tests/transport_selector_test.cpp "unit;latency;regression")
  cito_add_test(cito_best_effort_delivery_tests tests/best_effort_delivery_test.cpp "unit;latency;regression")
  cito_add_test(cito_reliability_tests tests/reliability_test.cpp "unit;latency;regression")
  cito_add_test(cito_reliable_packet_tests tests/reliable_packet_test.cpp "unit;latency;regression")
  cito_add_test(cito_host_runtime_core_tests tests/host_runtime_core_test.cpp "unit;latency;regression")
  cito_add_test(cito_request_reply_core_tests tests/request_reply_core_test.cpp "unit;latency;regression")
  cito_add_test(cito_request_api_tests tests/request_api_test.cpp "unit;latency;regression")
  cito_add_test(cito_request_discovery_tests tests/request_discovery_test.cpp "unit;latency;regression")
  cito_add_test(cito_request_transport_tests tests/request_transport_test.cpp "unit;latency;regression")
  cito_add_test(cito_request_network_adapter_tests tests/request_network_adapter_test.cpp "unit;latency;regression")
  cito_add_test(cito_request_wait_tests tests/request_wait_test.cpp "unit;latency;regression")
  cito_add_test(cito_request_public_remote_tests tests/request_public_remote_test.cpp "unit;smoke;latency;regression")
  cito_add_test(cito_request_session_lifecycle_tests tests/request_session_lifecycle_test.cpp "unit;latency;regression")
  cito_add_test(cito_host_runtime_core_smoke tests/host_runtime_core_smoke.cpp "smoke;latency;regression")
  cito_add_test(cito_discovery_control_core_tests tests/discovery_control_core_test.cpp "unit;latency;regression")
  cito_add_test(cito_discovery_control_core_smoke tests/discovery_control_core_smoke.cpp "smoke;latency;regression")
  cito_add_test(cito_udp_runtime_adapter_tests tests/udp_runtime_adapter_test.cpp "unit;latency;regression")
  cito_add_test(cito_udp_runtime_adapter_smoke tests/udp_runtime_adapter_smoke.cpp "smoke;latency;regression")
  cito_add_test(cito_host_scale_200_tests tests/host_scale_200_test.cpp "smoke;latency;scale;regression")

  add_executable(cito_c_api_tests tests/c_api_test.c)
  target_link_libraries(cito_c_api_tests PRIVATE cito_c)
  target_compile_options(cito_c_api_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cito_c_api_tests COMMAND cito_c_api_tests)
  set_tests_properties(cito_c_api_tests PROPERTIES LABELS "unit;smoke;latency;regression")

  add_executable(cito_c_request_api_tests tests/c_request_api_test.c)
  target_link_libraries(cito_c_request_api_tests PRIVATE cito_c)
  target_compile_options(cito_c_request_api_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cito_c_request_api_tests COMMAND cito_c_request_api_tests)
  set_tests_properties(cito_c_request_api_tests PROPERTIES LABELS "unit;smoke;latency;regression")

  add_executable(cito_cpp_c_bridge_tests tests/cpp_c_bridge_test.cpp)
  target_link_libraries(cito_cpp_c_bridge_tests PRIVATE cito cito_c)
  target_compile_options(cito_cpp_c_bridge_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cito_cpp_c_bridge_tests COMMAND cito_cpp_c_bridge_tests)
  set_tests_properties(cito_cpp_c_bridge_tests PROPERTIES LABELS "unit;smoke;latency;regression")

  add_executable(cito_cpp_c_remote_request_bridge_tests
    tests/cpp_c_remote_request_bridge_test.cpp)
  target_link_libraries(cito_cpp_c_remote_request_bridge_tests PRIVATE cito cito_c)
  target_compile_options(cito_cpp_c_remote_request_bridge_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cito_cpp_c_remote_request_bridge_tests COMMAND cito_cpp_c_remote_request_bridge_tests)
  set_tests_properties(cito_cpp_c_remote_request_bridge_tests PROPERTIES LABELS "unit;smoke;latency;regression")

  add_test(
    NAME cito_python_smoke
    COMMAND ${CMAKE_COMMAND} -E env
            CITO_LIBRARY=$<TARGET_FILE:cito_c>
            PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_binding.py)
  set_tests_properties(cito_python_smoke PROPERTIES LABELS "smoke;regression")

  cito_add_test(cito_public_api_smoke tests/public_api_smoke.cpp "smoke;regression")
  cito_add_test(cito_interest_scale_tests tests/interest_scale_test.cpp "smoke;regression")
  cito_add_test(cito_runtime_smoke tests/runtime_smoke.cpp "smoke;regression")
  cito_add_test(cito_demand_router_smoke tests/demand_router_smoke.cpp "smoke;regression")
  cito_add_test(cito_type_model_smoke tests/type_model_smoke.cpp "smoke;regression")
  cito_add_test(cito_discovery_smoke tests/discovery_smoke.cpp "smoke;regression")
  cito_add_test(cito_demand_control_smoke tests/demand_control_smoke.cpp "smoke;latency;regression")
  cito_add_test(cito_discovery_lifecycle_smoke tests/discovery_lifecycle_smoke.cpp "smoke;latency;regression")
  cito_add_test(cito_reliability_smoke tests/reliability_smoke.cpp "smoke;latency;regression")

  add_test(NAME cito_idlc_unit
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_idlc.py)
  set_tests_properties(cito_idlc_unit PROPERTIES LABELS "unit;regression")

  add_test(
    NAME cito_python_wire_unit
    COMMAND ${CMAKE_COMMAND} -E env
            PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_wire.py)
  set_tests_properties(cito_python_wire_unit PROPERTIES LABELS "unit;regression")

  set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
  set(GENERATED_PY_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated_py)
  foreach(schema position telemetry request_reply)
    add_custom_command(
      OUTPUT ${GENERATED_DIR}/${schema}.hpp
      COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED_DIR}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/cito_idlc.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/idl/${schema}.idl
              -o ${GENERATED_DIR}/${schema}.hpp
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/cito_idlc.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/idl/${schema}.idl
      VERBATIM)
  endforeach()

  foreach(schema position telemetry request_reply)
    add_custom_command(
      OUTPUT ${GENERATED_PY_DIR}/${schema}.py
      COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED_PY_DIR}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/cito_idlc.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/idl/${schema}.idl
              --lang python
              -o ${GENERATED_PY_DIR}/${schema}.py
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/cito_idlc.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/idl/${schema}.idl
      VERBATIM)
  endforeach()

  add_custom_target(
    cito_python_generated ALL
    DEPENDS
      ${GENERATED_PY_DIR}/position.py
      ${GENERATED_PY_DIR}/telemetry.py
      ${GENERATED_PY_DIR}/request_reply.py)

  add_test(
    NAME cito_python_typed_smoke
    COMMAND ${CMAKE_COMMAND} -E env
            CITO_LIBRARY=$<TARGET_FILE:cito_c>
            PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python:${GENERATED_PY_DIR}
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_typed_binding.py)
  set_tests_properties(
    cito_python_typed_smoke
    PROPERTIES LABELS "smoke;latency;regression")

  add_test(
    NAME cito_python_request_reply_smoke
    COMMAND ${CMAKE_COMMAND} -E env
            CITO_LIBRARY=$<TARGET_FILE:cito_c>
            PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python:${GENERATED_PY_DIR}
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_request_reply.py)
  set_tests_properties(
    cito_python_request_reply_smoke
    PROPERTIES LABELS "smoke;latency;regression")

  add_executable(cito_idl_static_smoke
    tests/idl_static_smoke.cpp
    ${GENERATED_DIR}/position.hpp)
  target_link_libraries(cito_idl_static_smoke PRIVATE cito)
  target_include_directories(cito_idl_static_smoke PRIVATE ${GENERATED_DIR})
  target_compile_options(cito_idl_static_smoke PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cito_idl_static_smoke COMMAND cito_idl_static_smoke)
  set_tests_properties(cito_idl_static_smoke PROPERTIES LABELS "smoke;latency;regression")

  add_executable(cito_idl_python_fixture
    tests/idl_python_fixture.cpp
    ${GENERATED_DIR}/position.hpp)
  target_link_libraries(cito_idl_python_fixture PRIVATE cito)
  target_include_directories(cito_idl_python_fixture PRIVATE ${GENERATED_DIR})
  target_compile_options(cito_idl_python_fixture PRIVATE -Wall -Wextra -Wpedantic)

  add_test(
    NAME cito_idl_python_wire_smoke
    COMMAND ${CMAKE_COMMAND} -E env
            PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python:${GENERATED_PY_DIR}
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_idl_python_wire.py
            $<TARGET_FILE:cito_idl_python_fixture>
            ${GENERATED_PY_DIR})
  set_tests_properties(
    cito_idl_python_wire_smoke
    PROPERTIES LABELS "smoke;latency;regression")

  add_executable(cito_request_api_smoke
    tests/request_api_smoke.cpp
    ${GENERATED_DIR}/request_reply.hpp)
  target_link_libraries(cito_request_api_smoke PRIVATE cito)
  target_include_directories(cito_request_api_smoke PRIVATE ${GENERATED_DIR})
  target_compile_options(cito_request_api_smoke PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cito_request_api_smoke COMMAND cito_request_api_smoke)
  set_tests_properties(cito_request_api_smoke PROPERTIES LABELS "smoke;latency;regression")

  add_executable(cito_idl_request_reply_smoke
    tests/idl_request_reply_smoke.cpp
    ${GENERATED_DIR}/request_reply.hpp)
  target_link_libraries(cito_idl_request_reply_smoke PRIVATE cito)
  target_include_directories(cito_idl_request_reply_smoke PRIVATE ${GENERATED_DIR})
  target_compile_options(cito_idl_request_reply_smoke PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cito_idl_request_reply_smoke COMMAND cito_idl_request_reply_smoke)
  set_tests_properties(cito_idl_request_reply_smoke PROPERTIES LABELS "smoke;latency;regression")

  add_test(
    NAME cito_python_request_reply_idl_smoke
    COMMAND ${CMAKE_COMMAND} -E env
            PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python:${GENERATED_PY_DIR}
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_idl_request_reply.py)
  set_tests_properties(
    cito_python_request_reply_idl_smoke
    PROPERTIES LABELS "smoke;latency;regression")

  add_executable(cito_idl_complex_smoke
    tests/idl_complex_smoke.cpp
    ${GENERATED_DIR}/telemetry.hpp)
  target_link_libraries(cito_idl_complex_smoke PRIVATE cito)
  target_include_directories(cito_idl_complex_smoke PRIVATE ${GENERATED_DIR})
  target_compile_options(cito_idl_complex_smoke PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cito_idl_complex_smoke COMMAND cito_idl_complex_smoke)
  set_tests_properties(cito_idl_complex_smoke PROPERTIES LABELS "smoke;latency;regression")

  if(UNIX AND NOT APPLE)
    add_executable(cito_posix_udp_control_manager_tests
      tests/posix_udp_control_manager_test.cpp)
    target_link_libraries(cito_posix_udp_control_manager_tests PRIVATE cito cito_posix_runtime)
    target_compile_options(cito_posix_udp_control_manager_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME cito_posix_udp_control_manager_tests COMMAND cito_posix_udp_control_manager_tests)
    set_tests_properties(cito_posix_udp_control_manager_tests PROPERTIES LABELS "unit;smoke;latency;regression")

    cito_add_test(cito_shm_ring_tests tests/shm_ring_test.cpp "unit;latency;regression")
    target_link_libraries(cito_shm_ring_tests PRIVATE rt)

    add_executable(cito_shm_node tests/net/shm_node.cpp)
    target_link_libraries(cito_shm_node PRIVATE cito rt)
    target_compile_options(cito_shm_node PRIVATE -Wall -Wextra -Wpedantic)

    add_test(
      NAME cito_shm_multiprocess_smoke
      COMMAND bash
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_shm_smoke.sh
              $<TARGET_FILE:cito_shm_node>)
    set_tests_properties(
      cito_shm_multiprocess_smoke
      PROPERTIES LABELS "smoke;latency;regression")

    add_executable(cito_shm_typed_node
      tests/net/shm_typed_node.cpp
      ${GENERATED_DIR}/position.hpp)
    target_link_libraries(cito_shm_typed_node PRIVATE cito rt)
    target_include_directories(cito_shm_typed_node PRIVATE ${GENERATED_DIR})
    target_compile_options(cito_shm_typed_node PRIVATE -Wall -Wextra -Wpedantic)

    add_test(
      NAME cito_shm_typed_smoke
      COMMAND bash
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_shm_typed_smoke.sh
              $<TARGET_FILE:cito_shm_typed_node>)
    set_tests_properties(
      cito_shm_typed_smoke
      PROPERTIES LABELS "smoke;latency;regression")

    add_executable(cito_udp_node tests/net/udp_node.cpp)
    target_link_libraries(cito_udp_node PRIVATE cito)
    target_compile_options(cito_udp_node PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(cito_runtime_udp_node tests/net/runtime_udp_node.cpp)
    target_link_libraries(cito_runtime_udp_node PRIVATE cito)
    target_compile_options(cito_runtime_udp_node PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  if(CITO_ENABLE_MSQUIC)
    add_executable(cito_quic_msquic_node tests/net/quic_msquic_node.cpp)
    target_include_directories(cito_quic_msquic_node PRIVATE ${MSQUIC_INCLUDE_DIR})
    target_link_libraries(cito_quic_msquic_node PRIVATE cito ${MSQUIC_LIBRARY})
    target_compile_options(cito_quic_msquic_node PRIVATE -Wall -Wextra)

    add_executable(cito_quic_request_node tests/net/quic_request_node.cpp)
    target_include_directories(cito_quic_request_node PRIVATE ${MSQUIC_INCLUDE_DIR})
    target_link_libraries(cito_quic_request_node PRIVATE cito ${MSQUIC_LIBRARY})
    target_compile_options(cito_quic_request_node PRIVATE -Wall -Wextra -Wpedantic)

    if(UNIX AND NOT APPLE)
      add_executable(cito_quic_public_request_node
        tests/net/quic_public_request_node.cpp)
      target_include_directories(cito_quic_public_request_node PRIVATE ${MSQUIC_INCLUDE_DIR})
      target_link_libraries(cito_quic_public_request_node PRIVATE cito cito_c cito_msquic cito_posix_runtime)
      target_compile_options(cito_quic_public_request_node PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endif()
endif()

# Distribution targets. Preserve current native ABI and protocol identifiers.
set_target_properties(cito_core PROPERTIES EXPORT_NAME Runtime)
set_target_properties(cito PROPERTIES EXPORT_NAME Core)
set_target_properties(cito_c PROPERTIES EXPORT_NAME C)
add_library(CitoQ::Runtime ALIAS cito_core)
add_library(CitoQ::Core ALIAS cito)
add_library(CitoQ::C ALIAS cito_c)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS cito_core cito cito_c EXPORT CitoQTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(PROGRAMS tools/cito_idlc.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME citoq-idlc)
install(EXPORT CitoQTargets FILE CitoQTargets.cmake NAMESPACE CitoQ::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CitoQ)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CitoQConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CitoQConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CitoQ)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CitoQConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/CitoQConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/CitoQConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CitoQ)
