#include "Decimal.h"
#include "IbkrBroker.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace algo;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
class MockTws {
    int server_{-1};
    std::atomic<int> client_{-1};
    std::thread worker_;
    std::string account_, currency_;
    bool complete_;
    int history_error_;
    static bool read_all(int fd, char *data, std::size_t size) {
        while (size) {
            const auto n = recv(fd, data, size, 0);
            if (n <= 0)
                return false;
            data += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }
    static bool write_all(int fd, const char *data, std::size_t size) {
        while (size) {
            const auto n = send(fd, data, size, MSG_NOSIGNAL);
            if (n <= 0)
                return false;
            data += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }
    static std::string frame(int fd) {
        std::uint32_t length;
        if (!read_all(fd, reinterpret_cast<char *>(&length), 4))
            return {};
        length = ntohl(length);
        if (!length || length > 100000)
            return {};
        std::string message(length, '\0');
        if (!read_all(fd, message.data(), length))
            return {};
        return message;
    }
    static bool send_frame(int fd, std::initializer_list<std::string> fields) {
        std::string body;
        for (const auto &f : fields) {
            body += f;
            body += '\0';
        }
        const auto size = htonl(static_cast<std::uint32_t>(body.size()));
        return write_all(fd, reinterpret_cast<const char *>(&size), 4) &&
               write_all(fd, body.data(), body.size());
    }
    void serve() {
        const int fd = accept(server_, nullptr, nullptr);
        client_ = fd;
        if (fd < 0)
            return;
        timeval timeout{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char prefix[4];
        if (!read_all(fd, prefix, 4) || std::memcmp(prefix, "API\0", 4) != 0)
            return;
        if (frame(fd).empty())
            return;
        send_frame(fd, {"178", "20260101 00:00:00 UTC"});
        if (frame(fd).empty())
            return; // startApi from the real SDK.
        send_frame(fd, {"9", "1", "1000"});
        send_frame(fd, {"15", "1", account_});
        while (true) {
            const auto request = frame(fd);
            if (request.empty())
                return;
            std::vector<std::string> fields;
            for (std::size_t begin = 0, end; begin < request.size(); begin = end + 1) {
                end = request.find('\0', begin);
                if (end == std::string::npos) break;
                fields.push_back(request.substr(begin, end - begin));
            }
            const auto id = fields.at(0);
            if (id == "9") { // reqContractDetails version 8, request ID.
                const auto req = fields.at(2);
                send_frame(fd, {"10", req, "AAPL", "STK", "", "0", "", "SMART", "USD", "AAPL",
                    "NMS", "NMS", "265598", "0.01", "", "MKT,LMT", "SMART", "1", "0", "Apple", "NASDAQ",
                    "", "", "", "", "US/Eastern", "", "", "", "0", "0", "1", "", "", "", "", "COMMON", "1", "1", "1"});
                send_frame(fd, {"52", "1", req});
            }
            if (id == "20") { // reqHistoricalData: version omitted at server version 178.
                const auto req = fields.at(1);
                send_frame(fd, {"4", "2", req, "2188", "Up-to-the-second historical data requires additional subscription for the API.", ""});
                if (history_error_ == 1) {
                    send_frame(fd, {"4", "2", req, "162", "Historical data permission denied", ""});
                } else if (history_error_ == 0) {
                    send_frame(fd, {"17", req, "20260101", "20260102", "2",
                        "20260101", "100", "102", "99", "101", "1000", "100", "10",
                        "20260102", "101", "103", "100", "102", "1000", "101", "10"});
                }
            }
            if (id == "1") { // reqMktData
                const auto req = fields.at(2);
                send_frame(fd, {"4", "2", req, "10167", "Displaying delayed market data", ""});
                send_frame(fd, {"58", "1", req, "3"});
                send_frame(fd, {"1", "3", req, "66", "-1", "0", "0"});
                send_frame(fd, {"1", "3", req, "67", "101.25", "1", "0"});
                send_frame(fd, {"1", "3", req, "68", "101", "1", "0"});
            }
            if (id == "61")
                send_frame(fd, {"62", "1"}); // positionEnd
            if (id == "16")
                send_frame(fd, {"53", "1"}); // openOrderEnd
            if (id == "62") {
                send_frame(fd, {"63", "1", "1", account_, "TotalCashValue", "12345.50", currency_});
                if (complete_)
                    send_frame(fd, {"64", "1", "1"});
            }
        }
    }

  public:
    int port{};
    MockTws(std::string account = "DU_TEST", std::string currency = "USD", bool complete = true, int history_error = 0)
        : account_(std::move(account)), currency_(std::move(currency)), complete_(complete), history_error_(history_error) {
        server_ = socket(AF_INET, SOCK_STREAM, 0);
        require(server_ >= 0, "socket failed");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(server_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "bind failed");
        socklen_t size = sizeof(addr);
        require(getsockname(server_, reinterpret_cast<sockaddr *>(&addr), &size) == 0, "getsockname failed");
        port = ntohs(addr.sin_port);
        require(listen(server_, 1) == 0, "listen failed");
        worker_ = std::thread([this] { serve(); });
    }
    ~MockTws() {
        if (client_ >= 0)
            shutdown(client_, SHUT_RDWR);
        shutdown(server_, SHUT_RDWR);
        if (worker_.joinable())
            worker_.join();
        if (client_ >= 0)
            close(client_);
        close(server_);
    }
};
} // namespace
int main() {
    try {
        require(std::abs(DecimalFunctions::decimalToDouble(DecimalFunctions::doubleToDecimal(0.001)) -
                         0.001) < 1e-12,
                "decimal ABI roundtrip");
        {
            MockTws server;
            IbkrBroker broker({"127.0.0.1", server.port, 17, "DU_TEST", true, "USD", 2, 30});
            bool snapshot = false;
            BrokerCallbacks cb;
            cb.on_snapshot = [&](const auto &s) {
                require(s.complete, "snapshot incomplete");
                require(std::abs(s.cash - 12345.50) < 1e-9, "cash decode");
                require(s.account == "DU_TEST", "account identity");
                snapshot = true;
            };
            broker.set_callbacks(cb);
            broker.connect();
            require(!broker.ready(), "socket handshake must not imply readiness");
            broker.request_snapshot();
            require(snapshot && broker.ready(), "snapshot end-marker gating");
            bool denied = false;
            try {
                broker.submit({});
            } catch (const std::exception &) {
                denied = true;
            }
            require(denied, "read-only submission must fail");
            broker.disconnect();
            require(!broker.ready(), "disconnect readiness");
        }
        {
            IbkrBroker writable({"127.0.0.1", 4002, 17, "DU_TEST", false, "USD", 1, 30});
            bool denied = false;
            try { writable.quote(1, 3); }
            catch (const std::runtime_error &) { denied = true; }
            require(denied, "diagnostic quotes must reject an order-enabled adapter before network access");
        }
        for (int permission_error : {0, 1, 2}) {
            MockTws server("DU_TEST", "USD", true, permission_error);
            IbkrBroker broker({"127.0.0.1", server.port, 17, "DU_TEST", true, "USD", 1, 30});
            Instrument i;
            i.ticker = 1;
            i.symbol = "AAPL";
            i.con_id = 265598;
            broker.add_instrument(i);
            int notices = 0;
            BrokerCallbacks cb;
            cb.on_notice = [&](const auto &) { ++notices; };
            broker.set_callbacks(cb);
            broker.connect();
            broker.request_snapshot();
            bool failed = false;
            try {
                const auto bars = broker.historical_bars(1, "2 D", "1 day");
                require(bars.size() == 2 && bars[0].close == 101 && bars[1].date == "2026-01-02",
                        "2188 advisory must not discard historical bars");
            } catch (const std::runtime_error &) { failed = true; }
            require(failed == (permission_error != 0), "real errors/missing end marker must remain fatal");
            require(notices == 1, "2188 must be delivered as notice");
            if (!permission_error) {
                const auto q = broker.quote(1, 3);
                require(q.market_data_type == 3 && !q.bid && q.ask == 101.25 && q.last == 101,
                        "delayed quote fields/type and unavailable-price filtering");
                require(notices == 2, "delayed subscription notice must remain visible");
            }
            broker.disconnect();
        }
        for (int scenario = 0; scenario < 3; ++scenario) {
            MockTws server(scenario == 0 ? "DU_OTHER" : "DU_TEST", scenario == 1 ? "EUR" : "USD",
                           scenario != 2);
            IbkrBroker broker({"127.0.0.1", server.port, 17, "DU_TEST", true, "USD", 1, 30});
            bool failed = false;
            try {
                broker.connect();
                broker.request_snapshot();
            } catch (const std::exception &) {
                failed = true;
            }
            require(failed, "wrong account/currency or missing end-marker must fail");
            require(!broker.ready(), "failed synchronization cannot enable trading");
            broker.disconnect();
        }
        std::cout << "IBKR real-SDK loopback tests passed: handshake, account/currency verification, "
                     "snapshot end markers, timeout, read-only gate, disconnect, decimal ABI, historical advisory/errors and delayed quotes\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "IBKR test failed: " << e.what() << '\n';
        return 1;
    }
}
