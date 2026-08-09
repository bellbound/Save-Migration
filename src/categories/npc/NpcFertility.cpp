#include "categories/npc/NpcFertility.h"

#include <format>

#include "config/MigrationConfig.h"
#include "defer/PendingWorkQueue.h"
#include "model/FormRef.h"
#include "papyrus/ModProbe.h"
#include "papyrus/PapyrusInterface.h"
#include "papyrus/PapyrusVariableInterface.h"
#include "util/GameThread.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.fertility";
constexpr std::string_view kStorageScript = "_JSW_BB_Storage";

/// The per-actor parallel arrays, with their Papyrus element types so each write
/// can be type-asserted. `LastConception` is deliberately absent here and written
/// last - it is the pregnancy predicate.
struct ArraySpec {
    const char* property;
    const char* elementType;
};
constexpr ArraySpec kFloatArrays[] = {
    {"LastGameHours", "Float"},   {"LastInsemination", "Float"}, {"LastOvulation", "Float"},
    {"LastBirth", "Float"},       {"SpermCount", "Float"},       {"BabyAdded", "Float"},
    {"LocationLeftTime", "Float"},
};
constexpr ArraySpec kIntArrays[] = {
    {"LastGameHoursDelta", "Int"}, {"FatherRaceId", "Int"},
};
constexpr ArraySpec kStringArrays[] = {
    {"CurrentFather", "String"}, {"LastFather", "String"},
    {"LastMotherLocation", "String"},
};

/// Written last, on its own, for the reason in the class comment.
constexpr const char* kConceptionArray = "LastConception";

}  // namespace

const Core::CategoryDescriptor& NpcFertility::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Fertility Mode",
        // Before any cell attaches: _JSW_BB_Po3ActorDiscovery registers actors with
        // empty state on object-load, and landing first turns that into a no-op
        // re-find rather than a race we lose.
        .phase = Core::Phase::kIntegrationsState,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {.plugins = {},
                        .scriptNames = {std::string(kStorageScript)},
                        .dllNames = {}},
        .schemaVersion = 1,
    };
    return descriptor;
}

bool NpcFertility::ResolveHandles(Report::ReportSink& sink) {
    if (m_handles.valid) {
        return true;
    }
    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    m_handles.quest = vars->FindQuestByScriptName(std::string(kStorageScript));
    if (!m_handles.quest) {
        sink.SkipCategory(Report::ReasonCode::kModApiMissing,
                          "no quest carries _JSW_BB_Storage; Fertility Mode is not usable");
        return false;
    }

    // The version gate. An unrecognised or empty _updatedToVersion means the mod's
    // own migration has not run, and its arrays may not be in the shape we expect.
    const auto version =
        vars->GetString(m_handles.quest, std::string(kStorageScript), "_updatedToVersion");
    if (!version || version->empty()) {
        sink.SkipCategory(
            Report::ReasonCode::kModApiMissing,
            "_JSW_BB_Storage::_updatedToVersion is empty, meaning Fertility Mode has not run its "
            "own storage migration yet. Load the save once without Save Migration, let Fertility "
            "initialise, then retry - writing into un-migrated arrays would desync them.");
        return false;
    }
    spdlog::info("NpcFertility: storage version '{}'", *version);

    const auto faction = vars->GetVariable(m_handles.quest, std::string(kStorageScript),
                                          "ImmersiveEffectsFaction");
    if (faction.success) {
        if (const auto* formId = std::get_if<RE::FormID>(&faction.value)) {
            if (auto* form = RE::TESForm::LookupByID(*formId)) {
                m_handles.effectsFaction = form->As<RE::TESFaction>();
            }
        }
    }

    m_handles.valid = true;
    return true;
}

bool NpcFertility::VerifyLengthInvariant(Report::ReportSink& sink) {
    if (m_invariantChecked) {
        return m_invariantOk;
    }
    m_invariantChecked = true;

    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    const auto tracked =
        vars->GetArrayLength(m_handles.quest, std::string(kStorageScript), "TrackedActors");
    if (!tracked) {
        sink.FailCategory(Report::ReasonCode::kVmVariableNotFound,
                          "TrackedActors is not readable as an array");
        return false;
    }
    const uint32_t expected = *tracked + 1;

    const auto check = [&](const char* property) {
        const auto length =
            vars->GetArrayLength(m_handles.quest, std::string(kStorageScript), property);
        if (!length) {
            sink.Warn(Report::ReasonCode::kVmVariableNotFound,
                      std::format("_JSW_BB_Storage::{} is not readable as an array", property));
            return false;
        }
        if (*length != expected) {
            // The mod maintains every parallel array at TrackedActors.Length + 1 on
            // purpose. A short one means an existing desync, and writing into it
            // would corrupt a different actor's state.
            sink.Error(Report::ReasonCode::kVmVariableNotFound,
                       std::format("_JSW_BB_Storage::{} is {} long but TrackedActors + 1 is {}. "
                                   "Refusing to write into a short array - that would land on the "
                                   "wrong actor. Fertility Mode's own UpdateStorage() should fix "
                                   "this; check its MCM tracking list.",
                                   property, *length, expected));
            return false;
        }
        return true;
    };

    bool ok = true;
    for (const auto& spec : kFloatArrays) ok = check(spec.property) && ok;
    for (const auto& spec : kIntArrays) ok = check(spec.property) && ok;
    for (const auto& spec : kStringArrays) ok = check(spec.property) && ok;
    ok = check(kConceptionArray) && ok;

    m_invariantOk = ok;
    if (ok) {
        spdlog::info("NpcFertility: length invariant holds (TrackedActors + 1 == {})", expected);
    }
    return ok;
}

void NpcFertility::BeginCollect(Core::CollectContext& ctx) { ResolveHandles(ctx.report); }

void NpcFertility::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (!m_handles.valid || !subject.actor) {
        return;
    }

    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();

    // Find this actor's index in TrackedActors. Everything else is parallel to it.
    const auto trackedActors =
        vars->GetArrayVariable(m_handles.quest, std::string(kStorageScript), "TrackedActors");
    if (!trackedActors.success) {
        return;
    }
    const auto wantedId = subject.actor->GetFormID();
    std::optional<uint32_t> index;
    for (uint32_t i = 0; i < trackedActors.elements.size(); ++i) {
        if (const auto* formId = std::get_if<RE::FormID>(&trackedActors.elements[i])) {
            if (*formId == wantedId) {
                index = i;
                break;
            }
        }
    }
    if (!index) {
        return;  // not tracked: nothing to carry
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["sourceIndex"] = *index;

    const auto readFloat = [&](const char* property) -> std::optional<float> {
        const auto array =
            vars->GetArrayVariable(m_handles.quest, std::string(kStorageScript), property);
        if (!array.success || *index >= array.elements.size()) {
            return std::nullopt;
        }
        if (const auto* value = std::get_if<float>(&array.elements[*index])) {
            return *value;
        }
        return std::nullopt;
    };
    const auto readInt = [&](const char* property) -> std::optional<int32_t> {
        const auto array =
            vars->GetArrayVariable(m_handles.quest, std::string(kStorageScript), property);
        if (!array.success || *index >= array.elements.size()) {
            return std::nullopt;
        }
        if (const auto* value = std::get_if<int32_t>(&array.elements[*index])) {
            return *value;
        }
        return std::nullopt;
    };
    const auto readString = [&](const char* property) -> std::optional<std::string> {
        const auto array =
            vars->GetArrayVariable(m_handles.quest, std::string(kStorageScript), property);
        if (!array.success || *index >= array.elements.size()) {
            return std::nullopt;
        }
        if (const auto* value = std::get_if<std::string>(&array.elements[*index])) {
            return *value;
        }
        return std::nullopt;
    };

    auto values = nlohmann::json::object();
    for (const auto& spec : kFloatArrays) {
        if (const auto value = readFloat(spec.property)) {
            values[spec.property] = *value;
        }
    }
    if (const auto value = readFloat(kConceptionArray)) {
        values[kConceptionArray] = *value;
    }
    for (const auto& spec : kIntArrays) {
        if (const auto value = readInt(spec.property)) {
            values[spec.property] = *value;
        }
    }
    for (const auto& spec : kStringArrays) {
        if (const auto value = readString(spec.property)) {
            values[spec.property] = *value;
        }
    }
    // A derived, human-readable restatement of what `LastConception` already
    // says. Nothing reads these back - the restore writes the raw arrays - but
    // the raw arrays are a wall of parallel floats, and "is this NPC pregnant?"
    // is the one question anyone looks at this file to answer.
    //
    // The predicate is not a guess: `LastConception[index] > 0.0` is what every
    // Fertility Mode script tests, from the widget to the birth handler, and
    // `GetCurrentGameTime() - LastConception[index]` is how _JSW_BB_Storage
    // itself computes the term.
    if (const auto conception = values.find(kConceptionArray);
        conception != values.end() && conception->is_number()) {
        const float value = conception->get<float>();
        payload["pregnant"] = value > 0.0f;
        if (value > 0.0f) {
            if (auto* calendar = RE::Calendar::GetSingleton()) {
                payload["daysPregnant"] = calendar->GetCurrentGameTime() - value;
            }
        }
    } else {
        payload["pregnant"] = false;
    }

    payload["values"] = std::move(values);

    // FatherRaceId holds a *runtime* race form ID, which is meaningless in another
    // save. Convert it to a FormKey now, while this session can still translate it.
    if (const auto raceId = readInt("FatherRaceId"); raceId && *raceId > 0) {
        if (auto* form = RE::TESForm::LookupByID(static_cast<RE::FormID>(*raceId))) {
            payload["fatherRaceKey"] = Model::FormKeyUtil::BuildFormKey(form);
        }
    }

    if (m_handles.effectsFaction) {
        payload["effectsFactionRank"] =
            subject.actor->GetFactionRank(m_handles.effectsFaction, false);
    }

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/fertility", subject.refKey), subject.refKey,
        std::format("{} (tracked at index {})", subject.displayName, *index));
}

void NpcFertility::EndCollect(Core::CollectContext& ctx) {
    if (!m_handles.valid) {
        return;
    }
    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    auto& payload = ctx.Payload(kId, Describe().schemaVersion);

    if (const auto version =
            vars->GetString(m_handles.quest, std::string(kStorageScript), "_updatedToVersion")) {
        payload["storageVersion"] = *version;
    }

    // Spawned children: recorded so the user knows what to re-summon, not so we can
    // recreate them. They are PlaceAtMe products with 0xFF form IDs.
    if (const auto children = vars->GetArrayVariable(m_handles.quest, std::string(kStorageScript),
                                                     "PlayerChildActorIndex");
        children.success) {
        auto indices = nlohmann::json::array();
        for (const auto& element : children.elements) {
            if (const auto* value = std::get_if<int32_t>(&element); value && *value >= 0) {
                indices.push_back(*value);
            }
        }
        payload["playerChildActorIndex"] = std::move(indices);
        if (!payload["playerChildActorIndex"].empty()) {
            ctx.report.Warn(
                Report::ReasonCode::kPartialByDesign,
                std::format("{} spawned child actor(s) recorded by base index only. The actors "
                            "themselves are PlaceAtMe products with 0xFF form IDs and cannot be "
                            "carried across saves; re-summon them from Fertility Mode's MCM.",
                            payload["playerChildActorIndex"].size()));
        }
    }
}

void NpcFertility::BeginApply(Core::ApplyContext& ctx) {
    if (!ResolveHandles(ctx.report)) {
        return;
    }
    if (Config::MigrationConfig::FertilityDryRun()) {
        ctx.report.Warn(Report::ReasonCode::kSkippedByIni,
                        "bFertilityDryRun=1: every intended write is logged and none performed. "
                        "Read the log, then set it to 0.");
    }
}

void NpcFertility::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (!m_handles.valid || !subject.actor) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object() || !payload.contains("values")) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto itemId = std::format("{}/fertility", subject.refKey);
    const bool dryRun = Config::MigrationConfig::FertilityDryRun();

    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();

    // Get-or-create. Returns the existing index when already tracked, which is what
    // makes this safe against _JSW_BB_Po3ActorDiscovery having got there first.
    const auto displayName = subject.displayName;
    const auto refKey = subject.refKey;
    const auto values = payload.value("values", nlohmann::json::object());
    const auto fatherRaceKey = payload.value("fatherRaceKey", std::string{});
    const int32_t factionRank = payload.value("effectsFactionRank", -1);
    auto handles = m_handles;
    auto* self = this;

    const bool dispatched = papyrus->CallMethodInt(
        m_handles.quest, std::string(kStorageScript), "TrackedActorAdd",
        {static_cast<RE::Actor*>(subject.actor)},
        [self, handles, values, fatherRaceKey, factionRank, displayName, refKey,
         dryRun](int32_t index) {
            if (index < 0) {
                spdlog::warn(
                    "NpcFertility: TrackedActorAdd refused '{}' (index {}). Fertility's 128-actor "
                    "tracking limit may be reached.",
                    displayName, index);
                return;
            }
            // Back to the game thread: we are on the VM callback thread, and
            // everything below is a VM interaction.
            Util::OnGameThread([self, handles, values, fatherRaceKey, factionRank, displayName,
                                index, dryRun]() {
                auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
                auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();

                // Normalise every array's length before touching any of them.
                papyrus->CallMethod(handles.quest, std::string(kStorageScript), "UpdateStorage", {});

                Util::OnGameThread([self, handles, values, fatherRaceKey, factionRank, displayName,
                                    index, dryRun]() {
                    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();

                    // Re-check the invariant *after* UpdateStorage, since that is
                    // what is supposed to establish it.
                    self->m_invariantChecked = false;
                    const auto tracked = vars->GetArrayLength(
                        handles.quest, std::string(kStorageScript), "TrackedActors");
                    if (!tracked) {
                        spdlog::error("NpcFertility: TrackedActors unreadable after UpdateStorage");
                        return;
                    }
                    const uint32_t expected = *tracked + 1;

                    const auto writeOne = [&](const char* property, const char* elementType,
                                              const Papyrus::VariableValue& value) {
                        const auto length = vars->GetArrayLength(
                            handles.quest, std::string(kStorageScript), property);
                        if (!length || *length != expected) {
                            spdlog::error(
                                "NpcFertility: refusing to write {}[{}] for '{}' - array is {} "
                                "long, expected {}",
                                property, index, displayName, length ? *length : 0, expected);
                            return;
                        }
                        if (!vars->AssertPropertyType(std::string(kStorageScript), property,
                                                      elementType)) {
                            return;
                        }
                        if (dryRun) {
                            spdlog::info("NpcFertility[DRY RUN]: would write {}[{}] for '{}'",
                                         property, index, displayName);
                            return;
                        }
                        vars->SetArrayElement(handles.quest, std::string(kStorageScript), property,
                                              static_cast<uint32_t>(index), value);
                    };

                    for (const auto& spec : kFloatArrays) {
                        if (const auto entry = values.find(spec.property);
                            entry != values.end() && entry->is_number()) {
                            writeOne(spec.property, spec.elementType, entry->get<float>());
                        }
                    }
                    for (const auto& spec : kIntArrays) {
                        if (const auto entry = values.find(spec.property);
                            entry != values.end() && entry->is_number()) {
                            // FatherRaceId is rewritten below from the FormKey; skip
                            // the stale runtime id here.
                            if (std::string_view(spec.property) == "FatherRaceId" &&
                                !fatherRaceKey.empty()) {
                                continue;
                            }
                            writeOne(spec.property, spec.elementType, entry->get<int32_t>());
                        }
                    }
                    for (const auto& spec : kStringArrays) {
                        if (const auto entry = values.find(spec.property);
                            entry != values.end() && entry->is_string()) {
                            writeOne(spec.property, spec.elementType, entry->get<std::string>());
                        }
                    }

                    // The father's race, translated from the FormKey rather than
                    // carried as a stale runtime id.
                    if (!fatherRaceKey.empty()) {
                        if (auto* race =
                                Model::FormResolver::Get().ResolveChecked<RE::TESRace>(fatherRaceKey)) {
                            writeOne("FatherRaceId", "Int",
                                     static_cast<int32_t>(race->GetFormID()));
                        }
                    }

                    // LastConception last: it is the pregnancy predicate, so writing
                    // it before its supporting timestamps would briefly present a
                    // pregnancy with no context to any poll that ran in between.
                    if (const auto entry = values.find(kConceptionArray);
                        entry != values.end() && entry->is_number()) {
                        writeOne(kConceptionArray, "Float", entry->get<float>());
                    }

                    spdlog::info("NpcFertility: {} state for '{}' at index {}",
                                 dryRun ? "logged (dry run)" : "wrote", displayName, index);
                });
            });
        });

    if (!dispatched) {
        ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kPapyrusCallFailed,
                          std::format("could not dispatch TrackedActorAdd for '{}'", displayName));
        return;
    }

    // The faction rank is re-asserted in phase 2, after the equipment churn, because
    // `unequipOthers` during an outfit apply can strip the baby item that goes with it.
    if (factionRank >= 0 && m_handles.effectsFaction && !dryRun) {
        Defer::PendingItem item;
        item.categoryId = std::string(kId);
        item.subjectFormKey = refKey;
        item.trigger = Defer::TriggerBits(Defer::Trigger::kActorLoaded);
        item.maxAttempts = 8;
        item.payload = Util::SafeDump(payload);
        ctx.pending.Enqueue(std::move(item));
    }

    ++m_written;
    ctx.report.Succeeded(subjectRef, itemId, subject.refKey, subject.displayName);
    (void)vars;
}

bool NpcFertility::ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    // This is the *only* place the queued faction rank is ever written.
    //
    // Without this override the base-class default runs `ApplyActor` again, and
    // `ApplyActor` does not apply a rank - it enqueues one. So the replay
    // re-queued the same item, reported success, and the recorded rank was never
    // written at all. Worse, it made the category the one shape the drain has to
    // defend against: an applier that re-queues its own key on every pass.
    //
    // It also has to resolve its own handles. A deferred item can be replayed in a
    // session where `BeginApply` never ran - the queue lives in the co-save and
    // outlives the restore that filled it - and `m_handles` is then default
    // constructed, which made `ApplyActor` return at its first line and retire the
    // item having done nothing and said nothing.
    if (!subject.actor) {
        return true;
    }
    if (!ResolveHandles(ctx.report)) {
        return true;  // Fertility is not usable here; retrying will not change that
    }
    if (!m_handles.effectsFaction) {
        return true;
    }
    if (Config::MigrationConfig::FertilityDryRun()) {
        spdlog::info("NpcFertility: dry run, not re-asserting the effects faction rank for '{}'",
                     subject.displayName);
        return true;
    }

    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    const int32_t factionRank = payload.value("effectsFactionRank", -1);
    if (factionRank < 0) {
        return true;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto itemId = std::format("{}/fertility_faction", subject.refKey);

    // `AddToFaction` sets the rank on an existing membership as well as creating
    // one, so it covers both the actor Fertility had already enrolled and the one
    // it had not.
    subject.actor->AddToFaction(m_handles.effectsFaction, static_cast<int8_t>(factionRank));

    const auto landed = subject.actor->GetFactionRank(m_handles.effectsFaction, false);
    if (landed == factionRank) {
        ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                             std::format("{} (effects faction rank {})", subject.displayName,
                                         factionRank));
    } else {
        ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kValidationMismatch,
                          std::format("wrote effects faction rank {} for '{}' but read back {}",
                                      factionRank, subject.displayName, landed));
    }
    return true;
}

}  // namespace SaveMigration::Categories
