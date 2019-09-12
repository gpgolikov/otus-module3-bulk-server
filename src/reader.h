#pragma once

#include <string_view>
#include <memory>

#include "forward.h"

namespace griha {

class Reader {

    struct Impl;

public:
    struct State;
    using StatePtr = std::shared_ptr<State>;

    struct Metrics {
        size_t nlines;
        size_t nstatements;
        size_t nblocks;
    };

public:
    Reader(size_t block_size);
    ~Reader();

    Reader(Reader&&);
    Reader& operator= (Reader&&);

    // delete copy operations to simplify Reader class
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    void subscribe(ReaderSubscriberPtr subscriber);

    StatePtr consume(std::string_view line, StatePtr state = StatePtr{});

    const Metrics& get_metrics() const;

private:
    std::unique_ptr<Impl> priv_;
};

} // namespace griha