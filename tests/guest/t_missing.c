// SPDX-License-Identifier: GPL-3.0-or-later
// Calls an import no module defines and the host does not implement: the run must stop and name it.
#include "nx.h"
int nn_not_implemented_anywhere(int);
int test_main(void) {
    puts_("calling the missing import\n");
    return nn_not_implemented_anywhere(1);
}
