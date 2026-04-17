#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the software watchdog task.
// Must be called after all subsystems are initialized.
void watchdog_start(void);

#ifdef __cplusplus
}
#endif
