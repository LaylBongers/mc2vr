# What the game IS (docs/reimpl.md): typed game data, fixed-address globals,
# hook-site addresses, and original functions as typed callables with their
# register conventions (function.hpp). No mod behavior lives here.
add_library(mc2vr_game STATIC
    function.cpp
)

# Include root is src/: `#include "game/pg_primitive.hpp"`.
target_include_directories(mc2vr_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mc2vr_game PUBLIC cxx_std_20)
