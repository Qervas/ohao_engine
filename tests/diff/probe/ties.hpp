// The eight startup source-parsing ties.
//
// Each returns true when the tie holds and false when it does not, having
// already printed its own diagnosis. main() runs all eight, in the order
// declared here, BEFORE any Vulkan object exists -- a tie that fails is a
// silent-wrong-answer bug in a check, and none of the GPU work is worth
// doing until they hold.
//
// The implementations, and the argument each one makes for why a runtime
// source parse is the right instrument rather than a static_assert or a
// comment, are in ties.cpp.
#pragma once

namespace ohao::diff::probe {

bool checkNeeStrideTie();
bool checkWfScatterSinkLayoutTie();
bool checkDrawsPerBounceTie();
bool checkTraverseInstantiationTie();
bool checkScatterPushSizeTie();
bool checkBsdfShaderConstantTies();
bool checkParityRefConstantsTie();
bool checkTexelOrderingTie();

}  // namespace ohao::diff::probe
