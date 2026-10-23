#ifndef LMMOLECULE_OPENMM_SHIM_H
#define LMMOLECULE_OPENMM_SHIM_H

#include <stdint.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <windows.h>
#include <intrin.h>
static inline void lmmolecule_cpuid_no_avx(int cpuInfo[4], int infoType) {
  __cpuid(cpuInfo, infoType);
  if (infoType == 1) {
    cpuInfo[2] &= ~((int)1 << 28);
  }
}
#define __cpuid lmmolecule_cpuid_no_avx
#endif

#endif
