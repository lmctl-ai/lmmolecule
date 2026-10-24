# Libraries and licenses

| component | version (commit) | license | how used |
| --- | --- | --- | --- |
| OpenMM Core / OLLA / Reference / CPU / Serialization | 8.2.0 (`53770948`) | MIT / BSD-3-Clause | 180 C++ sources compiled into static `openmm` library by `cmake/OpenMM.cmake` |
| Lepton (`libraries/lepton`) | OpenMM 8.2.0 | MIT | expression parser, analytical differentiation, bytecode evaluator |
| PocketFFT (`libraries/pocketfft`) | OpenMM 8.2.0 | BSD-3-Clause | 3D FFT inside `ReferencePME.cpp` for Particle Mesh Ewald |
| SFMT (`libraries/sfmt`) | 1.4 | BSD-3-Clause | Mersenne Twister RNG for thermostats, barostats, and velocity seeding |
| libLBFGS (`libraries/lbfgs`) | 1.9 | MIT | `LocalEnergyMinimizer` (L-BFGS) |
| Hilbert (`libraries/hilbert`) | OpenMM 8.2.0 | Free-use notice | Hilbert curve spatial atom sorting in `CpuNeighborList` |
| Vecmath (`libraries/vecmath`) | OpenMM 8.2.0 | MIT | vector/matrix math |
| irrXML (`libraries/irrxml`) | 1.2 | zlib | XML serialization |
| dtoa / g_fmt (`serialization/src`) | 1991 | Lucent permissive | floating-point formatting |
| Quern / JAMA / TNT | OpenMM 8.2.0 | Public Domain / NIST | QR and eigensolvers |

## License audit and exclusion of `plugins/cpupme`

Most of OpenMM is MIT/BSD/public-domain, but `plugins/cpupme/src/CpuPmeKernelFactory.cpp` carries
an LGPL-3.0+ header. In OpenMM's CPU platform (`CpuCalcNonbondedForceKernel.cpp`), when the
`cpupme` plugin is not registered, PME automatically falls back to `ReferencePME`
(`platforms/reference/src/SimTKReference/ReferencePME.cpp`, BSD-3-Clause) backed by
`pocketfft_hdronly.h` (BSD-3-Clause). `cmake/OpenMM.cmake` therefore excludes `plugins/cpupme`
entirely so zero LGPL/GPL files are compiled or linked.

## Static build details (`cmake/OpenMM.cmake`)

- OpenMM's upstream root `CMakeLists.txt` invokes Python/SWIG/Doxygen and builds shared plugins.
  Instead, `cmake/OpenMM.cmake` compiles the 180 required C++ sources directly with
  `OPENMM_BUILDING_STATIC_ibrary` and registers the CPU platform statically via
  `extern "C" void registerCpuPlatform()`.
- `olla/src/Platform.cpp` is compiled with `-D__PNACL__` so it omits `<dlfcn.h>` (`dlopen`);
  this dates from the fully static builds and is harmless with the dynamic glibc link.
- On x86_64, `CpuNonbondedForceAvx.cpp` and `CpuCustomNonbondedForceAvx.cpp` are compiled with
  `-mavx` (fixing an upstream bug in `platforms/cpu/staticTarget/CMakeLists.txt`), while
  `CpuNonbondedForceAvx2.cpp` is compiled without `-mavx2` so `isAvx2Supported()` returns `false`
  and avoids a Windows MinGW inline-asm CPUID issue in `vectorizeAvx2.h`.
- `libraries/lbfgs/src/lbfgs.cpp` is compiled without `USE_SSE=1` because `arithmetic_sse_double.h`
  pairs `_aligned_malloc` (`_WIN32`) with `free` when `_MSC_VER` is not defined (MinGW).
- On Linux only glibc (libc/libm/libpthread/libdl, >= 2.35) is linked dynamically (Rule 14);
  everything else is static via `-static-libstdc++ -static-libgcc`. With `LMMOLECULE_FULL_STATIC=ON`
  (not the default), `-static`, `-Wl,--whole-archive -lpthread -Wl,--no-whole-archive` links all of
  `libpthread.a` so `OpenMM::ThreadPool` (`pthread_create` / `pthread_cond_wait`) works safely in a
  fully static binary.
