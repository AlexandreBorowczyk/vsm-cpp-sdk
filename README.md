# VSM SDK

C++11 VSM SDK for [Universal ground Control Software](http://www.ugcs.com/ "UgCS").

## General information

C++ VSM SDK is implemented using C++11 language which is also a requirement
for SDK users code.

VSM SDK uses CMake as its build system to keep it platform independent.

Essentially what CMake does is it wraps the platform specific build
commands/files under common interface. Out-of-source builds are supported and recommended.

### System requirements

Currently supported build toolchains/compilers and their respective versions are:
- Python 2.7
- CMake 2.8.8 or higher

Windows:
- Windows 7, 8, 10 
- MinGW-w64 with GCC 5.2.0+ are recommended.

Linux:
- Ubuntu 14.04+
- GCC 4.8.1+

Mac:
- OSX Yosemite and above
- GCC 4.9+ from MacPorts


## Setting up the build environment

Below are instructions on how to prepare build environment on Windows and Ubuntu Linux.
After the required components are installed the workflow is identical on all platforms.

### MacOS X

Your should use MacPorts for GCC installation.


    $ curl -O https://distfiles.macports.org/MacPorts/MacPorts-2.3.4.tar.bz2
    $ tar xf MacPorts-2.3.4.tar.bz2
    $ cd MacPorts-2.3.3/
    $ ./configure
    $ make
    # make install
    # port selfupdate
    # port install gcc49
    # port select --set gcc mp-gcc49


After that you can run CMake in the same way as on other platforms.

### Windows
#### MinGW

Download and install the latest MinGW-w64 from
http://sourceforge.net/projects/mingw-w64.

Use [MinGW installer](http://sourceforge.net/projects/mingw-w64/files/Toolchains%20targetting%20Win32/Personal%20Builds/mingw-builds/installer).
When it asks for configuration you should input:
 - Version: 5.2
 - Architecture: your architecture
 - Threads: posix
 - Exception: dwarf for 32-bits, seh for 64-bits.

Make sure mingw bin directory is added to PATH (E.g. C:\\mingw\\mingw64\\bin)

#### CMake

Get the latest cmake from http://www.cmake.org.

Make sure cmake is added to PATH (E.g. C:\\Program Files (x86)\\CMake 2.8\\bin)

#### Python

Get the latest Python 2.7 from http://www.python.org/download/

Make sure python is added to PATH (E.g. C:\\Python27)

### Linux

Ubuntu 14.04+ has all the required packages in default repositories.

    # apt install gcc g++ cmake python2.7


## Eclipse setup (optional)

Eclipse is recommended IDE for all platforms.

You should have the latest Java for eclipse to run.

Some plugins are recommended for the development of VSM SDK itself. They are not
required for standalone VSM development.

CMake editor plugin: http://sourceforge.net/projects/cmakeed/

UML diagram plugin: http://plantuml.sourceforge.net/eclipse.html

The vsm-cpp-sdk directory includes eclipse project file which is tuned
for underlying build system.
To use it go to "File/Import/Existing Projects into Workspace"
Eclipse project features:
 -# Same project file can be used on both Linux and Windows.
 -# Custom Targets to create makefiles and build with one click.
 -# Source indexer tuned to support C++11 syntax and correct includes.
 -# Predefined build configurations for win and linux.
 
Lastly under Project/Properties/C++General/Preprocessor Includes/Providers:
Check the "CDT GCC built in compiler settings" checkbox.

## Compiling SDK

OEM manufacturers and enthusiasts are supposed to compile the SDK by themselves
so source code is provided for them (https://github.com/UgCS/vsm-cpp-sdk),
however they are not supposed to modify the SDK sources. If some functionality
is missing, it is always worth to contact UgCS and suggest a proposal. Good
proposals have a chance to be officially added to the SDK, so everybody can
benefit from it. There is no support for Microsoft MSVC compiler due to poor support
of C++11 language standard by Microsoft.

CMAKE (http://www.cmake.org/) cross-platform build system is used by VSM C++
SDK. You can use standard CMake variables to change the build process:

 - CMAKE_BUILD_TYPE - possible values are "DEBUG" for debug
builds and "RELEASE" for release build.
 - CMAKE_INSTALL_PREFIX - change the installation directory of "make install" target.
(Default is "/usr/local/" which installs the SDK in directory "/usr/local/opt/vsm-sdk".)

Python 2.6 (or later from branch 2.x) is needed for make file generation.

### Building SDK from command line

Assume you are working from your $HOME directory:

1) Clone the SDK repository:
	
       git clone git@github.com:UgCS/vsm-cpp-sdk.git
   
2) Make and enter into build directory:

       mkdir -p build/vsm-sdk
       cd build/vsm-sdk

3) Create make files:

       cmake -DUGCS_INSTALL_DIR=~/install/ -DCMAKE_BUILD_TYPE=Release -G"Unix Makefiles" ~/vsm-cpp-sdk

4) Launch the build:

       cmake --build . -- install -j8

	If build suceeds you'll have VSM SDK installed in directory $HOME/install/opt/vsm-sdk
   
Proceeed to VSM build instrucions

### Building Ardupilot VSM from command line

Assume you are working from your $HOME directory:

1) Clone the Ardupilot and common repositories:

    Ardupilot VSM requires vsm-cpp-common repository which includes MAVLINK frameworks which is shared among all VSMs which support MAVLINK protocol.

       git clone git@github.com:UgCS/vsm-cpp-common.git
       git clone git@github.com:UgCS/vsm-cpp-ardupilot.git
   
2) Make and enter into build directory:

       mkdir -p build/vsm-ardupilot
       cd build/vsm-ardupilot

3) Create make files:

       cmake -DVSM_SDK_DIR=$HOME/install/opt/vsm-sdk -DCOMMON_SOURCES=$HOME/vsm-cpp-common -G"Unix Makefiles" $HOME/vsm-cpp-ardupilot

4) Launch the build:

       cmake --build .

	If build suceeds you'll have vsm-ardupilot executable in current directory.

### Building VSM SDK for Android

All the previous instructions valid and additionally ANDROID CMake variable should
be defined. Also Android NDK should be available and its path either defined in
ANDROID_NDK environment variable or passed directly to CMake. E.g. on Linux:

    cmake -DANDROID=1 -DANDROID_NDK=/opt/android-ndk-r10e ..

VSM SDK should be installed after compilation as usually. Android native
libraries are available in build directory under "android/libs" path after the
VSM is built.

Additional CMake and environment variables are accepted for Android build:

 - ANDROID_ABI_LIST can specify semicolon-separated list of target ABI (e.g. "armeabi;armeabi-v7a;x86").
 - ANDROID_PLATFORM can specify target Android platform (e.g. "android-19")

