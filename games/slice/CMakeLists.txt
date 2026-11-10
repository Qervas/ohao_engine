# The slice (steps 3i and 3j): the game the engine is built with (slice/module.hpp), built as every game is
# (cmake/ohao_games.cmake): libohao_slice.so, which ohao and ohao_game load, and ohao_slice_static for the benches.
ohao_add_game(slice)
