/* wb_cmd.c — command queue (declarations live in wb_cmd.h as inline).
 * This file exists to anchor the translation unit; the ring buffer is
 * header-inline and lock-free. Nothing else needed.
 */

#include "wbus_cmd.h"
