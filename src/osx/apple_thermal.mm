/* Copyright 2026 the topo contributors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   Modified for topo (fork of btop for Apple Silicon), 2026: see NOTICE.

indent = tab
tab-size = 4
*/

//? Stato termico di sistema (NSProcessInfo.thermalState): 0 nominal, 1 fair, 2 serious, 3 critical.

#import <Foundation/Foundation.h>

extern "C" int soc_thermal_state(void) {
	@autoreleasepool {
		return static_cast<int>([[NSProcessInfo processInfo] thermalState]);
	}
}
