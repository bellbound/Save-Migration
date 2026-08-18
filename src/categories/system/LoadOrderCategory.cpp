#include "categories/system/LoadOrderCategory.h"

#include <format>

#include "core/VRLayoutProbe.h"
#include "model/WellKnownForms.h"
#include "store/LoadOrderFingerprint.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "system.load_order";
}

const Core::CategoryDescriptor& LoadOrderCategory::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Load order",
        .phase = Core::Phase::kFingerprint,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void LoadOrderCategory::Collect(Core::CollectContext& ctx) {
    auto& fingerprint = Store::LoadOrderFingerprint::Get();
    if (!fingerprint.IsCaptured()) {
        fingerprint.CaptureCurrent();
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    payload["pluginCount"] = fingerprint.Current().size();
    payload["runtimeIsVR"] = REL::Module::IsVR();

    // The document already carries the full table in `loadOrder`; this category's
    // payload is the human-facing summary plus the probe verdict, so a report
    // reader does not have to open loadorder.json.
    auto unresolved = nlohmann::json::array();
    for (const auto& name : Model::WellKnownForms::Get().Unresolved()) {
        unresolved.push_back(name);
    }
    // Written only when something failed to resolve. On a healthy install this
    // was an empty array in every export, which is the same information as the
    // field not being there.
    if (!unresolved.empty()) {
        payload["unresolvedWellKnownForms"] = std::move(unresolved);
    }
    payload["vrLayoutTrusted"] = Core::VRLayoutProbe::Get().IsLayoutTrusted();
    payload["vrLayoutDetail"] = Core::VRLayoutProbe::Get().Detail();

    ctx.report.Succeeded(Report::SystemSubject("Load order"), "load_order", "",
                         std::format("{} plugins", fingerprint.Current().size()));

    if (!Core::VRLayoutProbe::Get().IsLayoutTrusted()) {
        ctx.report.Warn(Report::ReasonCode::kRuntimeLayoutSuspect,
                        std::format("VR layout probe failed ({}). Offset-dependent readers were "
                                    "disabled, so parts of this snapshot are deliberately thinner "
                                    "than usual.",
                                    Core::VRLayoutProbe::Get().Detail()));
    }
    for (const auto& name : Model::WellKnownForms::Get().Unresolved()) {
        ctx.report.Warn(Report::ReasonCode::kFormLookupFailed,
                        std::format("well-known form '{}' did not resolve; the roster source that "
                                    "depends on it contributed nothing",
                                    name));
    }
}

void LoadOrderCategory::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::SystemSubject("Load order");

    // The schema gate proper. `SnapshotReader` already refuses a newer manifest,
    // so reaching here means the structure is readable; what remains is telling
    // the user what changed underneath them.
    const bool snapshotWasVR = payload.value("runtimeIsVR", REL::Module::IsVR());
    if (snapshotWasVR != REL::Module::IsVR()) {
        // ESL form IDs are encoded differently between VR and SE/AE, so a
        // cross-runtime restore mis-resolves every light-plugin key.
        ctx.report.Error(Report::ReasonCode::kSchemaVersionUnsupported,
                         std::format("the snapshot was taken on {} but this is {}. Light-plugin form "
                                     "IDs are encoded differently between the two, so keys from ESL "
                                     "plugins will not resolve.",
                                     snapshotWasVR ? "VR" : "SE/AE",
                                     REL::Module::IsVR() ? "VR" : "SE/AE"),
                         false);
    }

    if (payload.value("vrLayoutTrusted", true) == false) {
        ctx.report.Warn(Report::ReasonCode::kRuntimeLayoutSuspect,
                        "the snapshot was taken with a suspect VR memory layout, so some recorded "
                        "values may be incomplete");
    }

    const auto pluginCount = payload.value("pluginCount", size_t{0});
    ctx.report.Succeeded(subject, "load_order_gate", "",
                         std::format("snapshot had {} plugins, {} missing here, {} new", pluginCount,
                                     ctx.missingPlugins.size(), 0));

    // One line for the whole set, not one per plugin. This used to print a warning
    // for every absent plugin - 820 of them on a modlist that has moved on - and a
    // missing plugin is a fact about the *load order*, which the LOAD ORDER section
    // above already lists in full. The per-item detail stays in each category's own
    // failures, where it is attached to something the player recognises.
    if (!ctx.missingPlugins.empty()) {
        ctx.report.Warn(
            Report::ReasonCode::kSourcePluginMissing,
            std::format("{} plugin(s) in the snapshot are not in this load order, so every key from "
                        "them was pre-failed without a lookup being attempted: {}",
                        ctx.missingPlugins.size(), Util::JoinCapped(ctx.missingPlugins, 8)));
    }
}

}  // namespace SaveMigration::Categories
