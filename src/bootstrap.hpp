#pragma once
#include <windows.h>
namespace litedx12 {
void InstallCapture();
// What Start returned: the loader's failure code, or 0 when startup succeeded.
DWORD StartupError();
}
