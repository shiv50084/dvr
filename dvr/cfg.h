/* generic definition file */

#ifndef __CFG_H__
#define __CFG_H__

//#define EAGLE32
//#define EAGLE34
#define EAGLE368

//#define MDVR_APP
//#define PWII_APP
#define TVS_APP

// different way of mcu programming
#define MCU_ZEUS

// option to enable ioprocess display mcu debugging message
//#define MCU_DEBUG  (1)

// to enable dvrsvr network message
// #define NETDBG

// to enable power cycling test firmware
// #define POWERCYCLETEST

// yet another g-force
#define SUPPORT_YAGF

#define	APPNAME	"TVSZ3"

// default config file
#define	CFG_DEFFILE	"/davinci/dvr/defconf"
#define	CFG_FILE	"/etc/dvr/dvr.conf"

// applications dir
#define APP_DIR		"/davinci/dvr"
// where dvr var files created
#define VAR_DIR		"/var/dvr"

#define WWWROOT	"/home/www"
#define WWWSERIALFILE "/tmp/wwwserialnofile"

#endif      // __CFG_H__



