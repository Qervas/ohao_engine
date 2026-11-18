# The machine every engine, game, bench and tool target is compiled for (ohao_machine): x86-64 with AVX2, FMA, F16C,
# BMI, LZCNT and POPCNT, floating point in SSE registers, and -pthread, as Jolt's interface asks of its users. One
# setting, so where a piece of arithmetic lives never changes how it is compiled: the instruction set decides how
# a * b + c contracts (FMA), and with it every world.hash. ohao_core and ohao_reflect, the two roots every engine
# library reaches, link it PUBLIC; Jolt is linked privately (ohao/physics) and its own flags stay its own.
# Not given it: the external libraries, and the CPU twin (tools/material: ohao_openpbr_cpu, openpbr_tables), whose
# checked-in energy tables are generated without it.
add_library(ohao_machine INTERFACE)
if(MSVC)
    target_compile_options(ohao_machine INTERFACE /arch:AVX2)
else()
    target_compile_options(ohao_machine INTERFACE -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse -pthread)
endif()
