#include <windows.h>

#include "CameraProbe.h"
#include "DiagonalSprint.h"
#include "FieldOfView.h"
#include "FootstepProbe.h"
#include "GameImage.h"
#include "Log.h"
#include "PlayerFov.h"
#include "Settings.h"
#include "SprintGateProbe.h"
#include "SprintProbe.h"
#include "ViewmodelProbe.h"

namespace {

constexpr const char* kModVersion = "1.1.0";
constexpr unsigned kDecryptTimeoutSeconds = 300;

DWORD WINAPI Start(LPVOID) {
    Log::Open();
    Log::Info("GTAV.FirstPersonExtended %s starting; game version %s (%s)", kModVersion,
              GameImage::Version().c_str(), GameImage::EditionName());
    if (!GameImage::WaitForDecryptedCode(kDecryptTimeoutSeconds)) {
        Log::Info("game code still encrypted after %us; all features disabled",
                  kDecryptTimeoutSeconds);
        return 0;
    }
    Log::Info("game code decrypted (%zu bytes)", GameImage::Code().size());
    const auto settings = Settings::Load();
    bool fovApplied = FieldOfView::Apply(float(settings.maxHorizontalFovDegrees), settings.universalFov);
    // Undoes the slider's widening for the player only, so it needs the FOV feature. UniversalFov
    // widens the sight too, so then it is always on while looking through the sight.
    if ((settings.playerFov || settings.universalFov) && fovApplied)
        PlayerFov::Apply(/*everyCamera=*/settings.playerFov);
    else if (settings.playerFov || settings.universalFov)
        Log::Info("player FOV: needs the FOV feature, which is disabled; stays original");
    else
        Log::Info("player FOV: disabled by GTAV.FirstPersonExtended.ini");
    if (settings.diagonalSprint) DiagonalSprint::Apply();
    else
        Log::Info("diagonal sprint: disabled by GTAV.FirstPersonExtended.ini");
#ifdef FPFIX_CAMERA_PROBE
    CameraProbe::Install();
#endif
#ifdef FPFIX_FOOTSTEP_PROBE
    FootstepProbe::Install();
#endif
#ifdef FPFIX_SPRINT_GATE_PROBE
    SprintGateProbe::Install();
#endif
#ifdef FPFIX_SPRINT_PROBE
    SprintProbe::Install();
#endif
#ifdef FPFIX_VIEWMODEL_PROBE
    ViewmodelProbe::Install();
#endif
    if (fovApplied) FieldOfView::Track();
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Work off the loader thread: scanning must wait for the EXE to decrypt itself.
        if (HANDLE t = CreateThread(nullptr, 0, Start, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
