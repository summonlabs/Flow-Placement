// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Flow Placement 1.0.0 - version and on-disk protocol constants.

#ifndef FLOWPLACE_VERSION_HPP
#define FLOWPLACE_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace flowplace {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";
inline constexpr std::string_view kProjectName = "flowplace";

// Version of the durable store container/record encoding.
inline constexpr std::uint32_t kStoreFormatVersion = 1;
// Version of the framed wire protocol.
inline constexpr std::uint32_t kWireProtocolVersion = 1;
// Version of the scenario text grammar understood by the CLI.
inline constexpr std::uint32_t kScenarioFormatVersion = 1;

// The C++ standard this build was compiled against (informational).
inline constexpr long kCppStandard = __cplusplus;

}  // namespace flowplace

#endif  // FLOWPLACE_VERSION_HPP
