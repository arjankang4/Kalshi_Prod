add_library(kp_warnings INTERFACE)
add_library(kp::warnings ALIAS kp_warnings)
target_compile_options(kp_warnings INTERFACE
    -Wall -Wextra
    -Wdouble-promotion   # a float sneaking into a double expression is a parity bug
    -Wshadow
    -Wno-unused-parameter
)
