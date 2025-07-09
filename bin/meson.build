# -----------------------------------------------------------------------------
# GGDirect Project Build Configuration
# -----------------------------------------------------------------------------
# This is the Meson build configuration for the GGDirect project. It defines the
# project settings, source files, compilation flags, and platform-specific 
# configurations. The script supports different build types: Debug, Release, 
# and Profiling.
# -----------------------------------------------------------------------------

project(
  'GGDirect', 'cpp',
  version : '0.0.1',
  #DEBUG
  default_options : ['warning_level=3', 'cpp_std=c++17', 'debug=true', 'buildtype=debug']
  #RELEASE
  #default_options : ['warning_level=3', 'cpp_std=c++17', 'debug=false', 'buildtype=release', 'optimization=3']
  #PROFILE
  #default_options : ['warning_level=3', 'cpp_std=c++17', 'debug=true', 'buildtype=debug', 'b_ndebug=false', 'b_lto=false']
)

# Build type options
DEBUG = 1
RELEASE = 2
PROFILE = 3
TYPE = DEBUG  # Change this to RELEASE or PROFILE for different build types

# Source files for the project
sources = [
  '../src/main.cpp',
  '../src/window.cpp',
  '../src/display.cpp',
  '../src/renderer.cpp',
  '../src/font.cpp',
  '../src/system.cpp',
  '../src/input.cpp',
  '../src/logger.cpp'
]

# SIMD and CPU-specific options
SSE     = '-msse'
AVX     = '-mavx'
AVX512  = '-mavx512f'

# Default SIMD mode (can be adjusted for specific requirements)
SIMD_MODE = ''

# Common C++ compiler flags
cpp_args = [
  '-Wno-narrowing',              # Disable narrowing warnings (e.g., casting to a smaller type)
  '-Wshadow',
  '-Wdangling-reference',
  '-Wformat=2',
  '-Wnon-virtual-dtor',
  '-Wcast-align',
  '-Wunused',
  '-Wmisleading-indentation',
  '-Wduplicated-cond',
  '-Wduplicated-branches',
  '-Wlogical-op',
  '-Wnull-dereference',
  '-Wuseless-cast',
  '-Wdouble-promotion',
  '-Wimplicit-fallthrough'
  # '-fvisibility=hidden',         # Hide symbols by default
  # '-fPIC',                       # Generate position-independent code
  # '-fno-semantic-interposition', # Disable semantic interposition
]

# Platform-specific settings
# Check if we're on an Android environment
if host_machine.system() == 'android'
  message('Android environment detected: Disabling SIMD flags')
  # Disable SIMD flags on Android due to potential compatibility issues
else
  if SIMD_MODE != ''
    cpp_args += [SIMD_MODE]  # Only add SIMD_MODE if it's not empty
  endif
endif

# Linker arguments (e.g., libraries)
link_args = []

# For PGO, use '-fprofile-generate' to generate and use: '-fprofile-use=GGDirect.p/*.gcda', '-fprofile-correction' in compile_commands.json.

# Add custom flags based on build type
if TYPE == DEBUG
  cpp_args += ['-DGGDirect_DEBUG', '-Og', '-flto', '-fwhole-program']
  link_args += ['-flto', '-fwhole-program']
elif TYPE == RELEASE
  cpp_args += ['-DGGDirect_RELEASE', '-march=native', '-flto', '-fwhole-program']
  link_args += ['-flto', '-fwhole-program']
elif TYPE == PROFILE
  cpp_args += ['-DGGDirect_PROFILE', '-DGGDirect_DEBUG', '-pg']
  link_args += ['-pg']
endif

if TYPE == DEBUG or TYPE == PROFILE
  # Windows-specific settings (e.g., for debugging)
  if host_machine.system() == 'windows'
    link_args += ['-lDbgHelp', '-Wl,--export-all-symbols']    # Link with DbgHelp library for better debugging support
  elif host_machine.system() == 'linux'
    # Linux-specific settings can be added here if needed
    cpp_args += ['-rdynamic']  
  endif
endif

# Dependencies
drm_dep = dependency('libdrm', required: true)
thread_dep = dependency('threads', required: true)
freetype_dep = dependency('freetype2', required: true)

dependencies = [drm_dep, thread_dep, freetype_dep]

# Define the executable to be built
executable(
  'GGDirect',                      # Output executable name
  sources,                     # Source files
  dependencies: dependencies,  # External dependencies
  install : true,               # Install the executable after building
  cpp_args: cpp_args,          # C++ compiler flags
  link_args: link_args         # Linker flags
)
