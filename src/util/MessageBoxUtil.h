#pragma once

#include <functional>
#include <string>
#include <vector>

namespace MessageBoxUtil
{
    // Callback type: receives the index of the button that was clicked (0-based)
    using Callback = std::function<void(unsigned int)>;

    // Display a message box with custom buttons and callback
    // bodyText: The message to display in the message box
    // buttons: Vector of button labels (e.g., {"Yes", "No", "Cancel"})
    // callback: Function called when user clicks a button, receives button index
    void Show(const std::string& bodyText,
              const std::vector<std::string>& buttons,
              Callback callback);

    // Display a simple OK message box (no callback needed)
    void ShowOK(const std::string& bodyText);

    // Display a Yes/No confirmation box
    // callback receives: 0 = Yes, 1 = No
    void ShowYesNo(const std::string& bodyText, Callback callback);

    // Display a Yes/No/Cancel confirmation box
    // callback receives: 0 = Yes, 1 = No, 2 = Cancel
    void ShowYesNoCancel(const std::string& bodyText, Callback callback);

    // True while a message box - anyone's, not just ours - owns the screen.
    //
    // Worth checking before queueing: mods routinely raise a box in the first
    // seconds after a load, and a box queued behind one is both invisible for as
    // long as the other is up and stacked in front of whatever the player was
    // actually answering. MessageBoxMenu also pauses the game, which suspends the
    // Papyrus VM, so "a box is open" is equally the answer to "can I call into
    // Papyrus right now".
    bool IsAnyOpen();
}
