#pragma once

#include <clab/observation.hpp>

#include <filesystem>

namespace clab {
void verify_checkpoint_target(const CheckpointRef& ref, const std::filesystem::path& path);
}
