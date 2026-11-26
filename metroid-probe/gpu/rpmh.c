/* Real XNU RPMh implementation with physical maps and a single boot CPU.
 * touch_power.h is extracted from overlay0029 by build.py; no touch entry
 * point is invoked. The same GPU header/command-DB validation is reused. */
#include "runtime.h"
#include "touch_power.h"
#include "metroid_gpu_rpmh.h"
