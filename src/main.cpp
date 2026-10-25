#include <iostream>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <thread>
#include <chrono>

#include "Bar.hpp"
#include "Portfolio.hpp"


int main() {
    // 1. Start with $1,000.
    Portfolio portfolio{1000.0};

    const std::vector<Bar> bars{
        {"2026-01-05", 1, 100.0, 102.0},
        {"2026-01-06", 1, 103.0, 101.0},
        {"2026-01-07", 1, 102.0, 105.0},
	};

	std::optional<double> pending_buy;

	for (std::size_t day{}; day < bars.size(); ++day) {
		const Bar& bar = bars[day];

		if (pending_buy.has_value()) {
			const bool bought = portfolio.buy(
				bar.ticker,
				pending_buy.value(),
				bar.open,
				1
			);
			std::cout << "Buy succeeded: " << bought << "\n";
			pending_buy.reset();
		}

		std::cout << bar.date
				  << " | Equity: "
				  << portfolio.equity({{bar.ticker, bar.close}})
				  << "\n";

		// buy on day 1
		if (day == 0) {
			pending_buy = 2.5;
		}

		// buy on day 2
		if (day == 1) {
			pending_buy = 1;
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}