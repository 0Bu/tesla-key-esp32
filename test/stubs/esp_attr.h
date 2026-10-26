#pragma once

// Host compilation exercises the real trend's locking/CRC/adoption logic. Native process memory
// has no ESP noinit section: reset retention remains a separate target/physical proof boundary.
#define __NOINIT_ATTR
