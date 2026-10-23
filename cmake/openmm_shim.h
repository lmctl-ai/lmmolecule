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
static inline void lmmolecule_no_cpuid(int cpuInfo[4], int) {
  cpuInfo[0] = 0;
  cpuInfo[1] = 0;
  cpuInfo[2] = 0;
  cpuInfo[3] = 0;
}
#define __cpuid lmmolecule_no_cpuid
#endif

#endif
