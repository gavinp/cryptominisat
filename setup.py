# -*- coding: utf-8 -*-
#
# CryptoMiniSat
#
# Original work Copyright (c) 2009-2014, Mate Soos. All rights reserved.
# Modified work Copyright (c) 2017, Pierre Vignet
#
#Permission is hereby granted, free of charge, to any person obtaining a copy
#of this software and associated documentation files (the "Software"), to deal
#in the Software without restriction, including without limitation the rights
#to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#copies of the Software, and to permit persons to whom the Software is
#furnished to do so, subject to the following conditions:
#
#The above copyright notice and this permission notice shall be included in
#all copies or substantial portions of the Software.
#
#THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
#THE SOFTWARE.

from distutils.core import setup, Extension
from distutils.cmd import Command
from distutils import sysconfig

__PACKAGE_VERSION__ = "0.1.1"
__LIBRARY_VERSION__ = "5.0.1"


# Delete unwanted flags for C compilation
# Distutils has the lovely feature of providing all the same flags that
# Python was compiled with. The result is that adding extra flags is easy,
# but removing them is a total pain. Doing so involves subclassing the
# compiler class, catching the arguments and manually removing the offending
# flag from the argument list used by the compile function.
# That's the theory anyway, the docs are too poor to actually guide you
# through what you have to do to make that happen.
d = sysconfig.get_config_vars()
for k, v in d.items():
    for unwanted in ('-Wstrict-prototypes', '-DNDEBUG', ' -g ',
                     '-O2', '-D_FORTIFY_SOURCE=2', '-fstack-protector-strong'):
        if str(v).find(unwanted) != -1:
            v = d[k] = str(v).replace(unwanted, ' ')

################################################################################

def _init_posix(init):
    """
    Forces g++ instead of gcc on most systems
    credits to eric jones (eric@enthought.com) (found at Google Groups)
    """
    def wrapper():
        init()

        config_vars = sysconfig.get_config_vars()  # by reference
        if config_vars["MACHDEP"].startswith("sun"):
            # Sun needs forced gcc/g++ compilation
            config_vars['CC'] = 'gcc'
            config_vars['CXX'] = 'g++'

    return wrapper

sysconfig._init_posix = _init_posix(sysconfig._init_posix)

################################################################################

class TestCommand(Command):
    """Call tests with the custom 'python setup.py test' command."""

    user_options = []

    def initialize_options(self):
        pass

    def finalize_options(self):
        pass

    def run(self):

        import tests as tp
        tp.run()

################################################################################

# Source files
cryptoms_lib_files = [
    "GitSHA1.cpp",
    "cnf.cpp",
    "propengine.cpp",
    "varreplacer.cpp",
    "clausecleaner.cpp",
    "clauseusagestats.cpp",
    "prober.cpp",
    "occsimplifier.cpp",
    "subsumestrengthen.cpp",
    "clauseallocator.cpp",
    "sccfinder.cpp",
    "solverconf.cpp",
    "distillerallwithall.cpp",
    "distillerlongwithimpl.cpp",
    "str_impl_w_impl_stamp.cpp",
    "solutionextender.cpp",
    "completedetachreattacher.cpp",
    "searcher.cpp",
    "solver.cpp",
    "gatefinder.cpp",
    "sqlstats.cpp",
    "implcache.cpp",
    "stamp.cpp",
    "compfinder.cpp",
    "comphandler.cpp",
    "hyperengine.cpp",
    "subsumeimplicit.cpp",
    "cleaningstats.cpp",
    "datasync.cpp",
    "reducedb.cpp",
    "clausedumper.cpp",
    "bva.cpp",
    "intree.cpp",
    "features_calc.cpp",
    "features_to_reconf.cpp",
    "solvefeatures.cpp",
    "searchstats.cpp",
    "xorfinder.cpp",
    "cryptominisat_c.cpp",
    "cryptominisat.cpp",
#    "gaussian.cpp",
#    "matrixfinder.cpp",
]

modules = [
    Extension(
        "pycryptosat",
        ["python/pycryptosat.cpp"] + ['src/' + fd for fd in cryptoms_lib_files],
        language = "c++",
        include_dirs=['src', '.'],
        extra_compile_args=[
            "-pthread",
            "-DUSE_PTHREADS",
            "-fopenmp",
            "-D_GLIBCXX_PARALLEL",
            "-flto",
            "-std=c++11",
            "-Wno-unused-variable",
            "-Wno-unused-but-set-variable",

            # Assume that signed arithmetic overflow of addition, subtraction
            # and multiplication wraps around using twos-complement
            # representation
            "-fwrapv",

            # Buffer Overflow protection (use both)
            #"-D_FORTIFY_SOURCE=2",
            #"-fstack-protector-strong",

            # Allows GCC to generate code that may not run
            # at all on processors other than the one indicated
            # -march=cpu-type implies -mtune=cpu-type.
            # See CPU type with:
            # gcc -march=native -Q --help=target | grep march
            "-march=native", # haswell
            # Produce code optimized by enabling all
            # instruction subsets supported by the local machine
            # See CPU type with:
            # gcc -mtune=native -Q --help=target | grep march
            "-mtune=native", # x86-64

            # Release/Debug flags
            "-Ofast",
            #"-O3",
            # "-g", # Not define NDEBUG macro => Debug build
            "-DNDEBUG", # Force release build
            #"-Wall",
        ],
        extra_link_args=[
            "-Ofast",
            "-flto",
            "-fopenmp",
        ]
    ),
]

setup(
    name = "pycryptosat",
    version = __PACKAGE_VERSION__,
    author = "Mate Soos",
    author_email = "soos.mate@gmail.com",
    url = "https://github.com/msoos/cryptominisat",
    license = "MIT",
    classifiers = [
        "Development Status :: 4 - Beta",
        "Intended Audience :: Developers",
        "Operating System :: OS Independent",
        "Programming Language :: C++",
        "Programming Language :: Python :: 2",
        "Programming Language :: Python :: 2.7",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.5",
        "License :: OSI Approved :: MIT License",
        "Topic :: Utilities",
    ],
    ext_modules = modules,
    py_modules = ['pycryptosat'],
    description = "Bindings to CryptoMiniSat {} (a SAT solver)".\
        format(__LIBRARY_VERSION__),
    long_description = open('python/README.rst').read(),
    cmdclass={
        'test': TestCommand
    }
)
