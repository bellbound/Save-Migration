#include "config/MigrationConfig.h"
#include "core/LifecycleController.h"
#include "core/MigrationState.h"
#include "core/SaveIdentity.h"
#include "core/SerializationHub.h"
#include "core/Worker.h"
#include "defer/PendingWorkQueue.h"
#include "log.h"
#include "papyrus/SaveMigrationApi.h"

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

    Config::MigrationConfig::Initialize();

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

    // Debug natives, so the state machine can be driven from the console during
    // verification rather than by restarting and hand-editing co-saves.
    if (auto* papyrus = SKSE::GetPapyrusInterface()) {
        papyrus->Register(Papyrus::SaveMigrationApi::Bind);
    } else {
        spdlog::warn("Save Migration: no Papyrus interface; debug natives unavailable");
    }

    spdlog::info("Save Migration loaded.");
    return true;
}
