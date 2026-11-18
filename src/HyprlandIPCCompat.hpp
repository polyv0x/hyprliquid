#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#if __has_include(<src/ipc/s1/S1.hpp>)
#include <hyprland/src/ipc/s1/S1.hpp>
#endif

namespace HyprlandCompat {

#if __has_include(<src/ipc/s1/S1.hpp>)
using CCommand = IPC::Socket1::SCommand;
#else
using CCommand = SHyprCtlCommand;
#endif

template <typename Callback>
SP<CCommand> registerCommand(HANDLE handle, const std::string& name, Callback callback) {
#if __has_include(<src/ipc/s1/S1.hpp>)
    // Both versions match the complete command, including arguments. PREFIX
    // preserves the old exact=false contract; handlers validate their grammar.
    return HyprlandAPI::registerHyprCtlCommand(handle, {name, IPC::Socket1::COMMAND_MATCH_PREFIX,
        [callback](const IPC::Socket1::SRequest& request) {
            return IPC::Socket1::SResponse(callback(request.command));
        }});
#else
    return HyprlandAPI::registerHyprCtlCommand(handle, {name, false,
        [callback](eHyprCtlOutputFormat, std::string request) { return callback(std::move(request)); }});
#endif
}

} // namespace HyprlandCompat
