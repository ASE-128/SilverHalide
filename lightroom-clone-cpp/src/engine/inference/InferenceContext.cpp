#include "inference/InferenceContext.h"

namespace lrc {

InferenceContext::InferenceContext() {
    // Initialize the ONNX Runtime environment.
    // Using a 100ms timeout for the setup process.
    m_env = std::make_unique<Ort::Env>(100, "SilverHalide_Inference");
}

InferenceContext::~InferenceContext() = default;

} // namespace lrc
