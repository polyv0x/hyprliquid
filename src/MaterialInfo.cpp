#include "MaterialInfo.hpp"
#include <json-c/json.h>

void addMaterialInfo(json_object* target, const SMaterial& material, bool layer) {
    json_object_object_add(target, "model", json_object_new_string(materialFamilyName(material.family)));
    json_object_object_add(target, "effective_blur_sigma", json_object_new_double(materialBlurSigma(material)));
    auto* applicable = json_object_new_array();
#define GLASS_PARAM(name, value, lo, hi) \
    if (materialParameter(material.family, #name)) \
        json_object_array_add(applicable, json_object_new_string(#name));
#include "MaterialParams.inc"
#undef GLASS_PARAM
    json_object_object_add(target, "applicable_parameters", applicable);
    if (material.family != EMaterialFamily::LIQUID) return;
    auto* info = json_object_new_object();
    auto* variants = json_tokener_parse(LIQUID_VARIANTS_JSON);
    const auto* environment = material.darkTheme ? "dark" : "default";
    json_object_object_add(info, "environment", json_object_new_string(environment));
    auto* parameters = json_object_object_get(variants, environment);
    json_object_object_add(info, "parameters", json_object_get(parameters));
    json_object* effective = nullptr;
    if (json_object_deep_copy(parameters, &effective, nullptr) == 0) {
        const auto resolved = effectiveLiquidMaterial(material);
        const auto number = [](json_object* object, const char* key, double value) {
            json_object_object_add(object, key, json_object_new_double(value));
        };
        const auto array = [&](json_object* object, const char* key, const auto& values) {
            auto* list = json_object_new_array();
            for (const auto value : values) json_object_array_add(list, json_object_new_double(value));
            json_object_object_add(object, key, list);
        };
        auto* blur = json_object_object_get(effective, "blur");
        number(blur, "radius", resolved.blur.radius);
        array(blur, "distances", resolved.blur.distances); array(blur, "opacities", resolved.blur.opacities);
        auto* refraction = json_object_object_get(effective, "refraction");
        number(refraction, "innerAmount", resolved.refraction.innerAmount); number(refraction, "innerHeight", resolved.refraction.innerHeight);
        number(refraction, "outerAmount", resolved.refraction.outerAmount); number(refraction, "outerHeight", resolved.refraction.outerHeight);
        array(refraction, "outerDistances", resolved.refraction.outerDistances);
        number(json_object_object_get(effective, "face"), "opacity", resolved.face.opacity);
        auto* shadow = json_object_object_get(effective, "shadow");
        number(shadow, "opacity", resolved.shadow.opacity); array(shadow, "offset", resolved.shadow.offset);
        auto* highlight = json_object_object_get(effective, "highlight");
        number(highlight, "keyHeight", resolved.highlight.keyHeight); number(highlight, "fillHeight", resolved.highlight.fillHeight);
        array(highlight, "keyColor", resolved.highlight.keyColor); array(highlight, "fillColor", resolved.highlight.fillColor);
        json_object_object_add(info, "effective_parameters", effective);
    }
    json_object_object_add(info, "display_angle_degrees", json_object_new_double(material.display_angle_degrees));
    json_object_object_add(info, "shadow_owner", json_object_new_string(layer ? "surface" : "material"));
    json_object_object_add(info, "output_extent", json_object_new_double(layer ? 0 : materialOutputExtent(material)));
    auto* assumptions = json_object_new_array();
    for (const auto* assumption : {
        "light uses the default material variant; dark uses the dark variant",
        "blur uses Gaussian sigma with a variance-corrected prefilter and a sharp/blur mix",
        "five blur stops are interpolated directly, including repeated stop positions",
        "face color uses luma, range, saturation and normal fill",
        "highlight matrix uses alpha-scaled bias and premultiplied source-over",
        "display angle rotates key/fill lighting; highlight response coefficients are fixed at zero",
        "native outline uses unit highlight normals and stabilized optical normals",
        "outer shadow locally darkens the destination with Gaussian coverage truncated at four radii; no displaced shadow sampling",
        "shadow displacement and color fields are inactive; layers retain surface shadows",
        "layer highlights follow rectangular bounds; arbitrary alpha SDF is not generated",
        "SDR only; adaptive feedback and headroom conversion are not implemented"})
        json_object_array_add(assumptions, json_object_new_string(assumption));
    json_object_object_add(info, "assumptions", assumptions);
    json_object_object_add(target, "model_data", info);
    json_object_put(variants);
}
