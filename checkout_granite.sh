#!/bin/bash

# Only checks out what is necessary to build standalone.
#
# Granite is a git submodule pinned by this repository (see .gitmodules).
# Do not use `git clone --recursive` or `git submodule update --recursive`
# for the standalone build: Granite has many nested submodules that are not needed.
set -e

cd "$(dirname "$0")"

git submodule sync --quiet Granite
git submodule update --init Granite

cd Granite
for module in third_party/volk third_party/khronos/vulkan-headers; do
	git submodule sync --quiet $module
	git submodule update --init $module
done
