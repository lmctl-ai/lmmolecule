# Build OpenMM 8.2.0 (Core API, Reference Platform, CPU Platform, Lepton, PocketFFT, L-BFGS, SFMT)
# as a single static library target. Only MIT/BSD/zlib/public-domain components are compiled;
# CUDA, OpenCL, and the CPU-PME plugin are intentionally excluded.
set(OMM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openmm")
if(NOT EXISTS "${OMM_DIR}/openmmapi/include/OpenMM.h")
  message(FATAL_ERROR "third_party/openmm missing: run scripts/sync-deps.sh")
endif()

set(OMM_SUBDIRS
  openmmapi
  olla
  libraries/jama
  libraries/quern
  libraries/lepton
  libraries/sfmt
  libraries/lbfgs
  libraries/hilbert
  libraries/irrxml
  libraries/vecmath
  platforms/reference
  platforms/cpu
)

set(OMM_SOURCES "")
set(OMM_INCLUDES "${OMM_DIR}/src")
foreach(sub ${OMM_SUBDIRS})
  file(GLOB s1 CONFIGURE_DEPENDS
    "${OMM_DIR}/${sub}/src/*.cpp"
    "${OMM_DIR}/${sub}/src/*.c"
    "${OMM_DIR}/${sub}/src/*/*.cpp"
    "${OMM_DIR}/${sub}/src/*/*.c"
  )
  list(APPEND OMM_SOURCES ${s1})
  list(APPEND OMM_INCLUDES
    "${OMM_DIR}/${sub}/include"
    "${OMM_DIR}/${sub}/include/openmm"
    "${OMM_DIR}/${sub}/include/openmm/internal"
    "${OMM_DIR}/${sub}/src"
    "${OMM_DIR}/${sub}/src/SimTKReference"
  )
endforeach()

list(APPEND OMM_SOURCES
  "${OMM_DIR}/serialization/src/SerializationNode.cpp"
  "${OMM_DIR}/serialization/src/SerializationProxy.cpp"
  "${OMM_DIR}/serialization/src/XmlSerializer.cpp"
  "${OMM_DIR}/serialization/src/dtoa.cpp"
  "${OMM_DIR}/serialization/src/g_fmt.cpp"
)
list(APPEND OMM_INCLUDES
  "${OMM_DIR}/libraries/pocketfft/include"
  "${OMM_DIR}/serialization/include"
  "${OMM_DIR}/serialization/include/openmm"
  "${OMM_DIR}/serialization/include/openmm/serialization"
)

add_library(openmm STATIC ${OMM_SOURCES})
set_target_properties(openmm PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
target_include_directories(openmm SYSTEM PUBLIC ${OMM_INCLUDES})
target_compile_definitions(openmm PUBLIC
  OPENMM_LIBRARY_NAME=OpenMM
  OPENMM_MAJOR_VERSION=8
  OPENMM_MINOR_VERSION=2
  OPENMM_BUILD_VERSION=0
  OPENMM_USE_STATIC_LIBRARIES
  OPENMM_BUILDING_STATIC_LIBRARY
  OPENMM_CPU_BUILDING_STATIC_LIBRARY
  LEPTON_USE_STATIC_LIBRARIES
  LEPTON_BUILDING_STATIC_LIBRARY
  PTW32_STATIC_LIB
  PTHREAD_BUILDING_STATIC_LIBRARY
  IEEE_8087
)
if(WIN32)
  target_compile_definitions(openmm PUBLIC WIN32 _copysign=std::copysign)
endif()

# Disable dynamic plugin dlopen/dlsym in static builds.
set_source_files_properties("${OMM_DIR}/olla/src/Platform.cpp" PROPERTIES COMPILE_DEFINITIONS "__PNACL__")

set(_OMM_ARCH "${CMAKE_OSX_ARCHITECTURES}")
if(NOT _OMM_ARCH)
  set(_OMM_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(_OMM_ARCH MATCHES "aarch64|AARCH64|arm64|ARM64")
  target_compile_definitions(openmm PUBLIC __ARM64__=1)
elseif(_OMM_ARCH MATCHES "x86_64|AMD64|amd64|i686|i386")
  target_compile_options(openmm PRIVATE -msse4.1)
  set_source_files_properties(
    "${OMM_DIR}/platforms/cpu/src/CpuNonbondedForceAvx.cpp"
    "${OMM_DIR}/platforms/cpu/src/CpuCustomNonbondedForceAvx.cpp"
    PROPERTIES COMPILE_OPTIONS "-mavx"
  )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(openmm PRIVATE
    -include stdint.h
    -Wno-register
    -Wno-write-strings
    -Wno-deprecated-declarations
    -Wno-parentheses
    -Wno-unused-parameter
    -Wno-sign-compare
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(openmm PRIVATE -Wno-switch -Wno-overloaded-virtual -Wno-unused-private-field)
  endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(openmm PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(openmm PUBLIC advapi32)
endif()
