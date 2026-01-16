#include "register_types.h"
#include "ohao_viewport.h"
#include "ohao_physics_body.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "ui/components/console_widget.hpp"

using namespace godot;

// Godot log callback for OHAO's ConsoleWidget
static void godot_log_callback(ohao::LogLevel level, const std::string& message) {
    godot::String godot_msg = godot::String(message.c_str());
    switch (level) {
        case ohao::LogLevel::Info:
        case ohao::LogLevel::Debug:
            UtilityFunctions::print("[OHAO] ", godot_msg);
            break;
        case ohao::LogLevel::Warning:
            UtilityFunctions::push_warning("[OHAO] ", godot_msg);
            break;
        case ohao::LogLevel::Error:
            UtilityFunctions::push_error("[OHAO] ", godot_msg);
            break;
    }
}

void initialize_ohao_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    // Set up OHAO logging to redirect to Godot's console
    ohao::ConsoleWidget::get().setLogCallback(godot_log_callback);

    // Register our custom classes
    ClassDB::register_class<OhaoViewport>();
    ClassDB::register_class<OhaoPhysicsBody>();
}

void uninitialize_ohao_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    // Clear the OHAO log callback
    ohao::ConsoleWidget::get().clearLogCallback();
}

extern "C" {
    GDExtensionBool GDE_EXPORT ohao_library_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        const GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization
    ) {
        godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

        init_obj.register_initializer(initialize_ohao_module);
        init_obj.register_terminator(uninitialize_ohao_module);
        init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

        return init_obj.init();
    }
}
