/* QDC507GLEFM21's bundled ACPH live-tuning initialization crashes before
 * static ACDB calibration. Disable only the optional live-tuning clients.
 * The loader's file parsing, allocation and calibration ioctls stay intact.
 * prepare.sh pins the exact vendor binaries before enabling this shim. */
int acph_init(void) { return 0; }
int acdb_rtac_init(void) { return 0; }
int adie_rtac_init(void) { return 0; }
