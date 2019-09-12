#include "interpreter.h"

#include <array>
#include <string>
#include <vector>
#include <list>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <fstream>
#include <sstream>

#include <boost/format.hpp>

#include <range/v3/utility/iterator.hpp>

#include "reader.h"
#include "reader_subscriber.h"
#include "statement.h"

namespace griha {

namespace {

struct Worker : ReaderSubscriber {

    struct Metrics {
        size_t nblocks;
        size_t nstatements;
    };
    
    std::vector<Metrics> thread_metrics;
    std::vector<std::thread> thread_pool;
    std::mutex guard;
    std::condition_variable cv_bulks;
    std::list<StatementContainer> bulks;
    bool stopped { false };

    template <typename Job>
    Worker(size_t nthreads, Job&& job) 
        : thread_metrics(nthreads, {0, 0}) {
        thread_pool.reserve(nthreads);
        for (auto i = 0u; i < nthreads; ++i) {
            thread_pool.push_back(std::thread {
                std::ref(*this), 
                std::forward<Job>(job), std::ref(thread_metrics[i])
            });
        }
    }

    ~Worker() {
        join();
    }

    template <typename Job>
    void operator ()(Job&& job, Metrics& metrics) {
        auto exit = false;
        while (!exit) {
            std::unique_lock<std::mutex> l { guard };
            cv_bulks.wait(l, [this] {
                return stopped || !bulks.empty();
            });

            exit = stopped;
            std::list<StatementContainer> bulks_local;
            std::swap(bulks, bulks_local);

            l.unlock();

            for (auto& stms : bulks_local) {
                job(stms);

                // calculate metrics
                ++metrics.nblocks;
                metrics.nstatements += stms.size();
            }
        }
    }


    void send(StatementContainer stms) {
        {
            std::lock_guard<std::mutex> l { guard };
            bulks.push_back(std::move(stms));
        }
        cv_bulks.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> l { guard };
            stopped = true;
        }
        cv_bulks.notify_all();
    }

    void join() {
        for (auto& t : thread_pool)
            if (t.joinable())
                t.join();
    }

    void on_block(const StatementContainer& stms) override {
        send(stms);
    }
};
using WorkerPtr = std::shared_ptr<Worker>;

} // unnamed namespace

struct Interpreter::State {
    Reader::StatePtr reader_state;

    explicit State(Reader::StatePtr rs) : reader_state(rs) {}
};

struct Interpreter::Impl {

    const std::string name;
    Reader reader;
    Logger logger;
    WorkerPtr log_worker;
    WorkerPtr file_worker;
    std::mutex guard;
    bool stopped { false };

    Impl(std::string n, Logger l, size_t block_size, size_t nthreads) 
        : name(std::move(n))
        , reader(block_size)
        , logger(l)
        , log_worker(std::make_shared<Worker>(1u, std::bind(&Impl::log_job, std::placeholders::_1, name, logger)))
        , file_worker(std::make_shared<Worker>(nthreads, &Impl::file_job)) {
        reader.subscribe(log_worker);
        reader.subscribe(file_worker);
    }

    StatePtr consume(std::string_view data, StatePtr state);
    void stop_and_print_metrics();

    static void log_job(const StatementContainer& stms, std::string_view name, Logger logger);
    static void file_job(const StatementContainer& stms);
};

auto Interpreter::Impl::consume(std::string_view data, StatePtr state) -> StatePtr{
    return std::make_shared<State>(
        reader.consume(data, state ? state->reader_state : Reader::StatePtr{})
    );
}

void Interpreter::Impl::stop_and_print_metrics() {
    // stop workers
    log_worker->stop();
    file_worker->stop();
    // wait for completing
    log_worker->join();
    file_worker->join();

    // print metrics
    decltype(auto) reader_metrics = reader.get_metrics();

    std::ostringstream os;
    os << '[' << name << "] Metrics" << std::endl;
    os << "\tReader:" << std::endl;
    os
        << "\t\tlines - " << reader_metrics.nlines
        << "; statements - " << reader_metrics.nstatements
        << "; blocks - " << reader_metrics.nblocks
        << std::endl;
    
    os << "\tLog:" << std::endl;
    os
        << "\t\tblocks - " << log_worker->thread_metrics[0].nblocks
        << "; statements - " << log_worker->thread_metrics[0].nstatements
        << std::endl;

    os << "\tFiles:" << std::endl;
    for (auto i = 0u; i < file_worker->thread_metrics.size(); ++i) {
        auto &m = file_worker->thread_metrics[i];
        os
            << "\t#" << i
            << "\tblocks - " << m.nblocks
            << "; statements - " << m.nstatements
            << std::endl;
    }
    
    logger.log(os.str());
}

void Interpreter::Impl::log_job(const StatementContainer& stms, std::string_view name, Logger logger) {
    using namespace std;
    using namespace ranges;

    std::ostringstream os;

    struct LoggerExecuter : Executer {
        
        ostream_joiner<std::string> osj;

        explicit LoggerExecuter(std::ostream& output)
            : osj(output, ", ") {}
        
        void execute(const SomeStatement &stm) override {
            *osj = stm.value();
        }
    } logger_executer { os };

    os << '[' << name << "] bulk: ";
    for (auto& stm : stms)
        stm->execute(logger_executer);
    
    logger.log(os.str());
}

void Interpreter::Impl::file_job(const StatementContainer& stms) {
    using namespace std;
    
    struct Printer : Executer {
        ofstream output;
        void execute(const SomeStatement &stm) override {
            output << stm.value() << endl;
        }
    };
    
    const auto now = chrono::system_clock::now();
    const auto now_ns = chrono::duration_cast<chrono::nanoseconds>(now.time_since_epoch());
    const auto filename = ( boost::format { "bulk_%1%_%2%.log"s }
                                % now_ns.count()
                                % std::this_thread::get_id() ).str();

    Printer printer;

    printer.output.open(filename);

    for (auto& stm : stms)
        stm->execute(printer);

    printer.output.flush();
    printer.output.close();
}

Interpreter::~Interpreter() = default;
Interpreter::Interpreter(Interpreter&&) = default;
Interpreter& Interpreter::operator= (Interpreter&&) = default;
    
Interpreter::Interpreter(Context context, std::string name)
    : priv_(std::make_unique<Impl>(
        std::move(name),
        std::move(context.logger),
        context.block_size,
        context.nthreads))
{}

auto Interpreter::consume(std::string_view data, StatePtr state) -> StatePtr {
    if (priv_->stopped)
        return StatePtr{};

    std::lock_guard l { priv_->guard };
    if (priv_->stopped)
        return StatePtr{};

    return priv_->consume(data, state);
}

void Interpreter::stop_and_log_metrics() const {
    if (priv_->stopped)
        return;

    {
        std::lock_guard l { priv_->guard };
        if (priv_->stopped)
           return;
        priv_->stopped = true;
    }

    priv_->stop_and_print_metrics();
}

} // namespace griha