// Desktop shim: controller/storage.h — no-op host replacement for the
// firmware's EEPROM persistence (tests/firmware_parity_test).
//
// The real Storage reads/writes the controller's EEPROM; on the host there is
// none. Multi::Init's fallback path (LoadMultiFromEeprom -> false -> factory
// defaults) is exactly what the parity oracle wants, so Load* return false
// and Write* are no-ops.
#pragma once

#include "avrlib/base.h"

namespace ambika
{

class Storage
{
public:
    Storage() {}

    static bool LoadMultiFromEeprom() { return false; }
    static void WriteMultiToEeprom() {}

    static bool LoadSystemSettingsFromEeprom() { return false; }
    static void WriteSystemSettingsToEeprom() {}

private:
    Storage (const Storage&);
    void operator= (const Storage&);
};

}  // namespace ambika
