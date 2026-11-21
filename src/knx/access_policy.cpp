#include "access_policy.h"

bool isPermitted(const AccessPolicy& policy, bool securityModeEnabled, const SecurityControl& secCtrl, AccessType type)
{
    const uint16_t mask = securityModeEnabled ? policy.on : policy.off;

    uint8_t column;
    if (secCtrl.dataSecurity == DataSecurity::None)
        column = 0; // Unlisted, plain
    else if (secCtrl.toolAccess)
        column = (secCtrl.dataSecurity == DataSecurity::AuthConf) ? 3 : 4; // Tool Key
    else
        column = (secCtrl.dataSecurity == DataSecurity::AuthConf) ? 1 : 2; // Role (P2P, not supported)

    const uint8_t bit = (type == AccessType::Write ? 9 : 8) - 2 * column;
    return ((mask >> bit) & 0x01) == 0x01;
}

AccessPolicy restartPolicy(RestartType restartType, EraseCode eraseCode)
{
    if (restartType == RestartType::BasicRestart)
        return Policy::Default;

    switch (eraseCode)
    {
        case EraseCode::ConfirmedRestart:
            return Policy::Default;
        case EraseCode::ResetIA:
            return Policy::Never;
        case EraseCode::FactoryReset:
        case EraseCode::ResetAP:
        case EraseCode::ResetParam:
        case EraseCode::ResetLinks:
        case EraseCode::FactoryResetWithoutIA:
            return Policy::ToolOnSec;
        default:
            // Unsupported erase codes are answered with an error anyway
            return Policy::Default;
    }
}
