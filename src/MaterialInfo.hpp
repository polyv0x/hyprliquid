#pragma once
#include "Material.hpp"
struct json_object;

// Additive API metadata, kept outside the compatible scalar material object.
void addMaterialInfo(json_object* target, const SMaterial& material, bool layer = false);
