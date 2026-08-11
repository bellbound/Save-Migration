#include "categories/npc/NpcFollowerSlavery.h"

#include <format>

#include <nlohmann/json.hpp>

#include "model/FormKeyUtil.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.follower_slavery";
constexpr std::string_view kPlugin = "Follower Slavery Mod.esp";
constexpr std::string_view kUtilityScript = "fsm_utilityscript";

/// The keyword FSM puts on an enslaved actor. Looked up by editor id rather
/// than by form id: keywords keep their editor ids at runtime - it is how TNG
/// reads its own addon keywords back - and an editor id survives the mod being
/// renumbered in a way a hard-coded form id does not.
constexpr std::string_view kSlaveKeywordEditorId = "fsm_Slave";

/// FSM's one-shot enslavement queue, relative to `Data/`.
constexpr std::string_view kQueueRelativePath = "SKSE/Plugins/FSM/JC/EnslaveOnLoadGame.json";

/// "a, b and c" for the reload notice.
std::string JoinNames(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            out += (i + 1 == names.size()) ? " and " : ", ";
        }
        out += names[i];
    }
    return out;
}

/// JContainers writes a form as `__formData|<plugin>|0x<lowercase hex>`, which
/// is the same two facts as our own key in a different order. Converting rather
/// than storing JContainers' spelling keeps the snapshot in one dialect.
std::string ToJContainersForm(std::string_view formKey) {
    const auto parsed = Model::FormKeyUtil::ParseFormKey(formKey);
    if (!parsed) {
        return {};
    }
    return std::format("__formData|{}|0x{:x}", parsed->pluginName, parsed->localFormId);
}

}  // namespace

const Core::CategoryDescriptor& NpcFollowerSlavery::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Follower Slavery Mod",
        // With the other behaviour-changing integrations, after the followers
        // have been regrouped. Nothing here takes effect this session anyway -
        // the queue is read by FSM on the *next* load - so the only thing the
        // phase decides is where it appears in the report.
        .phase = Core::Phase::kIntegrations,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {.plugins = {std::string(kPlugin)},
                        .scriptNames = {std::string(kUtilityScript)},
                        .dllNames = {}},
        .schemaVersion = 1,
    };
    return descriptor;
}

void NpcFollowerSlavery::BeginCollect(Core::CollectContext& ctx) {
    m_slaveKeyword = nullptr;
    m_found = 0;

    if (auto* handler = RE::TESDataHandler::GetSingleton()) {
        for (auto* keyword : handler->GetFormArray<RE::BGSKeyword>()) {
            const char* editorId = keyword ? keyword->GetFormEditorID() : nullptr;
            if (editorId && Util::IEquals(editorId, kSlaveKeywordEditorId)) {
                m_slaveKeyword = keyword;
                break;
            }
        }
    }

    if (!m_slaveKeyword) {
        ctx.report.SkipCategory(Report::ReasonCode::kModApiMissing,
                                "the 'fsm_Slave' keyword is not in this load order, so there is no "
                                "way to tell which followers Follower Slavery Mod has taken");
        return;
    }
    spdlog::info("NpcFollowerSlavery: fsm_Slave keyword is {:08X}", m_slaveKeyword->GetFormID());
}

void NpcFollowerSlavery::CollectActor(const Model::ActorSubject& subject,
                                      Core::CollectContext& ctx) {
    if (!m_slaveKeyword || !subject.actor || subject.isPlayer || subject.isDynamicRef) {
        return;
    }
    // The keyword goes on the actor reference, not the base, because two
    // references sharing a base can be in different states.
    if (!subject.actor->HasKeyword(m_slaveKeyword)) {
        return;
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["enslaved"] = true;
    payload["name"] = subject.displayName;
    if (auto* cell = subject.actor->GetParentCell()) {
        const char* cellName = cell->GetName();
        if (cellName && *cellName) {
            payload["lastSeenIn"] = Util::ConvertSkyrimTextToUTF8(cellName);
        }
    }

    ++m_found;
    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/fsm", subject.refKey), subject.refKey,
        std::format("{} is enslaved", subject.displayName));
}

void NpcFollowerSlavery::EndCollect(Core::CollectContext& ctx) {
    if (!m_slaveKeyword) {
        return;
    }
    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    // `slaveCount` and nothing else. There was a constant `note` here explaining
    // how detection works and why the master is not recorded; that belongs in
    // this file's comments, which already carry it, rather than in every payload.
    payload["slaveCount"] = m_found;

    if (m_found == 0) {
        ctx.report.Info("no follower in the roster is currently enslaved");
        return;
    }
    ctx.report.Info(std::format(
        "{} enslaved follower(s) recorded. Their masters are not carried across - FSM's own "
        "re-enslavement hook assigns a fresh master of the recorded type, which is a live NPC in "
        "the new save rather than a stale reference from the old one. Gear chests do not travel "
        "either; whatever a slave was carrying stays in the save it was taken from.",
        m_found));
}

void NpcFollowerSlavery::BeginApply(Core::ApplyContext&) {
    m_queuedFormKeys.clear();
    m_queuedNames.clear();
}

void NpcFollowerSlavery::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object() || !payload.value("enslaved", false)) {
        return;
    }
    if (!subject.actor) {
        return;
    }

    // Written against the key that resolved here, so a follower whose plugin is
    // absent has already been dropped by the roster and never reaches the file.
    const auto key = Model::FormKeyUtil::BuildFormKey(subject.actor);
    const auto encoded = ToJContainersForm(key.empty() ? subject.refKey : key);
    if (encoded.empty()) {
        ctx.report.Failed(
            Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
            std::format("{}/fsm", subject.refKey), Report::ReasonCode::kSubjectUnresolvable,
            "the reference could not be written in the form JContainers reads");
        return;
    }
    m_queuedFormKeys.push_back(encoded);
    m_queuedNames.push_back(subject.displayName);
}

void NpcFollowerSlavery::EndApply(Core::ApplyContext& ctx) {
    const auto subject = Report::SystemSubject("Follower Slavery Mod");
    if (m_queuedFormKeys.empty()) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone,
                                "the snapshot records no enslaved followers");
        return;
    }

    const auto path = Util::DataFolder() / kQueueRelativePath;

    // The existing file is read rather than overwritten wholesale, because
    // `master_types` is the user's own configuration - it is the list FSM shows
    // in its MCM and the one they curated - and the snapshot has no business
    // replacing it. Only the queue array is ours to fill.
    nlohmann::json document;
    std::string existing;
    if (Util::ReadFileToString(path, existing)) {
        document = nlohmann::json::parse(existing, nullptr, false);
    }
    if (!document.is_object()) {
        ctx.report.Failed(subject, "fsm_queue", Report::ReasonCode::kIoError,
                          std::format("{} is missing or not readable as JSON, so there is nothing "
                                      "to add the followers to. Is Follower Slavery Mod installed "
                                      "with its SKSE files?",
                                      kQueueRelativePath));
        return;
    }

    const auto types = document.find("master_types");
    if (types == document.end() || !types->is_array() || types->empty()) {
        ctx.report.Failed(subject, "fsm_queue", Report::ReasonCode::kModApiMissing,
                          "EnslaveOnLoadGame.json names no master types, and FSM refuses a queue "
                          "without one. Set at least one type in FSM's MCM first.");
        return;
    }

    auto queue = nlohmann::json::array();
    for (const auto& encoded : m_queuedFormKeys) {
        queue.push_back(encoded);
    }
    document["enslave_followers_on_load"] = std::move(queue);

    if (!Util::WriteFileAtomic(path, document.dump(2))) {
        ctx.report.Failed(subject, "fsm_queue", Report::ReasonCode::kIoError,
                          std::format("could not write {}", path.string()));
        return;
    }

    ctx.report.Succeeded(subject, "fsm_queue", "",
                         std::format("{} follower(s) queued for re-enslavement",
                                     m_queuedFormKeys.size()));
    ctx.report.RequireReload(std::format(
        "FSM_RELOAD_REQUIRED: {} follower(s) - {} - are queued in EnslaveOnLoadGame.json. Follower "
        "Slavery Mod reads that file during its own start-up and then erases the queue, so the "
        "enslavement happens on the *next* load rather than now. Save and reload once. Each will "
        "be taken by a random master of one of the types configured in FSM's MCM, because that is "
        "what FSM's own hook accepts - the original masters are not carried across.",
        m_queuedFormKeys.size(), JoinNames(m_queuedNames)));
}

}  // namespace SaveMigration::Categories
