#include "game/module.hpp"

#include "reflect/type_info.hpp"

namespace ohao::game {

bool decodeParams(Call& call, const reflect::TypeInfo& t, const reflect::JsonNode& params, reflect::DecodeContext& ctx,
                  void* out) {
    if (reflect::readParams(out, t, params, ctx)) return true;
    call.paramsFailed = true;
    return false;
}

} // namespace ohao::game
