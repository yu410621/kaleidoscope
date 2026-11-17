# -*- Python -*-
# lit configuration for the Kaleidoscope regression tests.
#
#   llvm-lit -v test                      # uses <repo>/build/toy
#   ninja -C build check                  # same, after rebuilding toy

import os

import lit.formats

config.name = "Kaleidoscope"

# Every test is a .ks file whose "# RUN:" lines are shell commands, executed
# by lit's built-in shell (the external-shell mode is deprecated in LLVM 23).
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".ks"]
config.test_source_root = os.path.dirname(__file__)

# -Dbuild_dir=<dir> on the lit command line overrides the default build tree.
build_dir = os.path.abspath(
    lit_config.params.get(
        "build_dir", os.path.join(config.test_source_root, "..", "build")
    )
)
config.test_exec_root = os.path.join(build_dir, "test")

# %toy expands to the compiler under test.
config.substitutions.append(("%toy", os.path.join(build_dir, "toy")))

# FileCheck and the other LLVM tools come from -Dllvm_tools_dir=<dir> if it
# is given, then from PATH.
path = os.environ.get("PATH", "")
llvm_tools_dir = lit_config.params.get("llvm_tools_dir")
if llvm_tools_dir:
    path = os.pathsep.join((llvm_tools_dir, path))
config.environment["PATH"] = path