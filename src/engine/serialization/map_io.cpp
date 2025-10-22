#include "map_io.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "engine/component/transform_component.hpp"
#include "renderer/components/mesh_component.hpp"
#include "physics/components/physics_component.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace ohao {

static std::string toLowerCopy(const std::string& s){
    std::string r = s; for(char& c : r){ c = (char)std::tolower((unsigned char)c);} return r;
}

MapIO::MapIO(Scene* scene) : scene(scene) {}

const char* MapIO::kMagic(){ return "OHAO_MAP"; }
int MapIO::kVersion(){ return 1; }

bool MapIO::save(const std::string& filePath){
    if(!scene){ return false; }
    nlohmann::json j;
    j["magic"] = kMagic();
    j["version"] = kVersion();
    j["name"] = scene->getName();

    // Actors
    nlohmann::json jActors = nlohmann::json::array();
    for(const auto& [id, actor] : scene->getAllActors()){
        if(actor){ jActors.push_back(serializeActor(actor.get())); }
    }
    j["actors"] = std::move(jActors);

    std::filesystem::path out(filePath);
    if(out.extension().empty()) out += ".omap";
    std::filesystem::create_directories(out.parent_path());
    std::ofstream f(out);
    if(!f.is_open()){ return false; }
    f << j.dump(2);
    return true;
}

bool MapIO::load(const std::string& filePath){
    if(!scene){ return false; }
    std::filesystem::path p(filePath);
    if(!std::filesystem::exists(p)){ return false; }
    std::ifstream f(p);
    if(!f.is_open()){ return false; }
    nlohmann::json j; f >> j;
    if(!j.is_object()){ return false; }
    if(!j.contains("magic") || j["magic"] != kMagic()){ return false; }

    scene->removeAllActors();
    if(j.contains("name")) scene->setName(j["name"].get<std::string>());

    if(j.contains("actors") && j["actors"].is_array()){
        for(const auto& ja : j["actors"]) { if(!deserializeActor(ja)) { std::cerr << "Actor load failed" << std::endl; } }
    }

    scene->updateSceneBuffers();
    return true;
}

nlohmann::json MapIO::serializeActor(const Actor* actor) const{
    nlohmann::json a;
    a["id"] = actor->getID();
    a["name"] = actor->getName();
    a["active"] = actor->isActive();
    a["parentId"] = actor->getParent() ? actor->getParent()->getID() : 0ULL;

    // Transform
    if(auto* tc = actor->getTransform()) a["transform"] = serializeTransform(tc);

    // Components
    nlohmann::json comps = nlohmann::json::array();
    if(auto mc = actor->getComponent<MeshComponent>()){ nlohmann::json c; c["type"] = "MeshComponent"; c["data"] = serializeMesh(mc.get()); comps.push_back(c);} 
    if(auto pc = actor->getComponent<PhysicsComponent>()){ nlohmann::json c; c["type"] = "PhysicsComponent"; c["data"] = serializePhysics(pc.get()); comps.push_back(c);} 
    a["components"] = std::move(comps);
    return a;
}

bool MapIO::deserializeActor(const nlohmann::json& j){
    std::string name = j.value("name", std::string("Actor"));
    auto actor = scene->createActor(name);
    if(!actor) return false;

    if(j.contains("active")) actor->setActive(j["active"].get<bool>());

    if(j.contains("transform")) if(auto* tc = actor->getTransform()) deserializeTransform(tc, j["transform"]);

    if(j.contains("components") && j["components"].is_array()){
        for(const auto& c : j["components"]) {
            std::string type = c.value("type", std::string());
            if(type == "MeshComponent"){
                auto mc = actor->getComponent<MeshComponent>(); if(!mc) mc = actor->addComponent<MeshComponent>();
                deserializeMesh(mc.get(), c["data"]);
            } else if(type == "PhysicsComponent"){
                auto pc = actor->getComponent<PhysicsComponent>(); if(!pc) pc = actor->addComponent<PhysicsComponent>();
                deserializePhysics(pc.get(), c["data"]);
            }
        }
    }

    return true;
}

nlohmann::json MapIO::serializeTransform(const TransformComponent* tc){
    nlohmann::json t;
    auto p = tc->getPosition(); t["position"] = {p.x,p.y,p.z};
    auto r = tc->getRotation(); t["rotation"] = {r.x,r.y,r.z};
    auto s = tc->getScale();    t["scale"]    = {s.x,s.y,s.z};
    return t;
}

void MapIO::deserializeTransform(TransformComponent* tc, const nlohmann::json& j){
    if(j.contains("position") && j["position"].is_array() && j["position"].size()==3){ tc->setPosition({j["position"][0],j["position"][1],j["position"][2]}); }
    if(j.contains("rotation") && j["rotation"].is_array() && j["rotation"].size()==3){ tc->setRotationEuler({j["rotation"][0],j["rotation"][1],j["rotation"][2]}); }
    if(j.contains("scale") && j["scale"].is_array() && j["scale"].size()==3){ tc->setScale({j["scale"][0],j["scale"][1],j["scale"][2]}); }
}

nlohmann::json MapIO::serializeMesh(const MeshComponent* mc){
    nlohmann::json m; m["enabled"] = mc->isEnabled(); return m;
}
void MapIO::deserializeMesh(MeshComponent* mc, const nlohmann::json& j){ if(j.contains("enabled")) mc->setEnabled(j["enabled"].get<bool>()); }

nlohmann::json MapIO::serializePhysics(const PhysicsComponent* pc){ nlohmann::json p; p["enabled"]=pc->isEnabled(); return p; }
void MapIO::deserializePhysics(PhysicsComponent* pc, const nlohmann::json& j){ if(j.contains("enabled")) pc->setEnabled(j["enabled"].get<bool>()); }

} // namespace ohao


