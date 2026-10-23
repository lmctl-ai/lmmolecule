# Next release version agreement

Main targets 0.1.2; the published 0.1.1 snapshot remains unchanged. Change VERSION, the CMake project version and the first line of THIRD_PARTY_NOTICES together before the next release. test.sh checks these against the built executable and refuses a GitHub v-tag with another version.

DSL schema format 2 is independent of the package version. The compiler rejects unknown schema metadata keys, so the check validates its name/format and compares the complete embedded schema to the committed file instead of adding a package-version field.

The 2026-10-23 host audit found 0.2.0 development metadata in lmcell, lmparticle, lmfluid and lmmolecule; lmweather, lmplot, lmfinance, lmphysics, lmnum, lmbio and lmspace retained 0.1.1. No release tags were changed.
