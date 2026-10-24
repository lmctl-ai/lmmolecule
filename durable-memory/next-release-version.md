# Next release version agreement

The package version is declared in VERSION; published snapshots remain unchanged. Change VERSION, the CMake project version and the first line of THIRD_PARTY_NOTICES together before the next release. test.sh checks these against the built executable and refuses a GitHub v-tag with another version.

DSL schema format 2 is independent of the package version. The compiler rejects unknown schema metadata keys, so the check validates its name/format and compares the complete embedded schema to the committed file instead of adding a package-version field.

Version progression: metadata updates must advance VERSION, CMake project version, and THIRD_PARTY_NOTICES synchronously.
