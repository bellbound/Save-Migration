#include "MessageBoxUtil.h"

namespace MessageBoxUtil
{
    // Custom callback class that wraps a std::function
    class MessageBoxResultCallback : public RE::IMessageBoxCallback
    {
    public:
        MessageBoxResultCallback(Callback callback) : _callback(std::move(callback)) {}
        ~MessageBoxResultCallback() override = default;

        void Run(RE::IMessageBoxCallback::Message message) override
        {
            if (_callback) {
                _callback(static_cast<unsigned int>(message));
            }
        }

    private:
        Callback _callback;
    };

    void Show(const std::string& bodyText,
              const std::vector<std::string>& buttons,
              Callback callback)
    {
        auto* factoryManager = RE::MessageDataFactoryManager::GetSingleton();
        if (!factoryManager) {
            return;
        }

        auto* uiStringHolder = RE::InterfaceStrings::GetSingleton();
        if (!uiStringHolder) {
            return;
        }

        auto* factory = factoryManager->GetCreator<RE::MessageBoxData>(uiStringHolder->messageBoxData);
        if (!factory) {
            return;
        }

        auto* messageBox = factory->Create();
        if (!messageBox) {
            return;
        }

        // Set callback if provided
        if (callback) {
            RE::BSTSmartPointer<RE::IMessageBoxCallback> callbackPtr =
                RE::make_smart<MessageBoxResultCallback>(std::move(callback));
            messageBox->callback = callbackPtr;
        }

        // Set message body text
        messageBox->bodyText = bodyText;

        // Add button labels
        for (const auto& button : buttons) {
            messageBox->buttonText.push_back(button.c_str());
        }

        // Queue the message box for display
        messageBox->QueueMessage();
    }

    void ShowOK(const std::string& bodyText)
    {
        Show(bodyText, {"OK"}, nullptr);
    }

    void ShowYesNo(const std::string& bodyText, Callback callback)
    {
        Show(bodyText, {"Yes", "No"}, std::move(callback));
    }

    void ShowYesNoCancel(const std::string& bodyText, Callback callback)
    {
        Show(bodyText, {"Yes", "No", "Cancel"}, std::move(callback));
    }
}
