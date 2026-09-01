#pragma once

#include "q_fixed.h"

#define Q_SINTAB_SIZE 1024   /* entries per full turn; keep in sync with gen_sintab.py */

/* sin(i * 2*pi / Q_SINTAB_SIZE) * 4096, plus one wrap-guard entry */
extern const q_fixed q_sintab[Q_SINTAB_SIZE + 1];
