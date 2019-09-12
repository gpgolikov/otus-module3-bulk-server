#pragma once

#include "forward.h"

namespace griha {

struct ReaderSubscriber {
    virtual ~ReaderSubscriber() {}
    virtual void on_block(const StatementContainer&) = 0;
};

} // namespace griha