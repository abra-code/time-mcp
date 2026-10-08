#!/bin/bash
#
# scripts/find-linux-toolchain.sh - find what the Makefile needs to build time-mcp for Linux
# on a Mac ("make linux").
#
# Xcode's compiler cannot build a Linux program: it has no Linux libraries and no linker for
# one. A swift.org toolchain has a clang and a linker that can, and the static Linux SDK of
# the same version has the C and C++ libraries (musl and libc++) that go into the program.
#
# Usage: scripts/find-linux-toolchain.sh
#
# Prints two lines and exits with status 0:
#   1. the toolchain's folder of tools (clang, clang++, ld.lld)
#   2. the SDK's folder that holds one folder per processor type (aarch64, x86_64)
# Prints nothing on standard output and exits with status 1 when there is no toolchain with
# its SDK, with what is missing and where to get it on standard error.
#
# The newest toolchain that has its SDK is taken, so an older pair still works after a newer
# toolchain is installed without one. A toolchain and an SDK belong together when the SDK's
# name starts with the toolchain's: swift-6.4.0-RELEASE and
# swift-6.4.0-RELEASE_static-linux-0.1.0. The SDK's own version and the version of musl, which
# are also part of its path, are taken as found.

SDKS_DIR="$HOME/Library/org.swift.swiftpm/swift-sdks"
TOOLS="clang clang++ ld.lld"
ARCHS="aarch64 x86_64"

# Every swift.org release toolchain, one per line as "<name><tab><path>". The installer puts
# them in the user's folder, or in /Library when it is told to install for all users.
list_toolchains() {
    local _dir
    local _path
    for _dir in "$HOME/Library/Developer/Toolchains" "/Library/Developer/Toolchains"; do
        for _path in "$_dir"/swift-*-RELEASE.xctoolchain; do
            # Also passes over the pattern itself, which is what the loop gets when no
            # toolchain is there.
            [ -d "$_path" ] || continue
            local _name="${_path##*/}"
            printf '%s\t%s\n' "${_name%.xctoolchain}" "$_path"
        done
    done
}

# has_tools <toolchain>  ->  status 0 when the toolchain has every program the build runs.
has_tools() {
    local _tool
    for _tool in $TOOLS; do
        [ -x "$1/usr/bin/$_tool" ] || return 1
    done
    return 0
}

# sdk_of <toolchain name>  ->  prints the newest static Linux SDK of that toolchain that has
# what the build uses for both processor types, or nothing.
sdk_of() {
    local _sdk
    local _arch
    local _found=""
    for _sdk in "$SDKS_DIR/$1"_static-linux-*.artifactbundle/*/swift-linux-musl/musl-*.sdk; do
        [ -d "$_sdk" ] || continue
        local _complete="yes"
        for _arch in $ARCHS; do
            # The headers and libraries, and the folder with the start files the link needs.
            if [ ! -d "$_sdk/$_arch/usr/include" ] || [ ! -d "$_sdk/$_arch/usr/lib/swift/clang" ]; then
                _complete="no"
            fi
        done
        if [ "$_complete" = "yes" ]; then
            _found="$_found$_sdk
"
        fi
    done
    [ -n "$_found" ] || return 0
    # Newest first, by version number (0.10.0 after 0.9.0).
    printf '%s' "$_found" | /usr/bin/sort -rV | /usr/bin/head -1
}

toolchains="$(list_toolchains | /usr/bin/sort -rV)"

without_sdk=""
while IFS=$'\t' read -r name path; do
    [ -n "$name" ] || continue
    has_tools "$path" || continue
    sdk="$(sdk_of "$name")"
    if [ -n "$sdk" ]; then
        printf '%s\n%s\n' "$path/usr/bin" "$sdk"
        exit 0
    fi
    without_sdk="$without_sdk $name"
done <<EOF_TOOLCHAINS
$toolchains
EOF_TOOLCHAINS

{
    printf 'time-mcp: "make linux" did not find a compiler and libraries for Linux.\n'
    if [ -n "$without_sdk" ]; then
        printf '  Found%s, but no static Linux SDK of the same version\n' "$without_sdk"
        printf '  (a folder <toolchain>_static-linux-<version>.artifactbundle in %s).\n' "$SDKS_DIR"
    else
        printf '  Found no swift.org toolchain (swift-<version>-RELEASE.xctoolchain in\n'
        printf '  ~/Library/Developer/Toolchains or /Library/Developer/Toolchains) with clang and ld.lld.\n'
    fi
    printf '  The build needs a swift.org toolchain and the static Linux SDK of the same version;\n'
    printf '  Xcode alone cannot build for Linux. Both are at https://www.swift.org/install/macos\n'
    printf '  ("Static Linux SDK" there). Scripts/install-linux-toolchain.sh of agent-vm\n'
    printf '  (https://github.com/abra-code/agent-vm) installs both into your home folder.\n'
    printf '  To use a toolchain and an SDK that are somewhere else:\n'
    printf '    make linux LINUX_TOOLCHAIN=<folder with clang, clang++ and ld.lld> LINUX_SDK=<folder with aarch64 and x86_64>\n'
} >&2
exit 1
