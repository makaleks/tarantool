build_module(module_api module_api.c)

# gh-5313: verify that module.h actually conforms to the C99
# standard.
set(CMAKE_C_FLAGS "-Wall -Wextra -std=c99")
set(CMAKE_C_FLAGS_DEBUG "-O0 -Werror")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2")
