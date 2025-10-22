#include "map_io.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "engine/component/transform_component.hpp"
#include "renderer/components/mesh_component.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/serialization/component_registry.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace ohao {

static std::string toLowerCopy(const std::string& s){ std::string r = s; for(char& c : r){ c = (char)std::tolower((unsigned char)c);} return r; }

MapIO::MapIO(Scene* scene) : scene(scene) {}

const char* MapIO::kMagic(){ return "OHAO_MAP"; }
int MapIO::kVersion(){ return 2; }

bool MapIO::save(const std::string& filePath){
    if(!scene){ return false; }
    nlohmann::json j;
    j["magic"] = kMagic();
    j["version"] = kVersion();
    j["name"] = scene->getName();
    j["mapGuid"] = ""; // TODO: add Scene GUID if needed

    nlohmann::json jActors = nlohmann::json::array();
    for(const auto& [id, actorPtr] : scene->getAllActors()){
        const Actor* actor = actorPtr.get();
        if(!actor) continue;
        jActors.push_back(serializeActor(actor));
    }
    j["actors"] = std::move(jActors);

    std::filesystem::path out(filePath); if(out.extension().empty()) out += ".omap"; std::filesystem::create_directories(out.parent_path());
    std::ofstream f(out); if(!f.is_open()){ return false; } f << j.dump(2); return true;
}

bool MapIO::load(const std::string& filePath){
    if(!scene){ return false; }
    std::filesystem::path p(filePath); if(!std::filesystem::exists(p)){ return false; }
    std::ifstream f(p); if(!f.is_open()){ return false; }
    nlohmann::json j; f >> j; if(!j.is_object()){ return false; }
    if(!j.contains("magic") || j["magic"] != kMagic()){ return false; }

    scene->removeAllActors();
    if(j.contains("name")) scene->setName(j["name"].get<std::string>());

    std::unordered_map<std::string, Actor*> guidToActor;
    if(j.contains("actors") && j["actors"].is_array()){
        for(const auto& ja : j["actors"]) { deserializeActorPass1(ja, guidToActor); }
        for(const auto& ja : j["actors"]) { deserializeActorPass2(ja, guidToActor); }
    }

    scene->updateSceneBuffers();
    return true;
}

nlohmann::json MapIO::serializeActor(const Actor* actor) const{
    nlohmann::json a;
    a["guid"] = actor->getGuid();
    a["name"] = actor->getName();
    a["active"] = actor->isActive();
    a["parentGuid"] = actor->getParent() ? actor->getParent()->getGuid() : std::string();

    if(auto* tc = actor->getTransform()) a["transform"] = serializeTransform(tc);

    nlohmann::json comps = nlohmann::json::array();
    for(const auto& comp : actor->getAllComponents()){
        if(!comp) continue;
        nlohmann::json rc;
        rc["type"] = comp->getTypeName();
        rc["guid"] = comp->getGuid();
        const ComponentEntry* entry = ComponentRegistry::get().find(rc["type"].get<std::string>());
        if(entry && entry->serialize){ rc["data"] = entry->serialize(*comp); }
        comps.push_back(std::move(rc));
    }
    a["components"] = std::move(comps);
    return a;
}

bool MapIO::deserializeActorPass1(const nlohmann::json& j, std::unordered_map<std::string, Actor*>& guidToActor){
    std::string name = j.value("name", std::string("Actor"));
    auto actorPtr = scene->createActor(name);
    if(!actorPtr) return false;
    Actor* actor = actorPtr.get();
    if(j.contains("guid")) actor->setGuid(j["guid"].get<std::string>());
    if(j.contains("active")) actor->setActive(j["active"].get<bool>());
    if(j.contains("transform")) if(auto* tc = actor->getTransform()) deserializeTransform(tc, j["transform"]);

    if(j.contains("components") && j["components"].is_array()){
        for(const auto& c : j["components"]) {
            std::string type = c.value("type", std::string());
            const ComponentEntry* entry = ComponentRegistry::get().find(type);
            if(!entry || !entry->create) continue;
            Component* comp = entry->create(actor);
            if(c.contains("guid")) comp->setGuid(c["guid"].get<std::string>());
            if(entry->deserialize && c.contains("data")) entry->deserialize(*comp, c["data"]);
        }
    }

    guidToActor[actor->getGuid()] = actor;
    return true;
}

bool MapIO::deserializeActorPass2(const nlohmann::json& j, const std::unordered_map<std::string, Actor*>& guidToActor){
    if(!j.contains("guid")) return true;
    std::string guid = j["guid"].get<std::string>();
    auto it = guidToActor.find(guid); if(it == guidToActor.end()) return false;
    Actor* actor = it->second;
    std::string parentGuid = j.value("parentGuid", std::string());
    if(!parentGuid.empty()){
        auto pit = guidToActor.find(parentGuid); if(pit != guidToActor.end()){
            actor->setParent(pit->second);
        }
    }
    return true;
}

nlohmann::json MapIO::serializeTransform(const TransformComponent* tc){
    nlohmann::json t;
    auto p = tc->getPosition(); t["position"] = {p.x,p.y,p.z};
    auto r = tc->getRotation(); t["rotation"] = {r.x,r.y,r.z,r.w};
    auto s = tc->getScale();    t["scale"]    = {s.x,s.y,s.z};
    return t;
}

void MapIO::deserializeTransform(TransformComponent* tc, const nlohmann::json& j){
    if(j.contains("position") && j["position"].is_array() && j["position"].size()==3){ tc->setPosition({j["position"][0],j["position"][1],j["position"][2]}); }
    if(j.contains("rotation") && j["rotation"].is_array() && j["rotation"].size()==4){ tc->setRotation({j["rotation"][3],j["rotation"][0],j["rotation"][1],j["rotation"][2]}); }
    if(j.contains("scale") && j["scale"].is_array() && j["scale"].size()==3){ tc->setScale({j["scale"][0],j["scale"][1],j["scale"][2]}); }
}

nlohmann::json MapIO::serializeMesh(const MeshComponent* mc){ nlohmann::json m; m["enabled"] = mc->isEnabled(); /* TODO: add model/material paths */ return m; }
void MapIO::deserializeMesh(MeshComponent* mc, const nlohmann::json& j){ if(j.contains("enabled")) mc->setEnabled(j["enabled"].get<bool>()); }

nlohmann::json MapIO::serializePhysics(const PhysicsComponent* pc){ nlohmann::json p; p["enabled"]=pc->isEnabled(); return p; }
void MapIO::deserializePhysics(PhysicsComponent* pc, const nlohmann::json& j){ if(j.contains("enabled")) pc->setEnabled(j["enabled"].get<bool>()); }

} // namespace ohao


