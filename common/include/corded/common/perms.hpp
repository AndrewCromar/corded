// Permissions a role can carry. A member's permissions are the union of the
// permissions of all their roles. See master_plan/04-community-model.md.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace corded::perm {

inline constexpr uint64_t ViewChannel = 1ull << 0;
inline constexpr uint64_t SendMessages = 1ull << 1;
inline constexpr uint64_t AddReactions = 1ull << 2;
inline constexpr uint64_t AttachFiles = 1ull << 3;
inline constexpr uint64_t MentionEveryone = 1ull << 4;
inline constexpr uint64_t ManageMessages = 1ull << 5;
inline constexpr uint64_t ManageChannels = 1ull << 6;
inline constexpr uint64_t ManageRoles = 1ull << 7;
inline constexpr uint64_t ManageNicknames = 1ull << 8;
inline constexpr uint64_t KickMembers = 1ull << 9;
inline constexpr uint64_t BanMembers = 1ull << 10;
inline constexpr uint64_t CreateInvite = 1ull << 11;
inline constexpr uint64_t ManageServer = 1ull << 12;
inline constexpr uint64_t Administrator = 1ull << 13;

inline constexpr uint64_t All = (1ull << 14) - 1;
// What the @everyone role starts with.
inline constexpr uint64_t Default = ViewChannel | SendMessages | AddReactions | AttachFiles;

inline const std::vector<std::pair<std::string_view, uint64_t>>& names() {
    static const std::vector<std::pair<std::string_view, uint64_t>> table = {
        {"view_channel", ViewChannel},       {"send_messages", SendMessages},
        {"add_reactions", AddReactions},     {"attach_files", AttachFiles},
        {"mention_everyone", MentionEveryone}, {"manage_messages", ManageMessages},
        {"manage_channels", ManageChannels}, {"manage_roles", ManageRoles},
        {"manage_nicknames", ManageNicknames}, {"kick_members", KickMembers},
        {"ban_members", BanMembers},         {"create_invite", CreateInvite},
        {"manage_server", ManageServer},     {"administrator", Administrator},
    };
    return table;
}

// Returns 0 for an unknown name.
inline uint64_t from_name(std::string_view name) {
    for (const auto& [n, bit] : names())
        if (n == name) return bit;
    return 0;
}

inline std::vector<std::string> to_names(uint64_t bits) {
    std::vector<std::string> out;
    for (const auto& [n, bit] : names())
        if (bits & bit) out.emplace_back(n);
    return out;
}

}  // namespace corded::perm
