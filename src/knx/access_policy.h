#pragma once

#include <stdint.h>
#include "knx_types.h"

/**
 * Access Policy as defined in KNX AN193 "Access Policies" and
 * KNX Standard 3/4/1 "Application Interface Layer", clause 6.2.
 *
 * Each policy consists of two 10 bit masks, one for Security Mode disabled (off)
 * and one for Security Mode enabled (on). Each mask holds five (W,R) bit pairs,
 * starting at the most significant bit:
 *
 *   column 0: Unlisted, plain (no security)   W = bit 9, R = bit 8
 *   column 1: Role R0..R15, A+C               W = bit 7, R = bit 6
 *   column 2: Role R0..R15, A only            W = bit 5, R = bit 4
 *   column 3: Tool Key, A+C                   W = bit 3, R = bit 2
 *   column 4: Tool Key, A only                W = bit 1, R = bit 0
 *
 * Example: 3FF/0CC = everything allowed with Security Mode off,
 *          only Role or Tool with A+C with Security Mode on.
 *
 * Point-to-point secure communication (Roles) is not supported by this stack.
 * Columns 1 and 2 are therefore never reached, but the masks are kept as
 * specified so that P2P could be added later without changing the tables.
 */
struct AccessPolicy
{
    uint16_t off; // Security Mode disabled
    uint16_t on;  // Security Mode enabled
};

namespace Policy
{
    constexpr AccessPolicy Open      {0x3FF, 0x3FF}; // no restriction
    constexpr AccessPolicy Default   {0x3FF, 0x0CC}; // standard for most properties and all memory areas
    constexpr AccessPolicy ToolOnSec {0x3FF, 0x00C}; // Security Mode on: Tool Key A+C only
    constexpr AccessPolicy ToolOnly  {0x00C, 0x00C}; // always Tool Key A+C only (key tables etc.)
    constexpr AccessPolicy ToolWrite {0x008, 0x008}; // write only with Tool Key A+C, never readable (PID_TOOL_KEY)
    constexpr AccessPolicy SecCtrl   {0x15F, 0x04C}; // PID_SECURITY_MODE, PID_LOAD_STATE_CONTROL of security object
    constexpr AccessPolicy Info      {0x3FF, 0x1FF}; // readable by everyone, writable by Role/Tool
    constexpr AccessPolicy Log       {0x1FF, 0x0CC}; // failure log, security report
    constexpr AccessPolicy Never     {0x3FF, 0x000}; // Security Mode on: not allowed at all
}

enum class AccessType : uint8_t
{
    Read,
    Write
};

/**
 * Evaluates the access policy for the given communication partner.
 * The communication partner is derived from the SecurityControl of the received service:
 *   dataSecurity == None  -> Unlisted (plain)
 *   toolAccess == true    -> Tool Key (A or A+C)
 */
bool isPermitted(const AccessPolicy& policy, bool securityModeEnabled, const SecurityControl& secCtrl, AccessType type);

/**
 * Access policy for A_Restart, see AN193 2.2.4.3.
 */
AccessPolicy restartPolicy(RestartType restartType, EraseCode eraseCode);
