/*
Copyright 2017 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS-IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "lullaby/base/entity_factory.h"

#include <memory>
#include <utility>

#include "lullaby/base/asset_loader.h"
#include "lullaby/util/file.h"
#include "lullaby/util/logging.h"

namespace lull {

EntityFactory::EntityFactory(Registry* registry)
    : registry_(registry), entity_generator_(0) {}

void EntityFactory::Initialize() {
  if (systems_.empty()) {
    LOG(DFATAL) << "Call Initialize after creating Systems.";
  }

  InitializeSystems();
  registry_->CheckAllDependencies();
}

void EntityFactory::RegisterDef(TypeId system_type, HashValue def_type) {
  type_map_[def_type] = system_type;
}

void EntityFactory::InitializeSystems() {
  for (auto& iter : systems_) {
    iter.second->Initialize();
  }
}

void EntityFactory::AddSystem(TypeId system_type, System* system) {
  if (!system) {
    return;
  }
  auto iter = systems_.find(system_type);
  if (iter == systems_.end()) {
    systems_.emplace(system_type, system);
  }
}

void EntityFactory::CreateTypeList(const char* const* names) {
  types_.reserve(type_map_.size());
  while (*names) {
    const System::DefType type = Hash(*names);
    types_.emplace_back(type);
    ++names;
  }
}

size_t EntityFactory::PerformReverseTypeLookup(HashValue name) const {
  for (size_t i = 0; i < types_.size(); ++i) {
    if (types_[i] == name) {
      return i;
    }
  }
  return 0;
}

Entity EntityFactory::Create() {
  Lock lock(mutex_);
  const Entity entity = ++entity_generator_;
  CHECK_NE(entity, kNullEntity) << "Overflow on Entity generation.";
  return entity;
}

Entity EntityFactory::Create(const std::string& name) {
  auto asset = GetBlueprintAsset(name);
  if (asset == nullptr) {
    LOG(ERROR) << "No such blueprint: " << name;
    return kNullEntity;
  }
  return CreateFromBlueprint(asset->GetData(), name);
}

Entity EntityFactory::Create(Blueprint* blueprint) {
  Entity entity = Create();
  CreateImpl(entity, blueprint);
  entity_to_blueprint_map_[entity] = "";
  return entity;
}

Entity EntityFactory::Create(BlueprintTree* blueprint) {
  Entity entity = Create();
  return Create(entity, blueprint);
}

Entity EntityFactory::Create(Entity entity, const std::string& name) {
  auto asset = GetBlueprintAsset(name);
  if (asset) {
    CreateImpl(entity, name, asset->GetData());
    return entity;
  } else {
    LOG(ERROR) << "No such blueprint: " << name;
    return kNullEntity;
  }
}

Entity EntityFactory::Create(Entity entity, BlueprintTree* blueprint) {
  CreateImpl(entity, blueprint);
  entity_to_blueprint_map_[entity] = "";
  return entity;
}

Span<uint8_t> EntityFactory::Finalize(Blueprint* blueprint) {
  Span<uint8_t> data;
  if (finalizer_) {
    data = blueprint->Finalize(finalizer_);
  }
  return data;
}

Entity EntityFactory::CreateFromBlueprint(const void* data,
                                          const std::string& name) {
  const Entity entity = Create();
  const bool success = CreateImpl(entity, name, data);
  return success ? entity : kNullEntity;
}

bool EntityFactory::CreateImpl(Entity entity, const std::string& name,
                               const void* data) {
  if (entity == kNullEntity) {
    LOG(DFATAL) << "Cannot create null entity: " << name;
    return false;
  }
  if (data == nullptr) {
    LOG(DFATAL) << "Cannot create entity from null data: " << name;
    return false;
  }

  BlueprintTree blueprint;
  if (loader_) {
    blueprint = loader_(data);
  } else {
    LOG(ERROR) << "Unable to convert raw data to blueprint.  Call ::Initialize "
                  "with arguments to specify how to perform this conversion. "
                  "Using empty blueprint instead";
  }

  entity_to_blueprint_map_[entity] = name;

  const bool result = CreateImpl(entity, &blueprint);
  return result;
}

bool EntityFactory::CreateImpl(Entity entity, BlueprintTree* blueprint) {
  return CreateImpl(entity, blueprint, blueprint->Children());
}

bool EntityFactory::CreateImpl(Entity entity, Blueprint* blueprint,
                               std::list<BlueprintTree>* children) {
  if (entity == kNullEntity) {
    LOG(DFATAL) << "Cannot create null entity";
    return false;
  }
  if (blueprint == nullptr) {
    LOG(DFATAL) << "Cannot create entity from null blueprint";
    return false;
  }

  blueprint->ForEachComponent([this, entity](const Blueprint& blueprint) {
    System* system = GetSystem(blueprint.GetLegacyDefType());
    if (system) {
      system->CreateComponent(entity, blueprint);
    } else {
      LOG(DFATAL) << "Unknown system when creating entity " << entity
                  << " from blueprint: " << entity_to_blueprint_map_[entity];
    }
  });
  // Construct children after parent creation, but before parent post-creation.
  // This allows the parent to discover/manipulate children during
  // PostCreateComponent.
  if (children) {
    for (auto& child_blueprint : *children) {
      create_child_fn_(entity, &child_blueprint);
    }
  }
  // Now invoke PostCreateComponent on parent.
  blueprint->ForEachComponent([this, entity](const Blueprint& blueprint) {
    System* system = GetSystem(blueprint.GetLegacyDefType());
    if (system) {
      system->PostCreateComponent(entity, blueprint);
    }
  });

  return true;
}

std::shared_ptr<SimpleAsset> EntityFactory::GetBlueprintAsset(
    const std::string& name) {
  std::string filename = name;
  if (!EndsWith(filename, ".json")) {
    filename += ".bin";
  }

  const HashValue key = Hash(filename.c_str());

  auto asset = blueprints_.Create(key, [&]() {
    AssetLoader* asset_loader = registry_->Get<AssetLoader>();
    return asset_loader->LoadNow<SimpleAsset>(filename);
  });

  if (asset->GetSize() == 0) {
    LOG(ERROR) << "Could not load entity blueprint: " << name;
    return nullptr;
  }
  return asset;
}

void EntityFactory::Destroy(Entity entity) {
  if (entity == kNullEntity) {
    return;
  }

  entity_to_blueprint_map_.erase(entity);
  for (auto& iter : systems_) {
    iter.second->Destroy(entity);
  }
}

void EntityFactory::QueueForDestruction(Entity entity) {
  if (entity == kNullEntity) {
    return;
  }
  Lock lock(mutex_);
  pending_destroy_.push(entity);
}

void EntityFactory::DestroyQueuedEntities() {
  // Swap the queue of entities to be destroyed so that it can safely be
  // modified in QueueForDestruction from another thread.
  std::queue<Entity> pending_destroy;
  {
    Lock lock(mutex_);
    using std::swap;
    std::swap(pending_destroy_, pending_destroy);
  }

  while (!pending_destroy.empty()) {
    Destroy(pending_destroy.front());
    pending_destroy.pop();
  }
}

System* EntityFactory::GetSystem(const System::DefType def_type) {
  const TypeId type_id = type_map_[def_type];
  return systems_[type_id];
}

const EntityFactory::BlueprintMap& EntityFactory::GetEntityToBlueprintMap()
    const {
  return entity_to_blueprint_map_;
}

}  // namespace lull
