#include "config/ConfigStoragePapyrusAdapter.h"
#include "config/MigrationConfig.h"
#include "core/LifecycleController.h"
#include "core/MigrationState.h"
#include "core/SaveIdentity.h"
#include "core/SerializationHub.h"
#include "core/Worker.h"
#include "defer/PendingWorkQueue.h"
#include "log.h"
#include "papyrus/SaveMigrationApi.h"
#include "papyrus/SaveMigrationMcmApi.h"

/// plugin.cpp owns nothing. It wires SKSE up and forwards the message stream to
/// LifecycleController, which is where every decision lives.
static void MessageHandler(SKSE::MessagingInterface::Message* message) {
    using namespace SaveMigration;
    auto& lifecycle = Core::LifecycleController::Get();

    switch (message->type) {
        case SKSE::MessagingInterface::kPostLoad:
            lifecycle.OnPostLoad();
            break;
        case SKSE::MessagingInterface::kPostPostLoad:
            lifecycle.OnPostPostLoad();
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            lifecycle.OnDataLoaded();
            break;
        case SKSE::MessagingInterface::kNewGame:
            lifecycle.OnNewGame();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            // message->data is the save path.
            lifecycle.OnPreLoadGame(static_cast<const char*>(message->data));
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            // data is a bool-shaped success flag.
            lifecycle.OnPostLoadGame(message->data != nullptr);
            break;
        case SKSE::MessagingInterface::kSaveGame:
            lifecycle.OnSaveGame(static_cast<const char*>(message->data));
            break;
        default:
            break;
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    using namespace SaveMigration;

    SKSE::Init(skse);
    SetupLog();

    spdlog::info("Save Migration loading (runtime {})...", REL::Module::IsVR() ? "VR" : "SE/AE");

    // Both namespaces are spelled out. There is a global `::Config` (the storage
    // layer, shared with the other plugins in this tree) and a
    // `SaveMigration::Config` (the typed facade over it), and inside
    // `using namespace SaveMigration` a bare `Config::` is ambiguous between them.
    SaveMigration::Config::MigrationConfig::Initialize();

    // One serialization owner, three records. SKSE permits a single set of
    // callbacks per plugin, so anything registering separately would silently
    // overwrite the others.
    auto& hub = Core::SerializationHub::Get();
    hub.RegisterHandler(&Core::SaveIdentity::Get());
    hub.RegisterHandler(&Core::MigrationState::Get());
    hub.RegisterHandler(&Defer::PendingWorkQueue::Get());
    hub.Initialize(SKSE::GetSerializationInterface());

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener("SKSE", MessageHandler)) {
        spdlog::error("Save Migration: failed to register the SKSE message listener");
        return false;
    }

    // Three separate binds, one per script name. The MCM cannot function without
    // the second and third: `SaveMigrationApi` is the menu's data source and
    // `SaveMigration_Config` is how it reads and writes the same INI this plugin
    // owns - which is the reason the menu is hand-written SkyUI rather than MCM
    // Helper, since MCM Helper can only ever see its own settings file.
    if (auto* papyrus = SKSE::GetPapyrusInterface()) {
        papyrus->Register(Papyrus::SaveMigrationApi::Bind);
        papyrus->Register(Papyrus::SaveMigrationMcmApi::Bind);
        papyrus->Register(::Config::PapyrusAdapter::Bind);
    } else {
        spdlog::error("Save Migration: no Papyrus interface; the MCM will not work");
    }

    spdlog::info("Save Migration loaded.");
    return true;
}
