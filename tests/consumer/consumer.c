/* Copyright 2026 davenonymous
 * SPDX-License-Identifier: MIT
 *
 * Prints the C API's version.
 */
#include <stdio.h>

#include "tfdemo.h"

int main(void) { return printf("tfdemo %s\n", tfdemo_version()) > 0 ? 0 : 1; }
