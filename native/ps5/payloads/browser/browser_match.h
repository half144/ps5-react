// Adapted from Orbit Store 1.0.1 by saawant12 (https://github.com/saawant12/orbit-store-ps5).
// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only browser capture; original corresponding source is recorded in README.md.
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef struct { const char *filename; char url[544]; } BrowserMatch;
bool browser_capture_url(const char *filename, const char *url);
bool browser_match(void *context, const unsigned char *bytes, size_t length);
