#include "reader.h"

#include <algorithm>
#include <string>
#include <string_view>

#include "reader_subscriber.h"
#include "statement_factory.h"

namespace griha {

namespace {

using namespace std::string_view_literals;

constexpr auto c_explicit_block_begin   = "{"sv;
constexpr auto c_explicit_block_end     = "}"sv;

bool is_block_begin(std::string_view line) {
    using namespace std;
    return line == c_explicit_block_begin;
}

bool is_block_end(std::string_view line) {
    using namespace std;
    return line == c_explicit_block_end;
}

} // unnamed namespace

struct Reader::State : std::enable_shared_from_this<State> {
    using ReaderImpl = Reader::Impl;

    virtual ~State() {}
    virtual StatePtr process(std::string_view) = 0;
};

struct InitialState;

struct Reader::Impl {

    explicit inline Impl(size_t bsize) : block_size(bsize) {}

    std::weak_ptr<InitialState> initial_state;

    const size_t block_size;
    std::vector<ReaderSubscriberPtr> subscribers;
    StatementFactory statement_factory;
    Reader::Metrics metrics;

    template <typename S> std::shared_ptr<S> get_state();

    StatementPtr parse(std::string_view line);

    StatePtr process(std::string_view line, StatePtr state);

    void notify_block(StatementContainer& stms);
};

struct InitialState : Reader::State {
    inline explicit InitialState(ReaderImpl& r_impl)
        : reader_impl(r_impl) {}

    inline ~InitialState();

    Reader::StatePtr process(std::string_view line) override;

    ReaderImpl& reader_impl;
    StatementContainer statements;
};

struct BlockState : Reader::State {
    inline BlockState(ReaderImpl& r_impl) 
        : reader_impl(r_impl) {}

    Reader::StatePtr process(std::string_view line) override;

    ReaderImpl& reader_impl;
    StatementContainer statements;
    size_t level { 1 };
};

struct ErrorState : Reader::State {
    inline ErrorState(ReaderImpl& r_impl)
        : reader_impl(r_impl) {}

    Reader::StatePtr process(std::string_view line) override;

    ReaderImpl& reader_impl;
    std::string error;
};

template <>
std::shared_ptr<InitialState> Reader::Impl::get_state<InitialState>() {
    if (initial_state.expired()) {
        auto ret = std::make_shared<InitialState>(*this);
        initial_state = ret;
        return ret;
    }

    return initial_state.lock();
} 

template <typename S>
std::shared_ptr<S> Reader::Impl::get_state() {
    return std::make_shared<S>(*this);
} 

StatementPtr Reader::Impl::parse(std::string_view line) {
    ++metrics.nstatements;
    return statement_factory.create(std::string { line });
}

auto Reader::Impl::process(std::string_view line, StatePtr state) -> StatePtr {
    ++metrics.nlines;
    if (state)
        return state->process(line);
    else
        return get_state<InitialState>()->process(line);
}

void Reader::Impl::notify_block(StatementContainer& stms) {
    if (stms.empty())
        return; // empty block doesn't require notification

    ++metrics.nblocks;

    for (auto& subscriber : subscribers)
        subscriber->on_block(stms);
    stms.clear();
}

InitialState::~InitialState() {
    reader_impl.notify_block(statements);
}

Reader::StatePtr InitialState::process(std::string_view line) {
    using namespace std::string_literals;

    if (is_block_end(line)) {
        auto ret = reader_impl.get_state<ErrorState>();
        ret->error = "unexpected end of block"s;
        return ret;
    } else if (is_block_begin(line)) {
        return reader_impl.get_state<BlockState>();
    } else {
        statements.push_back(reader_impl.parse(line));
        if (statements.size() == reader_impl.block_size) {
            // fixed block size has been reached
            reader_impl.notify_block(statements);
        }
    }

    return shared_from_this();
}

Reader::StatePtr BlockState::process(std::string_view line) {
    if (is_block_begin(line)) {
        // nested explicit blocks are ignored but correction of syntax is required
        ++level;
    } else if (is_block_end(line)) {
        if (--level == 0) {
            // explicit block has been ended
            // block has statements - notify about end of block
            reader_impl.notify_block(statements);
            return reader_impl.get_state<InitialState>();
        }
    } else {
        statements.push_back(reader_impl.parse(line));
    }

    return shared_from_this();
}

Reader::StatePtr ErrorState::process([[maybe_unused]] std::string_view line) {
    // do nothing
    return shared_from_this();
}

Reader::Reader(size_t block_size) 
    : priv_(std::make_unique<Impl>(block_size)) {}

Reader::~Reader() = default;
Reader::Reader(Reader&&) = default;
Reader& Reader::operator= (Reader&&) = default;

void Reader::subscribe(ReaderSubscriberPtr subscriber) {
    auto& subscribers = priv_->subscribers;
    auto it = std::find(subscribers.begin(), subscribers.end(), subscriber);
    if (it == subscribers.end())
        subscribers.push_back(std::move(subscriber));
}

auto Reader::consume(std::string_view line, StatePtr state) -> StatePtr {
    return priv_->process(line, state);
}

auto Reader::get_metrics() const -> const Metrics& {
    return priv_->metrics;
}

} // namespace griha