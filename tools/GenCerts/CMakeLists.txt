# Copyright 2026 Metaversal Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# ---------------------------------------------------------------------------
# GenCerts — regenerates the test CA + leaf + expired certs under tests/certs/.
# Linked against Sneeze so it picks up BoringSSL transitively. Not part of
# CTest — invoke manually when the certificate fixtures need to be refreshed.
# ---------------------------------------------------------------------------

add_executable (GenCerts main.cpp)

target_link_libraries (GenCerts PRIVATE Sneeze)
