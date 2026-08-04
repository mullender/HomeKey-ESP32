// Empty sdkconfig stub for host tests. On-target ESP-IDF generates the
// real sdkconfig.h into the build tree. Host tests put this directory
// first on the include path so main/include/defaults.h's unconditional
// #include "sdkconfig.h" resolves here without needing an IDF build.
//
// The installer test opts into CONFIG_INSTALLER_ATOMS3_LITE_DEFAULTS via
// a -D on the compiler command line, matching how the on-target installer
// build enables it via sdkconfig.defaults.installer.atoms3.
