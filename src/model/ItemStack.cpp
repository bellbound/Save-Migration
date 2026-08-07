#include "model/ItemStack.h"

namespace SaveMigration::Model {

namespace {

/// Only write a key when it has a value: an optional that is absent must not
/// round-trip as 0.0, because 0.0 charge and "no recorded charge" mean different
/// things at apply time.
template <class T>
void PutOptional(nlohmann::json& json, const char* key, const std::optional<T>& value) {
    if (value.has_value()) {
        json[key] = *value;
    }
}

template <class T>
std::optional<T> GetOptional(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || it->is_null()) {
        return std::nullopt;
    }
    return it->get<T>();
}

}  // namespace

bool ItemExtra::IsEmpty() const {
    return enchantmentKey.empty() && !charge.has_value() && !health.has_value() &&
           !soulLevel.has_value() && poisonKey.empty() && !poisonCount.has_value() &&
           displayName.empty();
}

nlohmann::json ItemExtra::ToJson() const {
    auto json = nlohmann::json::object();
    if (!enchantmentKey.empty()) {
        json["enchantment"] = enchantmentKey;
    }
    PutOptional(json, "charge", charge);
    PutOptional(json, "health", health);
    PutOptional(json, "soulLevel", soulLevel);
    if (!poisonKey.empty()) {
        json["poison"] = poisonKey;
    }
    PutOptional(json, "poisonCount", poisonCount);
    if (!displayName.empty()) {
        json["displayName"] = displayName;
    }
    return json;
}

ItemExtra ItemExtra::FromJson(const nlohmann::json& json) {
    ItemExtra extra;
    if (!json.is_object()) {
        return extra;
    }
    extra.enchantmentKey = json.value("enchantment", "");
    extra.charge = GetOptional<float>(json, "charge");
    extra.health = GetOptional<float>(json, "health");
    extra.soulLevel = GetOptional<int>(json, "soulLevel");
    extra.poisonKey = json.value("poison", "");
    extra.poisonCount = GetOptional<int>(json, "poisonCount");
    extra.displayName = json.value("displayName", "");
    return extra;
}

bool ItemReconstruct::IsEmpty() const {
    return baseKey.empty() && !health.has_value() && displayName.empty() &&
           playerEnchantment.is_null();
}

nlohmann::json ItemReconstruct::ToJson() const {
    auto json = nlohmann::json::object();
    if (!baseKey.empty()) {
        json["base"] = baseKey;
    }
    PutOptional(json, "health", health);
    if (!displayName.empty()) {
        json["displayName"] = displayName;
    }
    if (!playerEnchantment.is_null()) {
        json["playerEnchantment"] = playerEnchantment;
    }
    return json;
}

ItemReconstruct ItemReconstruct::FromJson(const nlohmann::json& json) {
    ItemReconstruct reconstruct;
    if (!json.is_object()) {
        return reconstruct;
    }
    reconstruct.baseKey = json.value("base", "");
    reconstruct.health = GetOptional<float>(json, "health");
    reconstruct.displayName = json.value("displayName", "");
    if (const auto it = json.find("playerEnchantment"); it != json.end()) {
        reconstruct.playerEnchantment = *it;
    }
    return reconstruct;
}

nlohmann::json ItemStack::ToJson() const {
    nlohmann::json json{{"form", formKey}, {"count", count}};
    if (!equipSlots.empty()) {
        json["equipSlots"] = equipSlots;
    }
    if (!extra.IsEmpty()) {
        json["extra"] = extra.ToJson();
    }
    if (!displayName.empty()) {
        json["displayName"] = displayName;
    }
    return json;
}

ItemStack ItemStack::FromJson(const nlohmann::json& json) {
    ItemStack stack;
    if (!json.is_object()) {
        return stack;
    }
    stack.formKey = json.value("form", "");
    stack.count = json.value("count", 0);
    if (const auto slots = json.find("equipSlots"); slots != json.end() && slots->is_array()) {
        for (const auto& slot : *slots) {
            if (slot.is_string()) {
                stack.equipSlots.push_back(slot.get<std::string>());
            }
        }
    }
    if (const auto extra = json.find("extra"); extra != json.end()) {
        stack.extra = ItemExtra::FromJson(*extra);
    }
    stack.displayName = json.value("displayName", "");
    return stack;
}

nlohmann::json UnmigratableItem::ToJson() const {
    nlohmann::json json{{"displayName", displayName}, {"reasonCode", reasonCode}, {"count", count}};
    if (!reconstruct.IsEmpty()) {
        json["reconstruct"] = reconstruct.ToJson();
    }
    return json;
}

UnmigratableItem UnmigratableItem::FromJson(const nlohmann::json& json) {
    UnmigratableItem item;
    if (!json.is_object()) {
        return item;
    }
    item.displayName = json.value("displayName", "");
    item.reasonCode = json.value("reasonCode", "");
    item.count = json.value("count", 1);
    if (const auto it = json.find("reconstruct"); it != json.end()) {
        item.reconstruct = ItemReconstruct::FromJson(*it);
    }
    return item;
}

}  // namespace SaveMigration::Model
