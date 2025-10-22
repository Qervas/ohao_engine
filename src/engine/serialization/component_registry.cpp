#include "component_registry.hpp"

// Central registration site for all core components
#include "engine/actor/actor.hpp"
#include "engine/component/transform_component.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/material_component.hpp"
#include "renderer/components/light_component.hpp"
#include "physics/components/physics_component.hpp"
#include <nlohmann/json.hpp>

namespace ohao {

ComponentRegistry& ComponentRegistry::get(){ static ComponentRegistry inst; return inst; }

void ComponentRegistry::registerComponent(const std::string& typeName, ComponentEntry entry){
    entries[typeName] = std::move(entry);
}

const ComponentEntry* ComponentRegistry::find(const std::string& typeName) const{
    auto it = entries.find(typeName);
    if(it==entries.end()) return nullptr; return &it->second;
}

// Static registration helpers
namespace {
using Json = nlohmann::json_abi_v3_11_3::json;

Component* CreateTransform(ohao::Actor* a){ auto c=a->getComponent<ohao::TransformComponent>(); if(!c) c=a->addComponent<ohao::TransformComponent>(); return c.get(); }
Json SerializeTransform(const ohao::Component& c){ const auto& tc=static_cast<const ohao::TransformComponent&>(c); Json j; auto p=tc.getPosition(); j["position"]={p.x,p.y,p.z}; auto r=tc.getRotation(); j["rotation"]={r.x,r.y,r.z,r.w}; auto s=tc.getScale(); j["scale"]={s.x,s.y,s.z}; return j; }
void DeserializeTransform(ohao::Component& c, const Json& j){
    auto& tc = static_cast<ohao::TransformComponent&>(c);
    if(j.contains("position") && j["position"].is_array() && j["position"].size()==3){
        tc.setPosition(glm::vec3(j["position"][0].get<float>(), j["position"][1].get<float>(), j["position"][2].get<float>()));
    }
    if(j.contains("rotation") && j["rotation"].is_array() && j["rotation"].size()==4){
        tc.setRotation(glm::quat(j["rotation"][3].get<float>(), j["rotation"][0].get<float>(), j["rotation"][1].get<float>(), j["rotation"][2].get<float>()));
    }
    if(j.contains("scale") && j["scale"].is_array() && j["scale"].size()==3){
        tc.setScale(glm::vec3(j["scale"][0].get<float>(), j["scale"][1].get<float>(), j["scale"][2].get<float>()));
    }
}

Component* CreateMesh(ohao::Actor* a){ auto c=a->getComponent<ohao::MeshComponent>(); if(!c) c=a->addComponent<ohao::MeshComponent>(); return c.get(); }
Json SerializeMesh(const ohao::Component& c){ const auto& mc=static_cast<const ohao::MeshComponent&>(c); Json j; j["enabled"]=mc.isEnabled(); return j; }
void DeserializeMesh(ohao::Component& c, const Json& j){ auto& mc=static_cast<ohao::MeshComponent&>(c); if(j.contains("enabled")) mc.setEnabled(j["enabled"].get<bool>()); }

Component* CreateMaterial(ohao::Actor* a){ auto c=a->getComponent<ohao::MaterialComponent>(); if(!c) c=a->addComponent<ohao::MaterialComponent>(); return c.get(); }
Json SerializeMaterial(const ohao::Component& c){ const auto& mc=static_cast<const ohao::MaterialComponent&>(c); Json j; auto m=mc.getMaterial(); auto col=m.baseColor; j["baseColor"]={col.x,col.y,col.z}; j["metallic"]=m.metallic; j["roughness"]=m.roughness; j["ao"]=m.ao; return j; }
void DeserializeMaterial(ohao::Component& c, const Json& j){
    auto& mc = static_cast<ohao::MaterialComponent&>(c);
    auto m = mc.getMaterial();
    if(j.contains("baseColor") && j["baseColor"].is_array() && j["baseColor"].size()==3){
        m.baseColor = glm::vec3(j["baseColor"][0].get<float>(), j["baseColor"][1].get<float>(), j["baseColor"][2].get<float>());
    }
    if(j.contains("metallic")) m.metallic = j["metallic"].get<float>();
    if(j.contains("roughness")) m.roughness = j["roughness"].get<float>();
    if(j.contains("ao")) m.ao = j["ao"].get<float>();
    mc.setMaterial(m);
}

Component* CreateLight(ohao::Actor* a){ auto c=a->getComponent<ohao::LightComponent>(); if(!c) c=a->addComponent<ohao::LightComponent>(); return c.get(); }
Json SerializeLight(const ohao::Component& c){ const auto& lc=static_cast<const ohao::LightComponent&>(c); Json j; j["type"]=static_cast<int>(lc.getLightType()); auto col=lc.getColor(); j["color"]={col.x,col.y,col.z}; j["intensity"]=lc.getIntensity(); j["range"]=lc.getRange(); j["innerCone"]=lc.getInnerConeAngle(); j["outerCone"]=lc.getOuterConeAngle(); auto dir=lc.getDirection(); j["direction"]={dir.x,dir.y,dir.z}; return j; }
void DeserializeLight(ohao::Component& c, const Json& j){
    auto& lc = static_cast<ohao::LightComponent&>(c);
    if(j.contains("type")) lc.setLightType(static_cast<ohao::LightType>(j["type"].get<int>()));
    if(j.contains("color") && j["color"].is_array() && j["color"].size()==3){ lc.setColor(glm::vec3(j["color"][0].get<float>(), j["color"][1].get<float>(), j["color"][2].get<float>())); }
    if(j.contains("intensity")) lc.setIntensity(j["intensity"].get<float>());
    if(j.contains("range")) lc.setRange(j["range"].get<float>());
    if(j.contains("innerCone")) lc.setInnerConeAngle(j["innerCone"].get<float>());
    if(j.contains("outerCone")) lc.setOuterConeAngle(j["outerCone"].get<float>());
    if(j.contains("direction") && j["direction"].is_array() && j["direction"].size()==3){ lc.setDirection(glm::vec3(j["direction"][0].get<float>(), j["direction"][1].get<float>(), j["direction"][2].get<float>())); }
}

Component* CreatePhysics(ohao::Actor* a){ auto c=a->getComponent<ohao::PhysicsComponent>(); if(!c) c=a->addComponent<ohao::PhysicsComponent>(); return c.get(); }
Json SerializePhysics(const ohao::Component& c){ const auto& pc=static_cast<const ohao::PhysicsComponent&>(c); Json j; j["enabled"]=pc.isEnabled(); return j; }
void DeserializePhysics(ohao::Component& c, const Json& j){ auto& pc=static_cast<ohao::PhysicsComponent&>(c); if(j.contains("enabled")) pc.setEnabled(j["enabled"].get<bool>()); }

struct AutoRegisterAll {
    AutoRegisterAll(){
        auto& R = ComponentRegistry::get();
        R.registerComponent("TransformComponent", {CreateTransform, SerializeTransform, DeserializeTransform});
        R.registerComponent("MeshComponent", {CreateMesh, SerializeMesh, DeserializeMesh});
        R.registerComponent("MaterialComponent", {CreateMaterial, SerializeMaterial, DeserializeMaterial});
        R.registerComponent("LightComponent", {CreateLight, SerializeLight, DeserializeLight});
        R.registerComponent("PhysicsComponent", {CreatePhysics, SerializePhysics, DeserializePhysics});
    }
};
static AutoRegisterAll s_autoRegisterAll;
} // namespace

} // namespace ohao


