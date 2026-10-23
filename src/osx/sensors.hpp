/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   Modified for sitop (fork of btop for Apple Silicon), 2026: see NOTICE.

indent = tab
tab-size = 4
*/

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <Availability.h>
#if __MAC_OS_X_VERSION_MIN_REQUIRED > 101504

namespace Cpu {
	class ThermalSensors {
	   public:
		long long getSensors();
		long long getSensors(std::vector<long long>& core_temps);
	};

	//? sitop: lettore HID persistente per i sensori di temperatura selezionati (cluster CPU, GPU).
	//? Il client e l'elenco dei servizi restano aperti; l'elenco viene riletto ogni refresh_s secondi.
	class HidThermal {
	   public:
		explicit HidThermal(std::function<bool(const std::string&)> want, double refresh_s = 30.0);
		~HidThermal();
		HidThermal(const HidThermal&) = delete;
		HidThermal& operator=(const HidThermal&) = delete;
		//? (nome Product, gradi) dei sensori voluti; valori grezzi, il filtro lo applica il chiamante
		std::vector<std::pair<std::string, double>> read();
		//? tutti i nomi dei sensori di temperatura visti (diagnostica)
		std::vector<std::string> allNames();

	   private:
		void refresh();
		std::function<bool(const std::string&)> want_;
		double refresh_s_;
		uint64_t last_refresh_ns_ = 0;
		void* client_ = nullptr;
		void* services_ = nullptr;  //? CFArrayRef
		std::vector<std::pair<std::string, void*>> selected_;  //? servizi voluti (borrowed da services_)
	};
}  // namespace Cpu
#endif
