#include "CsvDataFeed.hpp"
#include "Instrument.hpp"
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace algo {
CsvDataFeed::CsvDataFeed(std::string path) : path_(std::move(path)) {}
void CsvDataFeed::open() {
    input_.close();
    input_.clear();
    input_.open(path_);
    line_number_ = 1;
    last_date_.clear();
    seen_.clear();
    std::string header;
    if (!std::getline(input_, header))
        throw std::runtime_error("cannot read CSV: " + path_);
    if (!header.empty() && header.back() == '\r')
        header.pop_back();
    if (header != "date,ticker,open,close")
        throw std::runtime_error("CSV header must be date,ticker,open,close");
}
void CsvDataFeed::reset() {
    open();
}
std::optional<Bar> CsvDataFeed::read_next() {
    if (!input_.is_open())
        throw std::logic_error("feed is not open");
    std::string line;
    if (!std::getline(input_, line)) {
        if (input_.bad())
            throw std::runtime_error("CSV read failed");
        return std::nullopt;
    }
    ++line_number_;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    try {
        std::istringstream in(line);
        std::vector<std::string> f;
        std::string v;
        while (std::getline(in, v, ','))
            f.push_back(v);
        if (f.size() != 4 || line.back() == ',')
            throw std::runtime_error("expected four columns");
        Bar b{f[0], 0, 0, 0};
        if (!valid_timestamp(b.date))
            throw std::runtime_error("expected ISO date or UTC timestamp YYYY-MM-DDTHH:MM:SSZ");
        auto [p, ec] = std::from_chars(f[1].data(), f[1].data() + f[1].size(), b.ticker);
        if (ec != std::errc{} || p != f[1].data() + f[1].size() || b.ticker == 0)
            throw std::runtime_error("invalid ticker");
        std::size_t n;
        b.open = std::stod(f[2], &n);
        if (n != f[2].size())
            throw std::runtime_error("invalid open");
        b.close = std::stod(f[3], &n);
        if (n != f[3].size() || !std::isfinite(b.open) || !std::isfinite(b.close) || b.open <= 0 ||
            b.close <= 0)
            throw std::runtime_error("prices must be finite and positive");
        if (!last_date_.empty() && (b.date.size() != last_date_.size() || b.date < last_date_))
            throw std::runtime_error("timestamps must be ordered and use one format");
        if (b.date != last_date_)
            seen_.clear();
        if (!seen_.insert(b.ticker).second)
            throw std::runtime_error("duplicate ticker/timestamp");
        last_date_ = b.date;
        return b;
    } catch (const std::exception &e) {
        throw std::runtime_error(path_ + ":" + std::to_string(line_number_) + ": " + e.what());
    }
}
void CsvDataFeed::set_range(std::string from, std::string to) {
    for (const auto &value : {from, to})
        if (!value.empty() && (value.size() != 10 || !valid_timestamp(value)))
            throw std::invalid_argument("date range requires YYYY-MM-DD");
    if (!from.empty() && !to.empty() && from > to)
        throw std::invalid_argument("from must precede to");
    from_ = std::move(from);
    to_ = std::move(to);
}
std::optional<Bar> CsvDataFeed::next() {
    while (auto b = read_next()) {
        const auto date = b->date.substr(0, 10);
        if ((!from_.empty() && date < from_) || (!to_.empty() && date > to_))
            continue;
        return b;
    }
    return std::nullopt;
}
} // namespace algo
