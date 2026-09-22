import configparser
import glob
import os
import sys

from pybind11.setup_helpers import Pybind11Extension, build_ext
from setuptools import setup

if sys.platform == "darwin":
    # pybind11's setup helper falls back to a very old deployment target when
    # the env var is unset, which puts parts of the C++17/20 libc++ runtime
    # out of reach. Pin a modern floor here so that pip install / pip install
    # -e . do not depend on the caller's shell.
    os.environ.setdefault("MACOSX_DEPLOYMENT_TARGET", "11.0")

config = configparser.ConfigParser()
config.read("binding/module_config.ini")

ext_name = config["build"]["ext_name"]
binding_file = config["build"]["binding"]
src_file_list = (
    glob.glob(config["build"]["src"] + "/core/*.cpp", recursive=True) +
    # The C ABI waist (src/api/): the binding reaches the library only through
    # <xiapl/c_api.h>, so the waist's objects have to be inside the extension.
    glob.glob(config["build"]["src"] + "/api/*.cpp", recursive=True)
)

# Waist-consuming halves of the binding (binding/core_common.h documents the
# include-purity rule they follow, tests/check_binding_includes.sh enforces it).
# Globbed so a new core_*.cpp needs no edit here.
binding_core_files = sorted(glob.glob("binding/core_*.cpp"))


ext_modules = [
    Pybind11Extension(
        ext_name,
        [
            binding_file,
            *binding_core_files,
            *src_file_list,
        ],
        include_dirs=["include", "src"],
        cxx_std=20,
    ),
]

setup(
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
)
