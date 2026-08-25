#include "inference/ModelLoader.h"
#include <iostream>

namespace lrc {

std::vector<const char*> ModelLoader::get_default_providers() {
    // In a real implementation, we would check for CUDA availability.
    // For now, we provide a standard fallback list.
    return {"CUDAExecution_Provider", "CPUExecution_Provider"};
}

std::shared_ptr<InferenceSession> ModelLoader::load_model(const std::string& path) {
    auto session = std::make_shared<InferenceSession>(path);
    return session;
}

// Internal constructor implementation
InferenceSession::InferenceSession(const std::string& model_path)
    : m_providers(ModelLoader::get_default_providers()) {
    // Initialization of Ort::Session would go here.
}

} // namespace lcr
