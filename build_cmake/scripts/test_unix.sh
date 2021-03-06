#!/bin/bash -e

# Note this script is used by the build server so this script cannot rely
# on any sort of directory structure. It must be run from the root directory
# of the LiteCore CMake output
if [ ! -f LiteCore/tests/CppTests ]; then
  echo "CppTests not found!"
  exit 1
fi

if [ ! -f C/tests/C4Tests ]; then
  echo "C4Tests not found!"
  exit 1
fi

pushd LiteCore/tests
LiteCoreTestsQuiet=1 ./CppTests -r list
popd

pushd C/tests
LiteCoreTestsQuiet=1 ./C4Tests -r list
popd
