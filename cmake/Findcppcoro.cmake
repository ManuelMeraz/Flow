find_library(CPPCORO_LIBRARIES NAMES cppcoro REQUIRED)
find_path(CPPCORO_INCLUDE_DIRS NAMES cppcoro REQUIRED)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(cppcoro DEFAULT_MSG CPPCORO_LIBRARIES CPPCORO_INCLUDE_DIRS)
