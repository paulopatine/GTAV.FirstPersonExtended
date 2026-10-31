#pragma once

// Diagnostic build only (make gateprobe): logs, per held key combination, the value that the
// first-person sprint gate compares against its threshold and whether it denied the sprint.
namespace SprintGateProbe {
void Install();
}
