// inverse_fit — physical inverse renderer CLI.
//
// Implementation lives in ohao/inverse/{fit_config,scene_builder,io,
// render_session,fit_engine}.hpp. This binary is the thin entry point.
//
// Usage:
//   ./inverse_fit --selftest --preset lantern --quality draft
//   ./inverse_fit --selftest --preset mirror --quality draft
//   ./inverse_fit --target-image photo.png --fit-exposure --quality high

#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif
#include "stb_image.h"
#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"
// One TU owns the STB implementations; headers re-include for decls only.
#undef STB_IMAGE_IMPLEMENTATION
#undef STB_IMAGE_WRITE_IMPLEMENTATION

#include "inverse/fit_config.hpp"
#include "inverse/fit_engine.hpp"

int main(int argc, char** argv) {
    using namespace ohao::inverse;
    CliArgs args = parseArgs(argc, argv);
    return runInverseFit(std::move(args.cfg));
}
