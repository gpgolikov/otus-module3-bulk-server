#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio.hpp>

#include "interpreter.h"
#include "logger.h"

using namespace griha;
using namespace std::string_literals;
namespace asio = boost::asio;
using asio::ip::tcp;
using boost::system::error_code;

struct Connection : std::enable_shared_from_this<Connection> {

    constexpr static auto c_buffer_size = 1024u;

    Connection(tcp::socket sock, InterpreterPtr intrp) 
        : socket(std::move(sock))
        , interpreter(std::move(intrp)) {
        data.reserve(c_buffer_size);
    }

    void process() {
        do_read();
    }

    void do_read() {
        async_read_until(socket,
            asio::dynamic_buffer(data), '\n',
            [this, self = shared_from_this()] (const error_code& ec, std::size_t size) {
                if (size != 0) {
                    interpreter->consume(std::string_view { data.data(), size - 1 });
                    data.erase(0, size);
                }
                
                if (!ec)
                    do_read();
                else if (!data.empty())
                        interpreter->consume(data);
            });
    }

    tcp::socket socket;
    InterpreterPtr interpreter;
    std::string data;
};
using ConnectionPtr = std::shared_ptr<Connection>;

struct BulkServer {

    BulkServer(asio::io_context& context, unsigned short port, size_t block_size, size_t nthreads)
        : acceptor(context, tcp::endpoint { tcp::v4(), port })
        , interpreter(std::make_shared<Interpreter>(
            Interpreter::Context { Logger{}, block_size, nthreads },
            "inrpr"s)) {
        do_accept();
    }

    void do_accept() {
        acceptor.async_accept(
            [this] (const error_code& ec, tcp::socket socket) {
                if (!ec)
                    std::make_shared<Connection>(std::move(socket), interpreter)->process();
                do_accept();
            }
        );
    }

    tcp::acceptor acceptor;
    InterpreterPtr interpreter;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: bulk_server <port> <block_size> [<nthreads>]" << std::endl;
        return -1;
    }
    
    asio::io_context context;

    std::thread t_exit([&context]() {
        while (std::cin)
            std::cin.get();
        context.stop();
    });

    BulkServer bulk_server {
        context,
        static_cast<unsigned short>(std::stoul(argv[1])), // port
        std::stoul(argv[2]), // size of block
        argc == 3 ? 2u : std::stoul(argv[3]) // number of threads
    };

    context.run();
    t_exit.join();

    bulk_server.interpreter->stop_and_log_metrics();
}