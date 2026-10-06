// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "network.hpp"
#include "app_config.hpp"
#include "filesystem_access.hpp"
#include <unistd.h>
#if PS5_REACT_NETWORKING
extern "C" {
int sceNetInit(void);
int sceNetTerm(void);
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);
}
namespace network {
namespace { int pool = -1; bool initialized = false; }
bool platform_start(std::string& error) {
  if (!console_filesystem_accessible()) {
    error = "native networking requires successful console filesystem elevation"; return false;
  }
  if (access("/system/common/cert/CA_LIST.cer", R_OK) != 0) {
    error = "native networking cannot read the system certificate store"; return false;
  }
  initialized = sceNetInit() == 0;
  pool = sceNetPoolCreate("ps5-react-http", 1024*1024, 0);
  if (pool < 0) {
    if (initialized) sceNetTerm(); initialized = false;
    error = "native networking could not create its libSceNet pool"; return false;
  }
  return true;
}
void platform_stop() {
  if (pool >= 0) sceNetPoolDestroy(pool);
  if (initialized) sceNetTerm();
  pool = -1; initialized = false;
}
const char* ca_path() { return "/system/common/cert/CA_LIST.cer"; }
}
#endif
