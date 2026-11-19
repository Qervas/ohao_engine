# The physics module (physics/module.hpp): the physics scene and its systems, the players, their components, events
# and API methods; its backend (Jolt) is private (private/backend).
ohao_add_module(physics DEPENDS ecs visual LINKS ohao_core glm)
target_compile_definitions(ohao_physics PUBLIC GLM_ENABLE_EXPERIMENTAL)

# Link Jolt Physics backend (modular - builds even if Jolt unavailable)
if(TARGET Jolt)
    # Private: only the backend includes Jolt. The machine flags its interface carries are every engine target's
    # already (ohao_machine, cmake/ohao_machine.cmake).
    target_link_libraries(ohao_physics PRIVATE Jolt)
    target_compile_definitions(ohao_physics PRIVATE OHAO_HAS_JOLT=1)
    # JoltJobs derives from a Jolt class, and Jolt is built without RTTI; the module's precompiled header is built with
    # it, so this unit parses its own.
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/private/backend/jolt/jolt_jobs.cpp PROPERTIES COMPILE_OPTIONS
        "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>" SKIP_PRECOMPILE_HEADERS ON)
    message(STATUS "ohao_physics: Jolt Physics backend enabled")
else()
    target_compile_definitions(ohao_physics PRIVATE OHAO_HAS_JOLT=0)
    message(STATUS "ohao_physics: Jolt Physics NOT available, using null backend")
endif()
