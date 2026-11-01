// SPDX-License-Identifier: Apache-2.0
// Modified for topo (fork of btop for Apple Silicon), 2026: see NOTICE.
// Test dell'ordinamento "gpu" dei processi (solo macOS: su Linux/BSD l'ordinamento non esiste).

#include <gtest/gtest.h>

#include <vector>

#include "btop_shared.hpp"

namespace {
	auto make(std::initializer_list<std::pair<size_t, double>> v) -> std::vector<Proc::proc_info> {
		std::vector<Proc::proc_info> out;
		for (auto [pid, gpu] : v) {
			Proc::proc_info p;
			p.pid = pid;
			p.gpu_p = gpu;
			out.push_back(p);
		}
		return out;
	}
	auto pids(const std::vector<Proc::proc_info>& v) -> std::vector<size_t> {
		std::vector<size_t> out;
		for (const auto& p : v) out.push_back(p.pid);
		return out;
	}
}

TEST(proc_sort, gpu_descending_without_clients_last) {
	auto v = make({{1, -1}, {2, 0.0}, {3, 12.5}, {4, -1}, {5, 3.0}});
	Proc::proc_sorter(v, "gpu", false);
	EXPECT_EQ(pids(v), (std::vector<size_t>{3, 5, 2, 1, 4}));
}

TEST(proc_sort, gpu_reverse_keeps_without_clients_last) {
	auto v = make({{1, -1}, {2, 0.0}, {3, 12.5}, {4, -1}, {5, 3.0}});
	Proc::proc_sorter(v, "gpu", true);
	EXPECT_EQ(pids(v), (std::vector<size_t>{2, 5, 3, 1, 4}));
}
