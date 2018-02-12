/* ntservice.h
 *
 *  Copyright (c) 2006 Germán Méndez Bravo (Kronuz) <kronuz@users.sf.net>
 *  All rights reserved.
 *
 */

#ifndef SERVICE_H
#define SERVICE_H

#include <tchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*svcFunc) ();

int ServiceStart();
int ServiceStop();
int ServiceRestart();
int ServiceUninstall();
int ServiceInstall();
int ServiceRun();

void ServiceSetFunc(svcFunc runFunc, svcFunc pauseFunc, svcFunc continueFunc, svcFunc stopFunc);

extern TCHAR *PACKAGE_NAME;
extern TCHAR *PACKAGE_DISPLAY_NAME;
extern TCHAR *PACKAGE_DESCRIPTION;
extern TCHAR *PACKAGE_START_NAME;

#ifdef __cplusplus
}
#endif
#endif
