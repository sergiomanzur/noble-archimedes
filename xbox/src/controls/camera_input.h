#ifndef NOBLE_CAMERA_INPUT_H
#define NOBLE_CAMERA_INPUT_H
#ifdef __cplusplus
extern "C" {
#endif
/* Title hook operands AFTER the guest's controller/time multiplication.
 * Axis 0 is pitch (guest subtracts it); axis 1 is yaw (guest adds it).
 * Mouse counts are angular displacement and are never multiplied by dt.
 * Controller operands retain the guest filter and scale its fixed 2 degrees
 * per update to configured radians/second using backend logical-camera time.
 * Pad inversion is already applied by the action core; do not repeat it here.
 * Hook 0 captures the pair once; hook 1 consumes the cached yaw once.
 * Call only inside the original sub_00344D00 manual-camera branches.
 * An inactive backend returns the incoming double without consuming motion. */
double noble_camera_adjust(unsigned axis, double original_delta);
/* Default off. Summary emitted only when explicitly called at shutdown. */
void noble_camera_set_diagnostics(int enabled);
void noble_camera_report(void);
#ifdef __cplusplus
}
#endif
#endif
