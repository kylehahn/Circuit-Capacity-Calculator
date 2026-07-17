// tinygltf is header-only; pull its implementation in here only.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include <tiny_gltf.h>

#include "GlbIo.hpp"
#include "JsonIo.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace ccc::io {

namespace {
constexpr const char* kExtrasKey = "sensorEditor";

// Convert a nlohmann::json document into a tinygltf::Value tree (recursive).
tinygltf::Value jsonToTinyValue(const nlohmann::json& j) {
    using V = tinygltf::Value;
    if (j.is_object()) {
        V::Object o;
        for (auto it = j.begin(); it != j.end(); ++it)
            o.emplace(it.key(), jsonToTinyValue(it.value()));
        return V(o);
    }
    if (j.is_array()) {
        V::Array a;
        a.reserve(j.size());
        for (const auto& el : j) a.push_back(jsonToTinyValue(el));
        return V(a);
    }
    if (j.is_string())  return V(j.get<std::string>());
    if (j.is_boolean()) return V(j.get<bool>());
    if (j.is_number_integer()) return V(static_cast<int>(j.get<int64_t>()));
    if (j.is_number())  return V(j.get<double>());
    return V();   // null
}

// Reverse: tinygltf::Value tree → nlohmann::json
nlohmann::json tinyValueToJson(const tinygltf::Value& v) {
    using nlohmann::json;
    if (v.IsObject()) {
        json o = json::object();
        for (const auto& key : v.Keys()) o[key] = tinyValueToJson(v.Get(key));
        return o;
    }
    if (v.IsArray()) {
        json a = json::array();
        for (size_t i = 0; i < v.ArrayLen(); ++i) a.push_back(tinyValueToJson(v.Get(int(i))));
        return a;
    }
    if (v.IsString()) return v.Get<std::string>();
    if (v.IsBool())   return v.Get<bool>();
    if (v.IsInt())    return v.Get<int>();
    if (v.IsNumber()) return v.GetNumberAsDouble();
    return nullptr;
}
}  // namespace

void writeModelGlb(const ccc::core::Model& m, const std::string& path) {
    tinygltf::Model gltf;
    tinygltf::Asset asset;
    asset.version   = "2.0";
    asset.generator = "Circuit Capacity Calculator";

    // Embed the editable model as a structured JSON object under extras.
    nlohmann::json modelJson = modelToJson(m);
    tinygltf::Value::Object extras;
    extras.emplace(kExtrasKey, jsonToTinyValue(modelJson));
    asset.extras = tinygltf::Value(extras);

    gltf.asset = asset;

    // A single empty scene keeps the file valid for any glTF viewer.
    tinygltf::Scene scene;
    scene.name = "circuit_capacity_calculator";
    gltf.scenes.push_back(scene);
    gltf.defaultScene = 0;

    tinygltf::TinyGLTF writer;
    const bool ok = writer.WriteGltfSceneToFile(
        &gltf, path,
        /*embedImages=*/true,
        /*embedBuffers=*/true,
        /*prettyPrint=*/false,
        /*writeBinary=*/true);
    if (!ok) throw std::runtime_error("failed to write GLB: " + path);
}

ccc::core::Model readModelGlb(const std::string& path) {
    tinygltf::Model gltf;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    const bool ok = loader.LoadBinaryFromFile(&gltf, &err, &warn, path);
    if (!ok) throw std::runtime_error("failed to read GLB " + path + ": " + err);

    if (!gltf.asset.extras.IsObject() || !gltf.asset.extras.Has(kExtrasKey)) {
        throw std::runtime_error(
            "This GLB has no CCC editor metadata (asset.extras.sensorEditor "
            "is missing). Likely it was exported from KiCad or another tool. "
            "Direct import from KiCad GLB is not supported yet -- please use "
            "a .glb saved by Circuit Capacity Calculator, or use File > New "
            "and recreate the layout. File: " + path);
    }
    const auto& sensor = gltf.asset.extras.Get(kExtrasKey);
    nlohmann::json modelJson = tinyValueToJson(sensor);
    return modelFromJson(modelJson);
}

}  // namespace ccc::io
